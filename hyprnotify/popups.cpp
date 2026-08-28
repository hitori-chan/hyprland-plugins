// hyprnotify/popups.cpp — the v13 banner column: the heads-up cards flush
// under the 26px bar (top-right, the panel's own X and width) and the OSD
// cards that follow an open shade.
//
// A banner IS a collapsed shade card with two deltas (spec §2.6): the action
// row is always on (a banner is the alerting face of the card), and the
// expand chip becomes the shade chevron — a HUN never expands in place, the
// chevron jumps to the shade like the AOSP HUN expand button. Its veil is the
// card color (darker than the panel frost, which it floats over instead of
// joining), and urgent banners wear the urgent title, nothing else.
//
// Ordinary banners yield to the shade while it is open (renderAll); OSD-band
// cards keep this same anatomy and are placed below the open shade.

#include "ui.hpp"

namespace NHyprnotify {

    inline constexpr double HUN_GAP = 8; // the demo's .heads stack gap

    bool popupsAnimating(bool osdOnly) {
        if (!animationsOn())
            return false;
        for (const auto& N : notifs)
            if ((!osdOnly || inOsdBand(N->id)) && !N->waiting && N->banner && animT(N->born, Theme::MOTION_SPATIAL) < 1.f)
                return true;
        return false;
    }

    // ---- action rows: the same borderless tinted buttons as the shade ----

    struct SAct {
        const std::string*   id;
        const std::string*   label;
        const SP<ITexture>*  icon; // null = text only
    };

    // the synthetic inline-reply entry leads when the sender offered it; the
    // rest are the card's actions in Notify order. A HUN's Reply INVOKES the
    // action (the ROM opens the app's reply UI) — input.cpp keeps this card
    // from arming an inline field.
    static void actionsOf(const SNotif& N, std::vector<SAct>& out) {
        out.clear();
        static const std::string  REPLY_ID  = "inline-reply";
        static const std::string  REPLY_LBL = "Reply";
        static const SP<ITexture> NONE      = nullptr;
        if (N.canReply)
            out.push_back({&REPLY_ID, N.replyActionText.empty() ? &REPLY_LBL : &N.replyActionText, &NONE});
        for (const auto& A : N.actions)
            out.push_back({&A.id, &A.label, &A.iconTex});
    }

    // measures the button rows under maxW; boxes are RELATIVE to the row's
    // origin, parallel to `acts`. Returns the total height (0 = none).
    static double layoutActions(const SPaint& P, const SType& T, const std::vector<SAct>& acts, std::vector<CBox>& boxes, double maxW) {
        boxes.clear();
        if (acts.empty())
            return 0;
        const int MAXW = (int)std::max(1.0, maxW * P.scale);
        double    bx   = 0, rowY = 0;
        for (const auto& A : acts) {
            auto& SB = scratch();
            appendEsc(SB, *A.label);
            const auto   LBL = cachedText(SB, v13Action(), T.action, MAXW, linePx(T.action), 0, false, 500);
            const double LW  = texW(LBL, P.scale);
            const double IW  = A.icon ? BTN_ICON + BTN_ICON_GAP : 0;
            const double BW  = std::max(BTN_MIN_W, IW + LW + 2 * BTN_PADX);
            if (bx > 0 && bx + BW > maxW + 0.5) {
                bx = 0;
                rowY += BTN_H + BTN_GAP;
            }
            boxes.push_back(CBox{bx, rowY, BW, BTN_H});
            bx += BW + BTN_GAP;
        }
        return rowY + BTN_H;
    }

    // draws the laid-out rows at (x0, y0) and records the absolute boxes
    static void paintActions(const SPaint& P, const SType& T, uint32_t id, const std::vector<SAct>& acts, const std::vector<CBox>& boxes, double x0, double y0,
                             std::vector<CBox>& absOut) {
        absOut.clear();
        for (size_t i = 0; i < boxes.size(); i++) {
            const CBox B{x0 + boxes[i].x, y0 + boxes[i].y, boxes[i].w, boxes[i].h};
            absOut.push_back(B);
            const bool HOV = hovered.kind == SCard::POPUP && hovered.id == id && hovered.btn == (int)i;
            if (HOV)
                P.rect(B, v13RaisedH(), (int)std::lround(BTN_R * P.scale), rPow());
            double cx = B.x + BTN_PADX;
            if (acts[i].icon && texReady(*acts[i].icon)) {
                P.texFit(*acts[i].icon, CBox{cx, B.y + (B.h - BTN_ICON) / 2, BTN_ICON, BTN_ICON}, 0);
                cx += BTN_ICON + BTN_ICON_GAP;
            }
            auto& SB = scratch();
            appendEsc(SB, *acts[i].label);
            const auto LBL = cachedText(SB, v13Action(), T.action, (int)(B.w * P.scale), linePx(T.action), 0, false, 500);
            if (LBL && LBL->tex)
                P.tex(LBL->tex, cx, B.y + (B.h - LBL->tex->m_size.y / P.scale) / 2);
        }
    }

