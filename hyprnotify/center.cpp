// hyprnotify/center.cpp — the two-view notification center: the shade
// (resident live cards, unlabeled) and history behind the ⏱ button. Every
// row is the same two-state card; ≥2 same-app rows fold into the three-state
// group model (digest → 1-line children → child expanded) in BOTH views.
// The bottom bar is ⏱ · a context-sensitive Clear button · ⊖ DND.
//
// This unit owns the center's transient state (view, paging, every fold) —
// all of it resets when the center closes, Android-style.

#include "ui.hpp"

namespace NHyprnotify {

    // ---- state ----

    static bool                  s_on       = false;
    static bool                  s_viewHist = false;
    static size_t                s_skip     = 0; // wheel paging: top-level items skipped
    static size_t                s_items    = 0; // items the last layout had (clamps paging)
    static std::set<std::string> s_openShade, s_openHist; // expanded groups, per view
    static std::set<uint32_t>    s_openedLive, s_foldedLive; // chevron overrides for live rows
    static std::set<uint64_t>    s_histOpen;                 // expanded history rows/children
    static Time::steady_tp       s_openedAt;
    static bool                  s_animating = false;

    bool                         centerVisible() {
        return s_on;
    }
    bool centerInHistory() {
        return s_viewHist;
    }
    bool centerAnimating() {
        return s_animating;
    }

    static bool liveRowOpen(const SP<SNotif>& n) {
        // live arrives expanded and auto-folds when its banner expires; the
        // chevron overrides both ways
        return s_openedLive.contains(n->id) || (n->banner && !s_foldedLive.contains(n->id));
    }

    bool centerRowOpen(uint32_t id, uint64_t hseq) {
        if (hseq)
            return s_histOpen.contains(hseq);
        for (const auto& N : notifs)
            if (N->id == id)
                return liveRowOpen(N);
        return false;
    }

    void centerToggleRow(uint32_t id, uint64_t hseq) {
        if (hseq) {
            if (!s_histOpen.erase(hseq))
                s_histOpen.insert(hseq);
        } else {
            if (centerRowOpen(id, 0)) {
                s_openedLive.erase(id);
                s_foldedLive.insert(id);
            } else {
                s_foldedLive.erase(id);
                s_openedLive.insert(id);
            }
        }
        notifChanged();
    }

    void centerToggleGroup(const std::string& appKey, bool hist) {
        auto& SET = hist ? s_openHist : s_openShade;
        if (!SET.erase(appKey))
            SET.insert(appKey);
        notifChanged();
    }

    void centerFlipView() {
        s_viewHist = !s_viewHist;
        s_skip     = 0;
        notifChanged();
    }

    void centerPage(int dir) {
        if (s_items <= 1)
            return;
        s_skip = (size_t)std::clamp((int64_t)s_skip + dir, (int64_t)0, (int64_t)(s_items - 1));
        notifChanged();
    }

    void setCenter(bool on) {
        if (on == s_on)
            return;
        s_on = on;
        if (!on) {
            // Android parity: the view and every fold reset on close
            s_viewHist = false;
            s_skip     = 0;
            s_openShade.clear();
            s_openHist.clear();
            s_openedLive.clear();
            s_foldedLive.clear();
            s_histOpen.clear();
            s_animating = false;
        } else if (animationsOn()) {
            s_openedAt  = Time::steadyNow();
            s_animating = true;
        }
        notifChanged();
        Bus::emitStateSoon();
    }

    // ---- the display list (both views share the grouping model) ----

    struct SDisp {
        std::vector<SP<SNotif>> items; // newest first; 1 = a bare row
        std::string             key;   // the app key (groups)
    };

