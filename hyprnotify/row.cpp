// hyprnotify/row.cpp — one shade row, and the two faces of an app bundle.
//
// A ROW IS ITS BANNER. Collapsed it is a title, an age and the newest body
// line; open it is the whole card — header, title, body, progress, the
// sender's action buttons and (armed) the reply field. The two states share
// one function on purpose: the collapsed form must never say something the
// open one contradicts, and the budget in center.cpp measures with exactly
// the code that later paints.
//
// A BUNDLE has two faces made of the same parts: the DIGEST (folded — one
// app row with a count pill and two preview lines) and the GROUP (expanded —
// a header row with a close control, then every child as a full row). Children are rows
// with the badge taken off: the header already says which
// app, so repeating its icon per child said it four times over.
//
// Everything here obeys the texture rule (crash class 4): cachedText is
// requested UNCONDITIONALLY, and only the painting is gated on P.warm — the
// SPaint calls no-op inside a warm by themselves.

#include "ui.hpp"

namespace NHyprnotify {

    double renderRow(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box, bool open, bool more, const SRowStyle& ST, SCard& card, bool child) {
        const auto COLFG = color(cfg.colFg), COLTITLE = color(cfg.colTitle), COLSUB = color(cfg.colKicker), COLACC = color(cfg.colHighlight);
        const CHyprColor COLBODY = COLFG.modifyA(COLFG.a * 0.92);
        const auto       AGE     = ageString(N->arrived);
        const auto&      SUBHEX  = hexOfCached(COLSUB);
        const float      RP      = rPow();

        // warmGate.warming, not P.warm: measureRow runs this whole function with
        // P.warm forced on to suppress painting, and a build there would be a
        // build inside a render (crash class 4). Only the real warm may create.
        if (warmGate.warming)
            ensureIconTex(*N, (int)std::lround(std::max(ST.iconPx, (double)cfg.maxIcon->value()) * P.scale), 0, 0);
        if (warmGate.warming)
            for (auto& IM : N->bodyImages)
                ensureBodyImage(IM, (int)std::lround(BODYIMG_H * P.scale));

        const bool   LEADICON = hasLeadIcon(*N);
        const double ICONW    = LEADICON ? ST.iconPx : 0;
        const double TX       = box.x + ROW_PADX + (ICONW > 0 ? ICONW + ROW_ICON_GAP : 0);
        const double TEXTW    = box.x + box.w - ROW_PADX - TX;
        const int    TEXTWPX  = std::max(1, (int)std::floor(TEXTW * P.scale));

        double       th = 0;
        const double TY = box.y + ROW_PADT;

        if (!open) {
            // collapsed: bold "title • age" + the newest body line (+progress)
            auto& SB = scratch();
            SB += N->summary;
            SB += " <span foreground=\"";
            SB += SUBHEX;
            SB += "\">• ";
            SB += AGE;
            SB += "</span>";
            const auto LINE = cachedText(SB, COLTITLE, T.title, TEXTWPX, -1, 0, true, 600);
            const auto B1S  = lastLine(bodyForDisplay(*N));
            const auto B1   = B1S.empty() ? nullptr : cachedText(B1S, COLBODY, T.body, TEXTWPX, -1, 0, true, 400);
            th              = texH(LINE, P.scale) + (B1 ? 2 + texH(B1, P.scale) : 0) + (N->progress >= 0 ? PROGRESS_GAP + PROGRESS_H : 0);
            if (!P.warm) {
                if (LINE)
                    P.tex(LINE->tex, TX, TY);
                double yy = TY + texH(LINE, P.scale) + 2;
                if (B1) {
                    P.tex(B1->tex, TX, yy);
                    yy += texH(B1, P.scale);
                }
                if (N->progress >= 0) {
                    yy += PROGRESS_GAP;
                    paintProgress(P, TX, yy, TEXTW, N->progress, N->urgency >= 2);
                }
            }
        } else {
            // expanded: age/header line, title, body, progress, then the card's
            // own actions in Notify order. The PRIMARY gets no button here —
            // the row body fires it, exactly as the banner does.
            const int KICKWPX = std::max(1, (int)std::floor(TEXTW * P.scale));

            auto& KB = scratch();
            if (ST.headerHasApp) {
                appendEsc(KB, N->appName);
                KB += " • ";
            }
            KB += AGE;
            const auto KICK  = cachedText(KB, COLSUB, T.header, KICKWPX, -1, 0, true, 500);
            const auto TITLE = N->summary.empty() ? nullptr : cachedText(N->summary, COLTITLE, T.title, TEXTWPX, -1, 0, true, 600);
            // a merged chat is a transcript, so it gets Android's MessagingStyle
            // depth (~7 messages) where an ordinary card gets four lines
            const int  CAPL = (int)std::lround(T.body * 1.35 * (N->conversation ? 7 : 4));
            // linkCol collects the <a href> rects: without them a body click
            // meant for a URL would fire the card's primary instead
            const auto COLLINK = color(cfg.colLink);
            const auto& BODYTEXT = bodyForDisplay(*N);
            const auto BODY    = BODYTEXT.empty() ? nullptr : cachedText(BODYTEXT, COLBODY, T.body, TEXTWPX, CAPL, 1.1f, true, 400, &COLLINK);

            // The reply affordance is a chip among the buttons until it is
            // armed, and then the field takes a row of its own instead.
            const bool               ARMED    = ST.canReply && replyArmedOn(N->id);
            static const std::string REPLY_ID = "inline-reply", REPLY_LBL = "Reply";
            static std::vector<std::pair<const std::string*, const std::string*>> btnSrc; // reused
            btnSrc.clear();
            if (ST.canReply && N->canReply && !ARMED)
                btnSrc.emplace_back(&REPLY_ID, N->replySubmitText.empty() ? &REPLY_LBL : &N->replySubmitText);
            for (const auto& A : N->actions)
                btnSrc.emplace_back(&A.id, &A.label);

            static std::vector<CBox>               btnBoxes; // reused; main thread only
            static std::vector<const SCachedText*> btnLbls;
            btnBoxes.clear();
            btnLbls.clear();
            double btnH = 0;
            {
                double bx = 0, rowY = 0;
                for (const auto& [BID, BLBL] : btnSrc) {
                    auto& LB = scratch();
                    appendEsc(LB, *BLBL);
                    const auto   LBL = cachedText(LB, COLACC, T.action, TEXTWPX, -1, 0, true, 600);
                    const double BW  = std::min(TEXTW, texW(LBL, P.scale) + 2 * BTN_PADX);
                    if (bx > 0 && bx + BW > TEXTW + 0.5) {
                        bx = 0;
                        rowY += BTN_H + BTN_GAP;
                    }
                    btnBoxes.push_back(CBox{bx, rowY, BW, BTN_H});
                    btnLbls.push_back(LBL);
                    bx += BW + BTN_GAP;
                }
                btnH = btnBoxes.empty() ? 0 : rowY + BTN_H;
            }

            const double KH = texH(KICK, P.scale), TH = texH(TITLE, P.scale), BH = texH(BODY, P.scale);
            th = KH + (KH > 0 ? HEAD_GAP : 0) + TH + (TH > 0 && BH > 0 ? TITLE_GAP : 0) + BH + (N->progress >= 0 ? PROGRESS_GAP + PROGRESS_H : 0) +
                (btnH > 0 ? BTN_ROW_GAP + btnH : 0) + (ARMED ? BTN_ROW_GAP + BTN_H : 0);

            double yy = TY;
            if (!P.warm && KICK)
                P.tex(KICK->tex, TX, yy);
            yy += KH + (KH > 0 ? HEAD_GAP : 0);
            if (!P.warm && TITLE)
                P.tex(TITLE->tex, TX, yy);
            yy += TH + (TH > 0 && BH > 0 ? TITLE_GAP : 0);
            if (!P.warm && BODY)
                P.tex(BODY->tex, TX, yy);
            if (BODY) // hit rects in both modes, like the buttons: physical -> global logical
                for (const auto& L : BODY->links)
                    card.links.push_back({CBox{TX + L.rel.x / P.scale, yy + L.rel.y / P.scale, L.rel.w / P.scale, L.rel.h / P.scale}, L.href});
            yy += BH;
            if (N->progress >= 0) {
                yy += PROGRESS_GAP;
                paintProgress(P, TX, yy, TEXTW, N->progress, N->urgency >= 2);
                yy += PROGRESS_H;
            }
            if (btnH > 0) {
                yy += BTN_ROW_GAP;
                const double BX0 = TX - BTN_PADX; // optical: labels align to the content column
                for (size_t i = 0; i < btnBoxes.size(); i++) {
                    const CBox BOX{BX0 + btnBoxes[i].x, yy + btnBoxes[i].y, btnBoxes[i].w, btnBoxes[i].h};
                    if (!P.warm) {
                        if (hovered.id == N->id && hovered.btn == (int)i)
                            P.rect(BOX, tAccentDim(), (int)std::lround(BTN_H / 2 * P.scale));
                        if (btnLbls[i] && btnLbls[i]->tex)
                            P.tex(btnLbls[i]->tex, BOX.x + BTN_PADX, BOX.y + (BOX.h - btnLbls[i]->tex->m_size.y / P.scale) / 2);
                    }
                    card.buttons.push_back({BOX, *btnSrc[i].first});
                }
            }

            // ---- the armed inline-reply field ----
            if (ARMED) {
                yy += BTN_ROW_GAP;
                const auto&  TXT   = replyText();
                const auto   SLBL  = cachedText(N->replySubmitText.empty() ? "Send" : N->replySubmitText, tOnAccent(), T.action, TEXTWPX, -1, 0, false, 600);
                const double SENDW = std::min(TEXTW / 2, texW(SLBL, P.scale) + 2 * BTN_PADX);
                const CBox   FB{TX, yy, std::max(40.0, TEXTW - SENDW - BTN_GAP), BTN_H};
                const CBox   SB{TX + TEXTW - SENDW, yy, SENDW, BTN_H};
                const int    RB = (int)std::lround(BTN_H / 2 * P.scale);

                // the typed text, or the sender's placeholder while it is empty
                const auto ENT = TXT.empty() ? nullptr : cachedText(TXT, COLFG, T.action, std::max(1, (int)((FB.w - 2 * BTN_PADX) * P.scale)), -1, 0, false, 400);
                const auto PH  = TXT.empty() ? cachedText(N->replyPlaceholder.empty() ? "Type a reply..." : N->replyPlaceholder, COLSUB, T.action,
                                                         std::max(1, (int)((FB.w - 2 * BTN_PADX) * P.scale)), -1, 0, false, 400) :
                                               nullptr;
                if (!P.warm) {
                    P.rect(FB, tFill2(), RB);
                    P.ring(FB, COLACC, RB, RP); // armed: the field wears the accent
                    const auto* SHOW = ENT ? ENT : PH;
                    double      cx   = FB.x + BTN_PADX;
                    if (SHOW && SHOW->tex) {
                        P.tex(SHOW->tex, cx, FB.y + (FB.h - SHOW->tex->m_size.y / P.scale) / 2);
                        if (ENT)
                            cx += texW(ENT, P.scale);
                    }
                    // the caret sits at the end: editing is append + backspace
                    P.rect(CBox{std::min(cx + 1, FB.x + FB.w - 3), FB.y + 5, 1.5, FB.h - 10}, COLACC, 0);

                    const bool SHOV = hovered.id == N->id && hovered.part == 4;
                    P.rect(SB, TXT.empty() ? tFill2() : SHOV ? color(cfg.colHighlight) : tAccentDim(), RB);
                    if (SLBL && SLBL->tex)
                        P.tex(SLBL->tex, SB.x + (SB.w - SLBL->tex->m_size.x / P.scale) / 2, SB.y + (SB.h - SLBL->tex->m_size.y / P.scale) / 2);
                }
                card.replyField = FB;
                card.replySend  = SB;
            }

        }

        const double ROWH = std::max(th, ICONW) + ROW_PADT + ROW_PADB;

        if (!P.warm && LEADICON) {
            // collapsed rows center the icon; expanded top-pin it
            const double IY = open ? box.y + ROW_PADT : box.y + (ROWH - ICONW) / 2;
            paintIconColumn(P, *N, CBox{box.x + ROW_PADX, IY, ICONW, ICONW}, ST.withBadge && N->conversation, RP);
        }
        card.box        = CBox{box.x, box.y, box.w, ROWH};
        card.id         = N->id;
        card.kind       = child ? SCard::CHILD : SCard::ROW;
        card.expanded   = open;
        card.expandable = more;
        return ROWH;
    }

