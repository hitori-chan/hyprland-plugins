// hyprbar/util.cpp — small shared helpers: geometry, damage, colors, strings

#include "hyprbar.hpp"

namespace NHyprbar {

    bool warming = false, texStale = false; // the texture rule — see hyprbar.hpp

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

    CHyprColor color(const SP<Config::Values::CColorValue>& v) {
        struct SMemo {
            uint64_t   raw = 0;
            bool       set = false;
            CHyprColor col;
        };
        static std::unordered_map<const void*, SMemo> memo; // main thread only
        auto&                                         M = memo[v.get()];
        if (!M.set || M.raw != (uint64_t)v->value()) {
            M.raw = (uint64_t)v->value();
            M.set = true;
            M.col = CHyprColor{M.raw};
        }
        return M.col;
    }

    bool isTaskOn(const PHLWINDOW& w, const PHLWORKSPACE& ws) {
        return w && ws && w->m_isMapped && !w->isHidden() && w->m_workspace && w->m_workspace->m_id == ws->m_id;
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
