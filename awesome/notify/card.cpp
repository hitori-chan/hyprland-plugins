// awesome/notify/card.cpp — ONE notification card, wherever it shows: a
// banner in the column, a card in the center, alone or in its app's stack.
// The anatomy is the same everywhere: the icon column (the sender's face
// wearing the app's badge, or a group chat's initials), the title row
// (title, the unread count, the meta flush right), the body and the
// progress track; below, across the card's whole inner width, a banner's
// hero and an opened reply field. A card is its face, its words and its
// time, as macOS draws it at rest, and it grows no buttons under the
// pointer (the user's hold and right click do that work): the glass lifts,
// and the sender's actions live in the card menu. A banner and the center
// card it retreats into must never say different things — so they are one
// function.
//
// Layout requests every texture unconditionally and paints only through
// the paint context, which no-ops inside a warm (the texture rule, crash
// class 4). The center's budget measures with this same function, P.warm
// forced on, so a measured height is the painted one.

#include "ui.hpp"

namespace NAwesome::Notify {

    namespace {
        // one alt line of a failed body <img>: the text, its raster, its height
        struct SAltLine {
            std::string        text;
            const SCachedText* tex = nullptr;
            double             h   = 0;
        };

        // the meta at the title row's right: what state the card is in when
        // that matters more than when it came. `fresh`: the accent its time
        // wears (new since the center last showed it), null for none
        void composeMeta(std::string& out, const SNotif& N, bool banner, const CHyprColor* fresh) {
            if (N.urgency >= 2) {
                out += "Critical";
                return;
            }
            if (N.progress >= 0) {
                out += std::to_string(N.progress);
                out += '%';
                return;
            }
            // the app names itself unless the card already does: a chat's
            // badge says it, and so does a title that is the app's name
            if (!N.conversation && !N.appName.empty() && N.appName != N.summary) {
                appendEsc(out, N.appName);
                out += " · ";
            }
            if (fresh) {
                out += "<span foreground=\"";
                out += hexOfCached(*fresh);
                out += "\">";
            }
            out += banner ? ageString(N.arrived) : clockString(N.arrivedWall);
            if (fresh)
                out += "</span>";
        }
    }

