// hyprplace — spawn placement for floating windows:
//
//   1. an app reopens where its last window closed (per class, persisted
//      across relogs), when that spot is free — the app coming back, not a
//      second window of one already on screen (that places fresh, step 2)
//   2. otherwise the spot that overlaps the other windows the least —
//      KWin's default. A lone window keeps the compositor's centered spot
//      (nothing to overlap), a busy screen fills the gaps, and a full one
//      lands where it hides the least. No cascade, no center pile.
//
// Memory-first is what desktops converge on (macOS window restoration,
// Windows SetWindowPlacement); on X11 the apps did it themselves and
// Wayland toplevels can't, so the compositor remembers for them. The
// least-overlap fallback is KWin's default: it fills free space and, when
// the screen is full, minimizes how much windows cover each other. Windows
// that chose their spot (X11, dialogs anchored to a parent) keep it while
// it's free; X11 override-redirect surfaces are left alone; the result is
// clamped fully on-screen, border included (no_offscreen), unless the
// window is too big to fit. The whole close-box is remembered, and a
// genuinely resizable app is BORN at its remembered size: the
// window.predictSize hook fills the initial configure, so the client's
// first buffer is already the remembered size — no post-map resize, no
// second configure, nothing owned or re-asserted. A client whose
// resizability can't be read that early falls back to one ordinary
// configure at map. Unlike the old force path (client-serial stomp +
// forced configure), a grant in flight (born-fullscreen/maximized) keeps
// winning and the client's own later resizes are never fought. Fixed-size
// dialogs (min == max) keep the client's size and are never resized.
// Maximized windows AND floats sized to the whole workarea consume no
// free space; the placement scan then puts a new window where it overlaps
// them the least.

