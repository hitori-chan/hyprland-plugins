// hyprnotify/center.cpp — the one-scroll notification center: a single
// scrolling shade partitioned into three lifecycle sections drawn top to
// bottom and only when non-empty — URGENT (live critical, pinned), WAITING
// (live normal, still alive and counting unread) and EARLIER (history,
// dimmed). Opening the center absorbs the popped banners into WAITING, so
// closing never re-pops them. Every card folds two ways: a single row is
// collapsed ⇄ open; ≥2 same-app cards fold digest ⇄ open (children fully
// readable). The footer is ⊖ DND · a global "Clear all".
//
// This unit owns the center's transient state (paging, every fold) — all of
// it resets when the center closes, Android-style.

#include "ui.hpp"

namespace NHyprnotify {

    // ---- state ----

    static bool                  s_on    = false;
    static size_t                s_skip  = 0; // wheel paging: top-level items skipped
    static size_t                s_items = 0; // items the last layout had (clamps paging)
    static std::set<std::string> s_openGroups;             // expanded groups, keyed section+appKey
    static std::set<uint32_t>    s_openedLive, s_foldedLive; // chevron overrides for live rows
    static std::set<uint64_t>    s_histOpen;                 // expanded history rows
    static Time::steady_tp       s_openedAt;
    static bool                  s_animating = false;

    // the warm-measured layout cache (see renderCenter); dropped on close so
    // no strong SNotif refs (and their textures) outlive the visit
    struct SDisp {
        std::vector<SP<SNotif>> items; // newest first; 1 = a bare row
        std::string             key;   // the app key (groups)
        eSection                sec = SEC_WAITING;
    };
    static std::vector<SDisp>               s_disp;
    static std::vector<double>              s_itemH;
    static std::vector<std::vector<double>> s_childH;

    bool                         centerVisible() {
        return s_on;
    }
    bool centerAnimating() {
        return s_animating;
    }

    // group open-state is keyed by section AND app key: the same app can hold
    // a group in Waiting and another in Earlier, folded independently
    static std::string openId(eSection sec, const std::string& key) {
        return std::string(1, (char)('0' + (int)sec)) + key;
    }

    static bool liveRowOpen(const SP<SNotif>& n) {
        // live arrives expanded and auto-folds when its banner expires; the
        // chevron overrides both ways
        return s_openedLive.contains(n->id) || (n->banner && !s_foldedLive.contains(n->id));
    }

    // a single row's open state: history opts in per hseq, live follows the
    // banner/override rule
    static bool itemOpen(const SP<SNotif>& n) {
        return n->hseq ? s_histOpen.contains(n->hseq) : liveRowOpen(n);
    }

    static bool centerRowOpen(uint32_t id, uint64_t hseq) {
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

    void centerToggleGroup(int sec, const std::string& appKey) {
        const auto K = openId((eSection)sec, appKey);
        if (!s_openGroups.erase(K))
            s_openGroups.insert(K);
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
            // Android parity: paging and every fold reset on close
            s_skip = 0;
            s_openGroups.clear();
            s_openedLive.clear();
            s_foldedLive.clear();
            s_histOpen.clear();
            s_animating = false;
            s_disp.clear(); // strong refs must not outlive the visit
            s_itemH.clear();
            s_childH.clear();
        } else {
            // opening absorbs the popped stack into the shade — the banners
            // stand down into parked Waiting rows, so closing never re-pops
            Bus::absorbPopped();
            if (animationsOn()) {
                s_openedAt  = Time::steadyNow();
                s_animating = true;
            }
        }
        notifChanged();
        Bus::emitStateSoon();
    }

    // ---- the display list: three sections, each folded independently ----

    static void buildDisplay(std::vector<SDisp>& out) {
        out.clear();
        std::vector<SP<SNotif>> src;

        // fold >= 2 same-app entries within a section; the OSD band never groups
        const auto emit = [&](eSection sec) {
            std::map<std::string, size_t> firstOf; // app key -> out index, this section only
            for (const auto& N : src) {
                if (!inOsdBand(N->id) && !N->appKey.empty()) {
                    if (const auto IT = firstOf.find(N->appKey); IT != firstOf.end()) {
                        out[IT->second].items.push_back(N);
                        continue;
                    }
                    firstOf[N->appKey] = out.size();
                }
                out.push_back(SDisp{.items = {N}, .key = N->appKey, .sec = sec});
            }
        };

        // URGENT — live critical, newest first, pinned to the top
        src.clear();
        for (const auto& N : notifs)
            if (!N->waiting && !inOsdBand(N->id) && N->urgency >= 2)
                src.push_back(N);
        emit(SEC_URGENT);

        // WAITING — live normal; conversations (im.*/call.*) sort atop, order only
        src.clear();
        for (const auto& N : notifs)
            if (!N->waiting && !inOsdBand(N->id) && N->urgency < 2)
                src.push_back(N);
        std::stable_sort(src.begin(), src.end(), [](const auto& a, const auto& b) { return a->conversation && !b->conversation; });
        emit(SEC_WAITING);

        // EARLIER — history, newest first
        src.clear();
        const auto& H = Bus::historyView(); // oldest first
        for (size_t i = H.size(); i-- > 0;)
            src.push_back(H[i]);
        emit(SEC_EARLIER);
    }