    double renderCard(const SPaint& P, const SType& T, const SP<SNotif>& NP, const CBox& box, const SCardForm& F, SCard& hit) {
        const SNotif&      N      = *NP;
        const bool         BANNER = F.where == SCardForm::BANNER;
        const bool         FOLDED = !BANNER && !F.open;
        const SCard::eKind KIND   = BANNER ? SCard::POPUP : F.deck ? SCard::DECK : SCard::ROW;
        const double       X = box.x, Y = box.y, W = box.w;
        const double       PY     = PADY;
        const double       INNERW = W - 2 * PADX;

        const auto   COLBG = color(cfg().getColor("plugin:awesome:theme:col_glass")), COLRIM = color(cfg().getColor("plugin:awesome:theme:col_rim")),
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
        // a group with no picture of its own wears its initials on a colour
        // of its own, the app's badge on the corner (two senders' faces
        // under the badge hid one)
        const bool GROUPFACE = N.conversationKind == "group" && !N.hasPixels && N.image.empty();
        prepareCard(NP, P.scale);
        if (GATE.warming) {
            if (GROUPFACE) {
                auto& F = NP->chatFace;
                if (const auto& NAME = N.conversationTitle.empty() ? N.summary : N.conversationTitle; F.name != NAME) {
                    F.name = NAME;
                    F.key  = replyTarget(N);
                }
                ensureAvatarTex(F, (int)std::lround(ICONPX * P.scale));
            }
            if (BANNER)
                for (auto& IM : NP->bodyImages)
                    ensureBodyImage(IM, (int)std::lround(BODYIMG_H * P.scale));
        }

        // ---- the icon column ----
        // a group chat with two faces shows both, overlapping; otherwise the
        // lead face (a banner's hero leaves it the app's identity)
        // a critical banner over a share: its face and title, never its words
        const bool   HUSH  = BANNER && N.urgency >= 2 && Quiet::hushed();
        const bool   HERO  = BANNER && !HUSH && N.iconTex && N.heroTex;
        const bool   INITIALS = GROUPFACE && N.chatFace.avatarTex;
        const bool   LEAD     = INITIALS || hasLeadIcon(N, !BANNER);
        const double ICONW = LEAD ? ICONPX : 0;
        const double TX    = X + PADX + (LEAD ? ICONW + ICON_GAP : 0);
        const double TEXTW = X + W - PADX - TX;
        const int    TWPX  = std::max(1, (int)std::floor(TEXTW * P.scale));

        // ---- the title row: title, the unread count, the meta ----
        auto& MB = scratch();
        composeMeta(MB, N, BANNER, F.fresh ? &COLACC : nullptr);
        const auto   META  = cachedText(MB, N.urgency >= 2 ? COLURGENT : COLSUB, T.meta, TWPX / 2, -1, 0, true, 500);
        const double METAW = texW(META, P.scale);
        const auto   COUNT = N.conversation && N.unreadCount > 1 ? cachedText(std::to_string(N.unreadCount), COLTITLE, T.small, 64, -1, 0, false, 600) : nullptr;
        const double PILLW = COUNT && COUNT->tex ? std::max(PILL_H, texW(COUNT, P.scale) + 10) : 0;
        const auto&  TSRC  = !N.conversationTitle.empty() ? N.conversationTitle : N.summary;
        const int    TTLPX = std::max(1, (int)std::floor((TEXTW - (METAW > 0 ? METAW + META_GAP : 0) - (PILLW > 0 ? PILLW + 6 : 0)) * P.scale));
        const auto   TITLE = TSRC.empty() ? nullptr : cachedText(TSRC, COLTITLE, T.title, TTLPX, -1, 0, true, 600);
        const double ROWH  = std::max({texH(TITLE, P.scale), texH(META, P.scale), PILLW > 0 ? PILL_H : 0.0});

        // the reply field, once Reply (the card menu, or Tab in the center)
        // opened it: a chat answers in place, in the banner or the center card
        const bool REPLY = N.canReply && replyArmedOn(N.id);

        // ---- a banner's inline images and their alt lines ----
        static std::vector<CBox> imgBoxes; // reused; main thread only
        static std::vector<SAltLine> altLines;
        imgBoxes.clear();
        altLines.clear();
        double imgH = 0;
        if (BANNER && !HUSH) {
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
        // a chat shows what fits of its newest messages, banner and center
        // alike (the window is fitted below, once the lines are known);
        // folded, the newest message alone; hushed, none
        const std::string  FOLDLINE = FOLDED && !HUSH ? collapsedLine(NP) : std::string{};
        const std::string& RAWBODY  = FOLDED || HUSH ? FOLDLINE : N.body;
        const bool         CHAT     = !FOLDED && !HUSH && !N.messages.empty();
        const int          LINEPX   = bodyBudgetPx(P.scale, 1);
        const double       PROGH   = N.progress >= 0 ? PROGRESS_GAP + PROGRESS_H : 0;
        double             heroH   = HERO ? std::min(HERO_CAP, INNERW * N.iconTex->m_size.y / std::max(1.0, N.iconTex->m_size.x)) : 0;
        int                bodyCap = LINEPX;
        if (BANNER) {
            // max_height fits the blocks by priority: the title row, one body
            // line, the progress and an opened reply field first (what the
            // user is doing), then the hero, the images, their alt lines; the
            // body takes what is left. A block that does not fit is dropped
            // whole, from the end — painted past the glass once, it covered
            // whatever lay below and took none of its clicks.
            const double MAXH = std::max((double)cfg().getI("plugin:awesome:notify:max_height"), 60.0);
            double       room = MAXH - 2 * PY - ROWH - (RAWBODY.empty() ? 0 : ROW_GAP + LINEPX / P.scale) - PROGH - (REPLY ? BLOCK_GAP + FIELD_H : 0);
            const auto   take = [&](double h) {
                if (h <= 0 || h > room + 0.5)
                    return false;
                room -= h;
                return true;
            };
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
            bodyCap = bodyBudgetPx(P.scale, N.conversation ? 6 : 4);
        // linkCol collects the <a href> rects: a click meant for a URL must
        // not fire the card's default action (a deck's click expands its
        // stack, whatever it lands on)
        const bool         LINKS   = !FOLDED && !F.deck;
        // a chat's messages hang: a line at the margin begins one
        const int          HANGPX  = CHAT ? (int)std::lround(HANG * P.scale) : 0;
        const std::string& BODYSRC = CHAT ? fittedTranscript(NP, T.body, TWPX, bodyCap, HANGPX, LINKS ? &COLLINK : nullptr) : RAWBODY;
        const auto         BODY    = BODYSRC.empty() ? nullptr : cachedText(BODYSRC, COLFG, T.body, TWPX, bodyCap, 1.1f, true, 400, LINKS ? &COLLINK : nullptr, false, eShadow::SOFT, HANGPX);
        const double BH   = texH(BODY, P.scale);
        double       altH = 0;
        for (const auto& A : altLines)
            altH += A.h;

        // ---- the reply field's pieces ----
        static const std::string PLACEHOLDER = "Reply"; // no temporary per frame
        const SCachedText*       ENTRY = nullptr;
        const SCachedText*       HINT  = nullptr;
        if (REPLY) {
            const int FIELDPX = std::max(1, (int)std::floor((INNERW - FIELD_PADL - FIELD_GAP - SEND_D - FIELD_PADR) * P.scale));
            if (const auto& TXT = replyText(); !TXT.empty())
                ENTRY = cachedText(TXT, COLTITLE, T.action, FIELDPX, -1, 0, false, 400, nullptr, true);
            else
                HINT = cachedText(N.replyPlaceholder.empty() ? PLACEHOLDER : N.replyPlaceholder, COLSUB, T.action, FIELDPX, -1, 0, false, 400);
        }

        // ---- heights ----
        const double TEXTH = ROWH + (BH > 0 ? ROW_GAP + BH : 0) + (imgH > 0 ? IMG_ROW_GAP + imgH : 0) + (altH > 0 ? IMG_ROW_GAP + altH : 0) + PROGH;
        const double TOPH  = std::max(ICONW, TEXTH);
        const double BELOW = (heroH > 0 ? BLOCK_GAP + heroH : 0) + (REPLY ? BLOCK_GAP + FIELD_H : 0);
        const double H     = PY + TOPH + BELOW + PY;
        const CBox   CARD{X, Y, W, H};

        // ---- paint ----
        P.frost(CARD, cardLit(N.id, KIND) ? liftedGlass(COLBG) : COLBG, COLRIM, ROUND, RP);

        const double TOPY = Y + PY;
        if (INITIALS) {
            const CBox FACE{X + PADX, TOPY, ICONW, ICONW};
            P.texFit(N.chatFace.avatarTex, FACE, (int)std::lround(ICONW / 2 * P.scale), 2.f);
            paintBadge(P, N, FACE);
        } else if (LEAD)
            paintIconColumn(P, N, CBox{X + PADX, TOPY, ICONW, ICONW}, true, RP, !BANNER);

        // the title row, its pieces centred on one line
        double tx = TX;
        if (TITLE && TITLE->tex) {
            texAt(P, TITLE, tx, TOPY + (ROWH - texH(TITLE, P.scale)) / 2);
            tx += texW(TITLE, P.scale) + 6;
        }
        if (PILLW > 0) {
            const CBox PB{tx, TOPY + (ROWH - PILL_H) / 2, PILLW, PILL_H};
            P.rect(PB, tFill(), (int)std::lround(PILL_H / 2 * P.scale), 2.f);
            texCentered(P, COUNT, PB);
        }
        if (META && META->tex)
            texAt(P, META, X + W - PADX - METAW, TOPY + (ROWH - texH(META, P.scale)) / 2);

        double ty = TOPY + ROWH;
        if (BODY) {
            ty += ROW_GAP;
            texAt(P, BODY, TX, ty);
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
                    texAt(P, A.tex, TX, ty + (A.h - texH(A.tex, P.scale)) / 2);
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
        if (REPLY) {
            by += BLOCK_GAP;
            const CBox FB{X + PADX, by, INNERW, FIELD_H};
            const CBox SB{FB.x + FB.w - FIELD_PADR - SEND_D, FB.y + (FIELD_H - SEND_D) / 2, SEND_D, SEND_D};
            const int  RB    = (int)std::lround(FIELD_H / 2 * P.scale);
            const bool LIVE  = !replyText().empty();
            const auto ARROW = Glyph::tex(Glyph::eGlyph::SEND, LIVE ? tOnAccent() : tSendArrow(), (int)std::lround(SEND_ARROW * P.scale));
            // a well sunk into the glass: dark, its inner edge catching light
            P.rect(FB, tWell(), RB, 2.f);
            P.ring(CBox{FB}.expand(-1.0 / P.scale), tWellRim(), RB - 1, 2.f);
            P.ring(FB, COLACC, RB, 2.f); // it holds the keyboard: the field wears the accent
            double cx = FB.x + FIELD_PADL;
            if (const auto* SHOW = ENTRY ? ENTRY : HINT; SHOW && SHOW->tex) {
                texAt(P, SHOW, cx, FB.y + (FIELD_H - texH(SHOW, P.scale)) / 2);
                if (ENTRY)
                    cx += texW(ENTRY, P.scale);
            }
            // the caret sits at the end: editing is append + backspace
            P.rect(CBox{std::min(cx + 1, SB.x - 3), FB.y + 8, 1.5, FIELD_H - 16}, COLACC);
            P.rect(SB, LIVE ? COLACC : tControl(), (int)std::lround(SEND_D / 2 * P.scale), 2.f);
            if (ARROW)
                P.texIn(ARROW, SB);
            hit.replyField = FB;
            hit.replySend  = SB;
        }

        hit.box  = CARD;
        hit.id   = N.id;
        hit.rev  = N.rev;
        hit.kind = KIND;
        return H;
    }

    // measure without painting: same code, a paint context that draws nothing
    // (cachedText still resolves through the real warm gate)
    double measureCard(const SPaint& P, const SType& T, const SP<SNotif>& N, double w, const SCardForm& F) {
        SPaint MP = P;
        MP.warm   = true; // paints nothing; it does NOT license a texture build
        static SCard scratchCard; // a measure fills hit rects too: reused, never kept
        scratchCard.links.clear();
        return renderCard(MP, T, N, CBox{0, 0, w, 0}, F, scratchCard);
    }

} // namespace NAwesome::Notify
