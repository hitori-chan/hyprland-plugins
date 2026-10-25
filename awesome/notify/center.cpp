// awesome/notify/center.cpp — the shade: ONE list of live cards, newest first,
// Android's notification shade drawn by the compositor. There are no
// lifecycle sections and no history view: what you can see is what the model
// holds, and a dismissed card is gone (Android's shade has no recall either).
//
// This unit decides WHAT is shown and WHERE; card.cpp and bundle.cpp draw
// it. The center is the banners' own column, its controls on glass capsules
// above it; as macOS's, it draws no backdrop: the desktop is never darkened.
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
// so we keep going and fold only what would overflow. A row whose open form
// shows nothing the collapsed one doesn't gets no chevron at all.
//
// This unit owns the shade's transient state (paging, every fold override) —
// all of it resets when the shade closes, Android-style.

#include "ui.hpp"

namespace NAwesome::Notify {

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

    // keyboard navigation: an index into s_disp, -1 until an arrow key claims
    // the shade (so a mouse-only visit shows no selection at all). s_lastVis
    // is the last item the placement fitted — the paging keys work off it.
    static int      s_sel     = -1;
    static size_t   s_lastVis = 0;

    // the warm-measured layout cache (see renderCenter); dropped on close so
    // no strong SNotif refs (and their textures) outlive the visit
    static std::vector<SDisp>               s_disp;
    static std::vector<double>              s_itemH;
    static std::vector<uint8_t>             s_itemOpen; // the budget's verdict, per display item
    static std::vector<uint8_t>             s_itemMore; // the open form shows more than the collapsed one
    static std::vector<std::vector<double>> s_childH;
    static const std::vector<double>        NO_CHILDREN;
    // resolved open state from the last warm, keyed by identity: the click
    // handlers flip THIS, so a toggle means the same thing the eye just saw
    static std::unordered_map<uint32_t, bool>    s_rowState;
    static std::unordered_map<std::string, bool> s_groupState;

    bool                                         centerVisible() {
        return s_on;
    }
    bool centerAnimating() {
        // by the clock, not only the draw: a draw that never runs (the
        // session locked mid-spring) would leave the flag up and the motion
        // tick damaging the cards for nothing
        if (s_animating && animT(s_openedAt, Theme::MOTION_SPATIAL) >= 1.f)
            s_animating = false;
        return s_animating;
    }

    // Everything one visit accumulates. The close resets it (Android parity:
    // paging and every fold start over), and so must PLUGIN_EXIT — s_disp
    // holds STRONG card refs, so a session that exits with the shade open
    // would otherwise carry them past Model::exit and destroy them, textures
    // and all, at static-destruction time with the renderer already gone.
    static void resetVisit() {
        s_skip = s_items = 0;
        s_openedRow.clear();
        s_foldedRow.clear();
        s_openedGroup.clear();
        s_foldedGroup.clear();
        s_rowState.clear();
        s_groupState.clear();
        s_animating = false;
        s_sel       = -1;
        s_lastVis   = 0;
        s_disp.clear();
        s_itemH.clear();
        s_itemOpen.clear();
        s_itemMore.clear();
        s_childH.clear();
    }

    void centerExit() {
        s_on = false;
        resetVisit();
    }

