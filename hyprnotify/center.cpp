// hyprnotify/center.cpp — the shade: ONE list of live cards, newest first,
// Android's notification shade drawn by the compositor. There are no
// lifecycle sections and no history view: what you can see is what the model
// holds, and a dismissed card is gone (Android's shade has no recall either).
//
// RANKING — Android's, without the dividers: critical, then conversations,
// then normal, then silent; newest first inside each tier. An app's cards
// BUNDLE into one digest only once it has AUTOGROUP_AT of them (Android's
// GroupHelper.AUTOGROUP_AT_COUNT); below that every card stands alone.
// Conversations never bundle — each chat keeps its own card, and the bus
// merges that chat's messages into it.
//
// THE EXPANSION BUDGET — rows open by DEFAULT, not on demand. The walk starts
// at the top of the page and opens each row while the panel still has room,
// so the shade is readable the instant it appears instead of costing a click
// per card. Android expands only its top card; a desktop shade is far taller,
// so we keep going and fold only what would overflow. A row whose open form
// shows nothing the collapsed one doesn't gets no chevron at all.
//
// This unit owns the shade's transient state (paging, every fold override) —
// all of it resets when the shade closes, Android-style.

#include "ui.hpp"

namespace NHyprnotify {

    // Android's GroupHelper.AUTOGROUP_AT_COUNT: an app's notifications
    // auto-bundle at four, not at two.
    inline constexpr size_t AUTOGROUP_AT = 4;

    // ---- state ----

    static bool                  s_on    = false;
    static size_t                s_skip  = 0; // wheel paging: top-level items skipped
    static size_t                s_items = 0; // items the last layout had (clamps paging)
    static std::set<uint32_t>    s_openedRow, s_foldedRow;       // user overrides of the budget
    static std::set<std::string> s_openedGroup, s_foldedGroup;   // ditto, per app key
    static Time::steady_tp       s_openedAt;
    static bool                  s_animating = false;

    // keyboard navigation: an index into s_disp, -1 until an arrow key claims
    // the shade (so a mouse-only visit shows no selection at all). s_lastVis
    // is the last item the placement fitted — the paging keys work off it.
    static int                   s_sel     = -1;
    static size_t                s_lastVis = 0;

    // the warm-measured layout cache (see renderCenter); dropped on close so
    // no strong SNotif refs (and their textures) outlive the visit
    struct SDisp {
        std::vector<SP<SNotif>> items; // newest first; 1 = a bare row
        std::string             key;   // the app key (bundles)
    };
    static std::vector<SDisp>               s_disp;
    static std::vector<double>              s_itemH;
    static std::vector<uint8_t>             s_itemOpen; // the budget's verdict, per display item
    static std::vector<uint8_t>             s_itemMore; // the open form shows more than the collapsed one
    static std::vector<std::vector<double>> s_childH;
    // resolved open state from the last warm, keyed by identity: the click
    // handlers flip THIS, so a toggle means the same thing the eye just saw
    static std::unordered_map<uint32_t, bool>    s_rowState;
    static std::unordered_map<std::string, bool> s_groupState;

    bool                                         centerVisible() {
        return s_on;
    }
    bool centerAnimating() {
        return s_animating;
    }

    // ---- peek: the bell's hover opens the shade UNPINNED ----
    //
    // Summoning the shade cost a click for a glance. Hovering the bell opens
    // it on GTK's popup delay instead — but a hover-opened shade must not
    // outstay the pointer, and it must survive the trip from the bell down
    // into the panel. So the close is a grace timer that BOTH surfaces cancel:
    // the bar while the pointer is on the bell, the panel's own hover while it
    // is on a card. Any click pins the shade, and a pinned shade is an
    // ordinary one — hover never takes anything away.
    inline constexpr int64_t   PEEK_GRACE_MS = 400;
    static bool                s_peek = false, s_peekBell = false;
    static SP<CEventLoopTimer> s_peekOut;

    static void                armPeekOut(bool on) {
        if (s_peekOut)
            s_peekOut->updateTimeout(on ? std::optional{std::chrono::milliseconds(PEEK_GRACE_MS)} : std::nullopt);
    }

    bool centerPeeking() {
        return s_peek;
    }

    void centerPin() {
        if (!s_peek)
            return;
        s_peek = s_peekBell = false;
        armPeekOut(false);
        Bus::absorbPopped(); // the peek deferred this; the shade is a real one now
        notifChanged();
    }