    // ---- the chevron: a 25x17 stadium, or the HUN's lavender count pill ----

    static double chipCountText(const SType& T, double scale, int count) {
        auto& SB = scratch();
        SB += std::to_string(count);
        const auto N = cachedText(SB, v13HeadPillFg(), T.header, 64, linePx(T.header), 0, false, 600);
        return texW(N, scale);
    }

    static double chipW(const SType& T, double scale, int count) {
        if (count < 0)
            return CHIP_W;
        return CHIP_COUNT_PL + chipCountText(T, scale, count) + 2 + CHEV_D_COUNT + CHIP_COUNT_PR;
    }

    static void paintChev(const SPaint& P, const SType& T, double x, double y, int count, const CHyprColor& bg, const CHyprColor& fg) {
        const double H = count < 0 ? CHIP_H : CHIP_COUNT_H;
        const double W = chipW(T, P.scale, count);
        P.rect(CBox{x, y, W, H}, bg, (int)std::lround(H / 2 * P.scale), 2.f);
        double cx = x;
        if (count >= 0) {
            auto& SB = scratch();
            SB += std::to_string(count);
            const auto N = cachedText(SB, fg, T.header, 64, linePx(T.header), 0, false, 600);
            if (N && N->tex)
                P.tex(N->tex, cx + CHIP_COUNT_PL, y + (H - N->tex->m_size.y / P.scale) / 2);
            cx += CHIP_COUNT_PL + chipCountText(T, P.scale, count);
        }
        const bool NUM = count >= 0;
        const int  CPX = (int)std::lround((NUM ? CHEV_D_COUNT : CHEV_D) * P.scale);
        // the HUN chevron always points down: it expands INTO the shade
        const auto CHEV = controlIcon(eControlIcon::EXPAND_MORE, CPX, fg);
        if (CHEV)
            P.texFit(CHEV, CBox{cx + 2, y + (H - (NUM ? CHEV_D_COUNT : CHEV_D)) / 2, NUM ? CHEV_D_COUNT : CHEV_D, NUM ? CHEV_D_COUNT : CHEV_D}, 0);
    }

    // the plain-banner lead: the app identity or the content image, chip disc
    // + initial when nothing resolved (row.cpp's paintLead keeps its own copy
    // for the shade; the conversation branch is paintIconColumn's)
    static void paintLeadBanner(const SPaint& P, const SNotif& N, const CBox& cell) {
        const int    R    = (int)std::lround(cell.w / 2 * P.scale);
        const auto   LEAD = texReady(N.identTex) ? N.identTex : N.iconTex;
        if (N.conversation)
            paintIconColumn(P, N, cell, true, rPow());
        else if (texReady(LEAD))
            P.texCover(LEAD, cell, R, 2.f);
        if (texReady(LEAD))
            return;
        P.rect(cell, v13Chip(), R, 2.f);
        std::string name = N.appName;
        if (name.empty())
            name = titleForDisplay(N);
        std::string init;
        if (!name.empty())
            init = Pixel::firstCodepoint(name, 0);
        if (init.empty())
            init = "?";
        const auto GLYPH = cachedText(init, v13On(), (int)std::lround(15 * P.scale), (int)(cell.w * P.scale) + 8, linePx((int)std::lround(15 * P.scale)), 0, false, 600);
        if (GLYPH && GLYPH->tex)
            P.tex(GLYPH->tex, cell.x + (cell.w - GLYPH->tex->m_size.x / P.scale) / 2, cell.y + (cell.h - GLYPH->tex->m_size.y / P.scale) / 2);
    }