    void centerToggleRow(uint32_t id) {
        const auto IT = s_rowState.find(id);
        if (IT != s_rowState.end() && IT->second) {
            s_openedRow.erase(id);
            s_foldedRow.insert(id);
            if (replyArmedOn(id))
                replyClose(); // its field folds away with the row
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
        // a pointer page leaves the keyboard's row behind: the next arrow
        // enters the new page (Enter/Delete must never act off screen)
        s_sel = -1;
        notifChanged();
    }

    void centerDeselect() {
        if (s_sel < 0)
            return;
        s_sel = -1;
        notifChanged();
    }

    // a screenful, keeping the edge row in view: the paging chips' click
    void centerPageScreen(int dir) {
        centerPage(dir * (int)std::max<size_t>(1, s_lastVis > s_skip ? s_lastVis - s_skip : 1));
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

    bool centerHasRows() {
        return !s_disp.empty();
    }

    bool centerSelection(uint32_t& id, std::string& group) {
        if (s_sel < 0 || (size_t)s_sel >= s_disp.size())
            return false;
        const auto& D = s_disp[s_sel];
        group         = D.items.size() > 1 ? D.key : "";
        id            = D.items.front()->id;
        return true;
    }

    void setCenter(bool on, bool repop) {
        if (on == s_on)
            return;
        CardMenu::close(); // the layout it was opened over is going
        // what the center listed while open, and lists as it opens, has been
        // seen: the bell's dot goes out (the funnel below repaints it)
        Model::markSeen();
        s_on = on;
        if (!on) {
            resetVisit();
            replyExit(); // a field cannot outlive the panel it was drawn in
            Model::repopAbsorbed(repop); // an explicit close returns the parked stack
        } else {
            // Opening absorbs the popped stack — the banners stand down into
            // parked shade rows; an explicit close re-pops them (the user
            // must not lose sight of what the shade swallowed).
            Model::absorbPopped();
            if (animationsOn()) {
                s_openedAt  = Time::steadyNow();
                s_animating = true;
            }
        }
        notifChanged();
        // (the old org.hitori shell face emitted its state here; the bell
        //  now reads the model in the same .so — nothing to broadcast)
    }

    // ---- the display list: one ranked list, apps bundled at four ----

    // Android's shade ranking, minus the visible dividers: urgent things,
    // then the people, then everything else, then the quiet ones.
    static int tier(const SP<SNotif>& n) {
        if (n->urgency >= 2)
            return 0;
        if (n->urgency == 0)
            return 4;
        if (n->conversation)
            return 2;
        return 3;
    }

    // How many bundleable cards one (app, declared group) holds, and where
    // its digest landed. Even at the model cap the distinct keys are a
    // handful, so one linear scan beats the two string-keyed trees this used
    // to build from scratch on every warm AND every draw. A group's 4+ runs
    // inside the group, not across it: different x-notify-group-key
    // values never merge, and each owns its own digest and fold state.
    struct SOwner {
        std::string        key;
        size_t             count = 0;
        size_t             first = (size_t)-1; // index in out, once one is placed
    };

    static void buildDisplay(std::vector<SDisp>& out) {
        out.clear();
        static std::vector<SP<SNotif>> src;    // reused; main thread only
        static std::vector<SOwner>     owners; // ditto
        // key -> owner: a linear scan per card was quadratic in distinct
        // keys (a sender rotating its app name at max_notifs 512)
        static std::unordered_map<std::string, size_t> ownerIdx;   // ditto
        static std::vector<size_t>                     ownerOfSrc; // src[i]'s owner, -1: never bundles
        src.clear();
        owners.clear();
        ownerIdx.clear();
        ownerOfSrc.clear();
        for (const auto& N : notifs)
            if (!N->waiting && !Model::offShade(N))
                src.push_back(N);
        // notifs is newest-first; a STABLE sort by tier keeps that inside each
        std::ranges::stable_sort(src, [](const auto& a, const auto& b) { return tier(a) < tier(b); });

        for (const auto& N : src) { // conversations never bundle
            if (!Model::bundleable(N)) {
                ownerOfSrc.push_back((size_t)-1);
                continue;
            }
            const auto [IT, NEW] = ownerIdx.try_emplace(Model::groupKeyOf(N), owners.size());
            if (NEW)
                owners.push_back(SOwner{.key = IT->first});
            owners[IT->second].count++;
            ownerOfSrc.push_back(IT->second);
        }

        for (size_t i = 0; i < src.size(); i++) {
            const auto& N = src[i];
            if (ownerOfSrc[i] != (size_t)-1) {
                auto& O = owners[ownerOfSrc[i]];
                if (O.count >= AUTOGROUP_AT) {
                    if (O.first != (size_t)-1) {
                        out[O.first].items.push_back(N);
                        continue;
                    }
                    O.first = out.size();
                }
            }
            // the key rides the fold state, hover and selection — a bundle
            // head carries the (app, group) identity, singles the app
            out.push_back(SDisp{.items = {N}, .key = ownerOfSrc[i] != (size_t)-1 ? owners[ownerOfSrc[i]].key : Model::groupKeyOf(N)});
        }
        // the buffer is reused, its references are not: a dismissed card
        // (and its textures) must not live on until the shade next opens
        src.clear();
    }

    // ---- the expansion budget ----
    //
    // Walks the page from its top row down, opening what still fits. The top
    // row always opens (Android's one guarantee); a user override wins over
    // the budget in both directions. Rows past the fold are only measured
    // COLLAPSED — their open form would raster text no frame can show.
    static void runBudget(const SPaint& P, const SType& T, double contentW, double bodyCap, double gap) {
        double used = 0;
        for (size_t i = s_skip; i < s_disp.size(); i++) {
            const auto&  D    = s_disp[i];
            const double LEAD = i == s_skip ? 0 : gap;
            const bool   TOP  = i == s_skip;

            if (D.items.size() < 2) {
                const auto&  N          = D.items.front();
                const double CH         = measureCard(P, T, N, contentW, SCardForm{.where = SCardForm::CENTER, .open = false});
                // an armed reply field is drawn in the open card only: its
                // card is open whatever the budget or a fold says
                const bool   ARMED      = replyArmedOn(N->id);
                const bool   FORCE_OPEN = ARMED || s_openedRow.contains(N->id), FORCE_FOLD = !ARMED && s_foldedRow.contains(N->id);
                bool         open = false, more = true;
                double       h = CH;
                if (!FORCE_FOLD && (FORCE_OPEN || TOP || used + LEAD + CH < bodyCap)) {
                    const double OH = measureCard(P, T, N, contentW, SCardForm{.where = SCardForm::CENTER, .open = true});
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
                // a bundle: the digest, or a header and readable children
                const double DH         = digestH(T, D.items.size(), P.scale);
                const bool   FORCE_OPEN = s_openedGroup.contains(D.key), FORCE_FOLD = s_foldedGroup.contains(D.key);
                bool         open       = false;
                double       h          = DH;
                if (!FORCE_FOLD && (FORCE_OPEN || TOP || used + LEAD + DH < bodyCap)) {
                    // Children are measured down to the page's fold, never
                    // past it: a bundle that does not fit stays a digest,
                    // and one the user opened shows the children that fit
                    // and counts the rest in a tail (a 50-card bundle
                    // measured and painted every child, the ones below the
                    // screen included: tens of ms per center open)
                    const double        ROOM = bodyCap - used - LEAD;
                    const double        TAIL = FORCE_OPEN ? TAIL_H : 0;
                    double              oh   = groupChromeH(false);
                    bool                cut  = false;
                    std::vector<double> ch;
                    for (size_t k = 0; k < D.items.size() && !cut; k++) {
                        const double C    = measureCard(P, T, D.items[k], contentW, SCardForm{.where = SCardForm::CHILD});
                        const bool   LAST = k + 1 == D.items.size();
                        if (!ch.empty() && oh + C + (LAST ? 0 : TAIL) > ROOM) {
                            cut = true;
                            break;
                        }
                        ch.push_back(C);
                        oh += C;
                        cut = !FORCE_OPEN && oh > ROOM; // it cannot open: stop measuring
                    }
                    if (FORCE_OPEN || (!cut && oh <= ROOM)) {
                        open        = true;
                        h           = oh + (ch.size() < D.items.size() ? TAIL_H : 0);
                        s_childH[i] = std::move(ch);
                    }
                }
                s_itemH[i]          = h;
                s_itemOpen[i]       = open;
                s_itemMore[i]       = 1;
                s_groupState[D.key] = open;
            }
            used += LEAD + s_itemH[i];
            // past the fold: no later card is on this page, and measuring
            // them rastered two texts and queued an icon decode each for
            // content nothing draws (the age tick re-rastered them all).
            // Unmeasured cards never fit; paging re-runs the budget from the
            // new top.
            if (i > s_skip && used > bodyCap) {
                for (size_t k = i + 1; k < s_disp.size(); k++)
                    s_itemH[k] = bodyCap + 1;
                break;
            }
        }
    }

    // The last row placed from s_skip down at these heights (the draw's
    // placement, ahead of it).
    static size_t lastFit(double bodyCap, double gap) {
        double used = 0;
        size_t last = s_skip;
        for (size_t i = s_skip; i < s_itemH.size(); i++) {
            const double LEAD = i == s_skip ? 0 : gap;
            if (i > s_skip && used + LEAD + s_itemH[i] > bodyCap)
                break;
            used += LEAD + s_itemH[i];
            last = i;
        }
        return last;
    }

    // The row the keyboard works on: an armed reply field's, else the
    // selection; -1 for none.
    static int keyboardRow() {
        for (size_t i = 0; i < s_disp.size(); i++)
            for (const auto& N : s_disp[i].items)
                if (replyArmedOn(N->id))
                    return (int)i;
        return s_sel;
    }

    // ---- the center ----

    void renderCenter(const SPaint& PIN, const SType& T) {
        SPaint P = PIN;
        if (s_animating) { // the open spring: fade + a 6 px rise
            const float AT = animT(s_openedAt, Theme::MOTION_SPATIAL);
            if (AT >= 1.f)
                s_animating = false;
            else {
                P.alpha *= easeOutCubic(AT);
                P.dy -= (1.0 - easeOutBack(AT)) * 6.0;
            }
        }

        const auto  MB = P.mon->logicalBox();
        const float RP = rPow();
        const auto  COLTITLE = color(cfg().getColor("plugin:awesome:notify:col_title")), COLSUB = color(cfg().getColor("plugin:awesome:notify:col_kicker")),
                   COLACC = color(cfg().getColor("plugin:awesome:notify:col_highlight")), COLBG = color(cfg().getColor("plugin:awesome:notify:col_bg")),
                   COLRIM = color(cfg().getColor("plugin:awesome:notify:col_frame"));

        const double W   = std::max((double)cfg().getI("plugin:awesome:notify:width"), 120.0);
        const double GAP = std::max((double)cfg().getI("plugin:awesome:notify:margin"), 0.0);
        const double X   = MB.x + MB.w - EDGE - W;
        const double TOP = cardsTop(P.mon);
        const double Y0  = MB.y + TOP;
        const double CY0 = Y0 + HEADER_H + HEADER_GAP; // the cards' top

        // The column runs to what the monitor leaves below offset_y (a margin
        // of air), and the expansion budget spends exactly this. Overflow
        // becomes wheel paging, never off-screen bleed; a card caps its body
        // (card.cpp), so no single card can exceed the cap and the
        // always-place-the-first rule can't spill.
        const double AVAILH  = MB.h - TOP - cardsBottomReserved(P.mon) - GAP;
        const double BODYCAP = std::max(64.0, AVAILH - HEADER_H - HEADER_GAP);

        // The display list, every height AND every fold verdict are decided
        // once per WARM and reused by the draws between warms: hover fills
        // change nothing they depend on, and every model/fold change warms
        // first (notifChanged). The draw side lays out without measuring twice.
        if (P.warm) {
            // the selection follows its card (or bundle) across the rebuild:
            // a card landing above must not shift Enter/Delete onto a
            // neighbor. One whose card left keeps its INDEX — the card that
            // took the dismissed one's place, as a list does after a delete.
            // A field that died (its card left, its sender left the bus, a
            // replace moved it to another chat) goes now, not at the next
            // key, and takes the selection with it: a key typed for it must
            // not act on a card.
            if (replyHeld() && !replyArmed())
                s_sel = -1;
            uint32_t    selId = 0;
            std::string selGroup;
            const bool  SELECTED = centerSelection(selId, selGroup);
            buildDisplay(s_disp);
            if (SELECTED)
                for (size_t i = 0; i < s_disp.size(); i++) {
                    const auto& D = s_disp[i];
                    if (selGroup.empty() ? std::ranges::any_of(D.items, [&](const auto& N) { return N->id == selId; }) : D.items.size() > 1 && D.key == selGroup) {
                        s_sel = (int)i;
                        break;
                    }
                }
            s_skip = s_disp.empty() ? 0 : std::min(s_skip, s_disp.size() - 1);
            if (s_sel >= (int)s_disp.size())
                s_sel = s_disp.empty() ? -1 : (int)s_disp.size() - 1;
            s_itemH.assign(s_disp.size(), 0.0);
            s_itemOpen.assign(s_disp.size(), 0);
            s_itemMore.assign(s_disp.size(), 0);
            s_childH.assign(s_disp.size(), {});
            s_rowState.clear();
            s_groupState.clear();
            runBudget(P, T, W, BODYCAP, GAP);
            // the keyboard's card stays on the page: a step past the last one
            // pages by one, and a taller next card (or cards the new top
            // re-opened) can still leave it below the fold
            for (const int KEEP = keyboardRow(); KEEP > (int)s_skip && (size_t)KEEP > lastFit(BODYCAP, GAP);) {
                ++s_skip;
                s_rowState.clear();
                s_groupState.clear();
                runBudget(P, T, W, BODYCAP, GAP);
            }
        }
        const auto& disp = s_disp;
        s_items          = disp.size();
        s_skip           = disp.empty() ? 0 : std::min(s_skip, disp.size() - 1);

        // place the items that fit
        struct SPlaced {
            size_t idx;
            double h;
        };
        static std::vector<SPlaced> placed; // reused
        placed.clear();
        double usedH = 0;
        for (size_t i = s_skip; i < disp.size() && i < s_itemH.size(); i++) {
            const double LEAD = placed.empty() ? 0 : GAP;
            if (!placed.empty() && usedH + LEAD + s_itemH[i] > BODYCAP)
                break;
            usedH += LEAD + s_itemH[i];
            placed.push_back({i, s_itemH[i]});
        }
        s_lastVis = placed.empty() ? s_skip : placed.back().idx;
        const bool EMPTY = disp.empty();

        // the column swallows its clicks and owns the wheel, gaps included
        {
            SCard pc;
            pc.kind = SCard::PANEL;
            pc.box  = CBox{X, Y0, W, HEADER_H + HEADER_GAP + (EMPTY ? EMPTY_H : usedH)};
            cards.push_back(pc);
        }

        // ---- the controls: Do Not Disturb and Clear All ----
        // macOS's center draws no backdrop and no heading; what it offers
        // sits on glass of its own (its Show Less capsule), legible over any
        // wallpaper without darkening one.
        {
            const double CAP_H = 24, CAP_PADX = 10, CAP_GAP = 6, MOON_PX = 14;
            const double CY    = Y0 + (HEADER_H - CAP_H) / 2;
            const int    ROUND = (int)std::lround(CAP_H / 2 * P.scale);

            const bool   TARGET = std::ranges::any_of(notifs, [](const auto& N) { return !N->waiting && !Model::offShade(N); });
            const bool   CHOV   = hovered.kind == SCard::BTN_CLEAR && TARGET;
            const auto   CLEAR  = cachedText("Clear All", TARGET ? COLTITLE : COLSUB, T.meta, (int)std::lround(W * P.scale), -1, 0, false, 500);
            const double CW     = texW(CLEAR, P.scale) + 2 * CAP_PADX;
            const CBox   CB{X + W - CW, CY, CW, CAP_H};
            P.frost(CB, COLBG, COLRIM, ROUND, 2.f);
            if (CHOV)
                P.rect(CB, tFill(), ROUND, 2.f);
            texCentered(P, CLEAR, CB);
            SCard cc;
            cc.kind = SCard::BTN_CLEAR;
            cc.box  = CB;
            cards.push_back(cc);

            // the moon, lit in the accent while Do Not Disturb holds
            const bool LIT  = Model::suspendedNow();
            const bool DHOV = hovered.kind == SCard::BTN_DND;
            const CBox DB{CB.x - CAP_GAP - CAP_H, CY, CAP_H, CAP_H};
            P.frost(DB, COLBG, COLRIM, ROUND, 2.f);
            if (DHOV)
                P.rect(DB, tFill(), ROUND, 2.f);
            if (const auto MOON = Glyph::tex(Glyph::eGlyph::MOON, LIT ? COLACC : COLTITLE, (int)std::lround(MOON_PX * P.scale)))
                P.texIn(MOON, DB);
            SCard dc;
            dc.kind = SCard::BTN_DND;
            dc.box  = DB;
            cards.push_back(dc);
        }

        if (EMPTY) {
            const CBox EB{X, CY0, W, EMPTY_H};
            P.frost(EB, COLBG, COLRIM, rCard(P.scale), RP);
            texCentered(P, cachedText("No Notifications", COLSUB, T.body, (int)std::lround(W * P.scale), -1, 0, false, 500), EB);
        }

        double y = CY0;
        for (const auto& [IDX, IH] : placed) {
            const auto& D    = disp[IDX];
            const bool  OPEN = IDX < s_itemOpen.size() && s_itemOpen[IDX];
            const CBox  SLOT{X, y, W, IH};
            if (D.items.size() < 2) {
                SCard card = recycledCard();
                renderCard(P, T, D.items.front(), SLOT, SCardForm{.where = SCardForm::CENTER, .open = OPEN}, card);
                card.folded = !OPEN && IDX < s_itemMore.size() && s_itemMore[IDX];
                cards.push_back(std::move(card));
            } else if (!OPEN)
                paintDigest(P, T, D, SLOT);
            else // a ternary against a temporary copied the heights every frame
                paintGroup(P, T, D, SLOT, IDX < s_childH.size() ? s_childH[IDX] : NO_CHILDREN);

            // the keyboard's selection: the accent around the whole item, so
            // it never reads as a hover
            if ((int)IDX == s_sel)
                P.ring(SLOT, COLACC, rCard(P.scale), RP, 1.5);
            y += IH + GAP;
        }

        // Paging cues: a wheel-scroll is invisible otherwise. An up chevron
        // when cards sit above the fold, a "chevron N" chip when N sit below.
        // Drawn over the edge cards, they take their own clicks (a page, like
        // the wheel) — pushed last, they are hit first.
        if (!EMPTY) {
            size_t below = 0;
            for (size_t i = (placed.empty() ? 0 : placed.back().idx + 1); i < disp.size(); i++)
                below += disp[i].items.size();
            const double CHIP_H = 24;
            if (s_skip > 0) {
                const CBox UP{X + (W - 44) / 2, CY0 - CHIP_H / 2, 44, CHIP_H};
                P.frost(UP, COLBG, COLRIM, (int)std::lround(CHIP_H / 2 * P.scale), 2.f);
                if (const auto G = Glyph::tex(Glyph::eGlyph::CHEVRON_UP, COLTITLE, (int)std::lround(14 * P.scale)))
                    P.texIn(G, UP);
                SCard c;
                c.kind = SCard::PAGE_UP;
                c.box  = UP;
                cards.push_back(c);
            }
            if (below > 0) {
                const auto   NUM = cachedText(std::to_string(below), COLTITLE, T.small, 128, -1, 0, false, 600);
                const double CW  = 14 + 4 + texW(NUM, P.scale) + 24;
                const CBox   DN{X + (W - CW) / 2, CY0 + usedH - CHIP_H / 2, CW, CHIP_H};
                P.frost(DN, COLBG, COLRIM, (int)std::lround(CHIP_H / 2 * P.scale), 2.f);
                if (const auto G = Glyph::tex(Glyph::eGlyph::CHEVRON_DOWN, COLTITLE, (int)std::lround(14 * P.scale)))
                    P.texIn(G, CBox{DN.x + 12, DN.y, 14, CHIP_H});
                if (NUM && NUM->tex)
                    P.tex(NUM->tex, DN.x + 12 + 14 + 4, DN.y + (CHIP_H - texH(NUM, P.scale)) / 2);
                SCard c;
                c.kind = SCard::PAGE_DOWN;
                c.box  = DN;
                cards.push_back(c);
            }
        }
    }

} // namespace NAwesome::Notify
