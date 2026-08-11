// hyprnotify/center.cpp — the shade: ONE list of live cards, newest first,
// Android's notification shade drawn by the compositor. There are no
// lifecycle sections and no history view: what you can see is what the model
// holds, and a dismissed card is gone (Android's shade has no recall either).
//
// This unit decides WHAT is shown and WHERE; row.cpp draws it.
//
// RANKING — Android's, without the dividers: critical, then conversations,
// then normal, then silent; newest first inside each tier. An app's cards
// BUNDLE into one digest at four, matching the target Pixel GroupHelper.
// Declared groups win over automatic app/section groups. Conversation cards
// remain distinct children: conversation identity and group identity are
// separate contracts.
//
// EXPANSION is explicit. The top ranked item receives deterministic system
// expansion; a count/expand pill records user expansion or collapse. The
// viewport can reduce visible children, but never decides the state.
//
// This unit owns the shade's transient state. Paging and measured layout reset
// when the shade closes; explicit user expansion persists while its target is
// alive and is pruned when that row/group disappears.

#include "ui.hpp"

namespace NHyprnotify {

    inline constexpr size_t AUTOGROUP_AT = 4; // AOSP bundles at 4 notifications from same app

    // ---- state ----

    static bool                  s_on    = false;
    static size_t                s_skip  = 0; // wheel paging: top-level items skipped
    static size_t                s_items = 0; // items the last layout had (clamps paging)
    static std::set<uint32_t>    s_openedRow, s_foldedRow;     // explicit user row state
    static std::set<std::string> s_openedGroup, s_foldedGroup; // explicit user group state
    static Time::steady_tp       s_openedAt;
    static bool                  s_animating = false;

    static uint32_t    s_manageRow   = 0; // singleton wearing its manage panel
    static std::string s_manageGroup;    // bundle wearing its manage panel
    static Policy::eAlertingMode s_manageMode = Policy::eAlertingMode::DEFAULT; // staged until Done

    // the warm-measured layout cache (see renderCenter); dropped on close so
    // no strong SNotif refs (and their textures) outlive the visit
    static std::vector<SDisp>               s_disp;
    static std::vector<double>              s_itemH;
    static std::vector<uint8_t>             s_itemOpen; // presented open state, per display item
    static std::vector<uint8_t>             s_itemMore; // the open form shows more than the collapsed one
    static std::vector<std::vector<double>> s_childH;
    static double                           s_firstMinH = 0;
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

    // Everything one visit accumulates. Closing resets paging and measured
    // layout; explicit user folds remain tied to their live target. PLUGIN_EXIT
    // clears those too. s_disp
    // holds STRONG card refs, so a session that exits with the shade open
    // would otherwise carry them past Model::exit and destroy them, textures
    // and all, at static-destruction time with the renderer already gone.
    static void resetVisit(bool teardown = false) {
        Model::snoozeEndConfirm(); // the undo rows had exactly this surface
        s_skip = s_items = 0;
        if (teardown) {
            s_openedRow.clear();
            s_foldedRow.clear();
            s_openedGroup.clear();
            s_foldedGroup.clear();
        }
        s_rowState.clear();
        s_groupState.clear();
        s_animating = false;
        s_manageRow = 0;
        s_manageGroup.clear();
        s_manageMode = Policy::eAlertingMode::DEFAULT;
        s_disp.clear();
        s_itemH.clear();
        s_itemOpen.clear();
        s_itemMore.clear();
        s_childH.clear();
        s_firstMinH = 0;
    }

    void centerExit() {
        s_on = false;
        inputCancelLongPress();
        resetVisit(true);
    }

    // Only one target wears the panel: two open at once would be two menus,
    // and the shade would stop being a list of notifications.
    void centerToggleManage(uint32_t id) {
        replyClose();
        if (s_manageRow == id)
            s_manageRow = 0;
        else {
            const auto N = Model::byId(id);
            if (!N)
                return;
            s_manageRow = id;
            s_manageGroup.clear();
            s_manageMode = Policy::mode(N->appKey, N->conversationId, !N->conversationId.empty());
        }
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

    void centerCommitManage(uint32_t id, const std::string& groupKey) {
        SP<SNotif> N;
        if (!groupKey.empty()) {
            const auto IT = std::ranges::find_if(notifs, [&](const auto& C) { return inDisplayGroup(C, groupKey); });
            if (IT != notifs.end())
                N = *IT;
        } else
            N = Model::byId(id);

        s_manageRow = 0;
        s_manageGroup.clear();
        if (N)
            Policy::setMode(N->appKey, N->conversationId, groupKey.empty() && !N->conversationId.empty(), s_manageMode);
        notifChanged();
    }
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

    // ---- the display list: one ranked list, declared/automatic groups at two ----

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
            owners.push_back({.key = KEY, .classified = N->declaredGroupKey.empty() && Pixel::classifiedSection(N->section)});
            return owners.size() - 1;
        };