    static bool groupOpen(const SDisp& D) {
        return s_openGroups.contains(openId(D.sec, D.key));
    }

    // ---- one row, two states (singles and group children share it) ----

    struct SRowStyle {
        double iconPx;       // 34 rows, 28 children
        bool   withBadge;    // children ride plain avatars — the header owns identity
        bool   headerHasApp; // singles: "App • age"; children: age only
        bool   hasChevron;   // singles fold; expanded-group children are always open
    };
    static constexpr SRowStyle ROW_SINGLE{ROW_ICON, true, true, true};
    static constexpr SRowStyle ROW_CHILD{CHILD_ICON, false, false, false};

    // Lays out (and in draw mode paints) one row at box.x/box.y with box.w;
    // returns the row height and fills the card's hit boxes.
    static double renderRow(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box, bool open, eSection sec, const SRowStyle& ST, SCard& card, bool child) {
        const auto COLFG = color(cfg.colFg), COLTITLE = color(cfg.colTitle), COLSUB = color(cfg.colKicker), COLACC = color(cfg.colHighlight);
        const CHyprColor COLBODY = COLFG.modifyA(COLFG.a * 0.92);
        const auto       AGE     = ageString(N->arrived);
        const auto&      SUBHEX  = hexOfCached(COLSUB);
        const float      RP      = (float)cfg.roundingPower->value();

        if (P.warm)
            ensureIconTex(*N, (int)std::lround(std::max(ST.iconPx, (double)cfg.maxIcon->value()) * P.scale), 0, 0);

        const bool   LEADICON = hasLeadIcon(*N);
        const double ICONW    = LEADICON ? ST.iconPx : 0;
        const bool   HASIDENT = N->identTex && N->identTex->m_texID != 0;
        const bool   RTHUMB   = N->iconTex && N->iconTex->m_texID != 0 && !N->heroTex && HASIDENT; // content rides the right
        const double THUMBW   = RTHUMB ? ST.iconPx : 0;
        const double TX       = box.x + ROW_PADX + (ICONW > 0 ? ICONW + ROW_ICON_GAP : 0);
        const double RTRIM    = ST.hasChevron ? CHEV + 8 : 0;
        const double TEXTW    = box.x + box.w - ROW_PADX - RTRIM - (THUMBW > 0 ? THUMBW + ROW_ICON_GAP : 0) - TX;
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
            // expanded: age/header line, title, 4-line body, progress, the
            // card's ORIGINAL actions — live and history alike
            auto& KB = scratch();
            if (ST.headerHasApp) {
                appendEsc(KB, N->appName);
                KB += " • ";
            }
            KB += AGE;
            const auto KICK  = cachedText(KB, COLSUB, T.header, TEXTWPX, -1, 0, true, 500);
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
                    auto& LB = scratch();
                    appendEsc(LB, A.label);
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
                paintProgress(P, TX, yy, TEXTW, N->progress, N->urgency >= 2);
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
            paintIconColumn(P, *N, CBox{box.x + ROW_PADX, IY, ICONW, ICONW}, ST.withBadge, RP);
        }
        if (!P.warm && RTHUMB) { // the content photo/screenshot, right of the text
            const double TXR = box.x + box.w - ROW_PADX - RTRIM - THUMBW;
            const double TYR = open ? box.y + ROW_PADT : box.y + (ROWH - THUMBW) / 2;
            P.texFit(N->iconTex, CBox{TXR, TYR, THUMBW, THUMBW}, (int)std::lround(THUMBW * 10.0 / 44.0 * P.scale), RP);
        }

        // the chevron circle (singles only): collapsed centers, expanded pins
        if (ST.hasChevron) {
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
        card.sec  = sec;
        card.kind = child ? SCard::CHILD : SCard::ROW;
        return ROWH;
    }

