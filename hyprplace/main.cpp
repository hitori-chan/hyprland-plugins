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
// on-screen (no_offscreen). Position is always remembered; a genuinely
// resizable app's last size is remembered too and reimposed at spawn
// (KWin's "Remember" — the compositor owns the configure, so the client's
// own size memory can't fight it and a content-sizer like mpv can't drift
// back to its video size), while fixed-size dialogs (min == max) keep the
// client's size and are never resized. Maximized windows AND floats sized
// to the whole workarea consume no free space; when the chosen spot
// already holds a window, spawns cascade off it instead of stacking.

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
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
        UP<SEventLoopDoLaterLock> pendingPlace;
        UP<SEventLoopDoLaterLock> pendingSave;
        std::vector<PHLWINDOWREF> placeQueue;
        bool                      saveQueued = false;

        // each app's last window box (position + size), surviving relogs
        std::unordered_map<std::string, CBox> g_lastSpot;

        std::filesystem::path                     statePath() {
            const char* XDG  = std::getenv("XDG_STATE_HOME");
            const char* HOME = std::getenv("HOME");
            const auto  BASE = XDG && *XDG ? std::filesystem::path{XDG} : std::filesystem::path{HOME ? HOME : ""} / ".local/state";
            return BASE / "hyprplace" / "lastspot.tsv";
        }

        // x y w h class — class last so any app_id parses. Legacy rows are
        // x y class (position only): size is left zero and stays the
        // client's until the app closes once and a size is recorded.
        void loadSpots() {
            std::ifstream f(statePath());
            std::string   line;
            while (std::getline(f, line)) {
                std::vector<std::string> parts;
                std::string              field;
                std::istringstream       is(line);
                while (std::getline(is, field, '\t'))
                    parts.push_back(field);

                CBox        box;
                std::string cls;
                try {
                    if (parts.size() == 3) {
                        box.x = std::stod(parts[0]);
                        box.y = std::stod(parts[1]);
                        cls   = parts[2];
                    } else if (parts.size() == 5) {
                        box.x = std::stod(parts[0]);
                        box.y = std::stod(parts[1]);
                        box.w = std::stod(parts[2]);
                        box.h = std::stod(parts[3]);
                        cls   = parts[4];
                    } else
                        continue;
                } catch (...) { continue; }

                if (!cls.empty())
                    g_lastSpot[cls] = box;
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
                for (const auto& [CLS, B] : g_lastSpot)
                    f << std::llround(B.x) << '\t' << std::llround(B.y) << '\t' << std::llround(B.w) << '\t' << std::llround(B.h) << '\t' << CLS << '\n';
            }
            std::filesystem::rename(TMP, PATH, ec);
        }

        void rememberSpot(const std::string& cls, const CBox& box) {
            if (cls.empty())
                return;
            const auto IT = g_lastSpot.find(cls);
            if (IT != g_lastSpot.end() && IT->second == box)
                return;
            g_lastSpot[cls] = box;

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

        // a float sized to (or past) the whole workarea is maximized in all
        // but state — it consumes no free space, and its close-spot is not a
        // spot (a workarea-sized Firefox once blanked ALL free area, sending
        // every spawn to the same center)
        bool coversWorkarea(const CBox& b, const CBox& wa) {
            return b.x <= wa.x && b.y <= wa.y && b.x + b.w >= wa.x + wa.w && b.y + b.h >= wa.y + wa.h;
        }

        // A genuinely user-resizable toplevel — its last size is worth
        // restoring (mpv, terminals, browsers). A fixed-size dialog pins
        // min == max in both axes; its size stays the client's, never
        // reimposed (that would blink it — awesome never did). No toplevel
        // (X11, unmapped) = can't tell = treat as fixed.
        bool resizable(PHLWINDOW w) {
            if (w->m_isX11 || !w->m_xdgSurface || !w->m_xdgSurface->m_toplevel)
                return false;
            const auto MIN      = w->m_xdgSurface->m_toplevel->layoutMinSize();
            const auto MAX      = w->m_xdgSurface->m_toplevel->layoutMaxSize();
            const bool PINNED_X = MAX.x > 1 && MIN.x >= MAX.x;
            const bool PINNED_Y = MAX.y > 1 && MIN.y >= MAX.y;
            return !(PINNED_X && PINNED_Y);
        }

        void placeWindow(PHLWINDOW w) {
            // X11 override-redirect surfaces (menus, tooltips) place themselves
            if (!w || !w->m_isMapped || !w->m_isFloating || w->isX11OverrideRedirect() || !w->m_target || Fullscreen::controller()->isFullscreen(w))
                return;
            const auto WS  = w->m_workspace;
            const auto MON = w->m_monitor.lock();
            if (!WS || !MON)
                return;

            const auto WA  = MON->logicalBoxMinusReserved();
            const auto CUR = w->m_target->position();

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
                const auto OB = O->m_target->position();
                if (coversWorkarea(OB, WA))
                    continue;
                blockers.push_back(OB);
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

            // The size the window spawns at: the client's own, unless this
            // app is resizable and a real size was remembered for it. Then
            // reimpose the remembered size and own the configure, so a
            // content-sizer (mpv) can't drift back to its video size and an
            // app that self-remembers its size (Firefox) can't fight ours.
            const bool          RESIZABLE = resizable(w);
            std::optional<CBox> stored;
            if (!w->m_isX11 && !w->parent()) {
                if (const auto IT = g_lastSpot.find(w->m_initialClass); IT != g_lastSpot.end())
                    stored = IT->second;
            }
            Vector2D size      = CUR.size();
            bool     forceSize = false;
            if (RESIZABLE && stored && stored->w > 5 && stored->h > 5) {
                size      = stored->size();
                forceSize = true;
            }

            std::optional<Vector2D> pos;

            if (w->m_isX11 || w->parent()) {
                // the window chose this spot (X11 geometry, parent-anchored
                // dialog): keep it while it's free
                if (fits(CBox{CUR.pos(), size}))
                    return;
            } else {
                // 1: where — and, for a resizable app, how big — this app's
                // last window closed
                if (stored && fits(CBox{stored->pos(), size}))
                    pos = stored->pos();

                // 2: the workarea center
                if (!pos) {
                    const auto CENTERED = WA.pos() + WA.size() / 2.0 - size / 2.0;
                    if (fits(CBox{CENTERED, size}))
                        pos = CENTERED;
                }
            }

            // 3: the middle of the largest free rect that fits — of the
            // biggest hole when nothing does
            if (!pos && !areas.empty()) {
                const CBox* best = nullptr;
                for (const auto& R : areas)
                    if (R.w >= size.x && R.h >= size.y && (!best || R.w * R.h > best->w * best->h))
                        best = &R;
                if (!best)
                    for (const auto& R : areas)
                        if (!best || R.w * R.h > best->w * best->h)
                            best = &R;
                pos = best->pos() + best->size() / 2.0 - size / 2.0;
            }
            // Cascade off any pile: with the workarea fully covered (no pos)
            // or a too-small biggest hole picked deterministically, repeated
            // spawns land on the SAME spot and hide each other — step off it
            // diagonally instead, like macOS/Windows cascade.
            Vector2D chosen = pos.value_or(CUR.pos());
            for (int guard = 0; guard < 64; guard++) {
                bool onPile = false;
                for (const auto& B : blockers)
                    if (std::abs(B.x - chosen.x) < 2.0 && std::abs(B.y - chosen.y) < 2.0) {
                        onPile = true;
                        break;
                    }
                if (!onPile)
                    break;
                chosen += Vector2D{26.0, 26.0};
            }

            // no_offscreen: clamp into the workarea
            double nx = chosen.x, ny = chosen.y;
            nx = std::clamp(nx, WA.x, std::max(WA.x, WA.x + WA.w - size.x));
            ny = std::clamp(ny, WA.y, std::max(WA.y, WA.y + WA.h - size.y));

            if (nx == CUR.x && ny == CUR.y && !forceSize)
                return;
            // through the layout so the floating algorithm's lastBox tracking
            // follows the placement (a raw target move leaves it stale and a
            // fullscreen roundtrip would restore the pre-placement spot). For
            // a remembered size, take the size choice from the client and
            // force the configure out — adoptCompositorMax's proven sequence.
            if (forceSize)
                w->m_sizeFromClientSerial = 0;
            g_layoutManager->setTargetGeom(CBox{nx, ny, size.x, size.y}, w->m_target);
            w->m_target->warpPositionSize();
            if (forceSize)
                w->sendWindowSize(true);
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
        if (const auto MON = w->m_monitor.lock(); MON && coversWorkarea(w->m_target->position(), MON->logicalBoxMinusReserved()))
            return;
        rememberSpot(w->m_initialClass, w->m_target->position());
    }
}

using namespace NHyprplace;

static HANDLE                                 PHANDLE = nullptr;

static Hyprutils::Signal::CHyprSignalListener lOpen, lClose;

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

    return {"hyprplace", "spawn placement with geometry memory", "hitori", "1.2.0"};
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
