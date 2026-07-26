// common/arrival.hpp — ARRIVAL order for windows, and the cycle that walks it.
//
// The compositor's window list is the Z-ORDER, and click-to-raise rewrites it
// on every press: as a cycle order it makes forward stepping visit everything
// but backward stepping bounce between the two newest raises. awesome listed
// clients in arrival order, stable across raises, so the plugins that present
// or walk a window list keep their own sequence — the bar's tasklist rows and
// hyprclick's focus.byidx.
//
// Each plugin owns its OWN instance (this is a header, so every .so gets its
// own state): cross-plugin state travels over protocol state, never shared
// symbols. Windows are keyed by raw pointer, dropped in forget() from
// window.destroy before the address can be reused.
//
// CALL seqOf FROM window.open. A number is minted on first sight, so whatever
// looks first defines the order: stamp at map and the sequence is arrival, let
// a render walk or onWorkspace's comparator mint them and it is whatever order
// that walk happened to visit — which for a batch seen at once is the Z-order
// this class exists to avoid, frozen permanently.
#pragma once

#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace NHyprCommon {

    class CArrivalOrder {
      public:
        // the window's arrival number, assigned on first sight
        uint64_t seqOf(const void* w) {
            const auto [IT, NEW] = m_seq.try_emplace(w, m_next);
            if (NEW)
                m_next++;
            return IT->second;
        }

        void forget(const void* w) {
            m_seq.erase(w);
        }
        void clear() {
            m_seq.clear();
        }

        // The workspace's windows in arrival order — mapped and visible, plus
        // whatever else the caller excludes. By value on purpose: these are
        // STRONG refs, and a reused buffer would hold the windows it last saw
        // alive between gestures. Called per keybind or wheel notch, never per
        // frame.
        template <typename F>
        std::vector<PHLWINDOW> onWorkspace(const PHLWORKSPACE& ws, F&& keep) {
            std::vector<PHLWINDOW> out;
            if (!ws)
                return out;
            for (const auto& W : Desktop::windowState()->windows()) {
                if (!W || !W->m_isMapped || W->isHidden() || !W->m_workspace || W->m_workspace->m_id != ws->m_id)
                    continue;
                if (!keep(W))
                    continue;
                out.push_back(W);
            }
            std::ranges::sort(out, [this](const PHLWINDOW& a, const PHLWINDOW& b) { return seqOf(a.get()) < seqOf(b.get()); });
            return out;
        }

        std::vector<PHLWINDOW> onWorkspace(const PHLWORKSPACE& ws) {
            return onWorkspace(ws, [](const PHLWINDOW&) { return true; });
        }

        // steps from `from`, wrapping both ways. When `from` isn't in the list
        // there is nothing to step from — a focus that sits on another monitor,
        // on a minimized window, or nowhere — so the walk starts at the HEAD
        // rather than treating the head as the origin and stepping off it.
        static PHLWINDOW step(const std::vector<PHLWINDOW>& list, const PHLWINDOW& from, int steps) {
            if (list.empty())
                return nullptr;
            const int N = (int)list.size();
            for (int i = 0; i < N; i++)
                if (list[i] == from)
                    return list[((i + steps) % N + N) % N];
            return list.front();
        }

      private:
        std::unordered_map<const void*, uint64_t> m_seq;
        uint64_t                                  m_next = 0;
    };

} // namespace NHyprCommon