    // measure without painting: same code, a paint context that draws nothing
    // (cachedText still resolves through the real warm gate)
    double measureRow(const SPaint& P, const SType& T, const SP<SNotif>& N, double w, bool open, const SRowStyle& ST) {
        SPaint MP = P;
        MP.warm   = true; // paints nothing; it does NOT license a texture build
        SCard scratch;
        return renderRow(MP, T, N, CBox{0, 0, w, 0}, open, true, ST, scratch, false);
    }

    // ---- the three things a shade slot can hold ----

    void paintSingle(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box, bool open, bool more) {
        const bool HOV = hovered.kind == SCard::ROW && hovered.id == N->id && hovered.btn < 0 && hovered.part == 0;
        P.rect(box, HOV ? tAccentDim() : tFill(), rRow(P.scale), rPow());
        SCard card;
        renderRow(P, T, N, CBox{box.x, box.y, box.w, 0}, open, more, ROW_SINGLE, card, false);
        cards.push_back(std::move(card));
    }

    double digestH(const SType& T, size_t count, double scale) {
        return ROW_PADT + std::max(ROW_ICON, (double)T.title / scale + 2) + std::min<size_t>(2, count) * ((double)T.body / scale * 1.35 + 3) + ROW_PADB;
    }

    double groupHeadH() {
        return ROW_PADT + CHILD_ICON + ROW_PADB;
    }