    static void buildDisplay(std::vector<SDisp>& out, bool hist) {
        out.clear();
        static std::vector<SP<SNotif>> src; // reused; main thread only
        src.clear();
        if (hist) {
            const auto& H = Bus::historyView(); // oldest first
            for (size_t i = H.size(); i-- > 0;)
                src.push_back(H[i]);
        } else {
            for (const auto& N : notifs)
                if (!N->waiting)
                    src.push_back(N);
        }
        // fold >= 2 same-app entries; the OSD band never groups
        static std::map<std::string, size_t> firstOf; // app key -> out index; reused
        firstOf.clear();
        for (const auto& N : src) {
            if (!inOsdBand(N->id) && !N->appKey.empty()) {
                if (const auto IT = firstOf.find(N->appKey); IT != firstOf.end()) {
                    out[IT->second].items.push_back(N);
                    continue;
                }
                firstOf[N->appKey] = out.size();
            }
            out.push_back(SDisp{.items = {N}, .key = N->appKey});
        }
        src.clear();
    }

    // ---- one row, two states (singles and group children share it) ----

    struct SRowStyle {
        double iconPx;       // 34 rows, 28 children
        bool   withBadge;    // children ride plain avatars — the header owns identity
        bool   headerHasApp; // singles: "App • age"; children: age only
    };
    static constexpr SRowStyle ROW_SINGLE{ROW_ICON, true, true};
    static constexpr SRowStyle ROW_CHILD{CHILD_ICON, false, false};