#include "common/lifecycle.hpp"
#include "common/persist.hpp"
#include "common/queries.hpp"

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
        NHyprCommon::CHop         pendingPlace;
        std::vector<PHLWINDOWREF> placeQueue;

        // each app's last window box (position + size), surviving relogs
        std::unordered_map<std::string, CBox> g_lastSpot;

        // x y w h class — class last so any app_id parses. Legacy rows are
        // x y class (position only): size stays zero until the app closes
        // once and a full box is recorded.
        void loadSpots() {
            std::ifstream f(NHyprCommon::statePath("hyprplace", "lastspot.tsv"));
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
            std::ostringstream out;
            for (const auto& [CLS, B] : g_lastSpot)
                out << std::llround(B.x) << '\t' << std::llround(B.y) << '\t' << std::llround(B.w) << '\t' << std::llround(B.h) << '\t' << CLS << '\n';
            NHyprCommon::writeAtomic(NHyprCommon::statePath("hyprplace", "lastspot.tsv"), out.str());
        }

        NHyprCommon::CSaver g_saver{saveSpots};

        void rememberSpot(const std::string& cls, const CBox& box) {
            if (cls.empty())
                return;
            const auto IT = g_lastSpot.find(cls);
            if (IT != g_lastSpot.end() && IT->second == box)
                return;
            g_lastSpot[cls] = box;
            g_saver.dirty();
        }

        // a float sized to (or past) the whole workarea is maximized in all
        // but state — it consumes no free space, and its close-spot is not a
        // spot (a workarea-sized Firefox once blanked ALL free area, sending
        // every spawn to the same center)
        bool coversWorkarea(const CBox& b, const CBox& wa) {
            return b.x <= wa.x && b.y <= wa.y && b.x + b.w >= wa.x + wa.w && b.y + b.h >= wa.y + wa.h;
        }

        // Fill the initial configure with the remembered size (the
        // window.predictSize emission at the initial commit): the client's
        // first buffer is then already right and the map-time pass only
        // positions. Guards mirror placeWindow's size selection;
        // m_initialClass isn't captured that early, so the class comes off
        // the toplevel state directly (applied before this emission).
        void onPredictSize(PHLWINDOW w, Vector2D& size) {
            if (!w || w->parent() || !NHyprCommon::resizable(w))
                return;
            const auto CLS = w->fetchClass();
            if (CLS.empty())
                return;
            const auto IT = g_lastSpot.find(CLS);
            if (IT == g_lastSpot.end() || IT->second.w <= 5 || IT->second.h <= 5)
                return;
            for (const auto& O : Desktop::windowState()->windows())
                if (O != w && O->m_isMapped && !O->isHidden() && O->m_initialClass == CLS)
                    return; // a sibling is on screen: place fresh (see placeWindow)
            size = IT->second.size();
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

            // a client-maximized (hyprmax) or workarea-filling window is not
            // ours to place or resize — symmetric with onWindowClose. Without
            // this we reimpose a born-maximized app's old windowed box and
            // silently un-maximize it (isFullscreen alone misses it: hyprmax's
            // maximize never enters compositor fullscreen).
            if (NHyprCommon::toldMaximized(w) || coversWorkarea(CUR, WA))
                return;

            // the visible floating windows to stay clear of; maximized and
            // fullscreen ones cover no free space, as in awesome
            std::vector<CBox> blockers;
            for (const auto& O : Desktop::windowState()->windows()) {
                if (O == w || !O->m_isMapped || O->isHidden() || !O->m_isFloating || !O->m_target)
                    continue;
                if (O->m_workspace != WS && !(O->m_pinned && O->m_monitor.lock() == MON))
                    continue;
                if (Fullscreen::controller()->isFullscreen(O) || NHyprCommon::toldMaximized(O))
                    continue;
                const auto OB = O->m_target->position();
                if (coversWorkarea(OB, WA))
                    continue;
                blockers.push_back(OB);
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
            // app is resizable and a real size was remembered — then the
            // remembered box is applied whole, once. The remembered spot is
            // for an app coming back, not for a second window of an app
            // already on screen (a dialog or webview sharing the main
            // window's class — Telegram's Instant View is
            // org.telegram.desktop too — would inherit a box that clips it):
            // with a sibling of this class visible, place fresh.
            const bool          RESIZABLE = NHyprCommon::resizable(w);
            std::optional<CBox> stored;
            if (!w->m_isX11 && !w->parent()) {
                bool sibling = false;
                for (const auto& O : Desktop::windowState()->windows()) {
                    if (O != w && O->m_isMapped && !O->isHidden() && O->m_initialClass == w->m_initialClass) {
                        sibling = true;
                        break;
                    }
                }
                if (!sibling)
                    if (const auto IT = g_lastSpot.find(w->m_initialClass); IT != g_lastSpot.end())
                        stored = IT->second;
            }
            Vector2D size = CUR.size();
            if (RESIZABLE && stored && stored->w > 5 && stored->h > 5)
                size = stored->size();

            // no_offscreen: nudge the box fully into the workarea AND leave a
            // border's width of margin — the border is drawn outside the box,
            // so a box flush to the workarea edge clips it. Used for the
            // remembered spot and the final placement alike, so a window
            // dragged against an edge before close reopens against it, border
            // shown, never discarded to center. A window too big to fit even
            // without the margin drops it on that axis rather than going
            // off-screen — and a maximized/workarea-filling window, wider than
            // the margin allows, is left exactly where it is.
            const double BORDER    = std::max(0, w->getRealBorderSize());
            const auto   clampToWA = [&](const Vector2D& p) {
                const double mx  = size.x + 2 * BORDER <= WA.w ? BORDER : 0;
                const double my  = size.y + 2 * BORDER <= WA.h ? BORDER : 0;
                const double loX = WA.x + mx, hiX = WA.x + WA.w - mx - size.x;
                const double loY = WA.y + my, hiY = WA.y + WA.h - my - size.y;
                return Vector2D{std::clamp(p.x, loX, std::max(loX, hiX)), std::clamp(p.y, loY, std::max(loY, hiY))};
            };

            std::optional<Vector2D> pos;

            if (w->m_isX11 || w->parent()) {
                // the window chose this spot (X11 geometry, parent-anchored
                // dialog): keep it while it's free
                if (fits(CBox{CUR.pos(), size}))
                    return;
            } else {
                // 1: where — and, for a resizable app, how big — this app's
                // last window closed, clamped on-screen so a spot that ran
                // past an edge is honored (against the edge) rather than lost
                if (stored) {
                    const auto P = clampToWA(stored->pos());
                    if (fits(CBox{P, size}))
                        pos = P;
                }
            }

            // Memory missed (or its spot is taken): least-overlap placement,
            // KWin's default. A least-overlap top-left always sits at a grid
            // point of the windows' own edges (and the workarea corner), so
            // score the window there and keep the clearest — starting from,
            // and so preferring, its current centered spot. A lone window
            // stays centered, a busy screen fills the gaps top-left first, a
            // full one hides where it can (windows wider than the free space
            // have no gap and settle into the corners). One pass, no cascade,
            // no center pile.
            if (!pos) {
                const auto overlapAt = [&](const Vector2D& p) {
                    double sum = 0.0;
                    for (const auto& B : blockers) {
                        const double ix = std::min(p.x + size.x, B.x + B.w) - std::max(p.x, B.x);
                        const double iy = std::min(p.y + size.y, B.y + B.h) - std::max(p.y, B.y);
                        if (ix > 0 && iy > 0)
                            sum += ix * iy;
                    }
                    return sum;
                };

                // dedup so an aligned grid of windows stays a few coordinates
                std::vector<double> xs{WA.x}, ys{WA.y};
                for (const auto& B : blockers) {
                    xs.insert(xs.end(), {B.x, B.x + B.w});
                    ys.insert(ys.end(), {B.y, B.y + B.h});
                }
                std::sort(xs.begin(), xs.end());
                std::sort(ys.begin(), ys.end());
                xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
                ys.erase(std::unique(ys.begin(), ys.end()), ys.end());

                Vector2D best   = clampToWA(CUR.pos());
                double   bestOv = overlapAt(best);
                for (const double X : xs) {
                    if (bestOv <= 1.0) // a zero-overlap gap — nothing beats it
                        break;
                    for (const double Y : ys) {
                        const Vector2D P  = clampToWA(Vector2D{X, Y});
                        const double   OV = overlapAt(P);
                        if (OV < bestOv - 1.0) {
                            bestOv = OV;
                            best   = P;
                        }
                    }
                }
                pos = best;
            }

            const Vector2D chosen = pos.value_or(CUR.pos());

            // no_offscreen: clamp into the workarea
            const auto   FINAL = clampToWA(chosen);
            const double nx = FINAL.x, ny = FINAL.y;

            if (nx == CUR.x && ny == CUR.y && size == CUR.size())
                return;
            // through the layout so the floating algorithm's lastBox tracking
            // follows the placement (a raw target move leaves it stale and a
            // fullscreen roundtrip would restore the pre-placement spot). The
            // size change goes out as one ordinary configure — no serial
            // ownership, no force: a client-size grant in flight still wins,
            // and the client's own later resizes are never fought.
            g_layoutManager->setTargetGeom(CBox{nx, ny, size.x, size.y}, w->m_target);
            w->m_target->warpPositionSize();
            if (size != CUR.size())
                w->sendWindowSize();
        }
    }

    void onWindowOpen(PHLWINDOW w) {
        // deferred out of the map emission; runs before the first frame
        // renders. Several windows can map in one dispatch — queue them all
        // and drain once: re-arming the lock cancels the previous callback,
        // the queue survives.
        placeQueue.emplace_back(w);
        pendingPlace.arm([]() {
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
        if (NHyprCommon::toldMaximized(w) || Fullscreen::controller()->isFullscreen(w))
            return;
        if (const auto MON = w->m_monitor.lock(); MON && coversWorkarea(w->m_target->position(), MON->logicalBoxMinusReserved()))
            return;
        rememberSpot(w->m_initialClass, w->m_target->position());
    }
}

using namespace NHyprplace;

static HANDLE                  PHANDLE = nullptr;

static NHyprCommon::CLifecycle g_lifecycle;

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

    g_lifecycle.init();
    g_lifecycle.listen(Event::bus()->m_events.window.open, [](PHLWINDOW w) { onWindowOpen(w); });
    g_lifecycle.listen(Event::bus()->m_events.window.close, [](PHLWINDOW w) { onWindowClose(w); });
    g_lifecycle.listen(Event::bus()->m_events.window.predictSize, [](PHLWINDOW w, Vector2D& size) { onPredictSize(w, size); });

    return {"hyprplace", "spawn placement with geometry memory", "hitori", "2.0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_lifecycle.resetAll(); // listeners first, then every hop
    g_saver.flush();        // the deferred flush never runs at compositor exit
    placeQueue.clear();
    g_lastSpot.clear();
}