    void centerPeek(bool onBell) {
        s_peekBell = onBell;
        if (onBell) {
            armPeekOut(false);
            if (s_on)
                return; // already up (pinned, or peeked): hover adds nothing
            s_peek = true;
            setCenter(true);
            return;
        }
        if (s_peek)
            armPeekOut(!pointerOverCards()); // the pointer may already be in the panel
    }

    void centerPeekPointer(bool onCard) {
        if (s_peek)
            armPeekOut(!onCard && !s_peekBell);
    }

    void centerInit() {
        s_peekOut = makeShared<CEventLoopTimer>(
            std::nullopt,
            [](SP<CEventLoopTimer>, void*) {
                if (s_peek && !s_peekBell && !pointerOverCards())
                    setCenter(false);
            },
            nullptr);
        g_pEventLoopManager->addTimer(s_peekOut);
    }

    void centerExit() {
        if (s_peekOut && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(s_peekOut);
        s_peekOut.reset();
        s_peek = s_peekBell = false;
    }

    void centerToggleRow(uint32_t id) {
        const auto IT = s_rowState.find(id);
        if (IT != s_rowState.end() && IT->second) {
            s_openedRow.erase(id);
            s_foldedRow.insert(id);
        } else {
            s_foldedRow.erase(id);
            s_openedRow.insert(id);
        }
        notifChanged();
    }

    void centerToggleGroup(const std::string& appKey) {
        const auto IT = s_groupState.find(appKey);
        if (IT != s_groupState.end() && IT->second) {
            s_openedGroup.erase(appKey);
            s_foldedGroup.insert(appKey);
        } else {
            s_foldedGroup.erase(appKey);
            s_openedGroup.insert(appKey);
        }
        notifChanged();
    }

    void centerPage(int dir) {
        if (s_items <= 1)
            return;
        s_skip = (size_t)std::clamp((int64_t)s_skip + dir, (int64_t)0, (int64_t)(s_items - 1));
        notifChanged();
    }

    void centerSelectMove(int dir) {
        if (s_disp.empty()) {
            s_sel = -1;
            return;
        }
        const int LAST = (int)s_disp.size() - 1;
        if (s_sel < 0) // the first arrow enters the page, it does not jump
            s_sel = dir > 0 ? std::min((int)s_skip, LAST) : std::min((int)s_lastVis, LAST);
        else
            s_sel = std::clamp(s_sel + dir, 0, LAST);
        // page just far enough to keep the selection on screen; the next warm
        // re-fits the page around the new s_skip
        if (s_sel < (int)s_skip)
            s_skip = (size_t)s_sel;
        else if (s_sel > (int)s_lastVis)
            s_skip += (size_t)s_sel - s_lastVis;
        notifChanged();
    }

    bool centerSelection(uint32_t& id, std::string& group) {
        if (s_sel < 0 || (size_t)s_sel >= s_disp.size())
            return false;
        const auto& D = s_disp[s_sel];
        group         = D.items.size() > 1 ? D.key : "";
        id            = D.items.front()->id;
        return true;
    }

    void setCenter(bool on) {
        if (on == s_on)
            return;
        s_on = on;
        if (!on) {
            // Android parity: paging and every fold reset on close
            s_skip = 0;
            s_openedRow.clear();
            s_foldedRow.clear();
            s_openedGroup.clear();
            s_foldedGroup.clear();
            s_rowState.clear();
            s_groupState.clear();
            s_animating = false;
            s_sel       = -1;
            s_lastVis   = 0;
            s_peek = s_peekBell = false; // a closed shade is never a peeked one
            armPeekOut(false);
            s_disp.clear(); // strong refs must not outlive the visit
            s_itemH.clear();
            s_itemOpen.clear();
            s_itemMore.clear();
            s_childH.clear();
        } else {
            // Opening absorbs the popped stack — the banners stand down into
            // parked shade rows, so closing never re-pops them. A PEEK does
            // not: a pointer crossing the bell must not silently swallow
            // banners the user never read (centerPin absorbs instead).
            if (!s_peek)
                Bus::absorbPopped();
            if (animationsOn()) {
                s_openedAt  = Time::steadyNow();
                s_animating = true;
            }
        }
        notifChanged();
        Bus::emitStateSoon();
    }

    // ---- the display list: one ranked list, apps bundled at four ----

    // Android's shade ranking, minus the visible dividers: urgent things,
    // then the people, then the rest, then the silent ones.
    static int tier(const SP<SNotif>& n) {
        if (n->urgency >= 2)
            return 0;
        if (n->conversation)
            return 1;
        return n->urgency == 0 ? 3 : 2;
    }

    static void buildDisplay(std::vector<SDisp>& out) {
        out.clear();
        std::vector<SP<SNotif>> src;
        for (const auto& N : notifs)
            if (!N->waiting && !inOsdBand(N->id))
                src.push_back(N);
        // notifs is newest-first; a STABLE sort by tier keeps that inside each
        std::ranges::stable_sort(src, [](const auto& a, const auto& b) { return tier(a) < tier(b); });

        // how many bundleable cards each app holds (conversations never bundle)
        std::map<std::string, size_t> owned;
        for (const auto& N : src)
            if (!N->conversation && !N->appKey.empty())
                owned[N->appKey]++;

        std::map<std::string, size_t> firstOf; // app key -> out index
        for (const auto& N : src) {
            if (!N->conversation && !N->appKey.empty() && owned[N->appKey] >= AUTOGROUP_AT) {
                if (const auto IT = firstOf.find(N->appKey); IT != firstOf.end()) {
                    out[IT->second].items.push_back(N);
                    continue;
                }
                firstOf[N->appKey] = out.size();
            }
            out.push_back(SDisp{.items = {N}, .key = N->appKey});
        }
    }

    // ---- one row, two states (singles and bundle children share it) ----

    struct SRowStyle {
        double iconPx;       // 34 rows, 28 children
        bool   withBadge;    // children ride plain avatars — the header owns identity
        bool   headerHasApp; // singles: "App • age"; children: age only
        bool   hasChevron;   // singles fold; expanded-bundle children are always open
    };
    static constexpr SRowStyle ROW_SINGLE{ROW_ICON, true, true, true};
    static constexpr SRowStyle ROW_CHILD{CHILD_ICON, false, false, false};

    // Lays out (and in draw mode paints) one row at box.x/box.y with box.w;
    // returns the row height and fills the card's hit boxes. `more` drives the
    // chevron: an open row can always be folded, a collapsed one only offers
    // the affordance when there is something behind it.
    static double renderRow(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box, bool open, bool more, const SRowStyle& ST, SCard& card, bool child) {
        const auto COLFG = color(cfg.colFg), COLTITLE = color(cfg.colTitle), COLSUB = color(cfg.colKicker), COLACC = color(cfg.colHighlight);
        const CHyprColor COLBODY = COLFG.modifyA(COLFG.a * 0.92);
        const auto       AGE     = ageString(N->arrived);
        const auto&      SUBHEX  = hexOfCached(COLSUB);
        const float      RP      = (float)cfg.roundingPower->value();

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
            // expanded: age/header line, title, 4-line body, progress, then the
            // buttons — the card's PRIMARY first (nothing in the shade acts
            // without hitting a button), then its own actions in Notify order
            auto& KB = scratch();
            if (ST.headerHasApp) {
                appendEsc(KB, N->appName);
                KB += " • ";
            }
            KB += AGE;
            const auto KICK  = cachedText(KB, COLSUB, T.header, TEXTWPX, -1, 0, true, 500);
            const auto TITLE = N->summary.empty() ? nullptr : cachedText(N->summary, COLTITLE, T.title, TEXTWPX, -1, 0, true, 600);
            // a merged chat is a transcript, so it gets Android's MessagingStyle
            // depth (~7 messages) where an ordinary card gets four lines
            const int  CAPL = (int)std::lround(T.body * 1.35 * (N->conversation ? 7 : 4));
            const auto BODY = N->body.empty() ? nullptr : cachedText(N->body, COLBODY, T.body, TEXTWPX, CAPL, 1.1f, true, 400);

            const bool   LEADBTN = !N->defaultLabel.empty();
            const size_t NBTN    = N->actions.size() + (LEADBTN ? 1 : 0);
            const auto   BTNID   = [&](size_t k) -> const std::string& { return LEADBTN && k == 0 ? N->defaultAction : N->actions[k - (LEADBTN ? 1 : 0)].id; };
            const auto   BTNLBL  = [&](size_t k) -> const std::string& { return LEADBTN && k == 0 ? N->defaultLabel : N->actions[k - (LEADBTN ? 1 : 0)].label; };

            static std::vector<CBox>               btnBoxes; // reused; main thread only
            static std::vector<const SCachedText*> btnLbls;
            btnBoxes.clear();
            btnLbls.clear();
            double btnH = 0;
            {
                double bx = 0, rowY = 0;
                for (size_t k = 0; k < NBTN; k++) {
                    auto& LB = scratch();
                    appendEsc(LB, BTNLBL(k));
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
                        const bool BHOV = hovered.id == N->id && hovered.btn == (int)i;
                        // the primary wears a standing pill: it inherits the
                        // whole-row click the body used to carry
                        if (BHOV)
                            P.rect(BOX, tAccentDim(), (int)std::lround(BTN_H / 2 * P.scale));
                        else if (LEADBTN && i == 0)
                            P.rect(BOX, tFill2(), (int)std::lround(BTN_H / 2 * P.scale));
                        if (btnLbls[i] && btnLbls[i]->tex)
                            P.tex(btnLbls[i]->tex, BOX.x + BTN_PADX, BOX.y + (BOX.h - btnLbls[i]->tex->m_size.y / P.scale) / 2);
                    }
                    card.buttons.push_back({BOX, BTNID(i)});
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
    static double measureRow(const SPaint& P, const SType& T, const SP<SNotif>& N, double w, bool open, const SRowStyle& ST) {
        SPaint MP = P;
        MP.warm   = true;
        SCard scratch;
        return renderRow(MP, T, N, CBox{0, 0, w, 0}, open, true, ST, scratch, false);
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

        const double BAR_H = BAR_PADT + BAR_BTN + BAR_PADB;
        // The shade runs to what the monitor leaves below offset_y (a margin of
        // air) — Android's shade is the screen, and the expansion budget spends
        // exactly this. Overflow past it becomes wheel paging, never off-screen
        // bleed; renderRow caps a row's body at 4 lines (7 for a chat), so no single row can
        // exceed the cap and the always-place-the-first-row rule can't spill.
        const double AVAILH  = MB.h - (double)cfg.offsetY->value() - (double)cfg.margin->value();
        const double BODYCAP = std::max(ROW_ICON, AVAILH - BAR_H - BODY_PADT - BODY_PADB);

        // The display list, every height AND every fold verdict are decided
        // once per WARM and reused by the draws between warms: hover fills
        // change nothing they depend on, and every model/fold change warms
        // first (notifChanged). The draw side lays out without measuring twice.
        if (P.warm) {
            buildDisplay(s_disp);
            s_skip = s_disp.empty() ? 0 : std::min(s_skip, s_disp.size() - 1);
            // a dismissal shrinks the list under the selection: keeping the
            // INDEX lands it on the card that took the dismissed one's place
            if (s_sel >= (int)s_disp.size())
                s_sel = s_disp.empty() ? -1 : (int)s_disp.size() - 1;
            s_itemH.assign(s_disp.size(), 0.0);
            s_itemOpen.assign(s_disp.size(), 0);
            s_itemMore.assign(s_disp.size(), 0);
            s_childH.assign(s_disp.size(), {});
            s_rowState.clear();
            s_groupState.clear();

            // THE EXPANSION BUDGET. Walks the page from its top row down,
            // opening what still fits. The top row always opens (Android's one
            // guarantee); a user override wins over the budget in both
            // directions. Rows past the fold are only measured COLLAPSED —
            // their open form would raster text no frame can show.
            double used = 0;
            for (size_t i = s_skip; i < s_disp.size(); i++) {
                const auto&  D    = s_disp[i];
                const double LEAD = i == s_skip ? 0 : STACK_GAP;
                const bool   TOP  = i == s_skip;

                if (D.items.size() < 2) {
                    const auto&  N          = D.items.front();
                    const double CH         = measureRow(P, T, N, CONTENT_W, false, ROW_SINGLE);
                    const bool   FORCE_OPEN = s_openedRow.contains(N->id), FORCE_FOLD = s_foldedRow.contains(N->id);
                    bool         open = false, more = true;
                    double       h = CH;
                    if (!FORCE_FOLD && (FORCE_OPEN || TOP || used + LEAD + CH < BODYCAP)) {
                        const double OH = measureRow(P, T, N, CONTENT_W, true, ROW_SINGLE);
                        more            = OH > CH + 0.5;
                        open            = more && (FORCE_OPEN || TOP || used + LEAD + OH <= BODYCAP);
                        if (open)
                            h = OH;
                    }
                    s_itemH[i]        = h;
                    s_itemOpen[i]     = open;
                    s_itemMore[i]     = more;
                    s_rowState[N->id] = open;
                } else {
                    // a bundle: the digest card, or a header + readable children
                    double       dh   = ROW_PADT + std::max(ROW_ICON, (double)T.title / P.scale + 2);
                    const size_t PREV = std::min<size_t>(2, D.items.size());
                    dh += PREV * ((double)T.body / P.scale * 1.35 + 3) + ROW_PADB;

                    const bool FORCE_OPEN = s_openedGroup.contains(D.key), FORCE_FOLD = s_foldedGroup.contains(D.key);
                    bool       open = false;
                    double     h    = dh;
                    if (!FORCE_FOLD && (FORCE_OPEN || TOP || used + LEAD + dh < BODYCAP)) {
                        double              oh = ROW_PADT + CHILD_ICON + ROW_PADB; // the header row
                        std::vector<double> ch;
                        ch.reserve(D.items.size());
                        for (const auto& N : D.items) {
                            const double C = measureRow(P, T, N, CONTENT_W, true, ROW_CHILD); // expanded children are always open
                            ch.push_back(C);
                            oh += CHILD_GAP + C;
                        }
                        if (FORCE_OPEN || used + LEAD + oh <= BODYCAP) {
                            open        = true;
                            h           = oh;
                            s_childH[i] = std::move(ch);
                        }
                    }
                    s_itemH[i]          = h;
                    s_itemOpen[i]       = open;
                    s_itemMore[i]       = 1;
                    s_groupState[D.key] = open;
                }
                used += LEAD + s_itemH[i];
            }
        }
        const auto& disp = s_disp;
        s_items          = disp.size();
        s_skip           = disp.empty() ? 0 : std::min(s_skip, disp.size() - 1);

        // place the items that fit; STACK_GAP joins the cards into one column
        struct SPlaced {
            size_t idx;
            double h;
        };
        static std::vector<SPlaced> placed; // reused
        placed.clear();
        double usedH = 0;
        for (size_t i = s_skip; i < disp.size() && i < s_itemH.size(); i++) {
            const double LEAD = placed.empty() ? 0 : STACK_GAP;
            if (!placed.empty() && usedH + LEAD + s_itemH[i] > BODYCAP)
                break;
            usedH += LEAD + s_itemH[i];
            placed.push_back({i, s_itemH[i]});
        }
        s_lastVis = placed.empty() ? s_skip : placed.back().idx;

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

        const auto XG    = cachedText("✕", COLFG, T.small, 64, -1, 0, false, 600);
        const auto XGHOT = cachedText("✕", tOnAccent(), T.small, 64, -1, 0, false, 600);

        double     y = Y0 + BODY_PADT;

        if (EMPTY) {
            const auto E = cachedText("You're all caught up!", COLSUB, T.body, (int)(CENTER_W * P.scale), -1, 0, false, 500);
            if (!P.warm && E && E->tex)
                P.tex(E->tex, X + (CENTER_W - E->tex->m_size.x / P.scale) / 2, y + (EMPTYH - E->tex->m_size.y / P.scale) / 2);
            y += EMPTYH;
        }

        bool first = true;
        for (const auto& [IDX, IH] : placed) {
            const auto& D    = disp[IDX];
            const bool  OPEN = IDX < s_itemOpen.size() && s_itemOpen[IDX];
            const bool  MORE = IDX < s_itemMore.size() && s_itemMore[IDX];
            if (!first)
                y += STACK_GAP;
            first = false;

            if (D.items.size() < 2) {
                // ---- a bare row ----
                const auto& N   = D.items.front();
                const bool  HOV = hovered.kind == SCard::ROW && hovered.id == N->id && hovered.btn < 0 && hovered.part == 0;
                P.rect(CBox{CONTENT_X, y, CONTENT_W, IH}, HOV ? tAccentDim() : tFill(), RROW, RP);
                SCard card;
                renderRow(P, T, N, CBox{CONTENT_X, y, CONTENT_W, 0}, OPEN, MORE, ROW_SINGLE, card, false);
                cards.push_back(std::move(card));
            } else if (!OPEN) {
                // ---- digest: the folded app bundle ----
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
                    const auto LN = cachedText(PBUF, COLFG, T.body, std::max(1, (int)((CONTENT_X + CONTENT_W - ROW_PADX - px) * P.scale)), -1, 0, true, 400);
                    if (!P.warm && LN)
                        P.tex(LN->tex, px, py + (LH - texH(LN, P.scale)) / 2);
                    py += LH;
                }

                SCard card;
                card.kind  = SCard::DIGEST;
                card.box   = CBox{CONTENT_X, y, CONTENT_W, IH};
                card.group = D.key;
                cards.push_back(std::move(card));
            } else {
                // ---- open bundle: the header + fully-readable children ----
                const auto&  NEWEST = D.items.front();
                const bool   HHOV   = hovered.kind == SCard::GHEAD && hovered.group == D.key;
                const double HEADRH = ROW_PADT + CHILD_ICON + ROW_PADB;
                P.rect(CBox{CONTENT_X, y, CONTENT_W, HEADRH}, HHOV ? tAccentDim() : tFill(), RROW, RP);

                if (P.warm)
                    ensureIconTex(*NEWEST, (int)std::lround(cfg.maxIcon->value() * P.scale), 0, 0);
                const auto& IDT = NEWEST->identTex && NEWEST->identTex->m_texID ? NEWEST->identTex : NEWEST->iconTex;
                if (IDT)
                    P.texFit(IDT, CBox{CONTENT_X + ROW_PADX, y + ROW_PADT, CHILD_ICON, CHILD_ICON}, (int)std::lround(CHILD_ICON * 10.0 / 44.0 * P.scale), RP);

                // the static ✕ (dismiss the whole app's bundle)
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
                    card.close = XB;
                    cards.push_back(std::move(card));
                }

                // the children, each fully readable (no third fold state)
                double cy = y + HEADRH;
                for (size_t k = 0; k < D.items.size(); k++) {
                    const auto& N = D.items[k];
                    cy += CHILD_GAP;
                    const double CH2  = IDX < s_childH.size() && k < s_childH[IDX].size() ? s_childH[IDX][k] : measureRow(P, T, N, CONTENT_W, true, ROW_CHILD);
                    const bool   CHOV = hovered.kind == SCard::CHILD && hovered.id == N->id && hovered.btn < 0 && hovered.part == 0;
                    P.rect(CBox{CONTENT_X, cy, CONTENT_W, CH2}, CHOV ? tAccentDim() : tFill(), RIN, RP);
                    SCard card;
                    card.group = D.key;
                    renderRow(P, T, N, CBox{CONTENT_X, cy, CONTENT_W, 0}, true, false, ROW_CHILD, card, true);
                    cards.push_back(std::move(card));
                    cy += CH2;
                }
            }

            // the keyboard selection: a hairline in the accent, drawn over the
            // whole item (an open bundle's header and children together) so it
            // never reads as a hover, which is a fill
            if ((int)IDX == s_sel)
                P.ring(CBox{CONTENT_X, y, CONTENT_W, IH}, COLACC, RROW, RP);
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

        { // "Clear all" — the global sweep; greys when the shade is empty
            const double CW     = X + CENTER_W - BAR_PADX - bx;
            const CBox   B{bx, BARY, CW, BAR_BTN};
            const bool   TARGET = std::ranges::any_of(notifs, [](const auto& N) { return !N->waiting && !inOsdBand(N->id); });
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

        // Paging cues: a wheel-scroll is invisible otherwise. "▴" when rows
        // sit above the fold, a "▾ N" chip when N notifications sit below —
        // informational only, the wheel (input.cpp) does the scrolling. Both
        // live inside the already-damaged panel box, so no extra damage.
        if (!EMPTY) {
            size_t below = 0;
            for (size_t i = (placed.empty() ? 0 : placed.back().idx + 1); i < disp.size(); i++)
                below += disp[i].items.size();
            if (s_skip > 0) {
                const auto U = cachedText("▴", COLSUB, T.small, 64, -1, 0, false, 500);
                if (!P.warm && U && U->tex)
                    P.tex(U->tex, X + (CENTER_W - U->tex->m_size.x / P.scale) / 2, Y0 + 2);
            }
            if (below > 0) {
                auto& DB = scratch();
                DB += "▾ ";
                DB += std::to_string(below);
                const auto D2 = cachedText(DB, COLSUB, T.small, 128, -1, 0, false, 500);
                if (!P.warm && D2 && D2->tex) {
                    const double cw = D2->tex->m_size.x / P.scale, ch = D2->tex->m_size.y / P.scale;
                    const double cx = X + (CENTER_W - cw) / 2, cy = BARY - ch - 3;
                    P.rect(CBox{cx - 8, cy - 2, cw + 16, ch + 4}, tFill2(), (int)std::lround((ch / 2 + 2) * P.scale));
                    P.tex(D2->tex, cx, cy);
                }
            }
        }

        lastContentH = PANELH;
        lastContentW = CENTER_W;
    }

} // namespace NHyprnotify
