// hyprnotify/center.cpp — the v13 shade (A·ink): ONE list of live cards,
// newest first, the 2025 Pixel notification panel drawn by the compositor.
//
// This unit decides WHAT is shown and WHERE; row.cpp draws the cards.
//
// v13 layout (docs/hyprnotify-v13-spec.md, demo is the runnable reference):
// the 380px panel sits flush under the 26px bar at a 12px screen margin,
// 15px interior padding all around; cards stack at 18px; the footer is
// [history] [muted chip] [Clear all] [DND] at 37px stadium pills; the
// history panel (opened from the footer pill) reveals the last six
// dismissed cards between the list and the footer.
//
// RANKING — Android's, without the dividers: urgent, then priority
// conversations, then conversations, then normal, then silent/snoozed;
// newest first inside each tier. An app's cards BUNDLE into one digest at
// four for automatic app/section grouping, while an app-declared group
// becomes a bundle at two. Declared groups win over automatic ones.
//
// EXPANSION is explicit and user-driven: v13 has NO system auto-expansion.
// The count/expand chip records the user's choice. The viewport may reduce
// what is presented, but never decides the state.

#include "ui.hpp"

namespace NHyprnotify {

    inline constexpr size_t AUTOGROUP_AT = 4; // automatic app/section grouping
    inline constexpr size_t DECLARED_GROUP_AT = 2; // an explicit app group

    // the history panel's measured rows (demo: header 23, items 20 stacked
    // without gap, empty state 20)
    inline constexpr double HIST_HEAD_H  = 23;
    inline constexpr double HIST_EMPTY_H = 20;

    // ---- state ----

    static bool                  s_on    = false;
    static size_t                s_skip  = 0; // wheel paging: top-level items skipped
    static size_t                s_items = 0; // items the last layout had (clamps paging)
    static std::set<uint32_t>    s_openedRow, s_foldedRow;     // explicit user row state
    static std::set<std::string> s_openedGroup, s_foldedGroup; // explicit user bundle state
    static std::set<std::string> s_kidOpen; // v13 kid-level expansion, keyed by childKey
    static Time::steady_tp       s_openedAt;
    static bool                  s_animating = false;

    static uint32_t    s_manageRow   = 0; // singleton wearing its hold menu
    static std::string s_manageGroup;    // bundle wearing its hold menu
    static Policy::eAlertingMode s_manageMode = Policy::eAlertingMode::DEFAULT; // staged until Done
    static bool                  s_manageSnoozeOpen = false;
    static int64_t                s_manageSnoozeSecs = 3600; // AOSP's config_notification_snooze_time_default

    static bool s_histOpen = false;

    // the warm-measured layout cache (see renderCenter); dropped on close so
    // no strong SNotif refs (and their textures) outlive the visit
    static std::vector<SDisp>    s_disp;
    static std::vector<double>   s_itemH;
    static std::vector<uint8_t>  s_itemOpen; // presented open state, per display item
    static double                s_firstMinH = 0;
    // resolved open state from the last warm, keyed by identity: the click
    // handlers flip THIS, so a toggle means the same thing the eye just saw
    static std::unordered_map<uint32_t, bool>    s_rowState;
    static std::unordered_map<std::string, bool> s_groupState;

    bool                       centerVisible() {
        return s_on;
    }
    bool centerAnimating() {
        return s_animating;
    }

    // Everything one visit accumulates. Closing resets paging and measured
    // layout; explicit user folds remain tied to their live target. PLUGIN_EXIT
    // clears those too. s_disp holds STRONG card refs, so a session that exits
    // with the shade open would otherwise carry them past Model::exit and
    // destroy them, textures and all, at static-destruction time with the
    // renderer already gone.
    static void resetVisit(bool teardown = false) {
        Model::snoozeEndConfirm(); // the undo rows had exactly this surface
        s_skip = s_items = 0;
        if (teardown) {
            s_openedRow.clear();
            s_foldedRow.clear();
            s_openedGroup.clear();
            s_foldedGroup.clear();
            s_kidOpen.clear();
        }
        s_rowState.clear();
        s_groupState.clear();
        s_animating       = false;
        s_manageRow       = 0;
        s_manageGroup.clear();
        s_manageMode      = Policy::eAlertingMode::DEFAULT;
        s_manageSnoozeOpen = false;
        s_manageSnoozeSecs = 3600;
        s_histOpen        = false;
        s_disp.clear();
        s_itemH.clear();
        s_itemOpen.clear();
        s_firstMinH = 0;
    }

