// awesome/notify/paint.cpp — the shared card recipes (progress pill, icon
// column), the type scale, the motion curves and the damage margin. The
// paint context itself is the canvas' (core/canvas.hpp).
#include "ui.hpp"

#include "../core/config.hpp"

namespace NAwesome::Notify {

    double damageMargin(PHLMONITOR m) {
        // hairlines ride outside boxes, glass grows by the blur radius, and
        // shadows reach further still (their range covers the no-blur case)
        return (m ? std::ceil(m->m_scale) : 1.0) + 1.0 + std::max(blurRadius(), 26.0);
    }

    // ---- the radius family ----

    float rPow() {
        return (float)NAwesome::cfg().getD("plugin:awesome:notify:rounding_power");
    }
    int rPanel(double scale) {
        return (int)std::lround((std::max(0, NAwesome::cfg().getI("plugin:awesome:notify:rounding")) + 6) * scale);
    }
    int rRow(double scale) {
        return std::max(0, (int)std::lround((std::max(0, NAwesome::cfg().getI("plugin:awesome:notify:rounding")) - 2) * scale));
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
        const double FS = (double)NAwesome::cfg().getI("plugin:awesome:notify:font_size");
        const auto   PT = [&](double logical) { return std::max(1, (int)std::lround(logical * scale)); };
        // the spec's roles off the 12px base: header 11, title 13.5,
        // body 12.5, small 10.5, actions/bar 12.5
        return SType{PT(FS - 1), PT(FS + 1.5), PT(FS + 0.5), PT(FS - 1.5), PT(FS + 0.5), PT(FS + 0.5)};
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
            P.rect(CBox{x, y, std::max(w * pct / 100.0, PROGRESS_H), PROGRESS_H}, critical ? NAwesome::color(NAwesome::cfg().getColor("plugin:awesome:notify:col_urgent"))
                                                                                          : NAwesome::color(NAwesome::cfg().getColor("plugin:awesome:notify:col_highlight")),
                   PR);
    }

    // Android's conversation icon container: the AVATAR leads — the content
    // image, which for a chat is the sender's face — and the app IDENTITY
    // rides its bottom-right corner as a badge. ONE column says both who sent
    // it and which app carried it; two icons side by side said it twice, and
    // said the app twice over for every card of the same app. A card with no
    // content image leads with its identity (or the rolled fallback face) and
    // wears no badge — there would be nothing to distinguish it from.
    // Callers gate their layout on hasLeadIcon.
    void paintIconColumn(const SPaint& P, const SNotif& n, const CBox& cell, bool withBadge, float rp) {
        const bool  HASIDENT = n.identTex && n.identTex->m_texID != 0;
        const bool  AVATAR   = n.iconTex && n.iconTex->m_texID != 0 && !n.heroTex;
        const auto& LEAD     = AVATAR ? n.iconTex : n.identTex;
        if (!LEAD || LEAD->m_texID == 0)
            return;

        // faces are round, app icons are squircles — Android draws the same
        // split. A radius of half the box means CIRCLE, so it takes rounding
        // power 2 (the shell exponent is for card corners; every other circle
        // in the shade — chevron, ✕, pills — is drawn at the rect default).
        const bool   ROUNDFACE = AVATAR && n.conversation;
        const double R         = ROUNDFACE ? cell.w / 2 : cell.w * 10.0 / 44.0;
        const float  LRP       = ROUNDFACE ? 2.f : rp;
        P.texFit(LEAD, cell, (int)std::lround(R * P.scale), LRP);

        if (!withBadge || !AVATAR || !HASIDENT)
            return; // no badge to draw: the lead icon is the whole column
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
    }

} // namespace NAwesome::Notify
