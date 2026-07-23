// hyprnotify/paint.cpp — the paint context (glass, shadow, textures), the
// shared card recipes, the type scale and the motion curves. The config
// gates and color memos live in common/glass.hpp.

#include "ui.hpp"

namespace NHyprnotify {

    double damageMargin(PHLMONITOR m) {
        // hairlines ride outside boxes, glass grows by the blur radius, and
        // shadows reach further still (their range covers the no-blur case)
        return (m ? std::ceil(m->m_scale) : 1.0) + 1.0 + std::max(blurRadius(), 26.0);
    }

    // ---- motion ----

    float easeOutCubic(float t) {
        const float U = 1 - t;
        return 1 - U * U * U;
    }
    float easeOutBack(float t) { // the spatial overshoot (damping ~.75)
        const float U = t - 1;
        return 1 + 2.2f * U * U * U + 1.2f * U * U;
    }
    float animT(const Time::steady_tp& since, int ms) {
        const auto EL = std::chrono::duration_cast<std::chrono::milliseconds>(Time::steadyNow() - since).count();
        return std::clamp((float)EL / (float)ms, 0.f, 1.f);
    }

    // ---- the type scale (physical pt) ----

    SType typeScale(double scale) {
        const double FS = (double)cfg.fontSize->value();
        const auto   PT = [&](double logical) { return std::max(1, (int)std::lround(logical * scale)); };
        // the spec's roles off the 12px base: header 11, title 13.5,
        // body 12.5, small 10.5, actions/bar 12.5
        return SType{PT(FS - 1), PT(FS + 1.5), PT(FS + 0.5), PT(FS - 1.5), PT(FS + 0.5), PT(FS + 0.5)};
    }

    // ---- the paint context ----

    CBox SPaint::toPhys(const CBox& global) const {
        return CBox{global}.translate(Vector2D{-monPos.x, -monPos.y + dy}).scale(scale).round();
    }

    void SPaint::rect(const CBox& global, const CHyprColor& c, int round, float rp) const {
        if (warm)
            return;
        g_pHyprOpenGL->renderRect(toPhys(global), c.modifyA(c.a * alpha), {.round = round, .roundingPower = rp});
    }

    void SPaint::glass(const CBox& global, const CHyprColor& c, int round, float rp) const {
        if (warm)
            return;
        g_pHyprOpenGL->renderRect(toPhys(global), c.modifyA(c.a * alpha), {.round = round, .roundingPower = rp, .blur = blurOn(), .blurA = alpha});
    }

    void SPaint::shadow(const CBox& global, int round, float rp, int range) const {
        if (warm)
            return;
        static Config::CGradientValueData GRAD{CHyprColor{Theme::SHADOW}};
        g_pHyprOpenGL->renderRoundedShadow(toPhys(global), round, rp, (int)std::lround(range * scale), GRAD, alpha);
    }

    void SPaint::ring(const CBox& global, const CHyprColor& c, int round, float rp) const {
        if (warm)
            return;
        // the gradient ctor heap-allocates and OkLab-converts — memoize per color
        static std::unordered_map<uint64_t, Config::CGradientValueData> grads;
        const auto                                                      KEY = c.getAsHex();
        auto                                                            IT  = grads.find(KEY);
        if (IT == grads.end())
            IT = grads.emplace(KEY, Config::CGradientValueData{c}).first;
        g_pHyprOpenGL->renderBorder(toPhys(global), IT->second, {.round = round, .roundingPower = rp, .borderSize = std::max(1, (int)std::lround(scale)), .a = alpha});
    }

    void SPaint::tex(const SP<ITexture>& t, double gx, double gy) const {
        if (warm || !t || t->m_texID == 0)
            return;
        const auto P = toPhys(CBox{gx, gy, 1, 1});
        g_pHyprOpenGL->renderTexture(t, CBox{(double)P.x, (double)P.y, t->m_size.x, t->m_size.y}, {.a = alpha});
    }

    void SPaint::texFit(const SP<ITexture>& t, const CBox& cell, int round, float rp) const {
        if (warm || !t || t->m_texID == 0)
            return;
        g_pHyprOpenGL->renderTexture(t, toPhys(cell), {.a = alpha, .round = round, .roundingPower = rp});
    }

    // ---- shared card recipes (popups and center rows drifted apart once —
    //      the badge and the progress pill draw from ONE place now) ----

    bool hasLeadIcon(const SNotif& n) {
        return (n.iconTex && !n.heroTex) || (n.identTex && n.identTex->m_texID != 0);
    }

    void paintProgress(const SPaint& P, double x, double y, double w, int pct, bool critical) {
        const int PR = (int)std::lround(PROGRESS_H / 2 * P.scale);
        P.rect(CBox{x, y, w, PROGRESS_H}, tFill2(), PR);
        if (pct > 0)
            P.rect(CBox{x, y, std::max(w * pct / 100.0, PROGRESS_H), PROGRESS_H}, critical ? color(cfg.colUrgent) : color(cfg.colHighlight), PR);
    }

    // the content-first icon column: the content avatar leads wearing the
    // identity's corner badge (when both exist and differ); identity alone
    // otherwise. Callers gate on hasLeadIcon for their layout.
    void paintIconColumn(const SPaint& P, const SNotif& n, const CBox& cell, bool withBadge, float rp) {
        const bool  HASCONTENT = n.iconTex && !n.heroTex;
        const auto& LEAD       = HASCONTENT ? n.iconTex : n.identTex;
        if (!LEAD || LEAD->m_texID == 0)
            return;
        P.texFit(LEAD, cell, (int)std::lround(cell.w * 10.0 / 44.0 * P.scale), rp);
        const bool HASIDENT = n.identTex && n.identTex->m_texID != 0;
        if (withBadge && HASCONTENT && HASIDENT && (n.hasPixels || n.image != n.identity)) {
            const CBox BB{cell.x + cell.w - BADGE + 2, cell.y + cell.h - BADGE + 2, BADGE, BADGE};
            P.rect(CBox{BB}.expand(1.5), color(cfg.colBg).modifyA(1.0), (int)std::lround((BADGE / 2 + 1.5) * P.scale), 2.f);
            P.texFit(n.identTex, BB, (int)std::lround(BADGE / 2 * P.scale), 2.f);
        }
    }

} // namespace NHyprnotify