    void centerExit() {
        s_on = false;
        inputCancelLongPress();
        resetVisit(true);
    }

    // ---- the hold menu's staged state (one target at a time) ----

    // Only one target wears the menu: two open at once would be two menus,
    // and the shade would stop being a list of notifications.
    void centerToggleManage(uint32_t id) {
        replyClose();
        if (s_manageRow == id)
            s_manageRow = 0;
        else {
            const auto N = Model::byId(id);
            if (!N)
                return;
            s_manageRow   = id;
            s_manageGroup.clear();
            s_manageMode  = Policy::mode(N->appKey, N->conversationId, !N->conversationId.empty());
        }
        s_manageSnoozeOpen = false;
        s_manageSnoozeSecs = 3600;
        notifChanged();
    }
    static bool inDisplayGroup(const SP<SNotif>& N, const std::string& groupKey) {
        return N && !N->waiting && !N->snoozed && Pixel::displayGroupKey(N->appKey, N->declaredGroupKey, N->section) == groupKey;
    }

    void centerToggleManageGroup(const std::string& groupKey) {
        if (groupKey.empty())
            return;
        replyClose();
        if (s_manageGroup == groupKey)
            s_manageGroup.clear();
        else {
            const auto IT = std::ranges::find_if(notifs, [&](const auto& N) { return inDisplayGroup(N, groupKey); });
            if (IT == notifs.end())
                return;
            s_manageGroup = groupKey;
            s_manageRow   = 0;
            s_manageMode  = Policy::mode((*IT)->appKey, {}, false);
        }
        s_manageSnoozeOpen = false;
        s_manageSnoozeSecs = 3600;
        notifChanged();
    }

    Policy::eAlertingMode centerManageMode() {
        return s_manageMode;
    }

    void centerChooseManageMode(Policy::eAlertingMode mode) {
        if (s_manageRow == 0 && s_manageGroup.empty())
            return;
        s_manageMode = mode;
        notifChanged();
    }

    bool centerManageBundle() {
        return !s_manageGroup.empty();
    }

    bool centerManageConversation() {
        if (!s_manageGroup.empty()) {
            const auto IT = std::ranges::find_if(notifs, [&](const auto& N) { return inDisplayGroup(N, s_manageGroup); });
            return IT != notifs.end() && (*IT)->conversation;
        }
        const auto N = Model::byId(s_manageRow);
        return N && N->conversation;
    }

    bool          centerManageSnoozeOpen() {
        return s_manageSnoozeOpen;
    }
    void centerToggleManageSnooze() {
        if (s_manageRow == 0 && s_manageGroup.empty())
            return;
        s_manageSnoozeOpen = !s_manageSnoozeOpen;
        notifChanged();
    }
    int64_t centerManageSnoozeSecs() {
        return s_manageSnoozeSecs;
    }
    void centerChooseManageSnooze(int64_t seconds) {
        if (s_manageRow == 0 && s_manageGroup.empty())
            return;
        s_manageSnoozeSecs = seconds;
        notifChanged();
    }

    // Done commits whatever the panel holds: the staged importance, and —
    // when the snooze list is open — the snooze itself (AOSP handleCloseControls).
    void centerCommitManage(uint32_t id, const std::string& appKey) {
        SP<SNotif> N;
        if (!appKey.empty()) {
            const auto IT = std::ranges::find_if(notifs, [&](const auto& C) { return inDisplayGroup(C, appKey); });
            if (IT != notifs.end())
                N = *IT;
        } else
            N = Model::byId(id);

        s_manageRow          = 0;
        s_manageGroup.clear();
        const bool    SNOOZE = s_manageSnoozeOpen;
        const int64_t SECS   = s_manageSnoozeSecs;
        s_manageSnoozeOpen   = false;
        s_manageSnoozeSecs   = 3600;

        if (N) {
            if (SNOOZE) {
                // committing a snooze takes the card out of the list (a
                // bundle snoozes every child it wears)
                if (appKey.empty())
                    Model::snoozeWith(N->id, SECS);
                else
                    for (const auto& C : notifs)
                        if (inDisplayGroup(C, appKey))
                            Model::snoozeWith(C->id, SECS);
            } else
                Policy::setMode(N->appKey, N->conversationId, appKey.empty() && !N->conversationId.empty(), s_manageMode);
        }
        notifChanged();
    }

    // ---- expansion state ----