    double snoozeRowH() {
        return SNOOZE_H;
    }

    // ---- the manage panel ----
    //
    // Every verb the strip used to hide behind a symbol, named, at row width.
    // The mute
    // durations are iOS's ("Mute for 1 Hour", "Mute for Today"): permanent is
    // still here, but it stops being the only thing a click can mean — it is
    // the choice people regret, and its rule then sits in a file nobody reads.
    static std::string snoozeDurationLabel(int64_t seconds) {
        seconds = std::max<int64_t>(seconds, 0);
        if (seconds == 0)
            return "now";
        if (seconds % 3600 == 0) {
            const auto HOURS = seconds / 3600;
            return std::to_string(HOURS) + (HOURS == 1 ? " hour" : " hours");
        }
        if (seconds % 60 == 0) {
            const auto MINUTES = seconds / 60;
            return std::to_string(MINUTES) + " min";
        }
        return std::to_string(seconds) + " sec";
    }

    std::vector<SMenuEntry> menuEntries(const SP<SNotif>& N, bool bundle) {
        std::vector<SMenuEntry> out;
        const std::string APP = N->appName.empty() ? N->appKey : N->appName;
        if (!bundle) {
            const int64_t SNOOZE = std::max<int64_t>(cfg.snoozeSeconds->value(), 0);
            out.push_back({"◷", "Snooze " + snoozeDurationLabel(SNOOZE), 1, SNOOZE});
            if (SNOOZE != 3600)
                out.push_back({"◷", "Snooze 1 hour", 1, 3600});
        }
        if (Policy::silenced(N->appKey))
            out.push_back({"⊘", "Unmute " + APP, 3, 0, true});
        else {
            out.push_back({"⊘", "Mute " + APP + " for 1 hour", 2, 3600});
            out.push_back({"⊘", "Mute " + APP + " today", 2, -1});
            out.push_back({"⊘", "Mute " + APP + " always", 2, 0});
        }
        if (!bundle && N->conversation)
            out.push_back({"★", N->priority ? "Unmark this conversation" : "Priority conversation", 4, 0, N->priority});
        out.push_back({"✕", bundle ? "Dismiss " + APP : "Dismiss", 5, 0});
        return out;
    }

