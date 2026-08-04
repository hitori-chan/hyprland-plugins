// hyprnotify/paint.cpp — the paint context (glass, shadow, textures), the
// shared card recipes, the type scale and the motion curves. The config
// gates and color memos live in common/glass.hpp.

#include "ui.hpp"

#include <cairo/cairo.h>

namespace NHyprnotify {

    namespace {
        std::unordered_map<uint64_t, SP<ITexture>> controlIcons;

        uint64_t controlIconKey(eControlIcon icon, int px, const CHyprColor& color) {
            return ((uint64_t)icon << 56) ^ ((uint64_t)(uint32_t)px << 32) ^ color.getAsHex();
        }

        void strokeIcon(cairo_t* cr, eControlIcon icon, double px) {
            const double S = px / 24.0;
            cairo_set_line_width(cr, std::max(1.0, 2.0 * S));
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

            switch (icon) {
                case eControlIcon::CLOSE:
                    cairo_move_to(cr, 7.0 * S, 7.0 * S);
                    cairo_line_to(cr, 17.0 * S, 17.0 * S);
                    cairo_move_to(cr, 17.0 * S, 7.0 * S);
                    cairo_line_to(cr, 7.0 * S, 17.0 * S);
                    cairo_stroke(cr);
                    break;
                case eControlIcon::APPS:
                    for (int y = 0; y < 3; y++)
                        for (int x = 0; x < 3; x++)
                            cairo_rectangle(cr, (5.0 + x * 6.0) * S, (5.0 + y * 6.0) * S, 3.0 * S, 3.0 * S);
                    cairo_fill(cr);
                    break;
            }
        }
    }