    void centerToggleRow(uint32_t id) {
        replyClose();
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

    void centerToggleGroup(const std::string& groupKey) {
        if (groupKey.empty())
            return;
        replyClose();
        const auto IT = s_groupState.find(groupKey);
        if (IT != s_groupState.end() && IT->second) {
            s_openedGroup.erase(groupKey);
            s_foldedGroup.insert(groupKey);
        } else {
            s_foldedGroup.erase(groupKey);
            s_openedGroup.insert(groupKey);
        }
        notifChanged();
    }

    bool centerKidExpanded(const std::string& childKey) {
        return s_kidOpen.contains(childKey);
    }

    void centerToggleChild(const std::string& childKey) {
        if (childKey.empty())
            return;
        if (s_kidOpen.contains(childKey))
            s_kidOpen.erase(childKey);
        else
            s_kidOpen.insert(childKey);
        notifChanged();
    }

    bool centerHistOpen() {
        return s_histOpen;
    }

    void centerToggleHistory() {
        s_histOpen = !s_histOpen;
        notifChanged();
    }

    void centerHistoryClear() {
        Model::clearHistory();
        notifChanged();
    }

    void centerPage(int dir) {
        if (s_items <= 1)
            return;
        replyClose();
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

    // ---- the display list: one ranked list, explicit groups at two, automatic at four ----

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
        return !n->snoozed && !n->appKey.empty();
    }

    // How many bundleable cards one app holds, and where its digest landed.
    // Even at the model cap the distinct app keys are a handful, so one linear
    // scan beats the two string-keyed trees this used to build from scratch on
    // every warm AND every draw.
    struct SOwner {
        std::string key;
        size_t      count      = 0;
        size_t      first      = (size_t)-1; // index in out, once one is placed
        bool        classified = false;
        bool        declared   = false;
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
        // A replacement keeps its popup slot stable, but it refreshes `arrived`.
        // The shade ranks that updated card as newest inside its tier.
        std::ranges::stable_sort(src, [](const auto& a, const auto& b) {
            const int AT = tier(a), BT = tier(b);
            return AT != BT ? AT < BT : a->arrived > b->arrived;
        });

        const auto ownerOf = [](const SP<SNotif>& N) {
            const auto KEY = Pixel::displayGroupKey(N->appKey, N->declaredGroupKey, N->section);
            for (size_t i = 0; i < owners.size(); i++)
                if (owners[i].key == KEY)
                    return i;
            owners.push_back({.key = KEY,
                              .classified = N->declaredGroupKey.empty() && Pixel::classifiedSection(N->section),
                              .declared = !N->declaredGroupKey.empty()});
            return owners.size() - 1;
        };

        for (const auto& N : src)
            if (bundleable(N))
                owners[ownerOf(N)].count++;

        for (const auto& N : src) {
            if (bundleable(N)) {
                auto& O = owners[ownerOf(N)];
                const size_t THRESHOLD = O.declared ? DECLARED_GROUP_AT : AUTOGROUP_AT;
                if (O.count >= THRESHOLD) {
                    if (O.first != (size_t)-1) {
                        out[O.first].items.push_back(N);
                        continue;
                    }
                    O.first = out.size();
                    out.push_back(SDisp{.items = {N}, .key = O.key, .classified = O.classified});
                    continue;
                }
            }
            out.push_back(SDisp{.items = {N}});
        }
    }

    // The kid keys follow row.cpp's: "<cardId>|s:<sender>" (group kids),
    // "<cardId>|m:<msgId>" (1:1 kids), "<leadId>|c:<childId>" (bundle kids).
    // Expansion survives page turns and collapses; it dies with its card.
    static void pruneDisplayState() {
        std::set<uint32_t>    rows;
        std::set<std::string> groups, kids;
        for (const auto& D : s_disp) {
            for (const auto& N : D.items) {
                rows.insert(N->id);
                if (N->conversation) {
                    // ROM contract: one child row per message for ALL conversation types
                    // (2026-08-26 audit: key format must match convKids())
                    for (const auto& M : N->messages)
                        kids.insert(std::to_string(N->id) + "|m:" + M.id);
                }
            }
            if (D.items.size() >= 2) {
                groups.insert(D.key);
                const uint32_t LEAD = D.items.front()->id;
                for (const auto& N : D.items)
                    kids.insert(std::to_string(LEAD) + "|c:" + std::to_string(N->id));
            }
        }
        std::erase_if(s_openedRow, [&](uint32_t id) { return !rows.contains(id); });
        std::erase_if(s_foldedRow, [&](uint32_t id) { return !rows.contains(id); });
        std::erase_if(s_openedGroup, [&](const auto& key) { return !groups.contains(key); });
        std::erase_if(s_foldedGroup, [&](const auto& key) { return !groups.contains(key); });
        std::erase_if(s_kidOpen, [&](const auto& key) { return !kids.contains(key); });
        if (s_manageRow && !rows.contains(s_manageRow))
            s_manageRow = 0;
        if (!s_manageGroup.empty() && !groups.contains(s_manageGroup))
            s_manageGroup.clear();
    }

    // State is independent of viewport height. The viewport may reduce what
    // is presented, but never promotes a collapsed card.
    static void runLayout(const SPaint& P, const SType& T, double contentW, double bodyCap) {
        if (bodyCap <= 0)
            return;
        static const std::vector<double> NOCHILD; // the per-kid heights are the card engine's own record
        for (size_t i = s_skip; i < s_disp.size(); i++) {
            const auto& D = s_disp[i];

            if (D.items.size() < 2 && confirming(D.items.front())) {
                // fixed, and never folded: an undo row has one state
                s_itemH[i]    = snoozeRowH();
                s_itemOpen[i] = 0;
            } else if (D.items.size() < 2 && D.items.front()->id == s_manageRow) {
                s_itemH[i]    = holdMenuH();
                s_itemOpen[i] = 0;
            } else if (D.items.size() >= 2 && D.key == s_manageGroup) {
                s_itemH[i]    = holdMenuH();
                s_itemOpen[i] = 0;
            } else if (D.items.size() < 2) {
                const auto&     N    = D.items.front();
                const eCardKind KIND = N->conversation ? eCardKind::CONV : eCardKind::PLAIN;
                const bool FORCE_OPEN = s_openedRow.contains(N->id), FORCE_FOLD = s_foldedRow.contains(N->id);
                // an armed reply lives inside the newest expanded kid, so it
                // keeps its card open until it is closed or folded
                const bool want = FORCE_OPEN || (KIND == eCardKind::CONV && replyArmedOn(N->id) && !FORCE_FOLD);
                const double CH = measureCard(P, T, N, contentW, false, KIND);
                const double OH = want ? measureCard(P, T, N, contentW, true, KIND) : 0;
                const bool   PRESENT_OPEN = want && OH <= bodyCap;
                s_itemH[i]          = PRESENT_OPEN ? OH : CH;
                s_itemOpen[i]       = PRESENT_OPEN;
                s_rowState[N->id]   = PRESENT_OPEN;
            } else {
                const bool FORCE_OPEN = s_openedGroup.contains(D.key), FORCE_FOLD = s_foldedGroup.contains(D.key);
                const bool want       = FORCE_OPEN && !FORCE_FOLD;
                const double DH = measureBundle(P, T, D, contentW, false, NOCHILD);
                const double OH = want ? measureBundle(P, T, D, contentW, true, NOCHILD) : 0;
                const bool   PRESENT_OPEN = want && OH <= bodyCap;
                s_itemH[i]          = PRESENT_OPEN ? OH : DH;
                s_itemOpen[i]       = PRESENT_OPEN;
                s_groupState[D.key] = PRESENT_OPEN;
            }
        }
    }

    static double firstPresentationH(const SPaint& P, const SType& T, double contentW) {
        if (s_disp.empty() || s_skip >= s_disp.size())
            return 0;
        const auto& D = s_disp[s_skip];
        if (D.items.size() < 2 && confirming(D.items.front()))
            return snoozeRowH();
        if (D.items.size() < 2 && D.items.front()->id == s_manageRow)
            return holdMenuH();
        if (D.items.size() >= 2 && D.key == s_manageGroup)
            return holdMenuH();
        if (D.items.size() < 2) {
            const auto N = D.items.front();
            return measureCard(P, T, N, contentW, false, N->conversation ? eCardKind::CONV : eCardKind::PLAIN);
        }
        static const std::vector<double> NOCHILD;
        return measureBundle(P, T, D, contentW, false, NOCHILD);
    }

    // the dismissed cards the history panel shows: the last six, newest first
    static size_t histShown() {
        return std::min<size_t>(6, Model::history().size());
    }
    static double historyPanelH() {
        const auto N = histShown();
        return HIST_PT + HIST_HEAD_H + HIST_GAP + (N ? N * HIST_ITEM_H : HIST_EMPTY_H) + HIST_PB;
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
                P.dy    -= (1.0 - easeOutBack(AT)) * 6.0;
            }
        }

