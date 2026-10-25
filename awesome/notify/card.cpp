// awesome/notify/card.cpp — ONE notification card, wherever it shows: a
// banner in the column, a card in the center, a child inside an app's
// bundle. The anatomy is the same everywhere: the icon column (the sender's
// face wearing the app's badge, or a group chat's two faces), the title row
// (title, the unread count, the meta flush right), the body and the
// progress track; below, across the card's whole inner width, a banner's
// hero, the sender's actions as glass buttons, and in the center the reply
// field. A banner and the center card it retreats into must never say
// different things — so they are one function.
//
// Layout requests every texture unconditionally and paints only through
// the paint context, which no-ops inside a warm (the texture rule, crash
// class 4). The center's budget measures with this same function, P.warm
// forced on, so a measured height is the painted one.

#include "ui.hpp"

namespace NAwesome::Notify {

    namespace {
        struct SBtnCell {
            CBox               box; // relative to the button block's top-left
            const std::string* id    = nullptr;
            const SCachedText* label = nullptr;
            SP<ITexture>       icon;
        };

        // one alt line of a failed body <img>: the text, its raster, its height
        struct SAltLine {
            std::string        text;
            const SCachedText* tex = nullptr;
            double             h   = 0;
        };

        // The sender's buttons, equal widths across the inner width: up to
        // three to a row while every label fits, fewer per row when one
        // would not. Returns the block's height.
        double layoutButtons(std::vector<SBtnCell>& cells, double innerW, double scale) {
            const size_t N = cells.size();
            if (N == 0)
                return 0;
            size_t perRow = std::min<size_t>(N, 3);
            for (; perRow > 1; perRow--) {
                const double CW  = (innerW - BTN_GAP * (double)(perRow - 1)) / (double)perRow;
                const bool   FIT = std::ranges::all_of(cells, [&](const SBtnCell& C) {
                    return texW(C.label, scale) + (C.icon ? BTN_ICON + BTN_ICON_GAP : 0) + 2 * BTN_PADX <= CW + 0.5;
                });
                if (FIT)
                    break;
            }
            const size_t ROWS = (N + perRow - 1) / perRow;
            for (size_t i = 0; i < N; i++) {
                const size_t ROW = i / perRow, COL = i % perRow;
                // the last row splits the width among what it holds
                const size_t INROW = ROW + 1 == ROWS ? N - ROW * perRow : perRow;
                const double CW    = (innerW - BTN_GAP * (double)(INROW - 1)) / (double)INROW;
                cells[i].box       = CBox{(double)COL * (CW + BTN_GAP), (double)ROW * (BTN_H + BTN_GAP), CW, BTN_H};
            }
            return (double)ROWS * BTN_H + (double)(ROWS - 1) * BTN_GAP;
        }

        // the meta at the title row's right: what state the card is in when
        // that matters more than when it came
        void composeMeta(std::string& out, const SNotif& N, bool banner) {
            if (N.urgency >= 2) {
                out += "Critical";
                return;
            }
            if (N.progress >= 0) {
                out += std::to_string(N.progress);
                out += '%';
                return;
            }
            if (!banner) {
                out += clockString(N.arrivedWall);
                return;
            }
            if (!N.appName.empty() && N.appName != N.summary && N.appName != N.conversationTitle) {
                appendEsc(out, N.appName);
                out += " · ";
            }
            out += ageString(N.arrived);
        }
    }

