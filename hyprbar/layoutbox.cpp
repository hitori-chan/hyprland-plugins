// hyprbar/layoutbox.cpp — the per-tag layout registry. The compact-islands
// bar carries no layoutbox cell; the registry and its keybinds
// (hl.plugin.hyprbar.layout_next/prev) stay, and future layouts take real
// effect wherever they get implemented.

#include "hyprbar.hpp"

namespace NHyprbar {

    // the ordered registry (order matters, like awful.layout.layouts) and
    // each workspace's index — per tag
    static const std::vector<const char*>          LAYOUTS = {"floating"};
    static std::unordered_map<WORKSPACEID, size_t> wsLayout;

    void                                           layoutInc(int dir) {
        const auto MON = Desktop::focusState() ? Desktop::focusState()->monitor() : nullptr;
        if (!MON || !MON->m_activeWorkspace)
            return;
        const int64_t N   = (int64_t)LAYOUTS.size();
        auto&         IDX = wsLayout[MON->m_activeWorkspace->m_id];
        IDX               = (size_t)(((int64_t)IDX + dir % N + N) % N);
    }

    void layoutboxExit() {
        wsLayout.clear();
    }

} // namespace NHyprbar
