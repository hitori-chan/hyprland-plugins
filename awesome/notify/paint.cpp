// awesome/notify/paint.cpp — the shared card recipes (progress track, icon
// column, glass button), the type scale, the motion curves and the damage
// margin. The paint context itself is the canvas' (core/canvas.hpp).
#include "ui.hpp"

#include "../core/config.hpp"

namespace NAwesome::Notify {

    double damageMargin(PHLMONITOR m) {
        // rims ride outside boxes, glass grows by the blur radius, and the
        // drop reaches further still (its reach covers the no-blur case)
        return (m ? std::ceil(m->m_scale) : 1.0) + 1.0 + std::max(blurRadius(), SPaint::frostReach());
    }

    // ---- the radius ----

    float rPow() {
        return (float)NAwesome::cfg().getD("plugin:awesome:notify:rounding_power");
    }
    int rCard(double scale) {
        return (int)std::lround(std::max(0, NAwesome::cfg().getI("plugin:awesome:notify:rounding")) * scale);
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

    // ---- the type scale (physical px) ----

    SType typeScale(double scale) {
        const double FS = (double)NAwesome::cfg().getI("plugin:awesome:notify:font_size");
        const auto   PX = [&](double logical) { return std::max(1, (int)std::lround(logical * scale)); };
        // off the 13 px base: meta 12, title/body/actions 13, counts 11,
        // the center's heading 15
        return SType{.meta = PX(FS - 1), .title = PX(FS), .body = PX(FS), .small = PX(FS - 2), .action = PX(FS), .heading = PX(FS + 2)};
    }

    // ---- shared card recipes ----

    void prepareCard(const SP<SNotif>& n, double scale) {
        if (!NAwesome::Canvas::inst().gate().warming)
            return;
        const double W    = std::max((double)NAwesome::cfg().getI("plugin:awesome:notify:width"), 120.0);
        const double ICON = std::clamp((double)NAwesome::cfg().getI("plugin:awesome:notify:max_icon"), 16.0, 64.0);
        ensureIconTex(*n, (int)std::lround(ICON * scale), (int)std::lround((W - 2 * PADX) * scale), (int)std::lround(HERO_CAP * scale));
    }

    // The content image in an icon column: the avatar, or in the center
    // (which shows no hero) a hero's square thumbnail. A banner with a hero
    // leads with the app's identity instead: the preview has its own block.
    const SP<ITexture>& contentFace(const SNotif& n, bool shade) {
        static const SP<ITexture> NONE;
        if (!n.iconTex)
            return NONE;
        return !n.heroTex ? n.iconTex : shade ? n.thumbTex : NONE;
    }

    // An icon still decoding reserves its column: the text laid out full
    // width and re-laid narrower when the decode landed, a visible jump and
    // every text rastered twice.
    bool hasLeadIcon(const SNotif& n, bool shade) {
        const bool PENDING = (!n.identity.empty() && !n.identSettled) || (!n.fallbackPick.empty() && !n.identSettled) || (!n.image.empty() && !n.imageSettled) || n.pixelsJob != 0;
        return PENDING || contentFace(n, shade) || (n.identTex && n.identTex->m_texID != 0);
    }

    void paintProgress(const SPaint& P, double x, double y, double w, int pct, bool critical) {
        const int PR = (int)std::lround(PROGRESS_H / 2 * P.scale);
        P.rect(CBox{x, y, w, PROGRESS_H}, tTrack(), PR);
        if (pct > 0)
            P.rect(CBox{x, y, std::max(w * pct / 100.0, PROGRESS_H), PROGRESS_H},
                   critical ? NAwesome::color(NAwesome::cfg().getColor("plugin:awesome:notify:col_urgent")) : NAwesome::color(NAwesome::cfg().getColor("plugin:awesome:notify:col_highlight")), PR);
    }

    // one icon under two paths (a theme's sizes): libnotify sends its icon
    // as the app icon and the image both
    static bool sameIcon(std::string_view a, std::string_view b) {
        const auto STEM = [](std::string_view p) {
            p.remove_prefix(std::min(p.size(), p.rfind('/') + 1));
            return p.substr(0, p.rfind('.'));
        };
        return STEM(a) == STEM(b);
    }

    // The sender's FACE leads and the app's identity rides its corner as a
    // badge: one column says who sent it and which app carried it. A card
    // with no content image leads with its identity (or the rolled fallback
    // face) and wears no badge. Faces are circles, app icons rounded squares.
    void paintIconColumn(const SPaint& P, const SNotif& n, const CBox& cell, bool withBadge, float rp, bool shade) {
        const bool  HASIDENT = n.identTex && n.identTex->m_texID != 0;
        const auto& FACE     = contentFace(n, shade);
        const bool  AVATAR   = FACE && FACE->m_texID != 0;
        const auto& LEAD     = AVATAR ? FACE : n.identTex;
        if (!LEAD || LEAD->m_texID == 0)
            return;

        // a radius of half the box is a circle: rounding power 2
        const bool   ROUNDFACE = AVATAR && n.conversation;
        const double R         = ROUNDFACE ? cell.w / 2 : Theme::RAD_BUTTON * cell.w / 36.0;
        P.texFit(LEAD, cell, (int)std::lround(R * P.scale), ROUNDFACE ? 2.f : rp);

        // only a real app identity badges, and only a face that is not the
        // identity itself: the generic mark or a rolled face says nothing
        // about the app
        if (!withBadge || !AVATAR || !HASIDENT || n.identity.empty() || sameIcon(n.identity, n.image))
            return; // the lead icon is the whole column
        // the white ring cuts the glyph free of a light face and a dark one
        // alike: glass has no one colour to borrow, as a phone's card does.
        // A graphite disc under the glyph keeps a symbolic (white) icon read.
        const double D = cell.w * BADGE_D, OUT = cell.w * BADGE_OUT, RING = D * BADGE_RING;
        const CBox   BB{cell.x + cell.w + OUT - D, cell.y + cell.h + OUT - D, D, D};
        const CBox   IN{BB.x + RING, BB.y + RING, D - 2 * RING, D - 2 * RING};
        const int    RIN = (int)std::lround((D / 2 - RING) * P.scale);
        P.rect(BB, tBadgeRim(), (int)std::lround(D / 2 * P.scale), 2.f);
        P.rect(IN, tBadgeGround(), RIN, 2.f);
        P.texFit(n.identTex, IN, RIN, 2.f);
    }

    void texCentered(const SPaint& P, const SCachedText* t, const CBox& cell) {
        if (t && t->tex)
            P.tex(t->tex, cell.x + (cell.w - t->tex->m_size.x / P.scale) / 2, cell.y + (cell.h - t->tex->m_size.y / P.scale) / 2);
    }

    void paintChip(const SPaint& P, const CBox& box, bool hover) {
        const int R = (int)std::lround(std::min(box.w, box.h) / 2 * P.scale);
        P.shadow(CBox{box}.expand(CHIP_DROP).translate(Vector2D{0.0, CHIP_SINK}), R, 2.f, (int)CHIP_DROP);
        P.glass(box, tChip(), R, 2.f);
        if (hover)
            P.rect(box, tFill(), R, 2.f);
        P.ring(box, NAwesome::color(NAwesome::cfg().getColor("plugin:awesome:notify:col_frame")), R, 2.f);
    }

    // macOS's platter under the pointer: a touch lighter, a touch thicker
    // (rgba 38,38,44,.56 -> 44,44,52,.62 on its board)
    CHyprColor liftedGlass(const CHyprColor& c) {
        const auto UP = [](double v, double by) { return (float)std::min(1.0, v + by); };
        return CHyprColor{UP(c.r, 6 / 255.0), UP(c.g, 6 / 255.0), UP(c.b, 8 / 255.0), UP(c.a, 0.06)};
    }

} // namespace NAwesome::Notify