    // measure without painting: same code, a paint context that draws nothing
    // (cachedText still resolves through the real warm gate)
    static double measureRow(const SPaint& P, const SType& T, const SP<SNotif>& N, double w, bool open, eSection sec, const SRowStyle& ST) {
        SPaint MP = P;
        MP.warm   = true;
        SCard scratch;
        return renderRow(MP, T, N, CBox{0, 0, w, 0}, open, sec, ST, scratch, false);
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
        const int   RIN    = (int)std::lround(STACK_GAP * P.scale); // merged-card joint radius
        const float RP     = (float)cfg.roundingPower->value();

        const auto  COLBG = color(cfg.colBg), COLFG = color(cfg.colFg), COLTITLE = color(cfg.colTitle), COLSUB = color(cfg.colKicker), COLACC = color(cfg.colHighlight),
                   COLURGENT = color(cfg.colUrgent);
        const auto& SUBHEX   = hexOfCached(COLSUB);

        const double X  = MB.x + MB.w - EDGE - CENTER_W;
        const double Y0 = MB.y + (double)cfg.offsetY->value();

        const double CONTENT_X = X + BODY_PADX, CONTENT_W = CENTER_W - 2 * BODY_PADX;

        const double BAR_H   = BAR_PADT + BAR_BTN + BAR_PADB;
        const double BODYCAP = CENTER_MAXH - BAR_H - BODY_PADT - BODY_PADB;

        // The display list and every height are measured once per WARM and
        // reused by the draws between warms: hover fills change nothing the
        // measure depends on, and every model/fold change warms first
        // (notifChanged). The draw side lays out without measuring a row twice.
        if (P.warm) {
            buildDisplay(s_disp);
            s_itemH.assign(s_disp.size(), 0.0);
            s_childH.assign(s_disp.size(), {});
            for (size_t i = 0; i < s_disp.size(); i++) {
                const auto& D = s_disp[i];
                if (D.items.size() < 2) {
                    s_itemH[i] = measureRow(P, T, D.items.front(), CONTENT_W, itemOpen(D.items.front()), D.sec, ROW_SINGLE);
                    continue;
                }
                if (!groupOpen(D)) { // digest: icon line + <=2 preview lines
                    double       h    = ROW_PADT + std::max(ROW_ICON, (double)T.title / P.scale + 2);
                    const size_t PREV = std::min<size_t>(2, D.items.size());
                    s_itemH[i]        = h + PREV * ((double)T.body / P.scale * 1.35 + 3) + ROW_PADB;
                    continue;
                }
                double h = ROW_PADT + CHILD_ICON + ROW_PADB; // the header row
                s_childH[i].reserve(D.items.size());
                for (const auto& N : D.items) {
                    const double CH2 = measureRow(P, T, N, CONTENT_W, true, D.sec, ROW_CHILD); // expanded children are always open
                    s_childH[i].push_back(CH2);
                    h += CHILD_GAP + CH2;
                }
                s_itemH[i] = h;
            }
        }
        const auto& disp = s_disp;
        s_items          = disp.size();
        s_skip           = disp.empty() ? 0 : std::min(s_skip, disp.size() - 1);

        // per-section totals for the header counts
        size_t secCount[3] = {0, 0, 0};
        for (const auto& D : disp)
            secCount[(int)D.sec] += D.items.size();

        // place the items that fit, accounting for a section header wherever
        // the section changes (STACK_GAP joins cards in a section, SEC_GAP
        // separates sections)
        struct SPlaced {
            size_t idx;
            double h;
        };
        static std::vector<SPlaced> placed; // reused
        placed.clear();
        double   usedH   = 0;
        eSection lastSec = SEC_URGENT;
        for (size_t i = s_skip; i < disp.size() && i < s_itemH.size(); i++) {
            const bool   NEWSEC = placed.empty() || disp[i].sec != lastSec;
            const double LEAD   = NEWSEC ? (placed.empty() ? 0 : SEC_GAP) + SEC_HEAD_H + STACK_GAP : STACK_GAP;
            if (!placed.empty() && usedH + LEAD + s_itemH[i] > BODYCAP)
                break;
            usedH += LEAD + s_itemH[i];
            placed.push_back({i, s_itemH[i]});
            lastSec = disp[i].sec;
        }

        const bool   EMPTY  = disp.empty();
        const double EMPTYH = 46;
        const double BODYH  = EMPTY ? EMPTYH : usedH;
        const double PANELH = BODY_PADT + BODYH + BODY_PADB + BAR_H;
        const CBox   PANEL{X, Y0, CENTER_W, PANELH};

        P.shadow(PANEL, RPANEL, RP, 22);
        P.glass(PANEL, COLBG, RPANEL, RP);
        {
            SCard pc;
            pc.kind = SCard::PANEL;
            pc.box  = PANEL;
            cards.push_back(pc);
        }

        // glyphs the section headers reuse — request every warm
        const auto XG    = cachedText("✕", COLFG, T.small, 64, -1, 0, false, 600);
        const auto XGDIM = cachedText("✕", COLSUB, T.small, 64, -1, 0, false, 600);
        const auto XGHOT = cachedText("✕", tOnAccent(), T.small, 64, -1, 0, false, 600);

        double y = Y0 + BODY_PADT;

        if (EMPTY) {
            const auto E = cachedText("You're all caught up!", COLSUB, T.body, (int)(CENTER_W * P.scale), -1, 0, false, 500);
            if (!P.warm && E && E->tex)
                P.tex(E->tex, X + (CENTER_W - E->tex->m_size.x / P.scale) / 2, y + (EMPTYH - E->tex->m_size.y / P.scale) / 2);
            y += EMPTYH;
        }

        // ---- the section header: LABEL · count · dim-until-hover ✕ ----
        static const char* LABEL[3] = {"URGENT", "WAITING", "EARLIER"};
        const auto          drawSecHead = [&](eSection sec, double yy) {
            auto& HB = scratch();
            HB += LABEL[(int)sec];
            if (secCount[(int)sec] > 1) {
                HB += "  <span foreground=\"";
                HB += SUBHEX;
                HB += "\">";
                HB += std::to_string(secCount[(int)sec]);
                HB += "</span>";
            }
            const auto L = cachedText(HB, COLSUB, T.small, (int)(CONTENT_W * P.scale), -1, 0, true, 700);
            if (!P.warm && L && L->tex)
                P.tex(L->tex, CONTENT_X + 4, yy + (SEC_HEAD_H - texH(L, P.scale)) / 2);

            const CBox XB{CONTENT_X + CONTENT_W - SEC_XCIRC, yy + (SEC_HEAD_H - SEC_XCIRC) / 2, SEC_XCIRC, SEC_XCIRC};
            const bool XHOV = hovered.kind == SCard::SEC_CLEAR && hovered.sec == sec;
            if (!P.warm) {
                if (XHOV)
                    P.rect(XB, sec == SEC_EARLIER ? COLURGENT : tAccentDim(), (int)std::lround(SEC_XCIRC / 2 * P.scale));
                const auto* G = XHOV ? (sec == SEC_EARLIER ? XGHOT : XG) : XGDIM;
                if (G && G->tex)
                    P.tex(G->tex, XB.x + (XB.w - G->tex->m_size.x / P.scale) / 2, XB.y + (XB.h - G->tex->m_size.y / P.scale) / 2);
            }
            SCard c;
            c.kind = SCard::SEC_CLEAR;
            c.sec  = sec;
            c.box  = XB;
            cards.push_back(c);
        };

        eSection curSec = SEC_URGENT;
        bool     first  = true;
        for (const auto& [IDX, IH] : placed) {
            const auto& D = disp[IDX];

            if (first || D.sec != curSec) {
                if (!first)
                    y += SEC_GAP;
                drawSecHead(D.sec, y);
                y += SEC_HEAD_H + STACK_GAP;
                curSec = D.sec;
                first  = false;
            } else
                y += STACK_GAP;

            if (D.items.size() < 2) {
                // ---- a bare row ----
                const auto& N   = D.items.front();
                const bool  HOV = hovered.kind == SCard::ROW && hovered.id == (N->hseq ? 0 : N->id) && hovered.hseq == N->hseq && hovered.btn < 0 && hovered.part == 0;
                P.rect(CBox{CONTENT_X, y, CONTENT_W, IH}, HOV ? tAccentDim() : tFill(), RROW, RP);
                SCard card;
                renderRow(P, T, N, CBox{CONTENT_X, y, CONTENT_W, 0}, itemOpen(N), D.sec, ROW_SINGLE, card, false);
                cards.push_back(std::move(card));
            } else if (!groupOpen(D)) {
                // ---- digest: the folded group card ----
                const auto& NEWEST = D.items.front();
                const bool  HOV    = hovered.kind == SCard::DIGEST && hovered.group == D.key && hovered.sec == D.sec;
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
                    double             px = TX;
                    const SP<ITexture> PV = (N->identTex && N->identTex->m_texID) ? N->identTex : (!N->heroTex ? N->iconTex : nullptr); // sender face first
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
                    const auto LN = cachedText(PBUF, COLFG, T.body, std::max(1, (int)((CONTENT_X + CONTENT_W - ROW_PADX - px) * P.scale)), -1, 0, true, 400);
                    if (!P.warm && LN)
                        P.tex(LN->tex, px, py + (LH - texH(LN, P.scale)) / 2);
                    py += LH;
                }

                SCard card;
                card.kind  = SCard::DIGEST;
                card.box   = CBox{CONTENT_X, y, CONTENT_W, IH};
                card.group = D.key;
                card.sec   = D.sec;
                cards.push_back(std::move(card));
            } else {
                // ---- open group: the header + fully-readable children ----
                const auto&  NEWEST = D.items.front();
                const bool   HHOV   = hovered.kind == SCard::GHEAD && hovered.group == D.key && hovered.sec == D.sec;
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
                    P.tex(HL->tex, TX, y + ROW_PADT + (CHILD_ICON - texH(HL, P.scale)) / 2);

                {
                    SCard card;
                    card.kind  = SCard::GHEAD;
                    card.box   = CBox{CONTENT_X, y, CONTENT_W, HEADRH};
                    card.group = D.key;
                    card.sec   = D.sec;
                    card.close = XB;
                    cards.push_back(std::move(card));
                }

                // the children, each fully readable (no third fold state)
                double cy = y + HEADRH;
                for (size_t k = 0; k < D.items.size(); k++) {
                    const auto& N = D.items[k];
                    cy += CHILD_GAP;
                    const double CH2  = IDX < s_childH.size() && k < s_childH[IDX].size() ? s_childH[IDX][k] : measureRow(P, T, N, CONTENT_W, true, D.sec, ROW_CHILD);
                    const bool   CHOV = hovered.kind == SCard::CHILD && hovered.id == (N->hseq ? 0 : N->id) && hovered.hseq == N->hseq && hovered.btn < 0 && hovered.part == 0;
                    P.rect(CBox{CONTENT_X, cy, CONTENT_W, CH2}, CHOV ? tAccentDim() : tFill(), RIN, RP);
                    SCard card;
                    card.group = D.key;
                    renderRow(P, T, N, CBox{CONTENT_X, cy, CONTENT_W, 0}, true, D.sec, ROW_CHILD, card, true);
                    cards.push_back(std::move(card));
                    cy += CH2;
                }
            }
            y += IH;
        }

