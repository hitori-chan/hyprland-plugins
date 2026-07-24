// hyprbar/util.cpp — small shared helpers: geometry, damage, colors, strings

#include "hyprbar.hpp"

namespace NHyprbar {

    // ---- helpers ----

    double barHeight() {
        return std::max((double)cfg.height->value(), 8.0);
    }

    void damageBars() {
        if (!g_pHyprRenderer)
            return;
        const double H = Menubar::isOpen ? barHeight() * 2 : barHeight();
        for (const auto& M : State::monitorState()->monitors()) {
            const auto MB = M->logicalBox();
            g_pHyprRenderer->damageBox(CBox{MB.x, MB.y, MB.w, H});
        }
    }

    bool isTaskOn(const PHLWINDOW& w, const PHLWORKSPACE& ws) {
        // minimized windows are hidden but still belong to their tag, like
        // awesome — keep their row; other hidden windows (swallowed) stay out.
        return w && ws && w->m_isMapped && w->m_workspace && w->m_workspace->m_id == ws->m_id && (!w->isHidden() || Tasklist::isMinimized(w));
    }

    std::string lower(std::string s) {
        for (auto& c : s)
            c = std::tolower((unsigned char)c);
        return s;
    }

    std::string letterOf(const std::string& s) {
        if (s.empty())
            return "?";
        size_t n = 1;
        while (n < s.size() && (s[n] & 0xC0) == 0x80)
            n++;
        std::string L = s.substr(0, n);
        if (n == 1)
            L[0] = std::toupper((unsigned char)L[0]);
        return L;
    }

} // namespace NHyprbar