    SP<ITexture> controlIcon(eControlIcon icon, int physicalPx, const CHyprColor& color) {
        const int PX = std::clamp(physicalPx, 8, 256);
        const auto KEY = controlIconKey(icon, PX, color);
        if (const auto IT = controlIcons.find(KEY); IT != controlIcons.end())
            return IT->second;
        if (!warmGate.mayBuild() || !g_pHyprRenderer)
            return {};

        auto* SURF = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, PX, PX);
        auto* CR   = cairo_create(SURF);
        cairo_set_source_rgba(CR, color.r, color.g, color.b, color.a);
        strokeIcon(CR, icon, PX);
        cairo_destroy(CR);
        cairo_surface_flush(SURF);
        auto TEX = g_pHyprRenderer->createTexture(SURF);
        cairo_surface_destroy(SURF);
        if (!TEX)
            return {};
        if (controlIcons.size() >= 128)
            controlIcons.erase(controlIcons.begin());
        controlIcons.emplace(KEY, TEX);
        return TEX;
    }

    void controlIconCacheClear() {
        controlIcons.clear();
    }

    double damageMargin(PHLMONITOR m) {
        // hairlines ride outside boxes, glass grows by the blur radius, and
        // shadows reach further still (their range covers the no-blur case)
        return (m ? std::ceil(m->m_scale) : 1.0) + 1.0 + std::max(blurRadius(), 26.0);
    }

    // ---- the radius family ----

    float rPow() {
        return (float)cfg.roundingPower->value();
    }
    int rPanel(double scale) {
        return (int)std::lround((std::max(0, (int)cfg.rounding->value()) + 6) * scale);
    }
    int rRow(double scale) {
        return std::max(0, (int)std::lround((std::max(0, (int)cfg.rounding->value()) - 2) * scale));
    }
    int rJoint(double scale) {
        return (int)std::lround(STACK_GAP * scale);
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

    void SPaint::ring(const CBox& global, const CHyprColor& c, int round, float rp, double px) const {
        if (warm)
            return;
        // the gradient ctor heap-allocates and OkLab-converts — memoize per color
        static std::unordered_map<uint64_t, Config::CGradientValueData> grads;
        const auto                                                      KEY = c.getAsHex();
        auto                                                            IT  = grads.find(KEY);
        if (IT == grads.end())
            IT = grads.emplace(KEY, Config::CGradientValueData{c}).first;
        g_pHyprOpenGL->renderBorder(toPhys(global), IT->second, {.round = round, .roundingPower = rp, .borderSize = std::max(1, (int)std::lround(px * scale)), .a = alpha});
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

    // Android's conversation icon container: the AVATAR leads — the content
    // image, which for a chat is the sender's face — and the app IDENTITY
    // rides its bottom-right corner as a badge. ONE column says both who sent
    // it and which app carried it; two icons side by side said it twice, and
    // said the app twice over for every card of the same app. A card with no
    // content image leads with its identity only for conversation cards; an
    // ordinary notification keeps the standard single-icon anatomy.
    // Callers gate their layout on hasLeadIcon.
    void paintIconColumn(const SPaint& P, const SNotif& n, const CBox& cell, bool withBadge, float rp) {
        const bool  HASIDENT = n.identTex && n.identTex->m_texID != 0;
        const bool  AVATAR   = n.iconTex && n.iconTex->m_texID != 0 && !n.heroTex;
        const auto& LEAD     = AVATAR ? n.iconTex : n.identTex;
        if (!LEAD || LEAD->m_texID == 0)
            return;

        // Faces are round and app icons are squircles. The control geometry is
        // supplied by the warm-pass AOSP recipes, independent of this radius.
        const bool   ROUNDFACE = AVATAR && n.conversation;
        const double R         = ROUNDFACE ? cell.w / 2 : cell.w * 10.0 / 44.0;
        const float  LRP       = ROUNDFACE ? 2.f : rp;
        P.texFit(LEAD, cell, (int)std::lround(R * P.scale), LRP);

        // renderBorder grows OUTWARD from the box it is given, so every ring
        // here is drawn on a box inset by its own stroke: the band then lands
        // inside the shape's edge instead of making a marked icon bigger than
        // an unmarked one.
        const double RINGPX = cell.w * BADGE_D * BADGE_INSET;
        const auto   INSET  = [&](const CBox& b) { return CBox{b.x + RINGPX, b.y + RINGPX, b.w - 2 * RINGPX, b.h - 2 * RINGPX}; };

        if (!withBadge || !AVATAR || !HASIDENT) {
            // no badge to ring: a marked chat with no face wears it on the
            // icon it does have
            if (n.priority)
                P.ring(INSET(cell), color(cfg.colHighlight), (int)std::lround((R - RINGPX) * P.scale), LRP, RINGPX);
            return;
        }
        const double D = cell.w * BADGE_D, IN = D * BADGE_INSET;
        const CBox   BB{cell.x + cell.w * (1 + BADGE_PROT) - D, cell.y + cell.h * (1 + BADGE_PROT) - D, D, D};
        // The rim's job is to cut the app glyph free of the avatar it sits on.
        // AOSP tints conversation_badge_background (a white oval in the
        // drawable) to the notification's own background colour, so on a phone
        // the rim reads as a gap; our card is glass over whatever is beneath
        // it, and has no one colour to borrow — a near-white disc separates the
        // glyph against a light avatar and a dark one alike.
        P.rect(BB, CHyprColor{Theme::BADGE_RIM}, (int)std::lround(D / 2 * P.scale), 2.f);
        P.texFit(n.identTex, CBox{BB.x + IN, BB.y + IN, D - 2 * IN, D - 2 * IN}, (int)std::lround((D / 2 - IN) * P.scale), 2.f);
        // conversation_icon_badge_ring — the one AOSP ships with
        // visibility=gone and shows only once you mark the conversation. Its
        // stroke IS the rim's width at the rim's place (importance_ring_size
        // is the badge's own 20dp), so marking a chat recolours that band
        // rather than hanging a second circle off the outside of it.
        if (n.priority)
            P.ring(INSET(BB), color(cfg.colHighlight), (int)std::lround((D / 2 - RINGPX) * P.scale), 2.f, RINGPX);
    }

} // namespace NHyprnotify
