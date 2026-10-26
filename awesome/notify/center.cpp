// awesome/notify/center.cpp — the shade: ONE list of live cards, newest first,
// Android's notification shade drawn by the compositor. There are no
// lifecycle sections and no history view: what you can see is what the model
// holds, and a dismissed card is gone (Android's shade has no recall either).
//
// This unit decides WHAT is shown and WHERE; card.cpp and stack.cpp draw
// it. The center is the banners' own column under a heading, its controls on
// glass capsules beside it; as macOS's, it draws no backdrop: the desktop is
// never darkened.
//
// RANKING — Android's, without the dividers: critical, then conversations,
// then normal, then silent; newest first inside each tier. An app's cards
// STACK under their newest once it has two (macOS), its chats with the rest
// — each chat keeps its own card, and the model merges that chat's messages
// into it. A stack stands where its newest card ranks, collapsed until a
// click spreads it.
//
// THE EXPANSION BUDGET — cards open by DEFAULT, not on demand. The walk
// starts at the top of the page and opens each card while the panel still
// has room, so the center is readable the instant it appears instead of
// costing a click per card; it folds only what would overflow. Stacks are
// the exception: macOS spreads one only when asked.
//
// This unit owns the shade's transient state (paging, every fold override) —
// all of it resets when the shade closes, Android-style.

#include "ui.hpp"

namespace NAwesome::Notify {

    // macOS stacks an app's notifications as soon as there are two
    inline constexpr size_t AUTOGROUP_AT = 2;

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

    // the budget's verdict on one card it placed
    struct SPlacedCard {
        double h    = 0;
        bool   open = false; // drawn open
        bool   more = false; // its open form shows more than the folded one (a click unfolds)
    };

    // the warm-measured layout cache (see renderCenter); dropped on close so
    // no strong SNotif refs (and their textures) outlive the visit
    static std::vector<SDisp>                    s_disp;
    static std::vector<double>                   s_itemH;
    static std::vector<uint8_t>                  s_itemSpread; // a stack shown spread out
    // per item, the cards it shows: a single's, a collapsed stack's top
    // card, a spread stack's cards down to the page's fold
    static std::vector<std::vector<SPlacedCard>> s_cardsOf;
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
        s_itemSpread.clear();
        s_cardsOf.clear();
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

    // ---- the display list: one ranked list, an app's cards stacked ----

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

    // How many stackable cards one (app, declared group) holds, and where its
    // stack landed. Different x-notify-group-key values (macOS's thread
    // identifiers) never merge: each owns its own stack and fold state.
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
        static std::vector<size_t>                     ownerOfSrc; // src[i]'s owner, -1: never stacks
        src.clear();
        owners.clear();
        ownerIdx.clear();
        ownerOfSrc.clear();
        for (const auto& N : notifs)
            if (!N->waiting && !Model::offShade(N))
                src.push_back(N);
        // notifs is newest-first; a STABLE sort by tier keeps that inside each
        std::ranges::stable_sort(src, [](const auto& a, const auto& b) { return tier(a) < tier(b); });

        for (const auto& N : src) {
            if (!Model::stackable(N)) {
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
            // the key rides the fold state, hover and selection — a stack
            // carries the (app, group) identity, singles the app
            out.push_back(SDisp{.items = {N}, .key = ownerOfSrc[i] != (size_t)-1 ? owners[ownerOfSrc[i]].key : Model::groupKeyOf(N)});
        }
        // the buffer is reused, its references are not: a dismissed card
        // (and its textures) must not live on until the shade next opens
        src.clear();
    }

    // ---- the expansion budget ----
    //
    // Walks the page from its top row down, opening what still fits. The top
    // card always opens (Android's one guarantee); a user override wins over
    // the budget in both directions. Cards past the fold are only measured
    // FOLDED — their open form would raster text no frame can show.

    // one card's verdict: `extra` is what must still fit under it (a deck's
    // depth), `force` the open it must take (an armed field, the user's
    // unfold) or must not (the user's fold)
    static SPlacedCard budgetCard(const SPaint& P, const SType& T, const SP<SNotif>& N, double w, double room, double extra, bool top, bool deck) {
        const bool  ARMED = replyArmedOn(N->id); // a field shows only on the open card
        const bool  OPEN  = ARMED || s_openedRow.contains(N->id), FOLD = !ARMED && s_foldedRow.contains(N->id);
        SPlacedCard c;
        c.h    = measureCard(P, T, N, w, SCardForm{.where = SCardForm::CENTER, .open = false, .deck = deck});
        c.more = true;
        if (!FOLD && (OPEN || top || c.h + extra < room)) {
            const double OH = measureCard(P, T, N, w, SCardForm{.where = SCardForm::CENTER, .open = true, .deck = deck});
            c.more          = OH > c.h + 0.5;
            c.open          = c.more && (OPEN || top || OH + extra <= room);
            if (c.open)
                c.h = OH;
        }
        return c;
    }

