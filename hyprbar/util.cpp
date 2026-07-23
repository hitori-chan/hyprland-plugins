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
        // island shadows (10 logical px, painted at scale) and glass blur
        // reach past the band; the menubar's floating pill sits below it
        for (const auto& M : State::monitorState()->monitors()) {
            const double PAD = barBlurRadius() / M->m_scale + 10 + std::ceil(M->m_scale);
            const double H   = (Menubar::isOpen ? barHeight() * 2 + 4 : barHeight()) + PAD;
            const auto   MB  = M->logicalBox();
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
