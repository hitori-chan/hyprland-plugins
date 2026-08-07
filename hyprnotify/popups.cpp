// hyprnotify/popups.cpp — the banner column: glass cards top-right on the
// focused monitor, the one-card anatomy (icon column + header/title/body +
// original actions), the arrival spring.
//
// Only cards whose banner is up show here (residency hides expired banners
// into the center's shade). Ordinary banners yield to the panel while it is
// open; OSD-band cards keep this same card anatomy and are placed below the
// open shade with the configured inter-card gap.

#include "ui.hpp"

namespace NHyprnotify {

    bool popupsAnimating(bool osdOnly) {
        if (!animationsOn())
            return false;
        for (const auto& N : notifs)
            if ((!osdOnly || inOsdBand(N->id)) && !N->waiting && N->banner && animT(N->born, Theme::MOTION_SPATIAL) < 1.f)
                return true;
        return false;
    }

    double renderPopups(const SPaint& P, const SType& T, bool osdOnly, std::optional<double> startY, bool measureOnly) {
        const auto   MB      = P.mon->logicalBox();
        const double W       = std::max(1.0, std::min(std::max((double)cfg.width->value(), 120.0), std::max(1.0, MB.w - 2 * EDGE)));
        const double MAXH    = std::max((double)cfg.maxHeight->value(), 60.0);
        const double GAP     = std::max((double)cfg.margin->value(), 0.0);
        const double MAXICON = std::clamp((double)cfg.maxIcon->value(), 16.0, 64.0);
        const int    ROUND   = std::max(0, (int)std::lround(cfg.rounding->value() * P.scale));
        const float  RP      = (float)cfg.roundingPower->value();

        const auto   COLBG = color(cfg.colBg), COLFG = color(cfg.colFg), COLTITLE = color(cfg.colTitle), COLSUB = color(cfg.colKicker), COLURGENT = color(cfg.colUrgent),
                   COLACC = color(cfg.colHighlight), COLLINK = color(cfg.colLink);
        const CHyprColor COLBODY = COLFG.modifyA(COLFG.a * 0.92);

        const double X     = MB.x + MB.w - EDGE - W;
        const double START = startY.value_or(MB.y + std::clamp((double)cfg.offsetY->value(), 0.0, std::max(0.0, MB.h - 2 * PADY)));
        double       y     = START;

        for (const auto& N : notifs) {
            if (osdOnly && !inOsdBand(N->id))
                continue;
            if (N->waiting || !N->banner)
                continue; // residency: only banners show as popups
            if (y + 2 * PADY > MB.y + MB.h)
                break; // no room: the tail waits off-screen, timeouts running
            const double CARDH = std::min(MAXH, std::max(2 * PADY, MB.y + MB.h - y - 2 * PADY));

            const bool CRITICAL = N->urgency >= 2;
            const auto AGE      = ageString(N->arrived);

            // every build in the drawing units gates on warmGate.warming, never
            // on P.warm — a measuring pass forces P.warm on to paint nothing
            if (warmGate.warming)
                ensureIconTex(*N, (int)std::lround(MAXICON * P.scale), (int)std::lround(W * P.scale), (int)std::lround(HERO_CAP * P.scale));

            const bool   HERO  = N->iconTex && N->heroTex;
            const double HEROH = HERO ? std::min(N->iconTex->m_size.y / P.scale, std::max(0.0, CARDH - 2 * PADY)) : 0;

            // Conversations use one avatar-plus-badge column. Ordinary cards
            // keep app identity on the left and distinct non-wide content in a
            // right preview; wide content keeps the dedicated hero layout.
            const bool   HASIDENT = N->identTex && N->identTex->m_texID != 0;
            const bool   CONTENT  = N->iconTex && N->iconTex->m_texID != 0 && !N->heroTex;
            const bool   LEADICON = !HERO && hasLeadIcon(*N);
            const double ICONW    = LEADICON ? MAXICON : 0;
            const double PREVIEWCAP = W - 2 * PADX - ICONW - 2 * ICON_GAP - 80;
            const bool   RTHUMB     = !HERO && !N->conversation && CONTENT && HASIDENT && PREVIEWCAP >= 16;
            const double THUMBW     = RTHUMB ? std::min(MAXICON, PREVIEWCAP) : 0;

            const double TEXTW   = W - 2 * PADX - (ICONW > 0 ? ICONW + ICON_GAP : 0) - (THUMBW > 0 ? THUMBW + ICON_GAP : 0);
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
            if (warmGate.warming)
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

            if (warmGate.warming)
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
            const double AVAIL = CARDH - 2 * PADY - (HERO ? HEROH : 0) - HH - (HH > 0 ? HEAD_GAP : 0) - TH - TITLE_GAP - (N->progress >= 0 ? PROGRESS_GAP + PROGRESS_H : 0) -
                BTN_BLOCK - IMG_BLOCK;
            const int  LINEPX  = (int)std::lround(T.body * 1.35);
            const int  BODYCAP  = std::min(LINEPX * 8, std::max(0, (int)std::floor(AVAIL * P.scale)));
            const auto& BODYTEXT = bodyForDisplay(*N);
            const auto BODY     = BODYTEXT.empty() || BODYCAP < LINEPX ? nullptr : cachedText(BODYTEXT, COLBODY, T.body, TEXTWPX, BODYCAP, 1.1f, true, 400, &COLLINK);

            const double BH = texH(BODY, P.scale);
            double       th = HH + (HH > 0 ? HEAD_GAP : 0) + TH + (TH > 0 && BH > 0 ? TITLE_GAP : 0) + BH + IMG_BLOCK;
            if (N->progress >= 0)
                th += (th > 0 ? PROGRESS_GAP : 0) + PROGRESS_H;
            th += BTN_BLOCK;

            const double CH = HERO ? HEROH + PADY + std::min(th, std::max(0.0, CARDH - HERO_TEXT_MIN)) + PADY : std::min(CARDH, std::max(ICONW, th) + 2 * PADY);

            // per-card arrival motion: fade + an 8px drop. Keyed on `born`,
            // never `arrived` — an OSD replace refreshes arrived every step
            // and must not re-run the spring.
            SPaint      CP = P;
            const float AT = animationsOn() && N->banner ? animT(N->born, Theme::MOTION_SPATIAL) : 1.f;
            if (AT < 1.f) {
                CP.alpha = P.alpha * easeOutCubic(AT);
                const double DROP = (1.0 - easeOutBack(AT)) * 8.0;
                // A below-shade card must not animate through the panel. Keep
                // the normal spring for ordinary banners, but cap its upward
                // excursion at the gap reserved by the placement.
                CP.dy = P.dy - (startY ? std::min(DROP, GAP) : DROP);
            }

            const CBox CARD{X, y, W, CH};
            CP.shadow(CARD, ROUND, RP, 16);
            CP.glass(CARD, COLBG, ROUND, RP);
            if (CRITICAL) // the urgent edge: a hairline ring in the urgent color
                CP.ring(CARD, COLURGENT, ROUND, RP);

            if (HERO)
                CP.texFit(N->iconTex, CBox{X, y, W, HEROH}, ROUND, RP);
            else if (LEADICON)
                paintIconColumn(CP, *N, CBox{X + PADX, y + PADY, ICONW, ICONW}, N->conversation, RP);
            if (RTHUMB)
                CP.texFit(N->iconTex, CBox{X + W - PADX - THUMBW, y + PADY, THUMBW, THUMBW}, (int)std::lround(THUMBW * 10.0 / 44.0 * P.scale), RP);

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

            if (!measureOnly)
                cards.push_back(std::move(card));
            y += CH + GAP;
        }

        const double CONTENTH = std::max(0.0, y - GAP - START);
        if (osdOnly) {
            // The pass box starts at offset_y. Include the shade and the OSD
            // stack below it so the renderer damages both regions together.
            const double BASE = MB.y + (double)cfg.offsetY->value();
            lastContentH       = std::max(lastContentH, std::max(0.0, START + CONTENTH - BASE));
            lastContentW       = std::max(lastContentW, W);
        } else {
            lastContentH = CONTENTH;
            lastContentW = W;
        }
        return CONTENTH;
    }

} // namespace NHyprnotify