    double managePanelH(const SP<SNotif>& N, const std::string& group) {
        return ROW_PADT + CHILD_ICON + (double)menuEntries(N, !group.empty()).size() * MENU_ROW_H + ROW_PADB;
    }

    void paintManagePanel(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box, const std::string& group) {
        const auto  COLFG = color(cfg.colFg), COLSUB = color(cfg.colKicker), COLACC = color(cfg.colHighlight);
        const float RP = rPow();
        P.rect(box, tFill(), rRow(P.scale), RP);

        SCard card;
        card.kind = SCard::MANAGE;
        card.id   = N->id;
        card.box  = box;
        card.group = group;

        // The header keeps the target identifiable while its body is gone. A
        // visible close control is the only extra affordance; long-pressing
        // the target opens this panel again.
        if (warmGate.warming)
            ensureIconTex(*N, (int)std::lround(cfg.maxIcon->value() * P.scale), 0, 0);
        const auto& IDT = N->identTex && N->identTex->m_texID ? N->identTex : N->iconTex;
        if (!P.warm && IDT)
            P.texFit(IDT, CBox{box.x + ROW_PADX, box.y + ROW_PADT, CHILD_ICON, CHILD_ICON}, (int)std::lround(CHILD_ICON * 10.0 / 44.0 * P.scale), RP);

        const double TX  = box.x + ROW_PADX + CHILD_ICON + ROW_ICON_GAP;
        const CBox   OB{box.x + box.w - ROW_PADX - OVER_D, box.y + ROW_PADT + (CHILD_ICON - OVER_D) / 2, OVER_D, OVER_D};
        const auto   HEAD = cachedText(N->appName.empty() ? "Notification" : N->appName, COLSUB, T.header, std::max(1, (int)((OB.x - 6 - TX) * P.scale)), -1, 0, false, 500);
        if (!P.warm && HEAD && HEAD->tex)
            P.tex(HEAD->tex, TX, box.y + ROW_PADT + (CHILD_ICON - HEAD->tex->m_size.y / P.scale) / 2);

        const bool OHOV = hovered.kind == SCard::MANAGE && hovered.id == N->id && hovered.part == 2;
        const auto   XGN = cachedText("✕", COLFG, T.small, 64, -1, 0, false, 600);
        const auto   XGH = cachedText("✕", tOnAccent(), T.small, 64, -1, 0, false, 600);
        if (!P.warm) {
            P.rect(OB, OHOV ? tAccentDim() : tFill2(), (int)std::lround(OVER_D / 2 * P.scale));
            const auto* XG = OHOV ? XGH : XGN;
            if (XG && XG->tex)
                P.tex(XG->tex, OB.x + (OB.w - XG->tex->m_size.x / P.scale) / 2, OB.y + (OB.h - XG->tex->m_size.y / P.scale) / 2);
        }
        card.close = OB;

        double     y  = box.y + ROW_PADT + CHILD_ICON;
        const auto EN = menuEntries(N, !group.empty());
        for (size_t i = 0; i < EN.size(); i++) {
            const auto& E = EN[i];
            const CBox  RB{box.x + ROW_PADX / 2, y, box.w - ROW_PADX, MENU_ROW_H};
            // part codes 16.. address the entries: one rect per row, so a new
            // verb costs an entry and not a member
            const bool HOV = hovered.kind == SCard::MANAGE && hovered.id == N->id && hovered.part == (uint8_t)(16 + i);
            const auto G   = cachedText(E.glyph, E.lit ? COLACC : COLSUB, T.small, 64, -1, 0, false, 600);
            const auto L   = cachedText(E.label, E.lit ? COLACC : COLFG, T.body, std::max(1, (int)((RB.w - MENU_GLYPH_W) * P.scale)), -1, 0, false, 500);
            if (!P.warm) {
                if (HOV)
                    P.rect(RB, tAccentDim(), (int)std::lround(6 * P.scale), RP);
                if (G && G->tex)
                    P.tex(G->tex, RB.x + (MENU_GLYPH_W - G->tex->m_size.x / P.scale) / 2, RB.y + (MENU_ROW_H - G->tex->m_size.y / P.scale) / 2);
                if (L && L->tex)
                    P.tex(L->tex, RB.x + MENU_GLYPH_W, RB.y + (MENU_ROW_H - L->tex->m_size.y / P.scale) / 2);
            }
            card.manage.push_back({RB, (uint8_t)(16 + i)});
            y += MENU_ROW_H;
        }

        cards.push_back(std::move(card));
    }

