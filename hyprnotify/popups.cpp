// hyprnotify/popups.cpp — the banner column: glass cards top-right on the
// focused monitor, the one-card anatomy (icon column + header/title/body +
// original actions), the hover-✕, the arrival spring.
//
// Only cards whose banner is up show here (residency hides expired banners
// into the center's shade); while the center is open the column yields to
// the panel entirely — render.cpp picks the surface.

#include "ui.hpp"

namespace NHyprnotify {

    bool popupsAnimating() {
        if (!animationsOn())
            return false;
        for (const auto& N : notifs)
            if (!N->waiting && N->banner && animT(N->born, Theme::MOTION_SPATIAL) < 1.f)
                return true;
        return false;
    }

    void renderPopups(const SPaint& P, const SType& T) {
        const auto   MB      = P.mon->logicalBox();
        const double W       = std::max((double)cfg.width->value(), 120.0);
        const double MAXH    = std::max((double)cfg.maxHeight->value(), 60.0);
        const double GAP     = std::max((double)cfg.margin->value(), 0.0);
        const double MAXICON = std::clamp((double)cfg.maxIcon->value(), 16.0, 64.0);
        const int    ROUND   = std::max(0, (int)std::lround(cfg.rounding->value() * P.scale));
        const float  RP      = (float)cfg.roundingPower->value();

        const auto   COLBG = color(cfg.colBg), COLFG = color(cfg.colFg), COLTITLE = color(cfg.colTitle), COLSUB = color(cfg.colKicker), COLURGENT = color(cfg.colUrgent),
                   COLACC = color(cfg.colHighlight), COLLINK = color(cfg.colLink);
        const CHyprColor COLBODY = COLFG.modifyA(COLFG.a * 0.92);

        const double     X = MB.x + MB.w - EDGE - W;
        double           y = MB.y + (double)cfg.offsetY->value();

        for (const auto& N : notifs) {
            if (N->waiting || !N->banner)
                continue; // residency: only banners show as popups
            if (y + 2 * PADY > MB.y + MB.h)
                break; // no room: the tail waits off-screen, timeouts running

            const bool CRITICAL = N->urgency >= 2;
            const auto AGE      = ageString(N->arrived);

            if (P.warm)
                ensureIconTex(*N, (int)std::lround(MAXICON * P.scale), (int)std::lround(W * P.scale), (int)std::lround(HERO_CAP * P.scale));

            const bool   HERO  = N->iconTex && N->heroTex;
            const double HEROH = HERO ? N->iconTex->m_size.y / P.scale : 0;

            // the icon column: content avatar first (identity as the corner
            // badge when both exist and differ), identity alone otherwise,
            // none = text-only
            const bool   HASCONTENT = N->iconTex && !HERO;
            const bool   HASIDENT   = N->identTex && N->identTex->m_texID != 0;
            const bool   LEADICON   = HASCONTENT || HASIDENT;
            const bool   WITHBADGE  = HASCONTENT && HASIDENT && (N->hasPixels || N->image != N->identity);
            const double ICONW      = LEADICON ? MAXICON : 0;

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
            if (P.warm)
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
            const double BTN_BLOCK = btnH > 0 ? BTN_ROW_GAP + btnH : 0;

            if (P.warm)
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
            const double IMG_BLOCK = imgH > 0 ? IMG_ROW_GAP + imgH : 0;

            // the body cap: at most ~8 lines, and never past what max_height
            // leaves after the other blocks — an uncapped body painted
            // OUTSIDE the glass once actions and thumbnails stacked up (the
            // 02359ed lesson; a one-line floor keeps hostile configs sane)
            const double HH = texH(HEADER, P.scale), TH = texH(TITLE, P.scale);
            const double AVAIL = MAXH - 2 * PADY - (HERO ? HEROH : 0) - HH - (HH > 0 ? HEAD_GAP : 0) - TH - TITLE_GAP - (N->progress >= 0 ? PROGRESS_GAP + PROGRESS_H : 0) -
                BTN_BLOCK - IMG_BLOCK;
            const int  LINEPX  = (int)std::lround(T.body * 1.35);
            const int  BODYCAP = std::max(LINEPX, std::min(LINEPX * 8, (int)std::floor(AVAIL * P.scale)));
            const auto BODY    = N->body.empty() ? nullptr : cachedText(N->body, COLBODY, T.body, TEXTWPX, BODYCAP, 1.1f, true, 400, &COLLINK);

            const double BH = texH(BODY, P.scale);
            double       th = HH + (HH > 0 ? HEAD_GAP : 0) + TH + (TH > 0 && BH > 0 ? TITLE_GAP : 0) + BH + IMG_BLOCK;
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
            else if (LEADICON) {
                const auto& LEAD = HASCONTENT ? N->iconTex : N->identTex;
                const CBox  IB{X + PADX, y + PADY, ICONW, ICONW};
                const int   IR = (int)std::lround(ICONW * 10.0 / 44.0 * P.scale);
                CP.texFit(LEAD, IB, IR, RP);
                if (WITHBADGE) { // the identity corner badge, bottom-right, ringed in the glass color
                    const CBox BB{IB.x + IB.w - BADGE + 2, IB.y + IB.h - BADGE + 2, BADGE, BADGE};
                    CP.rect(CBox{BB}.expand(1.5), COLBG.modifyA(1.0), (int)std::lround((BADGE / 2 + 1.5) * P.scale), 2.f);
                    CP.texFit(N->identTex, BB, (int)std::lround(BADGE / 2 * P.scale), 2.f);
                }
            }

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
            if (N->progress >= 0) {
                ty += th > 0 ? PROGRESS_GAP : 0;
                const double FILLW = TEXTW * N->progress / 100.0;
                const int    PR    = (int)std::lround(PROGRESS_H / 2 * P.scale);
                CP.rect(CBox{TX, ty, TEXTW, PROGRESS_H}, tFill2(), PR);
                if (N->progress > 0)
                    CP.rect(CBox{TX, ty, std::max(FILLW, PROGRESS_H), PROGRESS_H}, CRITICAL ? COLURGENT : COLACC, PR);
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

        lastContentH = std::max(0.0, y - GAP - (MB.y + (double)cfg.offsetY->value()));
        lastContentW = W;
    }

} // namespace NHyprnotify
