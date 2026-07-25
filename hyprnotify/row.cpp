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
// a header row with a ✕, then every child as a full row). Children are rows
// with the chevron and the badge taken off: the header already says which
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

        if (P.warm)
            ensureIconTex(*N, (int)std::lround(std::max(ST.iconPx, (double)cfg.maxIcon->value()) * P.scale), 0, 0);

        const bool   LEADICON = hasLeadIcon(*N);
        const double ICONW    = LEADICON ? ST.iconPx : 0;
        const double TX       = box.x + ROW_PADX + (ICONW > 0 ? ICONW + ROW_ICON_GAP : 0);
        const bool   CHEVRON  = ST.hasChevron && (open || more);
        const double RTRIM    = CHEVRON ? CHEV + 8 : 0;
        const double TEXTW    = box.x + box.w - ROW_PADX - RTRIM - TX;
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
            const auto B1S  = lastLine(N->body);
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
            // the manage strip rides the header line; the kicker gives up
            // exactly its width so the two can never collide, hover or not
            const int    NMANAGE = ST.manage ? (N->conversation ? 3 : 2) : 0;
            const double MANAGEW = NMANAGE > 0 ? NMANAGE * MANAGE_D + (NMANAGE - 1) * MANAGE_GAP + 8 : 0;
            const int    KICKWPX = std::max(1, (int)std::floor((TEXTW - MANAGEW) * P.scale));

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
            const auto BODY    = N->body.empty() ? nullptr : cachedText(N->body, COLBODY, T.body, TEXTWPX, CAPL, 1.1f, true, 400, &COLLINK);

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
                const auto PH  = TXT.empty() ? cachedText(N->replyPlaceholder.empty() ? "Type a reply…" : N->replyPlaceholder, COLSUB, T.action,
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
                yy += BTN_H;
            }

            // ---- the manage strip: what Android's long-press menu holds ----
            //
            // Hover-revealed, like the banner's ✕, and hit-registered always:
            // reaching one means the pointer is already on the row, which is
            // what reveals them. Their width is reserved whether they show or
            // not — a strip that appeared on hover and reflowed the header
            // would re-key every raster under the pointer.
            if (NMANAGE > 0) {
                const bool SHOWN = hovered.id == N->id && (hovered.kind == SCard::ROW || hovered.kind == SCard::CHILD);
                const int  RM    = (int)std::lround(MANAGE_D / 2 * P.scale);
                // rightmost first, walking left: silence the app, put the
                // card away, mark the sender. Only a chat has a sender.
                struct SBtn {
                    const char* glyph;
                    uint8_t     part;
                    bool        lit;
                };
                const SBtn BTNS[3]{{"⊘", 5, Policy::silenced(N->appKey)}, {"◷", 7, false}, {"★", 6, N->priority}};
                double       mx = box.x + box.w - ROW_PADX - RTRIM - MANAGE_D;
                const double MY = box.y + ROW_PADT + (CHEV - MANAGE_D) / 2;
                for (int i = 0; i < NMANAGE; i++) {
                    const auto& B   = BTNS[i];
                    const bool  HOV = SHOWN && hovered.part == B.part;
                    const CBox  MB{mx, MY, MANAGE_D, MANAGE_D};
                    const auto  G = cachedText(B.glyph, B.lit ? tOnAccent() : COLSUB, T.small, 64, -1, 0, false, 600);
                    if (!P.warm && SHOWN) {
                        P.rect(MB, B.lit ? COLACC : HOV ? tAccentDim() : tFill2(), RM);
                        if (G && G->tex)
                            P.tex(G->tex, MB.x + (MB.w - G->tex->m_size.x / P.scale) / 2, MB.y + (MB.h - G->tex->m_size.y / P.scale) / 2);
                    }
                    card.manage.push_back({MB, B.part});
                    mx -= MANAGE_D + MANAGE_GAP;
                }
            }
        }

        const double ROWH = std::max(th, ICONW) + ROW_PADT + ROW_PADB;

        if (!P.warm && LEADICON) {
            // collapsed rows center the icon; expanded top-pin it
            const double IY = open ? box.y + ROW_PADT : box.y + (ROWH - ICONW) / 2;
            paintIconColumn(P, *N, CBox{box.x + ROW_PADX, IY, ICONW, ICONW}, ST.withBadge, RP);
        }
        // the chevron circle: an INDICATOR that the row folds, and a second
        // hit target for it — the whole row is the first one
        if (CHEVRON) {
            const double CY = open ? box.y + ROW_PADT : box.y + (ROWH - CHEV) / 2;
            const CBox   CB{box.x + box.w - ROW_PADX - CHEV, CY, CHEV, CHEV};
            const auto   G = cachedText(open ? "˄" : "˅", COLFG, T.small, 64, -1, 0, false, 600); // built in BOTH modes
            if (!P.warm) {
                const bool CHOV = hovered.id == N->id && hovered.part == 1 && hovered.btn < 0;
                P.rect(CB, CHOV ? tAccentDim() : tFill2(), (int)std::lround(CHEV / 2 * P.scale));
                if (G && G->tex)
                    P.tex(G->tex, CB.x + (CB.w - G->tex->m_size.x / P.scale) / 2, CB.y + (CB.h - G->tex->m_size.y / P.scale) / 2);
            }
            card.chevron = CB;
        }

        card.box  = CBox{box.x, box.y, box.w, ROWH};
        card.id   = N->id;
        card.kind = child ? SCard::CHILD : SCard::ROW;
        return ROWH;
    }

    // measure without painting: same code, a paint context that draws nothing
    // (cachedText still resolves through the real warm gate)
    double measureRow(const SPaint& P, const SType& T, const SP<SNotif>& N, double w, bool open, const SRowStyle& ST) {
        SPaint MP = P;
        MP.warm   = true;
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

    // The folded bundle: the app's identity, a count pill, and the two newest
    // cards previewed a line each — enough to decide whether to open it.
    void paintDigest(const SPaint& P, const SType& T, const SDisp& D, const CBox& box) {
        const auto  COLFG = color(cfg.colFg), COLTITLE = color(cfg.colTitle), COLSUB = color(cfg.colKicker);
        const auto& SUBHEX = hexOfCached(COLSUB);
        const float RP     = rPow();

        const auto& NEWEST = D.items.front();
        const bool  HOV    = hovered.kind == SCard::DIGEST && hovered.group == D.key;
        P.rect(box, HOV ? tAccentDim() : tFill(), rRow(P.scale), RP);

        if (P.warm)
            ensureIconTex(*NEWEST, (int)std::lround(cfg.maxIcon->value() * P.scale), 0, 0);
        const auto& IDT = NEWEST->identTex && NEWEST->identTex->m_texID ? NEWEST->identTex : NEWEST->iconTex;
        if (IDT)
            P.texFit(IDT, CBox{box.x + ROW_PADX, box.y + ROW_PADT, ROW_ICON, ROW_ICON}, (int)std::lround(ROW_ICON * 10.0 / 44.0 * P.scale), RP);

        const double TX    = box.x + ROW_PADX + ROW_ICON + ROW_ICON_GAP;
        const auto   PILL  = cachedText(std::to_string(D.items.size()) + " ˅", COLFG, T.small, 64, -1, 0, false, 600);
        const double PILLW = texW(PILL, P.scale) + 14;
        const CBox   PB{box.x + box.w - ROW_PADX - PILLW, box.y + ROW_PADT + (ROW_ICON - PILL_H) / 2, PILLW, PILL_H};
        if (!P.warm) {
            P.rect(PB, HOV ? tAccentDim() : tFill2(), (int)std::lround(PILL_H / 2 * P.scale));
            if (PILL && PILL->tex)
                P.tex(PILL->tex, PB.x + (PB.w - PILL->tex->m_size.x / P.scale) / 2, PB.y + (PB.h - PILL->tex->m_size.y / P.scale) / 2);
        }

        // a folded bundle is where an app most obviously earns a silencing,
        // so the strip reaches here too — revealed on hover, as on a row
        const bool MUTED = Policy::silenced(D.key);
        const CBox MB{PB.x - 6 - MANAGE_D, box.y + ROW_PADT + (ROW_ICON - MANAGE_D) / 2, MANAGE_D, MANAGE_D};
        {
            const auto G = cachedText("⊘", MUTED ? tOnAccent() : COLSUB, T.small, 64, -1, 0, false, 600);
            if (!P.warm && HOV) {
                P.rect(MB, MUTED ? color(cfg.colHighlight) : hovered.part == 5 ? tAccentDim() : tFill2(), (int)std::lround(MANAGE_D / 2 * P.scale));
                if (G && G->tex)
                    P.tex(G->tex, MB.x + (MB.w - G->tex->m_size.x / P.scale) / 2, MB.y + (MB.h - G->tex->m_size.y / P.scale) / 2);
            }
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
        const auto SUMLINE = cachedText(DB, COLTITLE, T.title, std::max(1, (int)((MB.x - 8 - TX) * P.scale)), -1, 0, true, 600);
        if (!P.warm && SUMLINE)
            P.tex(SUMLINE->tex, TX, box.y + ROW_PADT + (ROW_ICON - texH(SUMLINE, P.scale)) / 2);

        // <=2 preview lines, indented into the text column
        double       py   = box.y + ROW_PADT + std::max(ROW_ICON, (double)T.title / P.scale + 2);
        const size_t PREV = std::min<size_t>(2, D.items.size());
        for (size_t i = 0; i < PREV; i++) {
            const auto& N = D.items[i];
            if (P.warm)
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
        card.manage.push_back({MB, 5});
        cards.push_back(std::move(card));
    }

    // The expanded bundle: a header that owns the app's identity, its count
    // and the ✕ that dismisses the lot, then every child as a full row.
    void paintGroup(const SPaint& P, const SType& T, const SDisp& D, const CBox& box, const std::vector<double>& childH) {
        const auto  COLFG = color(cfg.colFg), COLTITLE = color(cfg.colTitle), COLSUB = color(cfg.colKicker), COLURGENT = color(cfg.colUrgent);
        const auto& SUBHEX = hexOfCached(COLSUB);
        const float RP     = rPow();

        const auto&  NEWEST = D.items.front();
        const bool   HHOV   = hovered.kind == SCard::GHEAD && hovered.group == D.key;
        const double HEADRH = groupHeadH();
        P.rect(CBox{box.x, box.y, box.w, HEADRH}, HHOV ? tAccentDim() : tFill(), rRow(P.scale), RP);

        if (P.warm)
            ensureIconTex(*NEWEST, (int)std::lround(cfg.maxIcon->value() * P.scale), 0, 0);
        const auto& IDT = NEWEST->identTex && NEWEST->identTex->m_texID ? NEWEST->identTex : NEWEST->iconTex;
        if (IDT)
            P.texFit(IDT, CBox{box.x + ROW_PADX, box.y + ROW_PADT, CHILD_ICON, CHILD_ICON}, (int)std::lround(CHILD_ICON * 10.0 / 44.0 * P.scale), RP);

        // the static ✕ (dismiss the whole app's bundle)
        const CBox XB{box.x + box.w - ROW_PADX - XCIRC, box.y + ROW_PADT + (CHILD_ICON - XCIRC) / 2, XCIRC, XCIRC};
        const bool XHOV = HHOV && hovered.part == 2;
        // BOTH colours, every pass: hover flips without a rewarm, so keying
        // this raster on the hover state would miss the cache for a frame
        const auto XG    = cachedText("✕", COLFG, T.small, 64, -1, 0, false, 600);
        const auto XGHOT = cachedText("✕", tOnAccent(), T.small, 64, -1, 0, false, 600);
        if (!P.warm) {
            P.rect(XB, XHOV ? COLURGENT : tFill2(), (int)std::lround(XCIRC / 2 * P.scale));
            if (const auto* G = XHOV ? XGHOT : XG; G && G->tex)
                P.tex(G->tex, XB.x + (XB.w - G->tex->m_size.x / P.scale) / 2, XB.y + (XB.h - G->tex->m_size.y / P.scale) / 2);
        }

        // the header is chrome, so its controls stand rather than hide — the
        // ✕ already does, and the ⊘ beside it manages the whole bundle's app
        const bool MUTED = Policy::silenced(D.key);
        const CBox MB{XB.x - 6 - MANAGE_D, box.y + ROW_PADT + (CHILD_ICON - MANAGE_D) / 2, MANAGE_D, MANAGE_D};
        {
            const bool MHOV = HHOV && hovered.part == 5;
            const auto G    = cachedText("⊘", MUTED ? tOnAccent() : COLSUB, T.small, 64, -1, 0, false, 600);
            if (!P.warm) {
                P.rect(MB, MUTED ? color(cfg.colHighlight) : MHOV ? tAccentDim() : tFill2(), (int)std::lround(MANAGE_D / 2 * P.scale));
                if (G && G->tex)
                    P.tex(G->tex, MB.x + (MB.w - G->tex->m_size.x / P.scale) / 2, MB.y + (MB.h - G->tex->m_size.y / P.scale) / 2);
            }
        }

        const auto   PILL  = cachedText(std::to_string(D.items.size()) + " ˄", COLFG, T.small, 64, -1, 0, false, 600);
        const double PILLW = texW(PILL, P.scale) + 14;
        const CBox   PB{MB.x - 6 - PILLW, box.y + ROW_PADT + (CHILD_ICON - PILL_H) / 2, PILLW, PILL_H};
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
            card.manage.push_back({MB, 5});
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