    // The undo row — Android's "Snoozed for 1 hour · Undo", in the slot the
    // card just held. It is the whole reason the schedule action stopped being irreversible:
    // the card has not gone anywhere yet, so there is still something to click.
    // Both controls STAND rather than hide: a control you have seconds to find
    // must not also have to be discovered.
    void paintSnoozeRow(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box) {
        const auto  COLSUB = color(cfg.colKicker), COLACC = color(cfg.colHighlight);
        const float RP     = rPow();
        P.rect(box, tFill(), rRow(P.scale), RP);

        SCard card;
        card.kind = SCard::SNOOZE;
        card.id   = N->id;
        card.box  = box;

        const double CY = box.y + (SNOOZE_H - MANAGE_D) / 2;

        // right edge, walking left: Undo, then the duration control
        const auto   UND  = cachedText("Undo", COLACC, T.action, 96, -1, 0, false, 600);
        const double UNDW = texW(UND, P.scale) + 2 * BTN_PADX;
        const CBox   UB{box.x + box.w - ROW_PADX - UNDW, box.y + (SNOOZE_H - BTN_H) / 2, UNDW, BTN_H};
        const bool   UHOV = hovered.kind == SCard::SNOOZE && hovered.id == N->id && hovered.part == 8;
        if (!P.warm) {
            if (UHOV)
                P.rect(UB, tAccentDim(), (int)std::lround(BTN_H / 2 * P.scale));
            if (UND && UND->tex)
                P.tex(UND->tex, UB.x + BTN_PADX, UB.y + (UB.h - UND->tex->m_size.y / P.scale) / 2);
        }
        card.manage.push_back({UB, 8});

        const CBox DB{UB.x - MANAGE_GAP - MANAGE_D, CY, MANAGE_D, MANAGE_D};
        const bool DHOV = hovered.kind == SCard::SNOOZE && hovered.id == N->id && hovered.part == 9;
        const auto DG   = cachedText("◷", COLSUB, T.small, 64, -1, 0, false, 600);
        if (!P.warm) {
            P.rect(DB, DHOV ? tAccentDim() : tFill2(), (int)std::lround(MANAGE_D / 2 * P.scale));
            if (DG && DG->tex)
                P.tex(DG->tex, DB.x + (DB.w - DG->tex->m_size.x / P.scale) / 2, DB.y + (DB.h - DG->tex->m_size.y / P.scale) / 2);
        }
        card.manage.push_back({DB, 9});

        auto& LB = scratch();
        LB += "◷ Snoozed ";
        appendEsc(LB, Model::snoozeLabel(N));
        const int  LW  = std::max(1, (int)std::floor((DB.x - MANAGE_GAP - box.x - ROW_PADX) * P.scale));
        const auto LBL = cachedText(LB, COLSUB, T.body, LW, -1, 0, true, 500);
        if (!P.warm) {
            if (LBL && LBL->tex)
                P.tex(LBL->tex, box.x + ROW_PADX, box.y + (SNOOZE_H - LBL->tex->m_size.y / P.scale) / 2);
        }

        cards.push_back(std::move(card));
    }