        for (const auto& N : src)
            if (bundleable(N))
                owners[ownerOf(N)].count++;

        for (const auto& N : src) {
            if (bundleable(N)) {
                auto& O = owners[ownerOf(N)];
                if (O.count >= AUTOGROUP_AT) {
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

    static void pruneDisplayState() {
        std::set<uint32_t>    rows;
        std::set<std::string> groups;
        for (const auto& D : s_disp) {
            for (const auto& N : D.items)
                rows.insert(N->id);
            if (D.items.size() >= 2)
                groups.insert(D.key);
        }
        std::erase_if(s_openedRow, [&](uint32_t id) { return !rows.contains(id); });
        std::erase_if(s_foldedRow, [&](uint32_t id) { return !rows.contains(id); });
        std::erase_if(s_openedGroup, [&](const auto& key) { return !groups.contains(key); });
        std::erase_if(s_foldedGroup, [&](const auto& key) { return !groups.contains(key); });
        if (s_manageRow && !rows.contains(s_manageRow))
            s_manageRow = 0;
        if (!s_manageGroup.empty() && !groups.contains(s_manageGroup))
            s_manageGroup.clear();
    }

    // State is independent of viewport height. The viewport may reduce the
    // number of painted children, but never promotes a collapsed group.
    static void runLayout(const SPaint& P, const SType& T, double contentW, double bodyCap) {
        if (bodyCap <= 0)
            return;
        for (size_t i = s_skip; i < s_disp.size(); i++) {
            const auto&  D    = s_disp[i];
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
                const double OH    = measureRow(P, T, N, contentW, true, ROW_SINGLE);
                const bool   more  = OH > CH + 0.5;
                const auto   STATE = FORCE_OPEN ? Pixel::eExpansion::USER_EXPANDED : (!FORCE_FOLD && TOP ? Pixel::eExpansion::SYSTEM_EXPANDED : Pixel::eExpansion::COLLAPSED);
                const bool   open  = STATE != Pixel::eExpansion::COLLAPSED;
                const bool   PRESENT_OPEN = open && OH <= bodyCap;
                const double h            = PRESENT_OPEN ? OH : CH;
                s_itemH[i]        = h;
                s_itemOpen[i]             = PRESENT_OPEN;
                s_itemMore[i]     = more;
                s_rowState[N->id]         = PRESENT_OPEN;
            } else {
                const size_t PREVIEW    = Pixel::maxVisibleChildren(D.classified, Pixel::eExpansion::COLLAPSED);
                const double DH         = digestH(T, std::min(PREVIEW, D.items.size()), P.scale);
                const bool   FORCE_OPEN = s_openedGroup.contains(D.key), FORCE_FOLD = s_foldedGroup.contains(D.key);
                const auto   STATE      = FORCE_OPEN ? Pixel::eExpansion::USER_EXPANDED : (!FORCE_FOLD && TOP ? Pixel::eExpansion::SYSTEM_EXPANDED : Pixel::eExpansion::COLLAPSED);
                const bool   open       = STATE != Pixel::eExpansion::COLLAPSED;
                double       h          = DH;
                if (open) {
                    double              oh = groupHeadH();
                    std::vector<double> ch;
                    bool                FIT   = true;
                    const size_t        LIMIT = std::min(D.items.size(), Pixel::maxVisibleChildren(D.classified, STATE));
                    ch.reserve(LIMIT);
                    for (size_t child = 0; child < LIMIT; child++) {
                        const auto&  N = D.items[child];
                        const double C = measureRow(P, T, N, contentW, true, ROW_CHILD, true); // expanded children are always open
                        if (oh + CHILD_GAP + C > bodyCap) {
                            if (ch.empty())
                                FIT = false;
                            break;
                        }
                        ch.push_back(C);
                        oh += CHILD_GAP + C;
                    }
                    if (FIT) {
                        h           = oh;
                        s_childH[i] = std::move(ch);
                    } else {
                        h = DH;
                    }
                }
                const bool PRESENT_OPEN = open && !s_childH[i].empty() && h <= bodyCap;
                s_itemH[i]              = PRESENT_OPEN ? h : DH;
                s_itemOpen[i]           = PRESENT_OPEN;
                s_itemMore[i]       = 1;
                s_groupState[D.key]     = PRESENT_OPEN;
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
            return managePanelH(D.items.front());
        if (D.items.size() >= 2 && D.key == s_manageGroup)
            return managePanelH(D.items.front(), D.key);
        if (D.items.size() < 2)
            return measureRow(P, T, D.items.front(), contentW, false, ROW_SINGLE);
        const size_t PREVIEW = std::min(D.items.size(), Pixel::maxVisibleChildren(D.classified, Pixel::eExpansion::COLLAPSED));
        return digestH(T, PREVIEW, P.scale);
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
        const double OFFSET    = std::clamp((double)cfg.offsetY->value(), 0.0, std::max(0.0, MB.h - MIN_PANEL - EDGE));
        const double PANEL_W   = std::max(1.0, std::min(CENTER_W, MB.w - 2 * EDGE));
        const double X         = MB.x + MB.w - EDGE - PANEL_W;
        const double Y0        = MB.y + OFFSET;

        const double CONTENT_W = std::max(1.0, PANEL_W - 2 * BODY_PADX);
        const double CONTENT_X = X + (PANEL_W - CONTENT_W) / 2;

        // The shade runs to what the monitor leaves below offset_y (a margin of
        // air) — Android's shade is the screen, and explicit presentation uses
        // exactly this. Overflow past it becomes wheel paging, never off-screen
        // bleed; renderRow caps a row's body at 4 lines (7 for a chat), so no
        // single row can exceed the cap and the always-place-the-first-row rule
        // can't spill.
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
            pruneDisplayState();
            s_firstMinH = firstPresentationH(P, T, CONTENT_W);
        }
        const auto& disp = s_disp;
        s_items          = disp.size();
        s_skip           = disp.empty() ? 0 : std::min(s_skip, disp.size() - 1);

        // An OSD may shorten the shade, but it cannot consume the first real
        // notification. Cap its reservation to the room left after the first
        // collapsed/management presentation; the OSD then starts below the
        // resulting panel and may naturally clip on an unusually short output.
        const double BASE_AVAILH  = std::max(0.0, MB.y + MB.h - Y0 - EDGE - std::max((double)cfg.margin->value(), 0.0));
        const double BASE_BODYCAP = std::max(0.0, BASE_AVAILH - BAR_H - BODY_PADT - BODY_PADB);
        if (!disp.empty() && s_firstMinH > 0)
            centerOsdReserve = std::min(centerOsdReserve, std::max(0.0, BASE_BODYCAP - s_firstMinH));
        const double AVAILH  = std::max(0.0, BASE_AVAILH - centerOsdReserve);
        const double BODYCAP = std::max(0.0, AVAILH - BAR_H - BODY_PADT - BODY_PADB);
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
        const bool   EMPTY  = disp.empty();
        const double EMPTYH = 46;
        const double BODYH  = EMPTY ? std::min(EMPTYH, BODYCAP) : usedH;
        const double PANELH = std::min(MB.y + MB.h - Y0 - EDGE - centerOsdReserve, BODY_PADT + BODYH + BODY_PADB + BAR_H);
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

        const bool TARGET = std::ranges::any_of(notifs, [](const auto& N) { return !N->waiting && !N->snoozed && !inOsdBand(N->id); });

        { // do-not-disturb
            const CBox B{bx, BARY, BAR_BTN, BAR_BTN};
            const bool LIT = Model::suspendedNow();
            const auto G   = controlIcon(eControlIcon::DO_NOT_DISTURB, (int)std::lround(BAR_ICON * P.scale), LIT ? onHighlight() : COLFG);
            if (!P.warm) {
                const bool HOV = hovered.kind == SCard::BTN_DND;
                P.rect(B, LIT ? COLACC : HOV ? stateLayer() : surfaceHigh(), (int)std::lround(BAR_BTN / 2 * P.scale));
                if (G)
                    P.texFit(G, CBox{B.x + (B.w - BAR_ICON) / 2, B.y + (B.h - BAR_ICON) / 2, BAR_ICON, BAR_ICON}, 0, 2.f);
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
            const auto HOT  = cachedText("Unmute all", COLACC, T.bar, 200, -1, 0, false, 600);
            const bool HOV  = hovered.kind == SCard::BTN_RULES;
            // the wider of the two: the chip must not resize under the pointer
            const double CW = std::max(texW(REST, P.scale), texW(HOT, P.scale)) + 18;
            const CBox   B{bx, BARY, CW, BAR_BTN};
            if (!P.warm) {
                P.rect(B, HOV ? stateLayer() : surfaceHigh(), (int)std::lround(BAR_BTN / 2 * P.scale));
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
            // Keep the established desktop footer track: the clear action fills
            // everything left after the semantic controls, as it did before
            // the Pixel footer study introduced a compact chip.
            const double CLEAR_W = std::max(0.0, X + PANEL_W - BAR_PADX - bx);
            const auto   REST    = cachedText("Clear all", TARGET ? COLFG : COLSUB.modifyA(0.35f), T.bar, (int)(std::max(1.0, CLEAR_W) * P.scale), -1, 0, false, 600);
            const auto   HOT     = cachedText("Clear all", COLACC, T.bar, (int)(std::max(1.0, CLEAR_W) * P.scale), -1, 0, false, 600);
            const CBox   B{bx, BARY, CLEAR_W, BAR_BTN};
            if (!P.warm) {
                const bool HOV = hovered.kind == SCard::BTN_CLEAR;
                P.rect(B, HOV && TARGET ? stateLayer() : surface().modifyA(TARGET ? 1.f : 0.38f), (int)std::lround(BAR_BTN / 2 * P.scale));
                const auto* L = HOV && TARGET ? HOT : REST;
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