    // Lays out (and in draw mode paints) one row at box.x/box.y with box.w;
    // returns the row height and fills the card's hit boxes.
    static double renderRow(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box, bool open, bool hist, const SRowStyle& ST, SCard& card, bool child) {
        const auto COLFG = color(cfg.colFg), COLTITLE = color(cfg.colTitle), COLSUB = color(cfg.colKicker), COLACC = color(cfg.colHighlight), COLURGENT = color(cfg.colUrgent);
        const CHyprColor COLBODY = COLFG.modifyA(COLFG.a * 0.92);
        const auto       AGE     = ageString(N->arrived);
        const auto       SUBHEX  = hexOf(COLSUB);
        const float      RP      = (float)cfg.roundingPower->value();

        if (P.warm)
            ensureIconTex(*N, (int)std::lround(std::max(ST.iconPx, (double)cfg.maxIcon->value()) * P.scale), 0, 0);

        const bool   HASCONTENT = N->iconTex && !N->heroTex;
        const bool   HASIDENT   = N->identTex && N->identTex->m_texID != 0;
        const auto&  LEAD       = HASCONTENT ? N->iconTex : N->identTex;
        const bool   LEADICON   = HASCONTENT || HASIDENT;
        const bool   WITHBADGE  = ST.withBadge && HASCONTENT && HASIDENT && (N->hasPixels || N->image != N->identity);

        const double ICONW   = LEADICON ? ST.iconPx : 0;
        const double TX      = box.x + ROW_PADX + (ICONW > 0 ? ICONW + ROW_ICON_GAP : 0);
        const double TEXTW   = box.x + box.w - ROW_PADX - CHEV - 8 - TX;
        const int    TEXTWPX = std::max(1, (int)std::floor(TEXTW * P.scale));

        double       th = 0;
        const double TY = box.y + ROW_PADT;

        if (!open) {
            // collapsed: bold "title • age" + the newest body line (+progress)
            const auto LINE = cachedText(N->summary + " <span foreground=\"" + SUBHEX + "\">• " + AGE + "</span>", COLTITLE, T.title, TEXTWPX, -1, 0, true, 600);
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
                    const int PR = (int)std::lround(PROGRESS_H / 2 * P.scale);
                    P.rect(CBox{TX, yy, TEXTW, PROGRESS_H}, tFill2(), PR);
                    if (N->progress > 0)
                        P.rect(CBox{TX, yy, std::max(TEXTW * N->progress / 100.0, PROGRESS_H), PROGRESS_H}, N->urgency >= 2 ? COLURGENT : COLACC, PR);
                }
            }
        } else {
            // expanded: age/header line, title, 4-line body, progress, the
            // card's ORIGINAL actions — live and history alike
            const auto KICK  = cachedText(ST.headerHasApp ? esc(N->appName) + " • " + AGE : AGE, COLSUB, T.header, TEXTWPX, -1, 0, true, 500);
            const auto TITLE = N->summary.empty() ? nullptr : cachedText(N->summary, COLTITLE, T.title, TEXTWPX, -1, 0, true, 600);
            const int  CAP4  = (int)std::lround(T.body * 1.35 * 4);
            const auto BODY  = N->body.empty() ? nullptr : cachedText(N->body, COLBODY, T.body, TEXTWPX, CAP4, 1.1f, true, 400);

            static std::vector<CBox>               btnBoxes; // reused; main thread only
            static std::vector<const SCachedText*> btnLbls;
            btnBoxes.clear();
            btnLbls.clear();
            double btnH = 0;
            {
                double bx = 0, rowY = 0;
                for (const auto& A : N->actions) {
                    const auto   LBL = cachedText(esc(A.label), COLACC, T.action, TEXTWPX, -1, 0, true, 600);
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
                (btnH > 0 ? BTN_ROW_GAP + btnH : 0);

            double yy = TY;
            if (!P.warm && KICK)
                P.tex(KICK->tex, TX, yy);
            yy += KH + (KH > 0 ? HEAD_GAP : 0);
            if (!P.warm && TITLE)
                P.tex(TITLE->tex, TX, yy);
            yy += TH + (TH > 0 && BH > 0 ? TITLE_GAP : 0);
            if (!P.warm && BODY)
                P.tex(BODY->tex, TX, yy);
            yy += BH;
            if (N->progress >= 0) {
                yy += PROGRESS_GAP;
                if (!P.warm) {
                    const int PR = (int)std::lround(PROGRESS_H / 2 * P.scale);
                    P.rect(CBox{TX, yy, TEXTW, PROGRESS_H}, tFill2(), PR);
                    if (N->progress > 0)
                        P.rect(CBox{TX, yy, std::max(TEXTW * N->progress / 100.0, PROGRESS_H), PROGRESS_H}, N->urgency >= 2 ? COLURGENT : COLACC, PR);
                }
                yy += PROGRESS_H;
            }
            if (btnH > 0) {
                yy += BTN_ROW_GAP;
                const double BX0 = TX - BTN_PADX; // optical: labels align to the content column
                for (size_t i = 0; i < btnBoxes.size(); i++) {
                    const CBox BOX{BX0 + btnBoxes[i].x, yy + btnBoxes[i].y, btnBoxes[i].w, btnBoxes[i].h};
                    if (!P.warm) {
                        const bool BHOV = hovered.id == (N->hseq ? 0 : N->id) && hovered.hseq == N->hseq && hovered.btn == (int)i;
                        if (BHOV)
                            P.rect(BOX, tAccentDim(), (int)std::lround(BTN_H / 2 * P.scale));
                        if (btnLbls[i] && btnLbls[i]->tex)
                            P.tex(btnLbls[i]->tex, BOX.x + BTN_PADX, BOX.y + (BOX.h - btnLbls[i]->tex->m_size.y / P.scale) / 2);
                    }
                    card.buttons.push_back({BOX, N->actions[i].id});
                }
            }
        }

        const double ROWH = std::max(th, ICONW) + ROW_PADT + ROW_PADB;

        if (!P.warm && LEADICON) {
            // collapsed rows center the icon; expanded top-pin it
            const double IY = open ? box.y + ROW_PADT : box.y + (ROWH - ICONW) / 2;
            const CBox   IB{box.x + ROW_PADX, IY, ICONW, ICONW};
            P.texFit(LEAD, IB, (int)std::lround(ICONW * 10.0 / 44.0 * P.scale), RP);
            if (WITHBADGE) {
                const CBox BB{IB.x + IB.w - BADGE + 2, IB.y + IB.h - BADGE + 2, BADGE, BADGE};
                P.rect(CBox{BB}.expand(1.5), color(cfg.colBg).modifyA(1.0), (int)std::lround((BADGE / 2 + 1.5) * P.scale), 2.f);
                P.texFit(N->identTex, BB, (int)std::lround(BADGE / 2 * P.scale), 2.f);
            }
        }

        // the chevron circle: collapsed centers, expanded pins to the top
        {
            const double CY = open ? box.y + ROW_PADT : box.y + (ROWH - CHEV) / 2;
            const CBox   CB{box.x + box.w - ROW_PADX - CHEV, CY, CHEV, CHEV};
            const auto   G = cachedText(open ? "˄" : "˅", COLFG, T.small, 64, -1, 0, false, 600); // built in BOTH modes
            if (!P.warm) {
                const bool CHOV = hovered.id == (N->hseq ? 0 : N->id) && hovered.hseq == N->hseq && hovered.part == 1 && hovered.btn < 0;
                P.rect(CB, CHOV ? tAccentDim() : tFill2(), (int)std::lround(CHEV / 2 * P.scale));
                if (G && G->tex)
                    P.tex(G->tex, CB.x + (CB.w - G->tex->m_size.x / P.scale) / 2, CB.y + (CB.h - G->tex->m_size.y / P.scale) / 2);
            }
            card.chevron = CB;
        }

        card.box  = CBox{box.x, box.y, box.w, ROWH};
        card.id   = N->hseq ? 0 : N->id;
        card.hseq = N->hseq;
        card.hist = hist;
        card.kind = child ? SCard::CHILD : SCard::ROW;
        return ROWH;
    }

    // measure without painting: same code, a paint context that draws nothing
    // (cachedText still resolves through the real warm gate)
    static double measureRow(const SPaint& P, const SType& T, const SP<SNotif>& N, double w, bool open, bool hist, const SRowStyle& ST) {
        SPaint MP = P;
        MP.warm   = true;
        SCard scratch;
        return renderRow(MP, T, N, CBox{0, 0, w, 0}, open, hist, ST, scratch, false);
    }

    // ---- the panel ----

    void renderCenter(const SPaint& PIN, const SType& T) {
        SPaint P = PIN;
        if (s_animating) { // the open spring: fade + a 6px rise
            const float AT = animT(s_openedAt, Theme::MOTION_SPATIAL);
            if (AT >= 1.f)
                s_animating = false;
            else {
                P.alpha *= easeOutCubic(AT);
                P.dy -= (1.0 - easeOutBack(AT)) * 6.0;
            }
        }

        const auto  MB     = P.mon->logicalBox();
        const int   RCARD  = std::max(0, (int)cfg.rounding->value());
        const int   RPANEL = (int)std::lround((RCARD + 6) * P.scale);
        const int   RROW   = std::max(0, (int)std::lround((RCARD - 2) * P.scale));
        const float RP     = (float)cfg.roundingPower->value();

        const auto  COLBG = color(cfg.colBg), COLFG = color(cfg.colFg), COLTITLE = color(cfg.colTitle), COLSUB = color(cfg.colKicker), COLACC = color(cfg.colHighlight),
                   COLURGENT = color(cfg.colUrgent);
        const auto SUBHEX    = hexOf(COLSUB);

        const double X  = MB.x + MB.w - EDGE - CENTER_W;
        const double Y0 = MB.y + (double)cfg.offsetY->value();

        static std::vector<SDisp> disp; // reused; main thread only
        buildDisplay(disp, s_viewHist);
        s_items = disp.size();
        s_skip  = disp.empty() ? 0 : std::min(s_skip, disp.size() - 1);

        const double CONTENT_X = X + BODY_PADX, CONTENT_W = CENTER_W - 2 * BODY_PADX;

        const double HEADH   = s_viewHist ? HEAD_PADT + (double)T.title / P.scale + HEAD_PADB : 0;
        const double BAR_H   = BAR_PADT + BAR_BTN + BAR_PADB;
        const double BODYCAP = CENTER_MAXH - HEADH - BAR_H - BODY_PADT - BODY_PADB;

        const auto&  OPENSET = s_viewHist ? s_openHist : s_openShade;
        const auto   itemOpen = [&](const SP<SNotif>& N) { return s_viewHist ? s_histOpen.contains(N->hseq) : liveRowOpen(N); };

        const auto   measureItem = [&](const SDisp& D) -> double {
            if (D.items.size() < 2)
                return measureRow(P, T, D.items.front(), CONTENT_W, itemOpen(D.items.front()), s_viewHist, ROW_SINGLE);
            if (!OPENSET.contains(D.key)) { // digest: icon line + <=2 preview lines
                double       h    = ROW_PADT + std::max(ROW_ICON, (double)T.title / P.scale + 2);
                const size_t PREV = std::min<size_t>(2, D.items.size());
                h += PREV * ((double)T.body / P.scale * 1.35 + 3) + ROW_PADB;
                return h;
            }
            double h = ROW_PADT + CHILD_ICON + ROW_PADB; // the header row
            for (const auto& N : D.items)
                h += CHILD_GAP + measureRow(P, T, N, CONTENT_W, itemOpen(N), s_viewHist, ROW_CHILD);
            return h;
        };

        struct SPlaced {
            size_t idx;
            double h;
        };
        static std::vector<SPlaced> placed; // reused
        placed.clear();
        double usedH = 0;
        for (size_t i = s_skip; i < disp.size(); i++) {
            const double H = measureItem(disp[i]);
            if (!placed.empty() && usedH + ROW_GAP + H > BODYCAP)
                break;
            usedH += (placed.empty() ? 0 : ROW_GAP) + H;
            placed.push_back({i, H});
        }

        // the panel always contains what it drew: a lone oversized item can
        // push past the soft cap rather than paint outside the glass
        const bool   EMPTY  = disp.empty();
        const double EMPTYH = 46;
        const double BODYH  = EMPTY ? EMPTYH : usedH;
        const double PANELH = HEADH + BODY_PADT + BODYH + BODY_PADB + BAR_H;
        const CBox   PANEL{X, Y0, CENTER_W, PANELH};

        P.shadow(PANEL, RPANEL, RP, 22);
        P.glass(PANEL, COLBG, RPANEL, RP);
        {
            SCard pc;
            pc.kind = SCard::PANEL;
            pc.box  = PANEL;
            cards.push_back(pc);
        }

        double y = Y0;
        if (s_viewHist) {
            const auto HT = cachedText("History", COLTITLE, T.title, (int)(CENTER_W * P.scale), -1, 0, false, 600);
            if (!P.warm && HT)
                P.tex(HT->tex, X + HEAD_PADX, Y0 + HEAD_PADT);
            y += HEADH;
        }
        y += BODY_PADT;

        if (EMPTY) {
            const auto E = cachedText(s_viewHist ? "No history" : "You're all caught up!", COLSUB, T.body, (int)(CENTER_W * P.scale), -1, 0, false, 500);
            if (!P.warm && E && E->tex)
                P.tex(E->tex, X + (CENTER_W - E->tex->m_size.x / P.scale) / 2, y + (EMPTYH - E->tex->m_size.y / P.scale) / 2);
            y += EMPTYH;
        }

        // glyphs the bottom bar and group headers reuse — request every warm
        const auto XG    = cachedText("✕", COLFG, T.small, 64, -1, 0, false, 600);
        const auto XGHOT = cachedText("✕", tOnAccent(), T.small, 64, -1, 0, false, 600);

        for (const auto& [IDX, IH] : placed) {
            const auto& D = disp[IDX];

            if (D.items.size() < 2) {
                // ---- a bare row ----
                const auto& N   = D.items.front();
                const bool  HOV = hovered.kind == SCard::ROW && hovered.id == (N->hseq ? 0 : N->id) && hovered.hseq == N->hseq && hovered.btn < 0 && hovered.part == 0;
                P.rect(CBox{CONTENT_X, y, CONTENT_W, IH}, HOV ? tAccentDim() : tFill(), RROW, RP);
                SCard card;
                renderRow(P, T, N, CBox{CONTENT_X, y, CONTENT_W, 0}, itemOpen(N), s_viewHist, ROW_SINGLE, card, false);
                cards.push_back(std::move(card));
            } else if (!OPENSET.contains(D.key)) {
                // ---- state 1: the digest card ----
                const auto& NEWEST = D.items.front();
                const bool  HOV    = hovered.kind == SCard::DIGEST && hovered.group == D.key;
                P.rect(CBox{CONTENT_X, y, CONTENT_W, IH}, HOV ? tAccentDim() : tFill(), RROW, RP);

                if (P.warm)
                    ensureIconTex(*NEWEST, (int)std::lround(cfg.maxIcon->value() * P.scale), 0, 0);
                const auto& IDT = NEWEST->identTex && NEWEST->identTex->m_texID ? NEWEST->identTex : NEWEST->iconTex;
                if (IDT)
                    P.texFit(IDT, CBox{CONTENT_X + ROW_PADX, y + ROW_PADT, ROW_ICON, ROW_ICON}, (int)std::lround(ROW_ICON * 10.0 / 44.0 * P.scale), RP);

                const double TX    = CONTENT_X + ROW_PADX + ROW_ICON + ROW_ICON_GAP;
                const auto   PILL  = cachedText(std::to_string(D.items.size()) + " ˅", COLFG, T.small, 64, -1, 0, false, 600);
                const double PILLW = texW(PILL, P.scale) + 14;
                const CBox   PB{CONTENT_X + CONTENT_W - ROW_PADX - PILLW, y + ROW_PADT + (ROW_ICON - PILL_H) / 2, PILLW, PILL_H};
                if (!P.warm) {
                    P.rect(PB, HOV ? tAccentDim() : tFill2(), (int)std::lround(PILL_H / 2 * P.scale));
                    if (PILL && PILL->tex)
                        P.tex(PILL->tex, PB.x + (PB.w - PILL->tex->m_size.x / P.scale) / 2, PB.y + (PB.h - PILL->tex->m_size.y / P.scale) / 2);
                }

                const auto SUMLINE = cachedText(esc(NEWEST->appName) + " <span foreground=\"" + SUBHEX + "\">• " + std::to_string(D.items.size()) +
                                                    (s_viewHist ? " kept • " : " • ") + ageString(NEWEST->arrived) + "</span>",
                                                COLTITLE, T.title, std::max(1, (int)((PB.x - 8 - TX) * P.scale)), -1, 0, true, 600);
                if (!P.warm && SUMLINE)
                    P.tex(SUMLINE->tex, TX, y + ROW_PADT + (ROW_ICON - texH(SUMLINE, P.scale)) / 2);

                // <=2 preview lines, indented into the text column
                double       py   = y + ROW_PADT + std::max(ROW_ICON, (double)T.title / P.scale + 2);
                const size_t PREV = std::min<size_t>(2, D.items.size());
                for (size_t i = 0; i < PREV; i++) {
                    const auto& N = D.items[i];
                    if (P.warm)
                        ensureIconTex(*N, (int)std::lround(cfg.maxIcon->value() * P.scale), 0, 0);
                    const double LH = (double)T.body / P.scale * 1.35;
                    py += 3;
                    double px = TX;
                    if (N->iconTex && !N->heroTex) {
                        P.texFit(N->iconTex, CBox{px, py + (LH - PREV_ICON) / 2, PREV_ICON, PREV_ICON}, (int)std::lround(PREV_ICON / 2 * P.scale), 2.f);
                        px += PREV_ICON + 6;
                    }
                    const auto LN = cachedText("<b>" + N->summary + "</b>  <span foreground=\"" + SUBHEX + "\">" + lastLine(N->body) + "</span>", COLFG, T.body,
                                               std::max(1, (int)((CONTENT_X + CONTENT_W - ROW_PADX - px) * P.scale)), -1, 0, true, 400);
                    if (!P.warm && LN)
                        P.tex(LN->tex, px, py + (LH - texH(LN, P.scale)) / 2);
                    py += LH;
                }

                SCard card;
                card.kind  = SCard::DIGEST;
                card.box   = CBox{CONTENT_X, y, CONTENT_W, IH};
                card.group = D.key;
                card.hist  = s_viewHist;
                cards.push_back(std::move(card));
            } else {
                // ---- state 2: the expanded group — header + segmented children ----
                const auto&  NEWEST = D.items.front();
                const bool   HHOV   = hovered.kind == SCard::GHEAD && hovered.group == D.key;
                const double HEADRH = ROW_PADT + CHILD_ICON + ROW_PADB;
                P.rect(CBox{CONTENT_X, y, CONTENT_W, HEADRH}, HHOV ? tAccentDim() : tFill(), RROW, RP);

                if (P.warm)
                    ensureIconTex(*NEWEST, (int)std::lround(cfg.maxIcon->value() * P.scale), 0, 0);
                const auto& IDT = NEWEST->identTex && NEWEST->identTex->m_texID ? NEWEST->identTex : NEWEST->iconTex;
                if (IDT)
                    P.texFit(IDT, CBox{CONTENT_X + ROW_PADX, y + ROW_PADT, CHILD_ICON, CHILD_ICON}, (int)std::lround(CHILD_ICON * 10.0 / 44.0 * P.scale), RP);

                // the static ✕ (dismiss/delete the whole group)
                const CBox XB{CONTENT_X + CONTENT_W - ROW_PADX - XCIRC, y + ROW_PADT + (CHILD_ICON - XCIRC) / 2, XCIRC, XCIRC};
                const bool XHOV = HHOV && hovered.part == 2;
                if (!P.warm) {
                    P.rect(XB, XHOV ? COLURGENT : tFill2(), (int)std::lround(XCIRC / 2 * P.scale));
                    const auto* G = XHOV ? XGHOT : XG;
                    if (G && G->tex)
                        P.tex(G->tex, XB.x + (XB.w - G->tex->m_size.x / P.scale) / 2, XB.y + (XB.h - G->tex->m_size.y / P.scale) / 2);
                }

                const auto   PILL  = cachedText(std::to_string(D.items.size()) + " ˄", COLFG, T.small, 64, -1, 0, false, 600);
                const double PILLW = texW(PILL, P.scale) + 14;
                const CBox   PB{XB.x - 6 - PILLW, y + ROW_PADT + (CHILD_ICON - PILL_H) / 2, PILLW, PILL_H};
                if (!P.warm) {
                    P.rect(PB, tFill2(), (int)std::lround(PILL_H / 2 * P.scale));
                    if (PILL && PILL->tex)
                        P.tex(PILL->tex, PB.x + (PB.w - PILL->tex->m_size.x / P.scale) / 2, PB.y + (PB.h - PILL->tex->m_size.y / P.scale) / 2);
                }

                const double TX = CONTENT_X + ROW_PADX + CHILD_ICON + ROW_ICON_GAP;
                const auto   HL = cachedText(esc(NEWEST->appName) + " <span foreground=\"" + SUBHEX + "\">• " + std::to_string(D.items.size()) +
                                                 (s_viewHist ? " kept • " : " • ") + ageString(NEWEST->arrived) + "</span>",
                                             COLTITLE, T.title, std::max(1, (int)((PB.x - 8 - TX) * P.scale)), -1, 0, true, 600);
                if (!P.warm && HL)
                    P.tex(HL->tex, TX, y + ROW_PADT + (CHILD_ICON - texH(HL, P.scale)) / 2);

                {
                    SCard card;
                    card.kind  = SCard::GHEAD;
                    card.box   = CBox{CONTENT_X, y, CONTENT_W, HEADRH};
                    card.group = D.key;
                    card.hist  = s_viewHist;
                    card.close = XB;
                    cards.push_back(std::move(card));
                }

                // states 2/3: the 1-line children, each its own two-state card
                double cy = y + HEADRH;
                for (const auto& N : D.items) {
                    cy += CHILD_GAP;
                    const double CH2  = measureRow(P, T, N, CONTENT_W, itemOpen(N), s_viewHist, ROW_CHILD);
                    const bool   CHOV = hovered.kind == SCard::CHILD && hovered.id == (N->hseq ? 0 : N->id) && hovered.hseq == N->hseq && hovered.btn < 0 && hovered.part == 0;
                    const int    RIN  = (int)std::lround(5 * P.scale);
                    P.rect(CBox{CONTENT_X, cy, CONTENT_W, CH2}, CHOV ? tAccentDim() : tFill(), RIN, RP);
                    SCard card;
                    card.group = D.key;
                    renderRow(P, T, N, CBox{CONTENT_X, cy, CONTENT_W, 0}, itemOpen(N), s_viewHist, ROW_CHILD, card, true);
                    cards.push_back(std::move(card));
                    cy += CH2;
                }
            }
            y += IH + ROW_GAP;
        }

        // ---- the bottom action bar: ⏱ · Clear · ⊖ ----
        const double BARY = Y0 + PANELH - BAR_PADB - BAR_BTN;
        double       bx   = X + BAR_PADX;

        const auto   circle = [&](const char* glyph, bool lit, SCard::eKind kind) {
            const CBox B{bx, BARY, BAR_BTN, BAR_BTN};
            const auto G = cachedText(glyph, lit ? tOnAccent() : COLFG, T.bar, 64, -1, 0, false, 600);
            if (!P.warm) {
                const bool HOV = hovered.kind == kind;
                P.rect(B, lit ? COLACC : HOV ? tAccentDim() : tFill2(), (int)std::lround(BAR_BTN / 2 * P.scale));
                if (G && G->tex)
                    P.tex(G->tex, B.x + (B.w - G->tex->m_size.x / P.scale) / 2, B.y + (B.h - G->tex->m_size.y / P.scale) / 2);
            }
            SCard c;
            c.kind = kind;
            c.box  = B;
            cards.push_back(c);
            bx += BAR_BTN + BAR_GAP;
        };

        circle("⏱", s_viewHist, SCard::BTN_HIST);

        { // "Clear all" / "Clear history" — greys when its target is empty
            const double CW = X + CENTER_W - BAR_PADX - BAR_BTN - BAR_GAP - bx;
            const CBox   B{bx, BARY, CW, BAR_BTN};
            const bool   TARGET = s_viewHist ? !Bus::historyView().empty() : std::ranges::any_of(notifs, [](const auto& N) { return !N->waiting; });
            const auto   L = cachedText(s_viewHist ? "Clear history" : "Clear all", TARGET ? COLFG : COLSUB.modifyA(0.35f), T.bar, (int)(CW * P.scale), -1, 0, false, 600);
            if (!P.warm) {
                const bool HOV = hovered.kind == SCard::BTN_CLEAR;
                P.rect(B, HOV && TARGET ? tAccentDim() : tFill2().modifyA(TARGET ? 0.09f : 0.035f), (int)std::lround(BAR_BTN / 2 * P.scale));
                if (L && L->tex)
                    P.tex(L->tex, B.x + (B.w - L->tex->m_size.x / P.scale) / 2, B.y + (B.h - L->tex->m_size.y / P.scale) / 2);
            }
            SCard c;
            c.kind = SCard::BTN_CLEAR;
            c.box  = B;
            cards.push_back(c);
            bx += CW + BAR_GAP;
        }

        circle("⊖", Bus::suspendedNow(), SCard::BTN_DND);

        lastContentH = PANELH;
        lastContentW = CENTER_W;
    }

} // namespace NHyprnotify