        const auto  MB     = P.mon->logicalBox();
        const int   RPANEL = rPanel(P.scale);
        const float RP     = rPow();

        const double MIN_PANEL = PANEL_PAD * 2 + FOOTER_H;
        const double OFFSET    = std::clamp((double)cfg.offsetY->value(), 0.0, std::max(0.0, MB.h - MIN_PANEL - EDGE));
        // The AOSP shade spans the screen: full width, 16px margins each side.
        const double PANEL_W   = std::max(1.0, MB.w - 2 * EDGE);
        const double X         = MB.x + MB.w - EDGE - PANEL_W;
        const double Y0        = MB.y + OFFSET;

        const double CONTENT_W = std::max(1.0, PANEL_W - 2 * PANEL_PAD);
        const double CONTENT_X = X + PANEL_PAD;

        // The history block sizes the panel before the cards are capped, so
        // the footer never falls off-screen. Flexbox does not collapse the
        // panel's margins: the panel's own 8px bottom margin PLUS the
        // footer's 20px top margin (demo-measured).
        const double HISTH    = s_histOpen ? historyPanelH() : 0;
        const double HISTGAP  = s_histOpen ? 8.0 + FOOTER_MT : 0;
        const double FOOT     = FOOTER_MT + FOOTER_H;
        const double BASE_AVAILH  = std::max(0.0, MB.y + MB.h - Y0 - EDGE - std::max((double)cfg.margin->value(), 0.0));
        const double BASE_BODYCAP = std::max(0.0, BASE_AVAILH - PANEL_PAD - HISTH - HISTGAP - FOOT - PANEL_PAD);
        if (!s_disp.empty() && s_firstMinH > 0)
            centerOsdReserve = std::min(centerOsdReserve, std::max(0.0, BASE_BODYCAP - s_firstMinH));
        const double AVAILH  = std::max(0.0, BASE_AVAILH - centerOsdReserve);
        const double BODYCAP = std::max(0.0, AVAILH - PANEL_PAD - HISTH - HISTGAP - FOOT - PANEL_PAD);

