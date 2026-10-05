// awesome/notify/popups.cpp — the banner column: glass cards top-right on the
// focused monitor, the one-card anatomy (icon column + header/title/body +
// original actions), the hover-✕, the arrival spring.
//
// Only cards whose banner is up show here (residency hides expired banners
// into the center's shade); while the center is open the column yields to
// the panel entirely — render.cpp picks the surface.

#include "ui.hpp"

namespace NAwesome::Notify {

    bool popupsAnimating() {
        if (!animationsOn())
            return false;
        for (const auto& N : notifs)
            if (!N->waiting && N->banner && animT(N->born, Theme::MOTION_SPATIAL) < 1.f)
                return true;
        return false;
    }

    // one alt line of a failed body <img>: the text, its warmed raster and
    // the height layout reserved for it
    struct SAltLine {
        std::string      text;
        const SCachedText* tex = nullptr;
        double            h    = 0;
    };

    void renderPopups(const SPaint& P, const SType& T) {
        const auto   MB      = P.mon->logicalBox();
        const double W       = std::max((double)NAwesome::cfg().getI("plugin:awesome:notify:width"), 120.0);
        const double MAXH    = std::max((double)NAwesome::cfg().getI("plugin:awesome:notify:max_height"), 60.0);
        const double GAP     = std::max((double)NAwesome::cfg().getI("plugin:awesome:notify:margin"), 0.0);
        const double MAXICON = std::clamp((double)NAwesome::cfg().getI("plugin:awesome:notify:max_icon"), 16.0, 64.0);
        const int    ROUND   = std::max(0, (int)std::lround(NAwesome::cfg().getI("plugin:awesome:notify:rounding") * P.scale));
        const float  RP      = (float)NAwesome::cfg().getD("plugin:awesome:notify:rounding_power");

        const auto   COLBG = NAwesome::color(NAwesome::cfg().getColor("plugin:awesome:notify:col_bg")), COLFG = NAwesome::color(NAwesome::cfg().getColor("plugin:awesome:notify:col_fg")), COLTITLE = NAwesome::color(NAwesome::cfg().getColor("plugin:awesome:notify:col_title")), COLSUB = NAwesome::color(NAwesome::cfg().getColor("plugin:awesome:notify:col_kicker")), COLURGENT = NAwesome::color(NAwesome::cfg().getColor("plugin:awesome:notify:col_urgent")),
                   COLACC = NAwesome::color(NAwesome::cfg().getColor("plugin:awesome:notify:col_highlight")), COLLINK = NAwesome::color(NAwesome::cfg().getColor("plugin:awesome:notify:col_link"));
        const CHyprColor COLBODY = COLFG.modifyA(COLFG.a * 0.92);

        const double     X = MB.x + MB.w - EDGE - W;
        double           y = MB.y + (double)NAwesome::cfg().getI("plugin:awesome:notify:offset_y");

        for (const auto& N : notifs) {
            if (N->waiting || !N->banner)
                continue; // residency: only banners show as popups
            if (y + 2 * PADY > MB.y + MB.h)
                break; // no room: the tail waits off-screen, timeouts running

            const bool CRITICAL = N->urgency >= 2;
            const auto AGE      = ageString(N->arrived);

            // every build in the drawing units gates on NAwesome::Canvas::inst().gate().warming, never
            // on P.warm — a measuring pass forces P.warm on to paint nothing
            if (NAwesome::Canvas::inst().gate().warming)
                ensureIconTex(*N, (int)std::lround(MAXICON * P.scale), (int)std::lround(W * P.scale), (int)std::lround(HERO_CAP * P.scale));

            const bool   HERO  = N->iconTex && N->heroTex;
            const double HEROH = HERO ? N->iconTex->m_size.y / P.scale : 0;

            // ONE icon column (paintIconColumn): the avatar leads and the app
            // identity badges its corner. A wide content image goes hero instead.
            const bool   LEADICON = !HERO && hasLeadIcon(*N);
            const double ICONW    = LEADICON ? MAXICON : 0;

            const double TEXTW   = W - 2 * PADX - (ICONW > 0 ? ICONW + ICON_GAP : 0);
            const int    TEXTWPX = std::max(1, (int)std::floor(TEXTW * P.scale));

            // text pieces (cache-keyed; ages re-key on bucket moves); the
            // body is rastered LAST — its cap subtracts every other block.
            // Compositions build into the reused scratch buffer: this runs
            // per card per layout pass, and fresh strings here were the
            // hottest allocation on the path.
            auto& SB = scratch();
            appendEsc(SB, N->appName);
            SB += " • ";
            SB += AGE;
            const auto HEADER = cachedText(SB, COLSUB, T.header, TEXTWPX, -1, 0, true, 500);
            const auto TITLE  = N->summary.empty() ? nullptr : cachedText(N->summary, COLTITLE, T.title, TEXTWPX, -1, 0, true, 600);

            // action labels + icons
            if (NAwesome::Canvas::inst().gate().warming)
                for (auto& A : N->actions)
                    ensureActionIcon(*N, A, (int)std::lround(BTN_ICON * P.scale));
            static std::vector<CBox> btnBoxes; // reused; main thread only
            btnBoxes.clear();
            double btnH = 0;
            {
                double bx = 0, rowY = 0;
                for (const auto& A : N->actions) {
                    auto& LB = scratch();
                    appendEsc(LB, A.label);
                    const auto   LBL = cachedText(LB, COLACC, T.action, TEXTWPX, -1, 0, true, 600);
                    const double LW  = texW(LBL, P.scale);
                    const double IW  = (N->actionIcons && A.iconTex) ? BTN_ICON + BTN_ICON_GAP : 0;
                    const double BW  = std::min(TEXTW, IW + LW + 2 * BTN_PADX);
                    if (bx > 0 && bx + BW > TEXTW + 0.5) {
                        bx = 0;
                        rowY += BTN_H + BTN_GAP;
                    }
                    btnBoxes.push_back(CBox{bx, rowY, BW, BTN_H});
                    bx += BW + BTN_GAP;
                }
                btnH = btnBoxes.empty() ? 0 : rowY + BTN_H;
            }

            if (NAwesome::Canvas::inst().gate().warming)
                for (auto& IM : N->bodyImages)
                    ensureBodyImage(IM, (int)std::lround(BODYIMG_H * P.scale));
            static std::vector<CBox> imgBoxes; // reused; main thread only
            imgBoxes.clear();
            double imgH = 0;
            {
                double bx = 0, rowY = 0;
                for (const auto& IM : N->bodyImages) {
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
            }

            // an <img> that failed to load keeps its alt text as a body line
            // — the sender meant to show something; the words outlive the
            // missing file
            static std::vector<SAltLine> altLines; // reused; main thread only
            altLines.clear();
            for (const auto& IM : N->bodyImages)
                if (!IM.tex && !IM.alt.empty())
                    altLines.push_back(SAltLine{.text = IM.alt});
            for (auto& A : altLines) {
                A.tex = cachedText(A.text, COLSUB, T.body, TEXTWPX, 1, 0, false, 400);
                A.h   = A.tex ? std::max(14.0, texH(A.tex, P.scale)) : 0;
            }

            // max_height fits the blocks by priority: header, title, one body
            // line, progress and the actions first (what the sender asks the
            // user to do), then the images, then their alt lines; the body
            // takes what is left (the cap below). A block that doesn't fit is
            // dropped whole, from the end — painted past the glass once, it
            // covered whatever lay below and took none of its clicks.
            const double HH = texH(HEADER, P.scale), TH = texH(TITLE, P.scale);
            // the banner previews the five newest transcript messages; the
            // shade renders the card's stored full window
            const std::string BODYSRC = N->conversation ? Model::conversationBody(N, Pixel::MAX_PREVIEWED_CONVERSATION_MESSAGES) : N->body;
            const int         LINEPX  = bodyBudgetPx(P.scale, 1);
            double room = (HERO ? MAXH - HERO_TEXT_MIN : MAXH - 2 * PADY) - HH - (HH > 0 ? HEAD_GAP : 0) - TH - TITLE_GAP - (BODYSRC.empty() ? 0 : LINEPX / P.scale) -
                (N->progress >= 0 ? PROGRESS_GAP + PROGRESS_H : 0);
            // whole rows of boxes (y from the block top) while the block fits
            const auto fitRows = [&](std::vector<CBox>& boxes, double gap) {
                while (!boxes.empty() && gap + boxes.back().y + boxes.back().h > room + 0.5)
                    boxes.pop_back();
                const double H = boxes.empty() ? 0 : gap + boxes.back().y + boxes.back().h;
                room -= H;
                return boxes.empty() ? 0.0 : H - gap;
            };
            btnH = fitRows(btnBoxes, BTN_ROW_GAP);
            imgH = fitRows(imgBoxes, IMG_ROW_GAP);
            double altH = 0;
            for (size_t i = 0; i < altLines.size(); i++) {
                if (IMG_ROW_GAP + altH + altLines[i].h > room + 0.5) {
                    altLines.resize(i);
                    break;
                }
                altH += altLines[i].h;
            }
            const double BTN_BLOCK = btnH > 0 ? BTN_ROW_GAP + btnH : 0;
            const double IMG_BLOCK = imgH > 0 ? IMG_ROW_GAP + imgH : 0;
            const double ALT_BLOCK = altH > 0 ? IMG_ROW_GAP + altH : 0;

            // the body cap: at most ~8 lines, and never past what max_height
            // leaves after the other blocks — an uncapped body painted
            // OUTSIDE the glass once actions and thumbnails stacked up (the
            // 02359ed lesson; a one-line floor keeps hostile configs sane)
            const double AVAIL = MAXH - 2 * PADY - (HERO ? HEROH : 0) - HH - (HH > 0 ? HEAD_GAP : 0) - TH - TITLE_GAP - (N->progress >= 0 ? PROGRESS_GAP + PROGRESS_H : 0) -
                BTN_BLOCK - IMG_BLOCK - ALT_BLOCK;
            const int  BODYCAP = std::max(LINEPX, std::min(bodyBudgetPx(P.scale, 8), (int)std::floor(AVAIL * P.scale)));
            const auto BODY    = BODYSRC.empty() ? nullptr : cachedText(BODYSRC, COLBODY, T.body, TEXTWPX, BODYCAP, 1.1f, true, 400, &COLLINK);

            const double BH = texH(BODY, P.scale);
            double       th = HH + (HH > 0 ? HEAD_GAP : 0) + TH + (TH > 0 && BH > 0 ? TITLE_GAP : 0) + BH + IMG_BLOCK + ALT_BLOCK;
            if (N->progress >= 0)
                th += (th > 0 ? PROGRESS_GAP : 0) + PROGRESS_H;
            th += BTN_BLOCK;

            const double CH = HERO ? HEROH + PADY + std::min(th, MAXH - HERO_TEXT_MIN) + PADY : std::min(MAXH, std::max(ICONW, th) + 2 * PADY);

            // per-card arrival motion: fade + an 8px drop. Keyed on `born`,
            // never `arrived` — an OSD replace refreshes arrived every step
            // and must not re-run the spring.
            SPaint      CP = P;
            const float AT = animationsOn() && N->banner ? animT(N->born, Theme::MOTION_SPATIAL) : 1.f;
            if (AT < 1.f) {
                CP.alpha = P.alpha * easeOutCubic(AT);
                CP.dy    = P.dy - (1.0 - easeOutBack(AT)) * 8.0;
            }

            const CBox CARD{X, y, W, CH};
            CP.shadow(CARD, ROUND, RP, 16);
            CP.glass(CARD, COLBG, ROUND, RP);
            if (CRITICAL) // the urgent edge: a hairline ring in the urgent color
                CP.ring(CARD, COLURGENT, ROUND, RP);

            if (HERO)
                CP.texFit(N->iconTex, CBox{X, y, W, HEROH}, ROUND, RP);
            else if (LEADICON)
                paintIconColumn(CP, *N, CBox{X + PADX, y + PADY, ICONW, ICONW}, true, RP);

            const double                 TX = X + PADX + (ICONW > 0 ? ICONW + ICON_GAP : 0);
            double                       ty = HERO ? y + HEROH + PADY : y + PADY;
            std::vector<SCard::SLinkHit> cardLinks;
            if (HEADER)
                CP.tex(HEADER->tex, TX, ty);
            ty += HH + (HH > 0 ? HEAD_GAP : 0);
            if (TITLE)
                CP.tex(TITLE->tex, TX, ty);
            ty += TH + (TH > 0 && BH > 0 ? TITLE_GAP : 0);
            if (BODY) {
                CP.tex(BODY->tex, TX, ty);
                for (const auto& L : BODY->links) // physical -> global logical
                    cardLinks.push_back({CBox{TX + L.rel.x / P.scale, ty + L.rel.y / P.scale, L.rel.w / P.scale, L.rel.h / P.scale}, L.href});
                ty += BH;
            }
            if (!imgBoxes.empty()) {
                ty += IMG_ROW_GAP;
                size_t bi = 0;
                for (const auto& IM : N->bodyImages)
                    if (IM.tex && bi < imgBoxes.size()) {
                        const auto& B = imgBoxes[bi++];
                        CP.texFit(IM.tex, CBox{TX + B.x, ty + B.y, B.w, B.h}, ROUND, RP);
                    }
                ty += imgH;
            }
            if (!altLines.empty()) {
                ty += IMG_ROW_GAP;
                for (const auto& A : altLines)
                    if (A.tex && A.tex->tex) {
                        CP.tex(A.tex->tex, TX, ty + (A.h - texH(A.tex, P.scale)) / 2);
                        ty += A.h;
                    }
            }
            if (N->progress >= 0) {
                ty += th > 0 ? PROGRESS_GAP : 0;
                paintProgress(CP, TX, ty, TEXTW, N->progress, CRITICAL);
                ty += PROGRESS_H;
            }

            // actions: borderless tinted text buttons, labels aligned to the
            // content column (the -BTN_PADX optical pull)
            std::vector<SCard::SBtn> cardBtns;
            if (!btnBoxes.empty()) {
                ty += BTN_ROW_GAP;
                const double BX0 = TX - BTN_PADX;
                for (size_t i = 0; i < btnBoxes.size(); i++) {
                    const auto& A = N->actions[i];
                    const CBox  BOX{BX0 + btnBoxes[i].x, ty + btnBoxes[i].y, btnBoxes[i].w, btnBoxes[i].h};
                    const bool  BHOV = hovered.kind == SCard::POPUP && hovered.id == N->id && hovered.btn == (int)i;
                    if (BHOV)
                        CP.rect(BOX, tAccentDim(), (int)std::lround(BTN_H / 2 * P.scale));
                    double cx = BOX.x + BTN_PADX;
                    if (N->actionIcons && A.iconTex) {
                        CP.texFit(A.iconTex, CBox{cx, BOX.y + (BOX.h - BTN_ICON) / 2, BTN_ICON, BTN_ICON}, 0);
                        cx += BTN_ICON + BTN_ICON_GAP;
                    }
                    auto& LB = scratch();
                    appendEsc(LB, A.label);
                    const auto LBL = cachedText(LB, COLACC, T.action, TEXTWPX, -1, 0, true, 600);
                    if (LBL && LBL->tex)
                        CP.tex(LBL->tex, cx, BOX.y + (BOX.h - LBL->tex->m_size.y / P.scale) / 2);
                    cardBtns.push_back({BOX, A.id});
                }
            }

            SCard card;
            card.kind    = SCard::POPUP;
            card.box     = CARD;
            card.id      = N->id;
            card.buttons = std::move(cardBtns);
            card.links   = std::move(cardLinks);

            // the hover-✕ (the desktop analog of swipe), revealed while the
            // pointer is on the card; its glyph builds in both modes
            const auto XG      = cachedText("✕", COLFG, T.small, 64, -1, 0, false, 600);
            const auto XGHOT   = cachedText("✕", tOnAccent(), T.small, 64, -1, 0, false, 600);
            const bool CARDHOV = hovered.kind == SCard::POPUP && hovered.id == N->id;
            if (CARDHOV) {
                const CBox XB{X + W - XCIRC - 8, y + 8, XCIRC, XCIRC};
                const bool XHOV = hovered.part == 2;
                CP.rect(XB, XHOV ? COLURGENT : tFill2(), (int)std::lround(XCIRC / 2 * P.scale));
                const auto* G = XHOV ? XGHOT : XG;
                if (G && G->tex)
                    CP.tex(G->tex, XB.x + (XB.w - G->tex->m_size.x / P.scale) / 2, XB.y + (XB.h - G->tex->m_size.y / P.scale) / 2);
                card.close = XB;
            }

            cards.push_back(std::move(card));
            y += CH + GAP;
        }

        lastContentH = std::max(0.0, y - GAP - (MB.y + (double)NAwesome::cfg().getI("plugin:awesome:notify:offset_y")));
        lastContentW = W;
    }

} // namespace NAwesome::Notify
