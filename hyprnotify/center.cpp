// hyprnotify/center.cpp — the shade: ONE list of live cards, newest first,
// Android's notification shade drawn by the compositor. There are no
// lifecycle sections and no history view: what you can see is what the model
// holds, and a dismissed card is gone (Android's shade has no recall either).
//
// This unit decides WHAT is shown and WHERE; row.cpp draws it.
//
// RANKING — Android's, without the dividers: critical, then conversations,
// then normal, then silent; newest first inside each tier. An app's cards
// BUNDLE into one digest only once it has AUTOGROUP_AT of them (Android's
// GroupHelper.AUTOGROUP_AT_COUNT); below that every card stands alone.
// Conversations never bundle — each chat keeps its own card, and the model
// merges that chat's messages into it.
//
// THE EXPANSION BUDGET — rows open by DEFAULT, not on demand. The walk starts
// at the top of the page and opens each row while the panel still has room,
// so the shade is readable the instant it appears instead of costing a click
// per card. Android expands only its top card; a desktop shade is far taller,
// so we keep going and fold only what would overflow. Rows whose compact form
// has nothing else to reveal simply act from their body.
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
    static std::set<uint32_t>    s_openedRow, s_foldedRow;     // user overrides of the budget
    static std::set<std::string> s_openedGroup, s_foldedGroup; // ditto, per app key
    static Time::steady_tp       s_openedAt;
    static bool                  s_animating = false;

    static uint32_t    s_manageRow   = 0; // singleton wearing its manage panel
    static std::string s_manageGroup;    // bundle wearing its manage panel

    // the warm-measured layout cache (see renderCenter); dropped on close so
    // no strong SNotif refs (and their textures) outlive the visit
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

    // Everything one visit accumulates. The close resets it (Android parity:
    // paging and every fold start over), and so must PLUGIN_EXIT — s_disp
    // holds STRONG card refs, so a session that exits with the shade open
    // would otherwise carry them past Model::exit and destroy them, textures
    // and all, at static-destruction time with the renderer already gone.
    static void resetVisit() {
        Model::snoozeEndConfirm(); // the undo rows had exactly this surface
        s_skip = s_items = 0;
        s_openedRow.clear();
        s_foldedRow.clear();
        s_openedGroup.clear();
        s_foldedGroup.clear();
        s_rowState.clear();
        s_groupState.clear();
        s_animating = false;
        s_manageRow = 0;
        s_manageGroup.clear();
        s_disp.clear();
        s_itemH.clear();
        s_itemOpen.clear();
        s_itemMore.clear();
        s_childH.clear();
    }

    void centerExit() {
        s_on = false;
        inputCancelLongPress();
        resetVisit();
    }

    // Only one target wears the panel: two open at once would be two menus,
    // and the shade would stop being a list of notifications.
    void centerToggleManage(uint32_t id) {
        if (s_manageRow == id)
            s_manageRow = 0;
        else {
            s_manageRow = id;
            s_manageGroup.clear();
        }
        notifChanged();
    }
    void centerToggleManageGroup(const std::string& appKey) {
        if (appKey.empty())
            return;
        if (s_manageGroup == appKey)
            s_manageGroup.clear();
        else {
            s_manageGroup = appKey;
            s_manageRow   = 0;
        }
        notifChanged();
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

    void setCenter(bool on) {
        if (on == s_on)
            return;
        s_on = on;
        if (!on) {
            inputCancelLongPress();
            resetVisit();
            replyExit(); // a field cannot outlive the panel it was drawn in
        } else {
            // Opening absorbs the popped stack so closing never re-pops
            // banners the user has now chosen to view.
            Model::absorbPopped();
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
    // then the people you marked, then the rest of the people, then
    // everything else, then the quiet ones. A silenced app ranks with the
    // quiet ones — that IS what silencing it asked for.
    static int tier(const SP<SNotif>& n) {
        if (n->urgency >= 2)
            return 0;
        if (n->urgency == 0 || Policy::silenced(n->appKey))
            return 4;
        if (n->conversation)
            return n->priority ? 1 : 2;
        return 3;
    }

    // a snoozed card still holding its slot for the undo window: it shows as a
    // one-line confirmation row, and bundles with nothing
    static bool confirming(const SP<SNotif>& n) {
        return n->snoozed && Model::snoozeConfirming(n);
    }
    static bool bundleable(const SP<SNotif>& n) {
        return !n->conversation && !n->snoozed && !n->appKey.empty();
    }

    // How many bundleable cards one app holds, and where its digest landed.
    // Even at the model cap the distinct app keys are a handful, so one linear
    // scan beats the two string-keyed trees this used to build from scratch on
    // every warm AND every draw.
    struct SOwner {
        const std::string* key;
        size_t             count = 0;
        size_t             first = (size_t)-1; // index in out, once one is placed
    };

    static void buildDisplay(std::vector<SDisp>& out) {
        out.clear();
        static std::vector<SP<SNotif>> src;    // reused; main thread only
        static std::vector<SOwner>     owners; // ditto
        src.clear();
        owners.clear();
        for (const auto& N : notifs)
            if (!N->waiting && !inOsdBand(N->id) && (!N->snoozed || confirming(N)))
                src.push_back(N);
        // notifs is newest-first; a STABLE sort by tier keeps that inside each
        std::ranges::stable_sort(src, [](const auto& a, const auto& b) { return tier(a) < tier(b); });

        // the keys point into src's notifications, which outlive this call
        const auto ownerOf = [](const std::string& k) {
            for (size_t i = 0; i < owners.size(); i++)
                if (*owners[i].key == k)
                    return i;
            owners.push_back({.key = &k});
            return owners.size() - 1;
        };

        for (const auto& N : src) // conversations never bundle
            if (bundleable(N))
                owners[ownerOf(N->appKey)].count++;

        for (const auto& N : src) {
            if (bundleable(N)) {
                auto& O = owners[ownerOf(N->appKey)];
                if (O.count >= AUTOGROUP_AT) {
                    if (O.first != (size_t)-1) {
                        out[O.first].items.push_back(N);
                        continue;
                    }
                    O.first = out.size();
                }
            }
            out.push_back(SDisp{.items = {N}, .key = N->appKey});
        }
    }

    // ---- the expansion budget ----
    //
    // Walks the page from its top row down, opening what still fits. The top
    // row always opens (Android's one guarantee); a user override wins over
    // the budget in both directions. Rows past the fold are only measured
    // COLLAPSED — their open form would raster text no frame can show.
    static void runBudget(const SPaint& P, const SType& T, double contentW, double bodyCap) {
        if (bodyCap <= 0)
            return;
        double used = 0;
        for (size_t i = s_skip; i < s_disp.size(); i++) {
            const auto&  D    = s_disp[i];
            const double LEAD = i == s_skip ? 0 : STACK_GAP;
            const bool   TOP  = i == s_skip;

            if (D.items.size() < 2 && confirming(D.items.front())) {
                // fixed, and never folded: an undo row has one state
                s_itemH[i]    = snoozeRowH();
                s_itemOpen[i] = 0;
                s_itemMore[i] = 0;
            } else if (D.items.size() < 2 && D.items.front()->id == s_manageRow) {
                s_itemH[i]    = managePanelH(D.items.front());
                s_itemOpen[i] = 0;
                s_itemMore[i] = 0;
            } else if (D.items.size() >= 2 && D.key == s_manageGroup) {
                s_itemH[i]    = managePanelH(D.items.front(), D.key);
                s_itemOpen[i] = 0;
                s_itemMore[i] = 0;
            } else if (D.items.size() < 2) {
                const auto&  N          = D.items.front();
                const double CH         = measureRow(P, T, N, contentW, false, ROW_SINGLE);
                const bool   FORCE_OPEN = s_openedRow.contains(N->id), FORCE_FOLD = s_foldedRow.contains(N->id);
                bool         open = false, more = true;
                double       h = CH;
                if (!FORCE_FOLD && (FORCE_OPEN || TOP || used + LEAD + CH < bodyCap)) {
                    const double OH = measureRow(P, T, N, contentW, true, ROW_SINGLE);
                    more            = OH > CH + 0.5;
                    open            = more && (FORCE_OPEN || TOP || used + LEAD + OH <= bodyCap);
                    if (open)
                        h = OH;
                }
                s_itemH[i]        = h;
                s_itemOpen[i]     = open;
                s_itemMore[i]     = more;
                s_rowState[N->id] = open;
            } else {
                // a bundle: the digest card, or a header + readable children
                const double DH         = digestH(T, D.items.size(), P.scale);
                const bool   FORCE_OPEN = s_openedGroup.contains(D.key), FORCE_FOLD = s_foldedGroup.contains(D.key);
                bool         open       = false;
                double       h          = DH;
                if (!FORCE_FOLD && (FORCE_OPEN || TOP || used + LEAD + DH < bodyCap)) {
                    double              oh = groupHeadH();
                    std::vector<double> ch;
                    ch.reserve(D.items.size());
                    for (const auto& N : D.items) {
                        const double C = measureRow(P, T, N, contentW, true, ROW_CHILD); // expanded children are always open
                        ch.push_back(C);
                        oh += CHILD_GAP + C;
                    }
                    if (FORCE_OPEN || used + LEAD + oh <= bodyCap) {
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
        const int   RPANEL = rPanel(P.scale);
        const float RP     = rPow();

        const auto  COLBG = color(cfg.colBg), COLFG = color(cfg.colFg), COLSUB = color(cfg.colKicker), COLACC = color(cfg.colHighlight);

        const double BAR_H     = BAR_PADT + BAR_BTN + BAR_PADB;
        const double MIN_PANEL = BODY_PADT + BODY_PADB + BAR_H;
        const double OFFSET    = std::clamp((double)cfg.offsetY->value(), 0.0, std::max(0.0, MB.h - MIN_PANEL));
        const double PANEL_W   = std::max(1.0, std::min(CENTER_W, MB.w - 2 * EDGE));
        const double X         = MB.x + MB.w - PANEL_W;
        const double Y0        = MB.y + OFFSET;

        const double CONTENT_W = std::max(1.0, PANEL_W - 2 * BODY_PADX);
        const double CONTENT_X = X + (PANEL_W - CONTENT_W) / 2;

        // The shade runs to what the monitor leaves below offset_y (a margin of
        // air) — Android's shade is the screen, and the expansion budget spends
        // exactly this. Overflow past it becomes wheel paging, never off-screen
        // bleed; renderRow caps a row's body at 4 lines (7 for a chat), so no
        // single row can exceed the cap and the always-place-the-first-row rule
        // can't spill.
        const double AVAILH  = std::max(0.0, MB.y + MB.h - Y0 - std::max((double)cfg.margin->value(), 0.0) - centerOsdReserve);
        const double BODYCAP = std::max(0.0, AVAILH - BAR_H - BODY_PADT - BODY_PADB);

        // The display list, every height AND every fold verdict are decided
        // once per WARM and reused by the draws between warms: hover fills
        // change nothing they depend on, and every model/fold change warms
        // first (notifChanged). The draw side lays out without measuring twice.
        if (P.warm) {
            buildDisplay(s_disp);
            s_skip = s_disp.empty() ? 0 : std::min(s_skip, s_disp.size() - 1);
            s_itemH.assign(s_disp.size(), 0.0);
            s_itemOpen.assign(s_disp.size(), 0);
            s_itemMore.assign(s_disp.size(), 0);
            s_childH.assign(s_disp.size(), {});
            s_rowState.clear();
            s_groupState.clear();
            runBudget(P, T, CONTENT_W, BODYCAP);
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
            if (s_itemH[i] > BODYCAP)
                break;
            if (!placed.empty() && usedH + LEAD + s_itemH[i] > BODYCAP)
                break;
            usedH += LEAD + s_itemH[i];
            placed.push_back({i, s_itemH[i]});
        }
        const bool   EMPTY  = disp.empty();
        const double EMPTYH = 46;
        const double BODYH  = EMPTY ? std::min(EMPTYH, BODYCAP) : usedH;
        const double PANELH = std::min(MB.y + MB.h - Y0 - centerOsdReserve, BODY_PADT + BODYH + BODY_PADB + BAR_H);
        const CBox   PANEL{X, Y0, PANEL_W, std::max(0.0, PANELH)};

        P.shadow(PANEL, RPANEL, RP, 22);
        P.glass(PANEL, COLBG, RPANEL, RP);
        {
            SCard pc;
            pc.kind = SCard::PANEL;
            pc.box  = PANEL;
            cards.push_back(pc);
        }

        double y = Y0 + BODY_PADT;

        if (EMPTY) {
            const auto E = cachedText("You're all caught up!", COLSUB, T.body, (int)(PANEL_W * P.scale), -1, 0, false, 500);
            if (!P.warm && E && E->tex)
                P.tex(E->tex, X + (PANEL_W - E->tex->m_size.x / P.scale) / 2, y + (EMPTYH - E->tex->m_size.y / P.scale) / 2);
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

            const CBox SLOT{CONTENT_X, y, CONTENT_W, IH};
            if (D.items.size() < 2 && confirming(D.items.front()))
                paintSnoozeRow(P, T, D.items.front(), SLOT);
            else if (D.items.size() < 2 && D.items.front()->id == s_manageRow)
                paintManagePanel(P, T, D.items.front(), SLOT);
            else if (D.items.size() >= 2 && D.key == s_manageGroup)
                paintManagePanel(P, T, D.items.front(), SLOT, D.key);
            else if (D.items.size() < 2)
                paintSingle(P, T, D.items.front(), SLOT, OPEN, MORE);
            else if (!OPEN)
                paintDigest(P, T, D, SLOT);
            else
                paintGroup(P, T, D, SLOT, IDX < s_childH.size() ? s_childH[IDX] : std::vector<double>{});

            y += IH;
        }

        // ---- the footer: DND control · a global "Clear all" ----
        const double BARY = Y0 + PANELH - BAR_PADB - BAR_BTN;
        double       bx   = X + BAR_PADX;

        { // do-not-disturb
            const CBox B{bx, BARY, BAR_BTN, BAR_BTN};
            const bool LIT = Model::suspendedNow();
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

        // A silence used to be invisible once set: the only places it showed
        // were a lit glyph on a card from the very app it was suppressing, and
        // `hyprctl hyprnotify policy`. You could mute something and lose it for
        // a month. The count stands in the footer whenever a rule is in force,
        // and names them on hover — the shade always admits what it is holding
        // back.
        const size_t MUTED = Policy::silencedCount();
        if (MUTED > 0) {
            // BOTH labels every pass: hover flips without a rewarm, so keying
            // this raster on the hover state would miss the cache for a frame
            auto&      SB   = scratch();
            SB += "⊘ ";
            SB += std::to_string(MUTED);
            const auto REST = cachedText(SB, COLSUB, T.bar, 64, -1, 0, false, 600);
            const auto HOT  = cachedText("Unmute all", COLFG, T.bar, 200, -1, 0, false, 600);
            const bool HOV  = hovered.kind == SCard::BTN_RULES;
            // the wider of the two: the chip must not resize under the pointer
            const double CW = std::max(texW(REST, P.scale), texW(HOT, P.scale)) + 18;
            const CBox   B{bx, BARY, CW, BAR_BTN};
            if (!P.warm) {
                P.rect(B, HOV ? tAccentDim() : tFill2(), (int)std::lround(BAR_BTN / 2 * P.scale));
                if (const auto* L = HOV ? HOT : REST; L && L->tex)
                    P.tex(L->tex, B.x + (B.w - L->tex->m_size.x / P.scale) / 2, B.y + (B.h - L->tex->m_size.y / P.scale) / 2);
            }
            SCard c;
            c.kind = SCard::BTN_RULES;
            c.box  = B;
            cards.push_back(c);
            bx += CW + BAR_GAP;
        }

        { // "Clear all" — the global sweep; greys when the shade is empty
            const double CW = X + PANEL_W - BAR_PADX - bx;
            const CBox   B{bx, BARY, CW, BAR_BTN};
            const bool   TARGET = std::ranges::any_of(notifs, [](const auto& N) { return !N->waiting && !N->snoozed && !inOsdBand(N->id); });
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

        // Paging counts keep wheel-scroll state visible without reintroducing
        // an expansion affordance. They are informational; input.cpp owns the
        // scrolling. Both live inside the already-damaged panel box.
        if (!EMPTY) {
            size_t above = 0, below = 0;
            for (size_t i = 0; i < s_skip; i++)
                above += disp[i].items.size();
            for (size_t i = (placed.empty() ? 0 : placed.back().idx + 1); i < disp.size(); i++)
                below += disp[i].items.size();
            if (above > 0) {
                auto& UB = scratch();
                UB += std::to_string(above);
                UB += " earlier";
                const auto U = cachedText(UB, COLSUB, T.small, 128, -1, 0, false, 500);
                if (!P.warm && U && U->tex)
                    P.tex(U->tex, X + (PANEL_W - U->tex->m_size.x / P.scale) / 2, Y0 + 2);
            }
            if (below > 0) {
                auto& DB = scratch();
                DB += std::to_string(below);
                DB += " more";
                const auto D2 = cachedText(DB, COLSUB, T.small, 128, -1, 0, false, 500);
                if (!P.warm && D2 && D2->tex) {
                    const double cw = D2->tex->m_size.x / P.scale, ch = D2->tex->m_size.y / P.scale;
                    const double cx = X + (PANEL_W - cw) / 2, cy = BARY - ch - 3;
                    P.tex(D2->tex, cx, cy);
                }
            }
        }

        lastContentH = PANELH;
        lastContentW = PANEL_W;
    }

} // namespace NHyprnotify