    double renderPopups(const SPaint& P, const SType& T, bool osdOnly, std::optional<double> startY, bool measureOnly) {
        const auto   MB   = P.mon->logicalBox();
        // The AOSP heads-up is a full-width banner; `width` > 0 pins a
        // narrower card (the pre-v14 380px strip) for users who want it.
        const double FULL = std::max(1.0, MB.w - 2 * EDGE);
        const double W    = cfg.width->value() > 0 ? std::min((double)cfg.width->value(), FULL) : FULL;
        const double GAP  = osdOnly ? std::max((double)cfg.margin->value(), 0.0) : HUN_GAP;
        const int    RR   = rRow(P.scale);
        const float  RP   = rPow();

        const double X     = MB.x + MB.w - EDGE - W;
        const double START = startY.value_or(MB.y + std::clamp((double)cfg.offsetY->value(), 0.0, std::max(0.0, MB.h - HUN_MIN_H - 2 * EDGE)));
        double       y     = START;

        const CHyprColor LINKCOL = v13Action(); // body-hyperlink color, a stable address for the cache

        static std::vector<SAct> acts; // reused; main thread only
        static std::vector<CBox> btnBoxes, btnAbs;

        for (const auto& N : notifs) {
            if (N->waiting || !N->banner)
                continue; // residency: only banners show as popups
            if (osdOnly && !inOsdBand(N->id))
                continue;
            if (y + HUN_MIN_H > MB.y + MB.h - 8)
                break; // no room: the tail waits off-screen, timeouts running

            // every texture build gates on the warm gate, never P.warm — a
            // measuring pass forces P.warm on to paint nothing, but it still
            // REQUESTS the glyphs the draw will need (the texture rule)
            if (warmGate.warming) {
                ensureIconTex(*N, (int)std::lround(CARD_ICON_D * P.scale));
                ensureConversationIcons(*N, (int)std::lround(CARD_ICON_D * P.scale));
                for (auto& A : N->actions)
                    ensureActionIcon(*N, A, (int)std::lround(BTN_ICON * P.scale));
            }

            const bool CONV = N->conversation;
            const bool URGENT = N->urgency >= 2;
            const auto AGE  = ageString(N->arrived);

            const double TX     = CARD_TEXT_X; // 76 — the avatar column is always reserved
            const double TEXTW  = std::max(1.0, W - TX - CARD_TEXT_INSET);
            const int    TEXTWPX = std::max(1, (int)std::lround(TEXTW * P.scale));

            // the action row: always on for a banner
            actionsOf(*N, acts);
            const double ACTSH = layoutActions(P, T, acts, btnBoxes, TEXTW);
            const double ACTS_BLOCK = ACTSH > 0 ? 3.0 + ACTSH : 0; // demo .hcard .acts margin-top

            double textH = 0;
            double kickH = 0, bodyH = 0;

            // pixel-parity banner (ledger A-141): the header line is the
            // semibold who/title + " · age"; the body below carries the
            // larger type, a group message led by its sender's name
            const auto PREV = CONV ? previewLines(*N, eCardKind::CONV, 1) : std::vector<SPreviewLine>{};
            std::string HEADL;
            if (CONV) {
                HEADL = PREV.empty() ? std::string{} : PREV.front().a;
                if (HEADL.empty())
                    HEADL = N->appName;
            } else {
                const auto& TITLETEXT = titleForDisplay(*N);
                HEADL = TITLETEXT.empty() ? N->appName : TITLETEXT;
            }

            auto& SB = scratch();
            appendEsc(SB, HEADL);
            const SCachedText* HEAD = cachedText(SB, URGENT && !CONV ? v13Urgent() : v13OnT(), T.title, TEXTWPX, linePx(T.title), 0, false, 600);
            SB.clear();
            appendEsc(SB, "· " + AGE);
            const SCachedText* TIME = cachedText(SB, v13On60(), T.header, TEXTWPX, linePx(T.header), 0, false, 400);
            kickH = std::max(KICK_MIN_H, std::max(texH(HEAD, P.scale), texH(TIME, P.scale)));

            SB.clear();
            if (CONV) {
                if (!PREV.empty() && N->conversationKind == "group" && !PREV.front().a.empty()) {
                    SB += "<span foreground=\"#" + hexOfCached(v13On()) + "\">";
                    appendEsc(SB, PREV.front().a);
                    SB += ":</span> ";
                    appendEsc(SB, PREV.front().b);
                } else if (!PREV.empty())
                    appendEsc(SB, PREV.front().b);
            } else
                appendEsc(SB, bodyForDisplay(*N));
            const SCachedText* BODY = SB.empty() ? nullptr : cachedText(SB, v13On82(), T.title, TEXTWPX, linePx(T.title), 18.0 / 15.0, true, 400, &LINKCOL);
            bodyH = texH(BODY, P.scale);

            textH = kickH + (bodyH > 0 ? BODY_MT : 0) + bodyH + ACTS_BLOCK;

            const double CH = std::max(HUN_MIN_H, std::max(CARD_ICON_D, textH) + CARD_PADT + CARD_PADB_ACTS);

            // per-card arrival motion: fade + an 8px drop, keyed on `born`
            SPaint CP = P;
            const float AT = animationsOn() && N->banner ? animT(N->born, Theme::MOTION_SPATIAL) : 1.f;
            if (AT < 1.f) {
                CP.alpha = P.alpha * easeOutCubic(AT);
                const double DROP = (1.0 - easeOutBack(AT)) * 8.0;
                CP.dy = P.dy - (startY ? std::min(DROP, GAP) : DROP); // a below-shade card must not animate through the panel
            }

            const CBox CARD{X, y, W, CH};
            CP.shadow(CARD, RR, RP, 16);
            CP.glass(CARD, v13Card(), RR, RP); // stroke-free, no rim (A-141)

            // the lead: centered on the text block (a banner is never open)
            const double ICONY = CARD_PADT + (textH - CARD_ICON_D) / 2;
            paintLeadBanner(CP, *N, CBox{X + CARD_ICON_X, y + ICONY, CARD_ICON_D, CARD_ICON_D});

            std::vector<SCard::SLinkHit> cardLinks;
            double               ty = y + CARD_PADT;

            if (HEAD && HEAD->tex)
                CP.tex(HEAD->tex, X + TX, ty);
            if (TIME && TIME->tex) // the tail rides the lead's baseline
                CP.tex(TIME->tex, X + TX + texW(HEAD, P.scale), ty + (texH(HEAD, P.scale) - texH(TIME, P.scale)));
            if (CONV && N->unreadCount > 0) { // the alert mark after the time
                const double KW = W - TX - (N->unreadCount > 1 ? KICK_RIGHT_COUNT : KICK_RIGHT);
                const auto IMP = controlIcon(eControlIcon::NOTIFICATION_ALERT, (int)std::lround(13 * P.scale), v13On82());
                if (IMP) {
                    const double IW = 13;
                    double       ix = X + TX + texW(HEAD, P.scale) + texW(TIME, P.scale) + 5;
                    if (ix + IW > X + TX + KW)
                        ix = X + TX + KW - IW;
                    CP.texFit(IMP, CBox{ix, ty + (kickH - IW) / 2, IW, IW}, 0);
                }
            }
            ty += kickH;
            if (BODY && BODY->tex) {
                ty += BODY_MT;
                CP.tex(BODY->tex, X + TX, ty);
                for (const auto& L : BODY->links) // physical -> global logical
                    cardLinks.push_back({CBox{X + TX + L.rel.x / P.scale, ty + L.rel.y / P.scale, L.rel.w / P.scale, L.rel.h / P.scale}, L.href});
                ty += bodyH;
            }

            if (ACTSH > 0) {
                ty += 3; // the demo's .acts margin-top
                paintActions(CP, T, N->id, acts, btnBoxes, X + TX, ty, btnAbs);
            }

            // the shade chevron: the HUN's only expand affordance — the
            // count pill, or the ROM's circular chevron button
            {
                const int    CHIPN = CONV && N->unreadCount > 1 ? (int)N->unreadCount : -1;
                const bool   HOV   = hovered.kind == SCard::POPUP && hovered.id == N->id && hovered.part == 5;
                if (CHIPN < 0)
                    paintChevronButton(CP, X + W - CHEV_BTN_X - CHEV_BTN_D, y + CHEV_BTN_Y, false, HOV);
                else
                    paintChev(CP, T, X + W - CHIP_X - chipW(T, P.scale, CHIPN), y + CHIP_Y, CHIPN, HOV ? v13RaisedH() : v13HeadPillBg(), v13HeadPillFg());
            }

            SCard card;
            card.kind            = SCard::POPUP;
            card.box             = CARD;
            card.id              = N->id;
            card.expansionButton = true;
            card.expandButton    = CBox{CARD.x + W - CHEV_HIT_W, CARD.y, CHEV_HIT_W, CHEV_HIT_H};
            if (!measureOnly) {
                for (size_t i = 0; i < btnAbs.size() && i < acts.size(); i++)
                    card.buttons.push_back({btnAbs[i], *acts[i].id});
                card.links = std::move(cardLinks);
                cards.push_back(std::move(card));
            }
            y += CH + GAP;
        }

        const double CONTENTH = std::max(0.0, y - GAP - START);
        if (osdOnly) {
            // The pass box starts at offset_y. Include the shade and the OSD
            // stack below it so the renderer damages both regions together.
            const double BASE = MB.y + (double)cfg.offsetY->value();
            lastContentH = std::max(lastContentH, std::max(0.0, START + CONTENTH - BASE));
            lastContentW = std::max(lastContentW, W);
        } else {
            lastContentH = CONTENTH;
            lastContentW = W;
        }
        return CONTENTH;
    }

} // namespace NHyprnotify
