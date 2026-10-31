// awesome/notify/popups.cpp — the banner column (glass cards top-right on
// the cards' monitor, newest first, the arrival spring) and the OSD band's
// capsules (top center: the system's volume, brightness, mic, touchpad and
// Do Not Disturb feedback).
//
// Only cards whose banner is up show here (residency hides expired banners
// into the center); while the center is open the column yields to it
// entirely — surface.cpp picks the surface. The capsules show either way.

#include "ui.hpp"

namespace NAwesome::Notify {

    // the OSD band's capsule form: an in-process post that names a glyph
    static bool isCapsule(const SP<SNotif>& n) {
        return n->osdGlyph != 0 && n->urgency < 2;
    }

    bool popupsAnimating() {
        if (!animationsOn())
            return false;
        for (const auto& N : notifs)
            if (!N->waiting && N->banner && !isCapsule(N) && animT(N->born, Theme::MOTION_SPATIAL) < 1.f)
                return true;
        return false;
    }

    void renderPopups(const SPaint& P, const SType& T) {
        const auto   MB  = P.mon->logicalBox();
        const double W   = std::max((double)cfg().getI("plugin:awesome:notify:width"), 120.0);
        const double GAP = std::max((double)cfg().getI("plugin:awesome:notify:margin"), 0.0);
        const double X   = MB.x + MB.w - EDGE - W;
        double       y   = MB.y + cardsTop(P.mon);

        for (const auto& N : notifs) {
            if (N->waiting || !N->banner || isCapsule(N))
                continue; // residency: only banners show here
            if (y + 2 * PADY > MB.y + MB.h)
                break; // no room: the tail waits off-screen, timeouts running

            // per-card arrival motion: fade + an 8 px drop. Keyed on `born`,
            // never `arrived` — a replace refreshes arrived every step and
            // must not re-run the spring.
            SPaint      CP = P;
            const float AT = animationsOn() ? animT(N->born, Theme::MOTION_SPATIAL) : 1.f;
            if (AT < 1.f) {
                CP.alpha = P.alpha * easeOutCubic(AT);
                CP.dy    = P.dy - (1.0 - easeOutBack(AT)) * 8.0;
            }

            SCard        card = recycledCard();
            const double H    = renderCard(CP, T, N, CBox{X, y, W, 0}, SCardForm{.where = SCardForm::BANNER}, card);
            cards.push_back(std::move(card));
            y += H + GAP;
        }
    }

    // ---- the capsules ----
    //
    // 256 x 36 at the top center: the glyph, a level track and its number
    // for a value, the words otherwise ("Muted" in the urgent colour). The
    // system module still posts ordinary cards through the model — the
    // band's replace-in-place, timeouts and the gate's state read stay one
    // path; only their face differs.
    void renderCapsules(const SPaint& P, const SType& T) {
        constexpr double CAP_W = 256, CAP_H = 36, CAP_PADL = 16, CAP_PADR = 16, GLYPH = 16, TRACK_H = 6;
        const auto       MB     = P.mon->logicalBox();
        const auto       COLBG = color(cfg().getColor("plugin:awesome:theme:col_glass")), COLRIM = color(cfg().getColor("plugin:awesome:theme:col_rim")),
                   COLTITLE = color(cfg().getColor("plugin:awesome:notify:col_title")), COLURGENT = color(cfg().getColor("plugin:awesome:notify:col_urgent"));
        const double GAP = std::max((double)cfg().getI("plugin:awesome:notify:margin"), 0.0);
        double       y   = MB.y + cardsTop(P.mon);

        for (const auto& N : notifs) {
            if (N->waiting || !N->banner || !isCapsule(N))
                continue;
            const bool   LEVEL = N->progress >= 0;
            const bool   ALARM = N->osdAlarm; // muted, a mic going live: the words take the urgent colour
            const auto   GL    = Glyph::tex((Glyph::eGlyph)N->osdGlyph, ALARM ? COLURGENT : COLTITLE, (int)std::lround(GLYPH * P.scale));
            // a level capsule's words are its number; a text capsule's are
            // the post's summary, its state riding the body ("Microphone" +
            // "Off" -> "Microphone Off")
            // ("Muted" stands in for a level's number)
            auto& TB = scratch();
            if (LEVEL && ALARM)
                appendEsc(TB, N->body);
            else if (LEVEL)
                TB += std::to_string(N->progress);
            else {
                appendEsc(TB, N->summary);
                if (!N->body.empty()) {
                    TB += ' ';
                    appendEsc(TB, N->body);
                }
            }
            const auto   WORDS = cachedText(TB, ALARM ? COLURGENT : COLTITLE, T.action, (int)std::lround(CAP_W * P.scale), -1, 0, true, LEVEL ? 600 : 500);
            const double WW    = texW(WORDS, P.scale);
            const double CW    = LEVEL ? CAP_W : std::clamp(CAP_PADL + GLYPH + 12 + WW + CAP_PADR, CAP_H * 2, CAP_W);
            const CBox   CAP{MB.x + (MB.w - CW) / 2, y, CW, CAP_H};

            P.frost(CAP, COLBG, COLRIM, (int)std::lround(CAP_H / 2 * P.scale), 2.f);
            const double GX = CAP.x + CAP_PADL;
            if (GL)
                P.texIn(GL, CBox{GX, CAP.y, GLYPH, CAP_H});
            if (LEVEL) {
                // the number's column is fixed (three digits), so the track
                // never jumps as the level crosses 10 or 100
                const double NUMW = ALARM ? WW : std::max(WW, 26.0);
                const double TX0 = GX + GLYPH + 12, TX1 = CAP.x + CAP.w - CAP_PADR - NUMW - 12;
                const CBox   TRK{TX0, CAP.y + (CAP_H - TRACK_H) / 2, std::max(0.0, TX1 - TX0), TRACK_H};
                const int    TR = (int)std::lround(TRACK_H / 2 * P.scale);
                P.rect(TRK, tTrack(), TR, 2.f);
                if (N->progress > 0)
                    P.rect(CBox{TRK.x, TRK.y, std::max(TRACK_H, TRK.w * std::min(N->progress, 100) / 100.0), TRACK_H}, ALARM ? tFillHover() : COLTITLE, TR, 2.f);
                if (WORDS && WORDS->tex)
                    texAt(P, WORDS, CAP.x + CAP.w - CAP_PADR - WW, CAP.y + (CAP_H - texH(WORDS, P.scale)) / 2);
            } else if (WORDS && WORDS->tex)
                texAt(P, WORDS, GX + GLYPH + 12, CAP.y + (CAP_H - texH(WORDS, P.scale)) / 2);

            // a capsule takes no input: it is pushed for the damage and the
            // pass's bounds, and the hit-tests skip it
            SCard card = recycledCard();
            card.kind  = SCard::CAPSULE;
            card.box   = CAP;
            card.id    = N->id;
            cards.push_back(std::move(card));
            y += CAP_H + GAP;
        }
    }

} // namespace NAwesome::Notify