    double renderCard(const SPaint& P, const SType& T, const SP<SNotif>& NP, const CBox& box, const SCardForm& F, SCard& hit) {
        const SNotif& N      = *NP;
        const bool    BANNER = F.where == SCardForm::BANNER, CHILD = F.where == SCardForm::CHILD;
        const bool    CENTER = F.where == SCardForm::CENTER;
        const bool    FOLDED = CENTER && !F.open;
        const double  X = box.x, Y = box.y, W = box.w;
        const double  PY = CHILD ? CHILD_PADY : PADY;
        const double  INNERW = W - 2 * PADX;

        const auto   COLBG = color(cfg().getColor("plugin:awesome:notify:col_bg")), COLRIM = color(cfg().getColor("plugin:awesome:notify:col_frame")),
                   COLFG = color(cfg().getColor("plugin:awesome:notify:col_fg")), COLTITLE = color(cfg().getColor("plugin:awesome:notify:col_title")),
                   COLSUB = color(cfg().getColor("plugin:awesome:notify:col_kicker")), COLURGENT = color(cfg().getColor("plugin:awesome:notify:col_urgent")),
                   COLACC = color(cfg().getColor("plugin:awesome:notify:col_highlight")), COLLINK = color(cfg().getColor("plugin:awesome:notify:col_link"));
        const float  RP    = rPow();
        const int    ROUND = rCard(P.scale);
        const double ICONPX = std::clamp((double)cfg().getI("plugin:awesome:notify:max_icon"), 16.0, 64.0);
        auto&        GATE   = NAwesome::Canvas::inst().gate();

        // every build gates on the canvas gate, never on P.warm: a measuring
        // pass forces P.warm on to paint nothing, and a build there would be
        // a build inside a render (crash class 4)
        prepareCard(NP, P.scale);
        if (GATE.warming) {
            for (auto& PA : NP->participants)
                ensureAvatarTex(PA, (int)std::lround(ICONPX * 0.66 * P.scale));
            for (auto& A : NP->actions)
                ensureActionIcon(*NP, A, (int)std::lround(BTN_ICON * P.scale));
            if (BANNER)
                for (auto& IM : NP->bodyImages)
                    ensureBodyImage(IM, (int)std::lround(BODYIMG_H * P.scale));
        }

        // ---- the icon column ----
        // a group chat with two faces shows both, overlapping; otherwise the
        // lead face (a banner's hero leaves it the app's identity)
        const bool   HERO  = BANNER && N.iconTex && N.heroTex;
        const bool   PILE  = !CHILD && N.conversationKind == "group" && N.participants.size() >= 2 && N.participants[0].avatarTex && N.participants[1].avatarTex;
        const bool   LEAD  = !CHILD && (PILE || hasLeadIcon(N, !BANNER));
        const double ICONW = LEAD ? ICONPX : 0;
        const double TX    = X + PADX + (LEAD ? ICONW + ICON_GAP : 0);
        const double TEXTW = X + W - PADX - TX;
        const int    TWPX  = std::max(1, (int)std::floor(TEXTW * P.scale));

        // ---- the title row: title, the unread count, the meta ----
        auto& MB = scratch();
        composeMeta(MB, N, BANNER);
        const auto   META  = cachedText(MB, N.urgency >= 2 ? COLURGENT : COLSUB, T.meta, TWPX / 2, -1, 0, true, 500);
        const double METAW = texW(META, P.scale);
        const auto   COUNT = N.conversation && N.unreadCount > 1 ? cachedText(std::to_string(N.unreadCount), COLTITLE, T.small, 64, -1, 0, false, 600) : nullptr;
        const double PILLW = COUNT && COUNT->tex ? std::max(PILL_H, texW(COUNT, P.scale) + 10) : 0;
        const auto&  TSRC  = !N.conversationTitle.empty() ? N.conversationTitle : N.summary;
        const int    TTLPX = std::max(1, (int)std::floor((TEXTW - (METAW > 0 ? METAW + META_GAP : 0) - (PILLW > 0 ? PILLW + 6 : 0)) * P.scale));
        const auto   TITLE = TSRC.empty() ? nullptr : cachedText(TSRC, COLTITLE, T.title, TTLPX, -1, 0, true, 600);
        const double ROWH  = std::max({texH(TITLE, P.scale), texH(META, P.scale), PILLW > 0 ? PILL_H : 0.0});

        // ---- the sender's buttons (an open card only) ----
        static std::vector<SBtnCell> btns; // reused; main thread only
        btns.clear();
        if (!FOLDED)
            for (const auto& A : NP->actions) {
                auto& LB = scratch();
                appendEsc(LB, A.label);
                btns.push_back(SBtnCell{.id = &A.id, .label = cachedText(LB, COLTITLE, T.action, (int)std::floor(INNERW * P.scale), -1, 0, true, 500),
                                        .icon = N.actionIcons ? A.iconTex : nullptr});
            }
        double btnH = layoutButtons(btns, INNERW, P.scale);

        // the center's reply field, armed or not: a chat answers in place
        const bool REPLY = CENTER && !FOLDED && N.canReply;
        const bool ARMED = REPLY && replyArmedOn(N.id);

        // ---- a banner's inline images and their alt lines ----
        static std::vector<CBox> imgBoxes; // reused; main thread only
        static std::vector<SAltLine> altLines;
        imgBoxes.clear();
        altLines.clear();
        double imgH = 0;
        if (BANNER) {
            double bx = 0, rowY = 0;
            for (const auto& IM : N.bodyImages) {
                if (!IM.tex)
                    continue;
                const double AR = IM.tex->m_size.y > 0 ? IM.tex->m_size.x / IM.tex->m_size.y : 1.0;
                const double WD = std::min(TEXTW, AR * BODYIMG_H);
                if (bx > 0 && bx + WD > TEXTW + 0.5) {
                    bx = 0;
                    rowY += BODYIMG_H + IMG_GAP;
                }
                imgBoxes.push_back(CBox{bx, rowY, WD, BODYIMG_H});
                bx += WD + IMG_GAP;
            }
            imgH = imgBoxes.empty() ? 0 : rowY + BODYIMG_H;
            // an <img> that failed keeps its alt text: the sender meant to
            // show something, and the words outlive the missing file
            for (const auto& IM : N.bodyImages)
                if (!IM.tex && !IM.alt.empty())
                    altLines.push_back(SAltLine{.text = IM.alt});
            for (auto& A : altLines) {
                A.tex = cachedText(A.text, COLSUB, T.body, TWPX, 1, 0, true, 400); // sanitized markup: entities decode
                A.h   = A.tex ? std::max(14.0, texH(A.tex, P.scale)) : 0;
            }
        }

        // ---- the body ----
        // a banner previews the newest five messages of a chat, the center
        // shows the card's stored window; folded, the newest line alone
        const std::string FOLDLINE = FOLDED ? collapsedLine(NP) : std::string{};
        const std::string& BODYSRC = FOLDED ? FOLDLINE : BANNER && N.conversation ? Model::conversationBody(NP, Pixel::MAX_PREVIEWED_CONVERSATION_MESSAGES) : N.body;
        const int          LINEPX  = bodyBudgetPx(P.scale, 1);
        const double       PROGH   = N.progress >= 0 ? PROGRESS_GAP + PROGRESS_H : 0;
        double             heroH   = HERO ? std::min(HERO_CAP, INNERW * N.iconTex->m_size.y / std::max(1.0, N.iconTex->m_size.x)) : 0;
        int                bodyCap = LINEPX;
        if (BANNER) {
            // max_height fits the blocks by priority: the title row, one body
            // line, the progress and the actions first (what the sender asks
            // of the user), then the hero, the images, their alt lines; the
            // body takes what is left. A block that does not fit is dropped
            // whole, from the end — painted past the glass once, it covered
            // whatever lay below and took none of its clicks.
            const double MAXH = std::max((double)cfg().getI("plugin:awesome:notify:max_height"), 60.0);
            double       room = MAXH - 2 * PY - ROWH - (BODYSRC.empty() ? 0 : ROW_GAP + LINEPX / P.scale) - PROGH;
            const auto   take = [&](double h) {
                if (h <= 0 || h > room + 0.5)
                    return false;
                room -= h;
                return true;
            };
            while (!btns.empty() && !take(BLOCK_GAP + btnH)) {
                btns.pop_back();
                btnH = layoutButtons(btns, INNERW, P.scale);
            }
            if (heroH > 0 && !take(BLOCK_GAP + heroH)) {
                heroH = std::max(0.0, room - BLOCK_GAP); // a short hero beats none, down to a sliver
                if (heroH < 48)
                    heroH = 0;
                room -= heroH > 0 ? BLOCK_GAP + heroH : 0;
            }
            while (!imgBoxes.empty() && IMG_ROW_GAP + imgBoxes.back().y + imgBoxes.back().h > room + 0.5)
                imgBoxes.pop_back();
            imgH = imgBoxes.empty() ? 0 : imgBoxes.back().y + imgBoxes.back().h;
            room -= imgH > 0 ? IMG_ROW_GAP + imgH : 0;
            double altH = 0;
            for (size_t i = 0; i < altLines.size(); i++) {
                if (IMG_ROW_GAP + altH + altLines[i].h > room + 0.5) {
                    altLines.resize(i);
                    break;
                }
                altH += altLines[i].h;
            }
            room -= altH > 0 ? IMG_ROW_GAP + altH : 0;
            bodyCap = std::max(LINEPX, std::min(bodyBudgetPx(P.scale, 8), (int)std::floor((room + LINEPX / P.scale) * P.scale)));
        } else if (!FOLDED)
            bodyCap = bodyBudgetPx(P.scale, CHILD ? 2 : N.conversation ? 6 : 4);
        // linkCol collects the <a href> rects: a click meant for a URL must
        // not fire the card's default action
        const auto   BODY = BODYSRC.empty() ? nullptr : cachedText(BODYSRC, COLFG, T.body, TWPX, bodyCap, 1.1f, true, 400, FOLDED ? nullptr : &COLLINK);
        const double BH   = texH(BODY, P.scale);
        double       altH = 0;
        for (const auto& A : altLines)
            altH += A.h;

        // ---- the reply field's pieces ----
        static const std::string PLACEHOLDER = "Reply"; // no temporary per frame
        const SCachedText*       ENTRY = nullptr;
        const SCachedText*       HINT  = nullptr;
        if (REPLY) {
            const int FIELDPX = std::max(1, (int)std::floor((INNERW - 2 * BTN_PADX - SEND_D) * P.scale));
            const auto& TXT   = ARMED ? replyText() : PLACEHOLDER; // the field shows its text only while armed
            if (ARMED && !TXT.empty())
                ENTRY = cachedText(TXT, COLTITLE, T.action, FIELDPX, -1, 0, false, 400, nullptr, true);
            else
                HINT = cachedText(N.replyPlaceholder.empty() ? PLACEHOLDER : N.replyPlaceholder, COLSUB, T.action, FIELDPX, -1, 0, false, 400);
        }

        // ---- heights ----
        const double TEXTH = ROWH + (BH > 0 ? ROW_GAP + BH : 0) + (imgH > 0 ? IMG_ROW_GAP + imgH : 0) + (altH > 0 ? IMG_ROW_GAP + altH : 0) + PROGH;
        const double TOPH  = std::max(ICONW, TEXTH);
        const double BELOW = (heroH > 0 ? BLOCK_GAP + heroH : 0) + (btnH > 0 ? BLOCK_GAP + btnH : 0) + (REPLY ? BLOCK_GAP + FIELD_H : 0);
        const double H     = PY + TOPH + BELOW + PY;
        const CBox   CARD{X, Y, W, H};

        // ---- paint ----
        if (!CHILD)
            P.frost(CARD, COLBG, COLRIM, ROUND, RP);

        const double TOPY = Y + PY;
        if (PILE) {
            // the two newest faces, the second over the first's corner on a
            // ring of the glass's own dark
            const double D = ICONW * 0.66;
            const auto&  A = N.participants[0].avatarTex;
            const auto&  B = N.participants[1].avatarTex;
            P.texFit(B, CBox{X + PADX, TOPY, D, D}, (int)std::lround(D / 2 * P.scale), 2.f);
            const CBox SECOND{X + PADX + ICONW - D, TOPY + ICONW - D, D, D};
            P.rect(CBox{SECOND}.expand(1.5), COLBG.modifyA(1.f), (int)std::lround((D / 2 + 1.5) * P.scale), 2.f);
            P.texFit(A, SECOND, (int)std::lround(D / 2 * P.scale), 2.f);
        } else if (LEAD)
            paintIconColumn(P, N, CBox{X + PADX, TOPY, ICONW, ICONW}, true, RP, !BANNER);

        // the title row, its pieces centred on one line
        double tx = TX;
        if (TITLE && TITLE->tex) {
            P.tex(TITLE->tex, tx, TOPY + (ROWH - texH(TITLE, P.scale)) / 2);
            tx += texW(TITLE, P.scale) + 6;
        }
        if (PILLW > 0) {
            const CBox PB{tx, TOPY + (ROWH - PILL_H) / 2, PILLW, PILL_H};
            P.rect(PB, tFill(), (int)std::lround(PILL_H / 2 * P.scale), 2.f);
            texCentered(P, COUNT, PB);
        }
        if (META && META->tex)
            P.tex(META->tex, X + W - PADX - METAW, TOPY + (ROWH - texH(META, P.scale)) / 2);

        double ty = TOPY + ROWH;
        if (BODY) {
            ty += ROW_GAP;
            P.tex(BODY->tex, TX, ty);
            for (const auto& L : BODY->links) // physical -> global logical, in both modes
                hit.links.push_back({CBox{TX + L.rel.x / P.scale, ty + L.rel.y / P.scale, L.rel.w / P.scale, L.rel.h / P.scale}, L.href});
            ty += BH;
        }
        if (imgH > 0) {
            ty += IMG_ROW_GAP;
            size_t bi = 0;
            for (const auto& IM : N.bodyImages)
                if (IM.tex && bi < imgBoxes.size()) {
                    const auto& B = imgBoxes[bi++];
                    P.texFit(IM.tex, CBox{TX + B.x, ty + B.y, B.w, B.h}, (int)std::lround(Theme::RAD_ROW * P.scale), RP);
                }
            ty += imgH;
        }
        if (altH > 0) {
            ty += IMG_ROW_GAP;
            for (const auto& A : altLines)
                if (A.tex && A.tex->tex) {
                    P.tex(A.tex->tex, TX, ty + (A.h - texH(A.tex, P.scale)) / 2);
                    ty += A.h;
                }
        }
        if (N.progress >= 0)
            paintProgress(P, TX, ty + PROGRESS_GAP, TEXTW, N.progress, N.urgency >= 2);

        // the full-width blocks
        double by = TOPY + TOPH;
        if (heroH > 0) {
            by += BLOCK_GAP;
            P.texFit(N.iconTex, CBox{X + PADX, by, INNERW, heroH}, (int)std::lround(Theme::RAD_BUTTON * P.scale), RP);
            by += heroH;
        }
        if (btnH > 0) {
            by += BLOCK_GAP;
            const bool MINE = hovered.id == N.id && (hovered.kind == SCard::POPUP) == BANNER;
            for (size_t i = 0; i < btns.size(); i++) {
                const CBox B{X + PADX + btns[i].box.x, by + btns[i].box.y, btns[i].box.w, btns[i].box.h};
                paintButton(P, B, btns[i].label, btns[i].icon, MINE && hovered.btn == (int)i);
                hit.buttons.push_back({B, *btns[i].id});
            }
            by += btnH;
        }
        if (REPLY) {
            by += BLOCK_GAP;
            const CBox FB{X + PADX, by, INNERW, FIELD_H};
            const CBox SB{FB.x + FB.w - 4 - SEND_D, FB.y + (FIELD_H - SEND_D) / 2, SEND_D, SEND_D};
            const int  RB = (int)std::lround(FIELD_H / 2 * P.scale);
            const bool LIVE = ARMED && !replyText().empty();
            const auto ARROW = Glyph::tex(Glyph::eGlyph::SEND, LIVE ? tOnAccent() : COLSUB, (int)std::lround(14 * P.scale));
            P.rect(FB, tFill(), RB, 2.f);
            if (ARMED)
                P.ring(FB, COLACC, RB, 2.f); // armed: the field wears the accent
            double cx = FB.x + BTN_PADX;
            if (const auto* SHOW = ENTRY ? ENTRY : HINT; SHOW && SHOW->tex) {
                P.tex(SHOW->tex, cx, FB.y + (FIELD_H - texH(SHOW, P.scale)) / 2);
                if (ENTRY)
                    cx += texW(ENTRY, P.scale);
            }
            if (ARMED) // the caret sits at the end: editing is append + backspace
                P.rect(CBox{std::min(cx + 1, SB.x - 3), FB.y + 8, 1.5, FIELD_H - 16}, COLACC);
            P.rect(SB, LIVE ? COLACC : tFill(), (int)std::lround(SEND_D / 2 * P.scale), 2.f);
            if (ARROW)
                P.texIn(ARROW, SB);
            if (ARMED) {
                hit.replyField = FB;
                hit.replySend  = SB;
            } else // the field arms on a click, as the button it replaced did
                hit.buttons.push_back({FB, "inline-reply"});
        }

        hit.box  = CARD;
        hit.id   = N.id;
        hit.rev  = N.rev;
        hit.kind = BANNER ? SCard::POPUP : CHILD ? SCard::CHILD : SCard::ROW;
        return H;
    }

    // measure without painting: same code, a paint context that draws nothing
    // (cachedText still resolves through the real warm gate)
    double measureCard(const SPaint& P, const SType& T, const SP<SNotif>& N, double w, const SCardForm& F) {
        SPaint MP = P;
        MP.warm   = true; // paints nothing; it does NOT license a texture build
        static SCard scratchCard; // a measure fills hit rects too: reused, never kept
        scratchCard.buttons.clear();
        scratchCard.links.clear();
        return renderCard(MP, T, N, CBox{0, 0, w, 0}, F, scratchCard);
    }

} // namespace NAwesome::Notify
