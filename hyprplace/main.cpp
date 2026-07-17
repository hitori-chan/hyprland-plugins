// hyprplace — spawn placement for floating windows:
//
//   1. an app reopens where its last window closed (per class, persisted
//      across relogs), when that spot is free
//   2. otherwise the workarea center, when free
//   3. otherwise the middle of the largest free rectangle that fits it —
//      of the biggest hole when nothing does. Open space, never a border.
//
// Memory-first is what desktops converge on (macOS window restoration,
// Windows SetWindowPlacement); on X11 the apps did it themselves and
// Wayland toplevels can't, so the compositor remembers for them. The gap
// step keeps awesome's spread-into-free-space feel without
// awful.placement's corner packing. Windows that chose their spot (X11,
// dialogs anchored to a parent) keep it while it's free; X11
// override-redirect surfaces are left alone; the result is clamped
// on-screen (no_offscreen). Only the position is remembered — the size
// is always the client's.

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace NHyprplace {

    namespace {
        UP<SEventLoopDoLaterLock>                 pendingPlace;
        UP<SEventLoopDoLaterLock>                 pendingSave;
        std::vector<PHLWINDOWREF>                 placeQueue;
        bool                                      saveQueued = false;

        // where each app's last window closed, surviving relogs
        std::unordered_map<std::string, Vector2D> g_lastSpot;

        std::filesystem::path                     statePath() {
            const char* XDG  = std::getenv("XDG_STATE_HOME");
            const char* HOME = std::getenv("HOME");
            const auto  BASE = XDG && *XDG ? std::filesystem::path{XDG} : std::filesystem::path{HOME ? HOME : ""} / ".local/state";
            return BASE / "hyprplace" / "lastspot.tsv";
        }

        // x y class — class last so any app_id parses.
        void loadSpots() {
            std::ifstream f(statePath());
            std::string   line;
            while (std::getline(f, line)) {
                std::istringstream is(line);
                Vector2D           pos;
                std::string        cls;
                if (is >> pos.x >> pos.y && is.get() == '\t' && std::getline(is, cls) && !cls.empty())
                    g_lastSpot[cls] = pos;
            }
        }

        void saveSpots() {
            const auto      PATH = statePath();
            std::error_code ec;
            std::filesystem::create_directories(PATH.parent_path(), ec);

            // temp + rename: a crash mid-write must not eat the whole store
            const auto TMP = PATH.string() + ".tmp";
            {
                std::ofstream f(TMP, std::ios::trunc);
                if (!f)
                    return;
                for (const auto& [CLS, P] : g_lastSpot)
                    f << std::llround(P.x) << '\t' << std::llround(P.y) << '\t' << CLS << '\n';
            }
            std::filesystem::rename(TMP, PATH, ec);
        }

        void rememberSpot(const std::string& cls, const Vector2D& pos) {
            if (cls.empty())
                return;
            const auto IT = g_lastSpot.find(cls);
            if (IT != g_lastSpot.end() && IT->second == pos)
                return;
            g_lastSpot[cls] = pos;

            // coalesce: a mass close (logout) must not storm the disk with
            // one full rewrite per window
            if (saveQueued)
                return;
            saveQueued  = true;
            pendingSave = g_pEventLoopManager->doLaterLock([]() {
                saveQueued = false;
                saveSpots();
            });
        }

        // gears.geometry.rectangle.area_remove: cut a box out of the
        // free-rect list, splitting every rect it intersects into up to 4
        // slivers around it.
        void areaRemove(std::vector<CBox>& areas, const CBox& elem) {
            for (int i = (int)areas.size() - 1; i >= 0; i--) {
                const CBox R = areas[i];
                if (!(elem.x < R.x + R.w && elem.x + elem.w > R.x && elem.y < R.y + R.h && elem.y + elem.h > R.y))
                    continue;
                areas.erase(areas.begin() + i);
                const double IX = std::max(R.x, elem.x), IY = std::max(R.y, elem.y);
                const double IX2 = std::min(R.x + R.w, elem.x + elem.w), IY2 = std::min(R.y + R.h, elem.y + elem.h);
                if (IX > R.x)
                    areas.push_back(CBox{R.x, R.y, IX - R.x, R.h});
                if (IY > R.y)
                    areas.push_back(CBox{R.x, R.y, R.w, IY - R.y});
                if (IX2 < R.x + R.w)
                    areas.push_back(CBox{IX2, R.y, R.x + R.w - IX2, R.h});
                if (IY2 < R.y + R.h)
                    areas.push_back(CBox{R.x, IY2, R.w, R.y + R.h - IY2});
            }
        }

        // maximize is client-only state here (hyprmax never enters compositor
        // fullscreen), so read back what the toplevel was last told.
        bool toldMaximized(PHLWINDOW w) {
            if (w->m_isX11 || !w->m_xdgSurface || !w->m_xdgSurface->m_toplevel)
                return false;
            const auto& STATES = w->m_xdgSurface->m_toplevel->m_pendingApply.states;
            return std::ranges::find(STATES, XDG_TOPLEVEL_STATE_MAXIMIZED) != STATES.end();
        }

        void placeWindow(PHLWINDOW w) {
            // X11 override-redirect surfaces (menus, tooltips) place themselves
            if (!w || !w->m_isMapped || !w->m_isFloating || w->isX11OverrideRedirect() || !w->m_target || Fullscreen::controller()->isFullscreen(w))
                return;
            const auto WS  = w->m_workspace;
            const auto MON = w->m_monitor.lock();
            if (!WS || !MON)
                return;

            const auto        WA  = MON->logicalBoxMinusReserved();
            const auto        CUR = w->m_target->position();

            // the visible floating windows to stay clear of; maximized and
            // fullscreen ones cover no free space, as in awesome
            std::vector<CBox> blockers;
            std::vector<CBox> areas{WA};
            for (const auto& O : Desktop::windowState()->windows()) {
                if (O == w || !O->m_isMapped || O->isHidden() || !O->m_isFloating || !O->m_target)
                    continue;
                if (O->m_workspace != WS && !(O->m_pinned && O->m_monitor.lock() == MON))
                    continue;
                if (Fullscreen::controller()->isFullscreen(O) || toldMaximized(O))
                    continue;
                blockers.push_back(O->m_target->position());
                areaRemove(areas, blockers.back());
            }

            const auto fits = [&](const CBox& b) {
                if (b.x < WA.x || b.y < WA.y || b.x + b.w > WA.x + WA.w || b.y + b.h > WA.y + WA.h)
                    return false;
                for (const auto& B : blockers)
                    if (b.x < B.x + B.w && b.x + b.w > B.x && b.y < B.y + B.h && b.y + b.h > B.y)
                        return false;
                return true;
            };

            std::optional<Vector2D> pos;

            if (w->m_isX11 || w->parent()) {
                // the window chose this spot (X11 geometry, parent-anchored
                // dialog): keep it while it's free
                if (fits(CUR))
                    return;
            } else {
                // 1: where this app's last window closed
                if (const auto IT = g_lastSpot.find(w->m_initialClass); IT != g_lastSpot.end()) {
                    if (fits(CBox{IT->second, CUR.size()}))
                        pos = IT->second;
                }

                // 2: the workarea center
                if (!pos) {
                    const auto CENTERED = WA.pos() + WA.size() / 2.0 - CUR.size() / 2.0;
                    if (fits(CBox{CENTERED, CUR.size()}))
                        pos = CENTERED;
                }
            }

            // 3: the middle of the largest free rect that fits — of the
            // biggest hole when nothing does
            if (!pos && !areas.empty()) {
                const CBox* best = nullptr;
                for (const auto& R : areas)
                    if (R.w >= CUR.w && R.h >= CUR.h && (!best || R.w * R.h > best->w * best->h))
                        best = &R;
                if (!best)
                    for (const auto& R : areas)
                        if (!best || R.w * R.h > best->w * best->h)
                            best = &R;
                pos = best->pos() + best->size() / 2.0 - CUR.size() / 2.0;
            }
            // no free rect at all (workarea fully covered): keep the
            // pre-placed spot, like awesome keeps a geometry that intersects
            // the workarea

            // no_offscreen: clamp into the workarea
            double nx = pos.value_or(CUR.pos()).x, ny = pos.value_or(CUR.pos()).y;
            nx = std::clamp(nx, WA.x, std::max(WA.x, WA.x + WA.w - CUR.w));
            ny = std::clamp(ny, WA.y, std::max(WA.y, WA.y + WA.h - CUR.h));

            if (nx == CUR.x && ny == CUR.y)
                return;
            w->m_target->setPositionGlobal(CBox{nx, ny, CUR.w, CUR.h});
            w->m_target->warpPositionSize();
        }
    }

    void onWindowOpen(PHLWINDOW w) {
        // deferred out of the map emission; runs before the first frame
        // renders. Several windows can map in one dispatch — queue them all
        // and drain once: re-arming the lock cancels the previous callback,
        // the queue survives.
        placeQueue.emplace_back(w);
        pendingPlace = g_pEventLoopManager->doLaterLock([]() {
            for (const auto& REF : placeQueue)
                placeWindow(REF.lock());
            placeQueue.clear();
        });
    }

    void onWindowClose(PHLWINDOW w) {
        // a maximized/fullscreen close-box is the workarea, not a spot; X11
        // windows and dialogs place themselves and never consult the memory
        if (!w || !w->m_isMapped || !w->m_isFloating || !w->m_target || w->m_isX11 || w->parent())
            return;
        if (toldMaximized(w) || Fullscreen::controller()->isFullscreen(w))
            return;
        rememberSpot(w->m_initialClass, w->m_target->position().pos());
    }
}

using namespace NHyprplace;

static HANDLE                                 PHANDLE = nullptr;

static Hyprutils::Signal::CHyprSignalListener lOpen, lClose;

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprplace] Version mismatch: rebuild the plugin against the running Hyprland", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hyprplace] version mismatch");
    }

    loadSpots();

    lOpen  = Event::bus()->m_events.window.open.listen([](PHLWINDOW w) { onWindowOpen(w); });
    lClose = Event::bus()->m_events.window.close.listen([](PHLWINDOW w) { onWindowClose(w); });

    return {"hyprplace", "spawn placement: an app reopens at its last spot, else centered, else the largest gap — never glued to a border", "hitori", "1.0.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    lOpen.reset();
    lClose.reset();
    pendingPlace.reset();
    pendingSave.reset();
    if (saveQueued) // the deferred flush never runs at compositor exit
        saveSpots();
    saveQueued = false;
    placeQueue.clear();
    g_lastSpot.clear();
}