    // The folded bundle: the app's identity, a count pill, and the two newest
    // cards previewed a line each — enough to decide whether to open it.
    void paintDigest(const SPaint& P, const SType& T, const SDisp& D, const CBox& box) {
        const auto  COLFG = color(cfg.colFg), COLTITLE = color(cfg.colTitle), COLSUB = color(cfg.colKicker);
        const auto& SUBHEX = hexOfCached(COLSUB);
        const float RP     = rPow();

        const auto& NEWEST = D.items.front();
        const bool  HOV    = hovered.kind == SCard::DIGEST && hovered.group == D.key;
        P.rect(box, HOV ? tAccentDim() : tFill(), rRow(P.scale), RP);

        if (warmGate.warming)
            ensureIconTex(*NEWEST, (int)std::lround(cfg.maxIcon->value() * P.scale), 0, 0);
        const auto& IDT = NEWEST->identTex && NEWEST->identTex->m_texID ? NEWEST->identTex : NEWEST->iconTex;
        if (IDT)
            P.texFit(IDT, CBox{box.x + ROW_PADX, box.y + ROW_PADT, ROW_ICON, ROW_ICON}, (int)std::lround(ROW_ICON * 10.0 / 44.0 * P.scale), RP);

        const double TX    = box.x + ROW_PADX + ROW_ICON + ROW_ICON_GAP;
        const auto   PILL  = cachedText(std::to_string(D.items.size()), COLFG, T.small, 64, -1, 0, false, 600);
        const double PILLW = texW(PILL, P.scale) + 14;
        const CBox   PB{box.x + box.w - ROW_PADX - PILLW, box.y + ROW_PADT + (ROW_ICON - PILL_H) / 2, PILLW, PILL_H};
        if (!P.warm) {
            P.rect(PB, HOV ? tAccentDim() : tFill2(), (int)std::lround(PILL_H / 2 * P.scale));
            if (PILL && PILL->tex)
                P.tex(PILL->tex, PB.x + (PB.w - PILL->tex->m_size.x / P.scale) / 2, PB.y + (PB.h - PILL->tex->m_size.y / P.scale) / 2);
        }

        auto& DB = scratch();
        appendEsc(DB, NEWEST->appName);
        DB += " <span foreground=\"";
        DB += SUBHEX;
        DB += "\">• ";
        DB += std::to_string(D.items.size());
        DB += " • ";
        DB += ageString(NEWEST->arrived);
        DB += "</span>";
        const auto SUMLINE = cachedText(DB, COLTITLE, T.title, std::max(1, (int)((PB.x - 8 - TX) * P.scale)), -1, 0, true, 600);
        if (!P.warm && SUMLINE)
            P.tex(SUMLINE->tex, TX, box.y + ROW_PADT + (ROW_ICON - texH(SUMLINE, P.scale)) / 2);

        // <=2 preview lines, indented into the text column
        double       py   = box.y + ROW_PADT + std::max(ROW_ICON, (double)T.title / P.scale + 2);
        const size_t PREV = std::min<size_t>(2, D.items.size());
        for (size_t i = 0; i < PREV; i++) {
            const auto& N = D.items[i];
            if (warmGate.warming)
                ensureIconTex(*N, (int)std::lround(cfg.maxIcon->value() * P.scale), 0, 0);
            const double LH = (double)T.body / P.scale * 1.35;
            py += 3;
            double px = TX;
            // each child's OWN face: a bundle's children all share one
            // app icon, so leading with the identity drew the same
            // glyph on every preview line
            const SP<ITexture> PV = (N->iconTex && N->iconTex->m_texID && !N->heroTex) ? N->iconTex : N->identTex;
            if (PV && PV->m_texID) {
                P.texFit(PV, CBox{px, py + (LH - PREV_ICON) / 2, PREV_ICON, PREV_ICON}, (int)std::lround(PREV_ICON / 2 * P.scale), 2.f);
                px += PREV_ICON + 6;
            }
            auto& PBUF = scratch();
            PBUF += "<b>";
            PBUF += N->summary;
            PBUF += "</b>  <span foreground=\"";
            PBUF += SUBHEX;
            PBUF += "\">";
            PBUF += lastLine(N->body);
            PBUF += "</span>";
            const auto LN = cachedText(PBUF, COLFG, T.body, std::max(1, (int)((box.x + box.w - ROW_PADX - px) * P.scale)), -1, 0, true, 400);
            if (!P.warm && LN)
                P.tex(LN->tex, px, py + (LH - texH(LN, P.scale)) / 2);
            py += LH;
        }

        SCard card;
        card.kind  = SCard::DIGEST;
        card.box   = box;
        card.group = D.key;
        cards.push_back(std::move(card));
    }