        // ---- the footer: ⊖ DND · a global "Clear all" ----
        const double BARY = Y0 + PANELH - BAR_PADB - BAR_BTN;
        double       bx   = X + BAR_PADX;

        { // ⊖ do-not-disturb
            const CBox B{bx, BARY, BAR_BTN, BAR_BTN};
            const bool LIT = Bus::suspendedNow();
            const auto G   = cachedText("⊖", LIT ? tOnAccent() : COLFG, T.bar, 64, -1, 0, false, 600);
            if (!P.warm) {
                const bool HOV = hovered.kind == SCard::BTN_DND;
                P.rect(B, LIT ? COLACC : HOV ? tAccentDim() : tFill2(), (int)std::lround(BAR_BTN / 2 * P.scale));
                if (G && G->tex)
                    P.tex(G->tex, B.x + (B.w - G->tex->m_size.x / P.scale) / 2, B.y + (B.h - G->tex->m_size.y / P.scale) / 2);
            }
            SCard c;
            c.kind = SCard::BTN_DND;
            c.box  = B;
            cards.push_back(c);
            bx += BAR_BTN + BAR_GAP;
        }

        { // "Clear all" — the global sweep (live + history); greys when both empty
            const double CW     = X + CENTER_W - BAR_PADX - bx;
            const CBox   B{bx, BARY, CW, BAR_BTN};
            const bool   TARGET = !Bus::historyView().empty() || std::ranges::any_of(notifs, [](const auto& N) { return !N->waiting && !inOsdBand(N->id); });
            const auto   L      = cachedText("Clear all", TARGET ? COLFG : COLSUB.modifyA(0.35f), T.bar, (int)(CW * P.scale), -1, 0, false, 600);
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
        }

        lastContentH = PANELH;
        lastContentW = CENTER_W;
    }

} // namespace NHyprnotify