    static void runBudget(const SPaint& P, const SType& T, double contentW, double bodyCap, double gap) {
        double used = 0;
        for (size_t i = s_skip; i < s_disp.size(); i++) {
            const auto&  D    = s_disp[i];
            const double LEAD = i == s_skip ? 0 : gap;
            const bool   TOP  = i == s_skip;
            const double ROOM = bodyCap - used - LEAD;
            auto&        CS   = s_cardsOf[i];

            if (D.items.size() < 2) {
                CS.push_back(budgetCard(P, T, D.items.front(), contentW, ROOM, 0, TOP, false));
                s_itemH[i]                      = CS.back().h;
                s_rowState[D.items.front()->id] = CS.back().open;
            } else {
                // a stack: collapsed unless the user spread it, or a field
                // being typed into is one of its cards
                const auto ARMED  = std::ranges::find_if(D.items, [](const auto& N) { return replyArmedOn(N->id); });
                const bool SPREAD = ARMED != D.items.end() || (s_openedGroup.contains(D.key) && !s_foldedGroup.contains(D.key));
                if (!SPREAD) {
                    const double DEPTH = deckDepth(D.items.size());
                    CS.push_back(budgetCard(P, T, D.items.front(), contentW, ROOM, DEPTH, TOP, true));
                    s_itemH[i] = CS.back().h + DEPTH;
                } else {
                    // the header, then the cards the page holds, readable
                    // whole; the rest counted in a tail ("+N more"). One the
                    // user spread shows its first card even past the fold,
                    // and the card being replied to is never cut.
                    const size_t KEEP = ARMED != D.items.end() ? (size_t)(ARMED - D.items.begin()) : 0;
                    double       h    = CAP_H;
                    for (size_t k = 0; k < D.items.size(); k++) {
                        const bool   LAST = k + 1 == D.items.size();
                        const double TAIL = LAST ? 0 : gap + CAP_H;
                        const auto   C    = budgetCard(P, T, D.items[k], contentW, ROOM - h - gap - TAIL, 0, TOP && k == 0, false);
                        if (k > KEEP && h + gap + C.h + TAIL > ROOM)
                            break;
                        CS.push_back(C);
                        s_rowState[D.items[k]->id] = C.open;
                        h += gap + C.h;
                    }
                    if (CS.size() < D.items.size())
                        h += gap + CAP_H;
                    s_itemH[i] = h;
                }
                s_itemSpread[i]     = SPREAD;
                s_groupState[D.key] = SPREAD;
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
            // the selection follows its card (or stack) across the rebuild:
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
            s_itemSpread.assign(s_disp.size(), 0);
            s_cardsOf.assign(s_disp.size(), {});
            s_rowState.clear();
            s_groupState.clear();
            runBudget(P, T, W, BODYCAP, GAP);
            // the keyboard's card stays on the page: a step past the last one
            // pages by one, and a taller next card (or cards the new top
            // re-opened) can still leave it below the fold
            for (const int KEEP = keyboardRow(); KEEP > (int)s_skip && (size_t)KEEP > lastFit(BODYCAP, GAP);) {
                ++s_skip;
                for (auto& CS : s_cardsOf)
                    CS.clear();
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

        // ---- the heading's row: "Notifications", Do Not Disturb, Clear All ----
        // No backdrop darkens the desktop, as on macOS: the controls sit on
        // glass of their own (its Show Less capsule), and the heading bare on
        // the desktop as its stack headers do, a soft shadow keeping it
        // legible over any wallpaper or window.
        {
            if (const auto HEAD = cachedText("Notifications", COLTITLE, T.heading, (int)std::lround(W * P.scale), -1, 0, false, 600, nullptr, false, /*shadow=*/true);
                HEAD && HEAD->tex) {
                const double PAD = HEAD->pad / P.scale;
                P.tex(HEAD->tex, X + 4 - PAD, Y0 + (HEADER_H - (texH(HEAD, P.scale) - 2 * PAD)) / 2 - PAD);
            }

            constexpr double MOON_PX = 14;
            const double     CY      = Y0 + (HEADER_H - CAP_H) / 2;
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
            const auto& D = disp[IDX];
            if (IDX >= s_cardsOf.size() || s_cardsOf[IDX].empty()) { // a draw ahead of its warm: nothing measured to place
                y += IH + GAP;
                continue;
            }
            const auto& CS = s_cardsOf[IDX];
            CBox        ring{X, y, W, IH}; // the keyboard's selection goes around this
            if (D.items.size() < 2) {
                SCard card = recycledCard();
                renderCard(P, T, D.items.front(), CBox{X, y, W, 0}, SCardForm{.where = SCardForm::CENTER, .open = CS.front().open}, card);
                card.folded = !CS.front().open && CS.front().more;
                cards.push_back(std::move(card));
            } else if (!s_itemSpread[IDX]) {
                // the deck first: the top card's glass covers all but its edges
                const double DEPTH = deckDepth(D.items.size());
                ring.h             = IH - DEPTH;
                paintDeck(P, ring, D.items.size());
                SCard card = recycledCard();
                renderCard(P, T, D.items.front(), CBox{X, y, W, 0}, SCardForm{.where = SCardForm::CENTER, .open = CS.front().open, .deck = true}, card);
                card.box.h += DEPTH; // the deck answers for the stack too
                card.group = D.key;
                cards.push_back(std::move(card));
            } else {
                paintStackHead(P, T, D, CBox{X, y, W, CAP_H});
                double cy = y + CAP_H + GAP;
                for (size_t k = 0; k < CS.size() && k < D.items.size(); k++) {
                    SCard card = recycledCard();
                    renderCard(P, T, D.items[k], CBox{X, cy, W, 0}, SCardForm{.where = SCardForm::CENTER, .open = CS[k].open}, card);
                    card.folded = !CS[k].open && CS[k].more;
                    card.group  = D.key;
                    cards.push_back(std::move(card));
                    cy += CS[k].h + GAP;
                }
                if (CS.size() < D.items.size())
                    paintStackTail(P, T, D.items.size() - CS.size(), CBox{X, cy, W, CAP_H});
            }

            // the keyboard's selection: the accent around the whole item, so
            // it never reads as a hover
            if ((int)IDX == s_sel)
                P.ring(ring, COLACC, rCard(P.scale), RP, 1.5);
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