    // The expanded bundle: a header that owns the app's identity, its count
    // and the close control that dismisses the lot, then every child as a full row.
    void paintGroup(const SPaint& P, const SType& T, const SDisp& D, const CBox& box, const std::vector<double>& childH) {
        const auto  COLFG = color(cfg.colFg), COLTITLE = color(cfg.colTitle), COLSUB = color(cfg.colKicker), COLURGENT = color(cfg.colUrgent);
        const auto& SUBHEX = hexOfCached(COLSUB);
        const float RP     = rPow();

        const auto&  NEWEST = D.items.front();
        const bool   HHOV   = hovered.kind == SCard::GHEAD && hovered.group == D.key;
        const double HEADRH = groupHeadH();
        P.rect(CBox{box.x, box.y, box.w, HEADRH}, HHOV ? tAccentDim() : tFill(), rRow(P.scale), RP);

        if (warmGate.warming)
            ensureIconTex(*NEWEST, (int)std::lround(cfg.maxIcon->value() * P.scale), 0, 0);
        const auto& IDT = NEWEST->identTex && NEWEST->identTex->m_texID ? NEWEST->identTex : NEWEST->iconTex;
        if (IDT)
            P.texFit(IDT, CBox{box.x + ROW_PADX, box.y + ROW_PADT, CHILD_ICON, CHILD_ICON}, (int)std::lround(CHILD_ICON * 10.0 / 44.0 * P.scale), RP);

        // the static close control (dismiss the whole app's bundle)
        const CBox XB{box.x + box.w - ROW_PADX - XCIRC, box.y + ROW_PADT + (CHILD_ICON - XCIRC) / 2, XCIRC, XCIRC};
        const bool XHOV = HHOV && hovered.part == 2;
        // BOTH colours, every pass: hover flips without a rewarm, so both
        // stable icon variants must be requested before the draw.
        const auto XG    = controlIcon(eControlIcon::CLOSE, (int)std::lround(XCIRC * P.scale), COLFG);
        const auto XGHOT = controlIcon(eControlIcon::CLOSE, (int)std::lround(XCIRC * P.scale), tOnAccent());
        if (!P.warm) {
            P.rect(XB, XHOV ? COLURGENT : tFill2(), (int)std::lround(XCIRC / 2 * P.scale));
            if (const auto* G = XHOV ? &XGHOT : &XG; *G)
                P.texFit(*G, XB, 0, 2.f);
        }

        const auto   PILL  = cachedText(std::to_string(D.items.size()), COLFG, T.small, 64, -1, 0, false, 600);
        const double PILLW = texW(PILL, P.scale) + 14;
        const CBox   PB{XB.x - 6 - PILLW, box.y + ROW_PADT + (CHILD_ICON - PILL_H) / 2, PILLW, PILL_H};
        if (!P.warm) {
            P.rect(PB, tFill2(), (int)std::lround(PILL_H / 2 * P.scale));
            if (PILL && PILL->tex)
                P.tex(PILL->tex, PB.x + (PB.w - PILL->tex->m_size.x / P.scale) / 2, PB.y + (PB.h - PILL->tex->m_size.y / P.scale) / 2);
        }

        const double TX = box.x + ROW_PADX + CHILD_ICON + ROW_ICON_GAP;
        auto&        HB = scratch();
        appendEsc(HB, NEWEST->appName);
        HB += " <span foreground=\"";
        HB += SUBHEX;
        HB += "\">• ";
        HB += std::to_string(D.items.size());
        HB += " • ";
        HB += ageString(NEWEST->arrived);
        HB += "</span>";
        const auto HL = cachedText(HB, COLTITLE, T.title, std::max(1, (int)((PB.x - 8 - TX) * P.scale)), -1, 0, true, 600);
        if (!P.warm && HL)
            P.tex(HL->tex, TX, box.y + ROW_PADT + (CHILD_ICON - texH(HL, P.scale)) / 2);

        {
            SCard card;
            card.kind  = SCard::GHEAD;
            card.box   = CBox{box.x, box.y, box.w, HEADRH};
            card.group = D.key;
            card.close = XB;
            cards.push_back(std::move(card));
        }

        // the children, each fully readable (no third fold state)
        double cy = box.y + HEADRH;
        for (size_t k = 0; k < D.items.size(); k++) {
            const auto& N = D.items[k];
            cy += CHILD_GAP;
            const double CH2  = k < childH.size() ? childH[k] : measureRow(P, T, N, box.w, true, ROW_CHILD);
            const bool   CHOV = hovered.kind == SCard::CHILD && hovered.id == N->id && hovered.btn < 0 && hovered.part == 0;
            P.rect(CBox{box.x, cy, box.w, CH2}, CHOV ? tAccentDim() : tFill(), rJoint(P.scale), RP);
            SCard card;
            card.group = D.key;
            renderRow(P, T, N, CBox{box.x, cy, box.w, 0}, true, false, ROW_CHILD, card, true);
            cards.push_back(std::move(card));
            cy += CH2;
        }
    }

} // namespace NHyprnotify