        // The display list, every height AND every fold verdict are decided
        // once per WARM and reused by the draws between warms: hover fills
        // change nothing they depend on, and every model/fold change warms
        // first (notifChanged). The draw side lays out without measuring twice.
        if (P.warm) {
            buildDisplay(s_disp);
            // The popup path requests these in its own warm block (popups.cpp);
            // rows born straight into an open shade never pass through it, so
            // their participant avatars would never build and every lead icon
            // would fall back to the app-initial disc.
            for (const auto& D : s_disp)
                for (const auto& N : D.items)
                    ensureConversationIcons(*N, (int)std::lround(CARD_ICON_D * P.scale));
            s_skip = s_disp.empty() ? 0 : std::min(s_skip, s_disp.size() - 1);
            s_itemH.assign(s_disp.size(), 0.0);
            s_itemOpen.assign(s_disp.size(), 0);
            s_rowState.clear();
            s_groupState.clear();
            pruneDisplayState();
            s_firstMinH = firstPresentationH(P, T, CONTENT_W);
        }
        const auto& disp = s_disp;
        s_items          = disp.size();
        s_skip           = disp.empty() ? 0 : std::min(s_skip, disp.size() - 1);

        // An OSD may shorten the shade, but it cannot consume the first real
        // notification. Cap its reservation to the room left after the first
        // collapsed presentation; the OSD then starts below the resulting
        // panel and may naturally clip on an unusually short output.
        if (P.warm)
            runLayout(P, T, CONTENT_W, BODYCAP);

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
            if (s_itemH[i] <= 0 || s_itemH[i] > BODYCAP)
                break;
            if (!placed.empty() && usedH + LEAD + s_itemH[i] > BODYCAP)
                break;
            usedH += LEAD + s_itemH[i];
            placed.push_back({i, s_itemH[i]});
        }
        const bool EMPTY = disp.empty();

        // the empty state (demo: "No new notifications", on-40, 13px)
        const double EMPTY_LINE = 18;
        const double CONTENTH   = EMPTY ? EMPTY_PT + EMPTY_LINE + EMPTY_PB : usedH;

        const double PANELH = std::min(std::max(0.0, MB.y + MB.h - Y0 - EDGE - centerOsdReserve),
                                       PANEL_PAD + CONTENTH + HISTH + HISTGAP + FOOT + PANEL_PAD);
        const CBox   PANEL{X, Y0, PANEL_W, std::max(0.0, PANELH)};

        P.shadow(PANEL, RPANEL, RP, 22);
        P.glass(PANEL, v13Panel(), RPANEL, RP); // the panel veil: frost by default, opaque when theme = "ink"
        {
            SCard pc;
            pc.kind = SCard::PANEL;
            pc.box  = PANEL;
            cards.push_back(pc);
        }

        double y = Y0 + PANEL_PAD;

        if (EMPTY) {
            const auto E = cachedText("No new notifications", v13On40(), T.body, (int)(std::max(1.0, PANEL_W - 2 * (PANEL_PAD + HIST_INSET)) * P.scale), -1, 0, false, 400);
            if (!P.warm && E && E->tex)
                P.tex(E->tex, X + (PANEL_W - E->tex->m_size.x / P.scale) / 2, y + EMPTY_PT + (EMPTY_LINE - E->tex->m_size.y / P.scale) / 2);
            y += EMPTY_PT + EMPTY_LINE + EMPTY_PB;
        }

        bool first = true;
        for (const auto& [IDX, IH] : placed) {
            const auto& D  = disp[IDX];
            const bool  OPEN = IDX < s_itemOpen.size() && s_itemOpen[IDX];
            if (!first)
                y += STACK_GAP;
            first = false;

            const CBox SLOT{CONTENT_X, y, CONTENT_W, IH};
            if (D.items.size() < 2 && confirming(D.items.front()))
                paintSnoozeRow(P, T, D.items.front(), SLOT);
            else if (D.items.size() < 2 && D.items.front()->id == s_manageRow)
                paintHoldMenu(P, T, D.items.front(), SLOT, std::string{});
            else if (D.items.size() >= 2 && D.key == s_manageGroup)
                paintHoldMenu(P, T, D.items.front(), SLOT, D.key);
            else if (D.items.size() < 2)
                paintCard(P, T, D.items.front(), SLOT, OPEN, false, D.items.front()->conversation ? eCardKind::CONV : eCardKind::PLAIN);
            else {
                static const std::vector<double> NOCHILD;
                paintBundle(P, T, D, SLOT, OPEN, NOCHILD);
            }

            y += IH;
        }

        // ---- the history panel (between the list and the footer) ----
        if (s_histOpen) {
            const double HBW = std::max(1.0, PANEL_W - 2 * (PANEL_PAD + HIST_INSET));
            const CBox   HB{X + PANEL_PAD + HIST_INSET, y, HBW, HISTH};
            P.rect(HB, v13Card(), RPANEL, RP);

            const auto HT = cachedText("Notification history", v13On(), T.bar, (int)((HBW - 2 * HIST_PX - 40) * P.scale), linePx(T.bar), 0, false, 600);
            if (!P.warm && HT && HT->tex)
                P.tex(HT->tex, HB.x + HIST_PX, HB.y + HIST_PT + (HIST_HEAD_H - HT->tex->m_size.y / P.scale) / 2);

            // Push the swallow-card BEFORE the Clear button: cardAt resolves
            // hits in reverse push order (last wins), so the button must be the
            // later card or the panel body would shadow it and swallow the click.
            SCard hb;
            hb.kind = SCard::HIST_BOX;
            hb.box  = HB;
            cards.push_back(hb);

            const auto& HIST = Model::history();
            if (!HIST.empty()) {
                // the Clear button: action color, an 8px corner on hover
                const double CW = texW(cachedText("Clear", v13Action(), T.small, 128, linePx(T.small), 0, false, 500), P.scale) + 16;
                const CBox   CB{HB.x + HB.w - HIST_PX - CW, HB.y + (HIST_PT + (HIST_HEAD_H - 24) / 2), CW, 24};
                if (!P.warm) {
                    const bool HOV = hovered.kind == SCard::HIST_CLEAR;
                    if (HOV)
                        P.rect(CB, v13RaisedH(), (int)std::lround(8 * P.scale), 2.f);
                    const auto CT = cachedText("Clear", v13Action(), T.small, 128, linePx(T.small), 0, false, 500);
                    if (CT && CT->tex)
                        P.tex(CT->tex, CB.x + 8, CB.y + (24 - CT->tex->m_size.y / P.scale) / 2);
                }
                SCard cc;
                cc.kind = SCard::HIST_CLEAR;
                cc.box  = CB;
                cards.push_back(cc);
            }

            const size_t SHOWN = histShown();
            // Built OUTSIDE the paint guard: the warm pass owns texture
            // creation, the render pass only paints. A build trapped in the
            // !warm block misses mayBuild every frame (never caching, never
            // painting) and spin-rewarms the shade — the footer's empty
            // buttons and this sheet's invisible rows were that bug.
            const auto EM = SHOWN == 0 ? cachedText("No dismissed notifications.", v13On60(), T.body, (int)((HBW - 2 * HIST_PX) * P.scale), linePx(T.body), 0, false, 400) : nullptr;
            if (!P.warm && EM && EM->tex)
                P.tex(EM->tex, HB.x + HIST_PX, HB.y + HIST_PT + HIST_HEAD_H + HIST_GAP + (HIST_EMPTY_H - EM->tex->m_size.y / P.scale) / 2);
            if (SHOWN > 0) {
                double iy = HB.y + HIST_PT + HIST_HEAD_H + HIST_GAP;
                for (size_t k = 0; k < SHOWN; k++) {
                    const auto& E = HIST[HIST.size() - 1 - k]; // newest first
                    const auto  I = historyIcon(E.iconSource, (int)std::lround(HIST_ICON_D * P.scale));
                    const auto  G = I ? nullptr : controlIcon(eControlIcon::NOTIFICATION_ALERT, (int)std::lround(15 * P.scale), v13On60());
                    // the app keeps 45% of the line, the title the rest
                    const double TW = HBW - 2 * HIST_PX - HIST_ICON_D - 9;
                    const auto   A  = cachedText(E.app, v13On82(), T.body, (int)(0.45 * TW * P.scale), linePx(T.body), 0, false, 400);
                    const auto   B  = cachedText(E.title, v13On60(), T.body, (int)((TW - 0.45 * TW - 6) * P.scale), linePx(T.body), 0, false, 400);
                    if (!P.warm) {
                        if (I)
                            P.texFit(I, CBox{HB.x + HIST_PX, iy + (HIST_ITEM_H - HIST_ICON_D) / 2, HIST_ICON_D, HIST_ICON_D}, (int)std::lround(HIST_ICON_D / 2 * P.scale), 2.f);
                        else if (G)
                            P.texFit(G, CBox{HB.x + HIST_PX + (HIST_ICON_D - 15) / 2, iy + (HIST_ITEM_H - 15) / 2, 15, 15}, 0);
                        const double AX = HB.x + HIST_PX + HIST_ICON_D + 9;
                        if (A && A->tex)
                            P.tex(A->tex, AX, iy + (HIST_ITEM_H - A->tex->m_size.y / P.scale) / 2);
                        if (B && B->tex)
                            P.tex(B->tex, AX + 0.45 * TW + 6, iy + (HIST_ITEM_H - B->tex->m_size.y / P.scale) / 2);
                    }
                    iy += HIST_ITEM_H;
                }
            }
            y += HISTH + 8; // the panel's own bottom margin; the footer's 20 adds below
        }

        // ---- the footer: [history] [muted chip] [Clear all] [DND] ----
        const double FOOTY = Y0 + PANELH - PANEL_PAD - FOOTER_H;
        double       bx    = X + PANEL_PAD;
        const double DNDX  = X + PANEL_W - PANEL_PAD - FOOTER_PILL_W;

        { // the history pill
            const CBox B{bx, FOOTY, FOOTER_PILL_W, FOOTER_H};
            // The build must run in the warm pass too: a texture created
            // during render paints nothing, and a draw-side miss only
            // schedules a rewarm — so building INSIDE the !warm guard could
            // never succeed (the footer sat empty forever).
            const bool HOV = hovered.kind == SCard::BTN_HISTORY;
            const auto G   = controlIcon(eControlIcon::HISTORY, (int)std::lround(FOOTER_ICON * P.scale), HOV ? v13On() : v13On82());
            if (!P.warm) {
                P.rect(B, HOV ? v13RaisedH() : v13Raised(), (int)std::lround(FOOTER_R * P.scale), RP);
                if (G)
                    P.texFit(G, CBox{B.x + (B.w - FOOTER_ICON) / 2, B.y + (B.h - FOOTER_ICON) / 2, FOOTER_ICON, FOOTER_ICON}, 0);
            }
            SCard c;
            c.kind = SCard::BTN_HISTORY;
            c.box  = B;
            cards.push_back(c);
            bx += FOOTER_PILL_W + FOOTER_GAP;
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
            const auto REST = cachedText(SB, v13On60(), T.small, 64, -1, 0, false, 600);
            const auto HOT  = cachedText("Unmute all", v13Action(), T.small, 200, -1, 0, false, 600);
            const bool HOV  = hovered.kind == SCard::BTN_RULES;
            // the wider of the two: the chip must not resize under the pointer
            const double CW = std::max(texW(REST, P.scale), texW(HOT, P.scale)) + 18;
            const CBox   B{bx, FOOTY, CW, FOOTER_H};
            if (!P.warm) {
                P.rect(B, HOV ? v13RaisedH() : v13Raised(), (int)std::lround(FOOTER_R * P.scale), RP);
                if (const auto* L = HOV ? HOT : REST; L && L->tex)
                    P.tex(L->tex, B.x + (B.w - L->tex->m_size.x / P.scale) / 2, B.y + (B.h - L->tex->m_size.y / P.scale) / 2);
            }
            SCard c;
            c.kind = SCard::BTN_RULES;
            c.box  = B;
            cards.push_back(c);
            bx += CW + FOOTER_GAP;
        }

        { // "Clear all" — the AOSP footer's compact centered pill; greys when
            // the shade is empty. A fixed 120-logical-px raster bound keeps
            // the cache address stable while the pill itself hugs the text.
            const int CLR_B = (int)std::lround(120 * P.scale);
            const auto   REST   = cachedText("Clear all", v13On(), T.bar, CLR_B, -1, 0, false, 500);
            const auto   REST_D = cachedText("Clear all", v13On40(), T.bar, CLR_B, -1, 0, false, 500);
            const double CLEAR_W = std::max(texW(REST, P.scale), texW(REST_D, P.scale)) + 2 * 18;
            double       CLR_X   = X + (PANEL_W - CLEAR_W) / 2;
            if (CLR_X < bx + FOOTER_GAP) // the muted chip can crowd the center on a narrow monitor
                CLR_X = std::min(bx + FOOTER_GAP, DNDX - FOOTER_GAP - CLEAR_W);

            const bool TARGET = std::ranges::any_of(notifs, [](const auto& N) { return !N->waiting && !N->snoozed && !inOsdBand(N->id); });
            const CBox B{CLR_X, FOOTY, CLEAR_W, FOOTER_H};
            if (!P.warm) {
                const bool HOV = hovered.kind == SCard::BTN_CLEAR;
                P.rect(B, HOV && TARGET ? v13RaisedH() : v13Raised(), (int)std::lround(FOOTER_R * P.scale), RP);
                const auto* L = TARGET ? REST : REST_D;
                if (L && L->tex)
                    P.tex(L->tex, B.x + (B.w - L->tex->m_size.x / P.scale) / 2, B.y + (B.h - L->tex->m_size.y / P.scale) / 2);
            }
            SCard c;
            c.kind = SCard::BTN_CLEAR;
            c.box  = B;
            cards.push_back(c);
        }

        { // do-not-disturb: the accent fill while it is on
            const CBox B{DNDX, FOOTY, FOOTER_PILL_W, FOOTER_H};
            const bool LIT = Model::suspendedNow();
            const bool HOV = hovered.kind == SCard::BTN_DND;
            // built outside the paint guard: the warm pass owns creation (see
            // the history pill above)
            const auto G = controlIcon(eControlIcon::DND_BELL_GEAR, (int)std::lround(22 * P.scale), LIT ? v13OnAccent() : (HOV ? v13On() : v13On82()));
            if (!P.warm) {
                P.rect(B, LIT ? v13Accent() : (HOV ? v13RaisedH() : v13Raised()), (int)std::lround(FOOTER_R * P.scale), RP);
                if (G)
                    P.texFit(G, CBox{B.x + (B.w - 22) / 2, B.y + (B.h - 22) / 2, 22, 22}, 0);
            }
            SCard c;
            c.kind = SCard::BTN_DND;
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
                const auto U = cachedText(UB, v13On60(), T.small, 128, -1, 0, false, 500);
                if (!P.warm && U && U->tex)
                    P.tex(U->tex, X + (PANEL_W - U->tex->m_size.x / P.scale) / 2, Y0 + 3);
            }
            if (below > 0 && !placed.empty() && !s_histOpen) {
                auto& DB = scratch();
                DB += std::to_string(below);
                DB += " more";
                const auto D2 = cachedText(DB, v13On60(), T.small, 128, -1, 0, false, 500);
                if (!P.warm && D2 && D2->tex) {
                    const double cw = D2->tex->m_size.x / P.scale;
                    const double cx = X + (PANEL_W - cw) / 2, cy = Y0 + PANEL_PAD + usedH + 4;
                    P.tex(D2->tex, cx, cy);
                }
            }
        }

        lastContentH = PANELH;
        lastContentW = PANEL_W;
    }

} // namespace NHyprnotify
