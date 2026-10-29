// awesome/windows/tile.hpp — a window the plugin set into a slot of the
// work area (a snap half or quarter, or maximized) is a flush tile: square
// corners, no drop shadow and no border, its surface the whole slot.
// Rounded, two snapped halves left wallpaper notches at the screen's
// corners and at both ends of their seam, and the focused one's drop fell
// across its neighbour; a border of any colour drew a frame round every
// tile and a double line down every seam (see-through, the wallpaper's;
// opaque, a grid no float wears). The look is set at the layout's override
// priority, so the user's window rules and setprop still win, and it
// leaves with the slot: a drag lifts a snapped window out, unmaximize
// restores it.
#pragma once

#include "core/queries.hpp"
#include "core/supervisor.hpp"

#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/desktop/view/window/WindowStateTracker.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <unordered_map>

namespace NAwesome::Windows::Tile {

    namespace {
        struct STile {
            bool          snapped   = false;
            bool          maximized = false;
            PHLMONITORREF snapMon; // a snap slot is that output's
        };
        inline std::unordered_map<PHLWINDOWREF, STile>& tiles() {
            static std::unordered_map<PHLWINDOWREF, STile> M;
            return M;
        }

        inline void look(const PHLWINDOW& w, bool flush) {
            auto& R = *w->m_ruleApplicator;
            g_pHyprRenderer->damageWindow(w); // the corners and the drop it had
            if (flush) {
                R.rounding().set(0, Desktop::Types::PRIORITY_LAYOUT);
                R.noShadow().set(true, Desktop::Types::PRIORITY_LAYOUT);
                R.borderSize().set(0, Desktop::Types::PRIORITY_LAYOUT);
            } else {
                R.rounding().unset(Desktop::Types::PRIORITY_LAYOUT);
                R.noShadow().unset(Desktop::Types::PRIORITY_LAYOUT);
                R.borderSize().unset(Desktop::Types::PRIORITY_LAYOUT);
            }
            w->presentation().refreshValues();
            g_pHyprRenderer->damageWindow(w);
        }

        template <typename F>
        inline void update(const PHLWINDOW& w, F&& edit) {
            if (!w || compositorShuttingDown())
                return;
            const PHLWINDOWREF WR{w};
            auto&              T      = tiles()[WR];
            const bool         BEFORE = T.snapped || T.maximized;
            edit(T);
            const bool AFTER = T.snapped || T.maximized;
            if (!AFTER)
                tiles().erase(WR);
            if (AFTER != BEFORE)
                look(w, AFTER);
        }
    }

    inline bool isSnapped(const PHLWINDOW& w) {
        const auto IT = tiles().find(PHLWINDOWREF{w});
        return IT != tiles().end() && IT->second.snapped;
    }

    // per drag motion too: leaving is a lookup until the window was snapped
    inline void snapped(const PHLWINDOW& w, bool on) {
        if (!on && !isSnapped(w))
            return;
        update(w, [&](STile& t) {
            t.snapped = on;
            t.snapMon = on ? w->m_monitor : PHLMONITORREF{};
        });
    }

    inline void maximized(const PHLWINDOW& w, bool on) {
        update(w, [&](STile& t) { t.maximized = on; });
    }

    inline void init() {
        auto& EV = Event::bus()->m_events;
        supervisor().listen(EV.window.destroy, [](PHLWINDOWREF wr) { tiles().erase(wr); });
        // a slot is a floating box on one output: a re-tile or a move to
        // another output's workspace takes the window out of it
        supervisor().listen(EV.window.updateState, [](PHLWINDOW w, const Desktop::View::SWindowStateUpdate& U) {
            if (w && U.floating == false)
                snapped(w, false);
        });
        supervisor().listen(EV.window.moveToWorkspace, [](PHLWINDOW w, PHLWORKSPACE ws) {
            if (const auto IT = tiles().find(PHLWINDOWREF{w}); IT != tiles().end() && IT->second.snapped && ws && ws->m_monitor.lock() != IT->second.snapMon.lock())
                snapped(w, false);
        });
    }

    // an unload gives every tile its corners and drop back (a hot-swap's new
    // instance re-adopts the maximized ones); a compositor shutting down is
    // left alone (invariant 8)
    inline void teardown() {
        if (!compositorShuttingDown())
            for (const auto& [WR, T] : tiles())
                if (const auto W = WR.lock(); W && W->m_ruleApplicator)
                    look(W, false);
        tiles().clear();
    }

} // namespace NAwesome::Windows::Tile
