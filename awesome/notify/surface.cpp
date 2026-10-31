// awesome/notify/surface.cpp — the canvas layer: warm/draw, damage, the
// tick timers, hover bookkeeping, and the scanout/solitary latch. The
// surfaces paint themselves (popups.cpp, center.cpp); text rasters live in
// text.cpp's keyed cache.
//
// The old plugin registered its own pass element; now the canvas owns the
// one pass and this file is its notify layer — same gates, same warm/draw
// split, same texture rule (crash class 4).
#include "ui.hpp"

namespace NAwesome::Notify {

    std::vector<SCard> cards;
    PHLMONITORREF      cardsMon;
    SHover             hovered;

    // Cards are rebuilt every frame; their hit-rect vectors and group key
    // keep their capacity across frames through this pool (renderAll
    // recycles, the painters take), so redrawing a shape already seen does
    // not allocate them again. Bounded like the cards themselves.
    static std::vector<SCard> cardPool;
    constexpr size_t          MAX_CARD_POOL = 256;
    SCard                     recycledCard() {
        if (cardPool.empty())
            return {};
        SCard old = std::move(cardPool.back());
        cardPool.pop_back();
        SCard fresh;
        fresh.links = std::move(old.links);
        fresh.group = std::move(old.group);
        fresh.links.clear();
        fresh.group.clear();
        return fresh;
    }

    bool cardLit(uint32_t id, SCard::eKind kind) {
        if (CardMenu::owner() == id)
            return true; // its menu is up: the card keeps the look it was opened from
        return hovered.id == id && hovered.kind == kind;
    }

    void refreshBannerHold() {
        uint32_t id = 0;
        if (CardMenu::isOpen())
            id = CardMenu::owner();
        else if (replyHeld())
            id = replyId();
        else if (hovered.kind == SCard::POPUP)
            id = hovered.id;
        Model::holdBanner(id);
    }

    static CBox                lastBox; // last damaged layout, global logical (already expanded)
    static SP<CEventLoopTimer> ageTick; // 30s: re-buckets the age lines
    static SP<CEventLoopTimer> motionTick; // ~16ms while something animates
    static NAwesome::CHop      pendingWarm;

    static PHLMONITOR focusedMon() {
        return Desktop::focusState() ? Desktop::focusState()->monitor() : nullptr;
    }

    // The monitor the cards live on: chosen where a damage of the old and
    // new spots follows (the first card's arrival, a deliberate focus move,
    // the shade opened from a monitor's bell — placeCardsOn) and reused by
    // every other warm. Re-reading the focus in each warm let a bar warm
    // (the pointer crossing monitors) move the hit boxes with no repaint:
    // the old monitor kept a card that no longer took clicks, the new one
    // took clicks on cards it never drew.
    static PHLMONITORREF layoutMon;

    static PHLMONITOR layoutMonitor() {
        if (const auto M = layoutMon.lock(); M && M->m_enabled)
            return M;
        layoutMon = focusedMon(); // first arrival, or its monitor went away
        return layoutMon.lock();
    }

    void placeCardsOn(PHLMONITOR mon) {
        if (!mon || layoutMon.lock() == mon)
            return;
        CardMenu::close(); // its anchor was on the other monitor
        layoutMon = mon;
        notifChanged(); // warms there and damages the old and new spots
    }

    // Residency keeps quiet cards in the model with NOTHING on screen: only
    // the open center or a live banner actually draws. Everything frame-rate
    // gates on this — the pass element, the scanout inhibit, the age tick —
    // or two resident cards would composite fullscreen video forever.
    static bool anythingToDraw() {
        if (centerVisible() || CardMenu::isOpen())
            return true;
        for (const auto& N : notifs)
            if (!N->waiting && N->banner)
                return true;
        return false;
    }

    // ---- the frame: one layout, two modes ----

    static void renderAll(PHLMONITOR mon, SPaint& P) {
        if (!mon)
            return;

        const auto T = typeScale(mon->m_scale); // P.warm is set by the caller

        for (auto& C : cards)
            if (cardPool.size() < MAX_CARD_POOL)
                cardPool.push_back(std::move(C));
        cards.clear(); // capacity retained: no per-frame allocations
        cardsMon = mon;

        // the center and the banners never coexist: opening the center folds
        // the live cards into its column (same textures, different layout).
        // The capsules (system feedback) show over either.
        if (centerVisible())
            renderCenter(P, T);
        else
            renderPopups(P, T);
        renderCapsules(P, T);
        CardMenu::render(P, T); // last: drawn over its card, hit first
    }

    // ---- damage ----

    // everything the last layout drew, global logical: the cards and the
    // capsules
    static CBox contentBox() {
        if (cards.empty())
            return {};
        double     x0 = cards.front().box.x, y0 = cards.front().box.y, x1 = x0, y1 = y0;
        const auto ADD = [&](const CBox& B) {
            x0 = std::min(x0, B.x);
            y0 = std::min(y0, B.y);
            x1 = std::max(x1, B.x + B.w);
            y1 = std::max(y1, B.y + B.h);
        };
        // the menu floats higher: its drop reaches past the cards' margin
        const double MENU_EXTRA = SPaint::frostReach(SPaint::eDepth::MENU) - SPaint::frostReach();
        for (const auto& C : cards)
            ADD(C.kind == SCard::MENU ? CBox{C.box}.expand(MENU_EXTRA) : C.box);
        return CBox{x0, y0, x1 - x0, y1 - y0};
    }

    void damageNotifs() {
        if (!g_pHyprRenderer)
            return;
        const auto M   = cardsMon.lock();
        const auto CUR = contentBox();
        const CBox NEW = CUR.w > 0 ? CBox{CUR}.expand(damageMargin(M)) : CBox{};
        if (lastBox.w > 0)
            g_pHyprRenderer->damageBox(lastBox); // stored already expanded
        if (NEW.w > 0)
            g_pHyprRenderer->damageBox(NEW);
        lastBox = NEW;
    }

    // the hover affordance repaints exactly the cards whose look changed —
    // their lifted glass, a stack header's chips; no textures move, so no
    // warm — plain damage from the motion listener
    void setHovered(const SHover& h) {
        if (h == hovered)
            return;
        if (g_pHyprRenderer) {
            const auto   M      = cardsMon.lock();
            const double MARGIN = (M ? std::ceil(M->m_scale) : 1.0) + 1.0;
            const auto   TOUCH  = [](const SCard& C, const SHover& H) { return C.id == H.id && C.kind == H.kind && C.group == H.group; };
            for (const auto& C : cards)
                if (TOUCH(C, hovered) || TOUCH(C, h))
                    g_pHyprRenderer->damageBox(CBox{C.box}.expand(MARGIN + CHIP_DROP + CHIP_SINK));
        }
        hovered = h;
        refreshBannerHold(); // reading a banner stops its clock
    }

    // ---- the tick timers ----

    static void armAgeTick() {
        if (!ageTick)
            return;
        // ages only matter where they SHOW — an all-resident model with the
        // center closed must tick nothing
        ageTick->updateTimeout(anythingToDraw() ? std::optional{std::chrono::seconds(30)} : std::nullopt);
    }

    static void armMotionTick() {
        if (!motionTick)
            return;
        const bool WANT = animationsOn() && (centerAnimating() || (!centerVisible() && popupsAnimating()));
        motionTick->updateTimeout(WANT ? std::optional{std::chrono::milliseconds(16)} : std::nullopt);
    }

    // ---- warm ----

    void warmNotifs() {
        // The canvas bracket runs during its own warmAll; a standalone
        // warm (the age tick, the decode poll) takes the gate itself
        const bool BRACKET = !NAwesome::Canvas::inst().gate().inPass;
        if (BRACKET && !NAwesome::Canvas::inst().gate().beginWarm())
            return;
        const auto MON = anythingToDraw() ? layoutMonitor() : nullptr;
        decodeWarmBegin();
        if (!MON) {
            // no content — or no monitor (disconnect transition): stale boxes
            // must not linger to swallow clicks over nothing; the next card
            // arrives on the focused monitor
            cards.clear();
            layoutMon.reset();
        } else {
            textCacheTick();
            SPaint P;
            P.mon   = MON;
            P.scale = MON->m_scale;
            P.mb    = MON->logicalBox();
            P.warm  = true;
            renderAll(MON, P);
            textCacheSweep();
        }
        decodeWarmEnd();
        if (BRACKET)
            NAwesome::Canvas::inst().gate().endWarm();
    }

    void notifChanged() {
        // one hop: bursts (an OSD volume sweep, a batch of closes) coalesce
        pendingWarm.arm([]() {
            CardMenu::sync(); // a menu whose card went closes with this same warm
            warmNotifs();
            damageNotifs();
            refreshPointerOwnership();
            armAgeTick();
            armMotionTick();
            // the shell's bell repaints off this same funnel (it reads the
            // model's badge counts directly — no bus); the hook fires after
            // this surface's own warm so its barChanged takes a clean bracket
            if (Model::badgeChangedHook())
                Model::badgeChangedHook()();
            // A card arriving over a solitary/scanned-out fullscreen window
            // (mpv under direct_scanout): the monitor presents the client's
            // buffer directly, so the per-card damageBox may not schedule a
            // compositor frame at all — and the solitary recheck (where
            // CSurface::overFullscreen blocks scanout) runs per frame. Force a
            // whole-monitor frame so the recheck runs and the card composites.
            // Full-monitor (not the card box) so it can't be occlusion-culled
            // behind the fullscreen surface; a no-op cost when the monitor
            // isn't latched.
            if (const auto MON = cardsMon.lock(); MON && g_pHyprRenderer && (MON->m_directScanoutIsActive || !MON->m_solitaryClient.expired()))
                if (anythingToDraw())
                    g_pHyprRenderer->damageMonitor(MON);
        });
    }

    // ---- the canvas layer ----

    class CSurface : public NAwesome::ILayer {
      public:
        static CSurface& inst() {
            static CSurface S;
            return S;
        }

        const char* name() const override {
            return "notify";
        }

        // The canvas bracket is already open here (its warmAll owns it).
        // The cards warm on THEIR changes (notifChanged, the age and decode
        // ticks), never on the bar's: a clock tick or a title change re-ran
        // the whole stack's layout, every row measured. Only the stale
        // rewarm (a draw that found a texture missing) reaches them here.
        // They lay out on their one monitor: warm once, for it.
        void warm(PHLMONITOR mon) override {
            if (!NAwesome::Canvas::inst().forcedWarm())
                return;
            const auto ON = anythingToDraw() ? layoutMonitor() : nullptr;
            if (!ON || mon == ON)
                warmNotifs();
        }

        void draw(PHLMONITOR mon, SPaint& ctx) override {
            // never above the lockscreen (the pass would leak there; these
            // are the user's notifications)
            if (NAwesome::sessionLocked())
                return;
            if (!anythingToDraw())
                return;
            // the pass follows the LAYOUT's monitor, not the focus
            if (!mon || mon != cardsMon.lock())
                return;
            ctx.warm = false;
            renderAll(mon, ctx);
        }

        void damage() override {
            damageNotifs();
        }

        // monitor-local LOGICAL px — the pass scales by m_scale itself
        std::optional<CBox> boundingBox(PHLMONITOR mon) const override {
            if (!mon || !anythingToDraw())
                return std::nullopt;
            const auto M = cardsMon.lock();
            if (!M || M != mon)
                return std::nullopt;
            // what the last layout drew (a warm precedes every change), its
            // drops and blur included
            const auto CB = contentBox();
            if (CB.w <= 0)
                return std::nullopt;
            const auto MB = mon->logicalBox();
            return CBox{CB}.expand(damageMargin(mon)).translate(Vector2D{-MB.x, -MB.y}).intersection(CBox{0, 0, MB.w, MB.h});
        }

        bool needsBlur(PHLMONITOR mon) const override {
            // the glass samples what's beneath, live — only while it paints,
            // and only while it is translucent (an opaque col_glass draws plain)
            return blurOn() && anythingToDraw() && mon && mon == cardsMon.lock() &&
                NAwesome::color(NAwesome::cfg().getColor("plugin:awesome:theme:col_glass")).a < 1.0;
        }

        // Notifications are on top: a VISIBLE card (or the open center)
        // composites over fullscreen video. A resident-only model (nothing
        // drawn) must not block scanout — two quiet shade cards would
        // composite fullscreen video forever — and a card never floats over
        // the lockscreen. The compositor drops scanout at the next frame
        // and re-engages it once the last card clears.
        bool overFullscreen(PHLMONITOR mon) const override {
            return anythingToDraw() && mon && mon == cardsMon.lock() && !NAwesome::sessionLocked();
        }
    };

    void surfaceInit() {
        ageTick = makeShared<CEventLoopTimer>(
            std::nullopt,
            [](SP<CEventLoopTimer>, void*) {
                notifChanged(); // age buckets re-key their own textures
                armAgeTick();
            },
            nullptr);
        g_pEventLoopManager->addTimer(ageTick);
        motionTick = makeShared<CEventLoopTimer>(
            std::nullopt,
            [](SP<CEventLoopTimer>, void*) {
                if (lastBox.w > 0 && g_pHyprRenderer)
                    g_pHyprRenderer->damageBox(lastBox);
                armMotionTick();
            },
            nullptr);
        g_pEventLoopManager->addTimer(motionTick);
        // the launcher's strip opened or shut: the cards restack under it
        Bands::changedHook() = []() { notifChanged(); };

        NAwesome::Canvas::inst().addLayer(&CSurface::inst(), NAwesome::eLayerZ::CARDS);
    }

    void surfaceExit() {
        Bands::changedHook() = nullptr;
        pendingWarm.reset();
        if (lastBox.w > 0 && g_pHyprRenderer)
            g_pHyprRenderer->damageBox(lastBox);
        lastBox = {};
        for (auto* T : {&ageTick, &motionTick}) {
            if (*T && g_pEventLoopManager)
                g_pEventLoopManager->removeTimer(*T);
            T->reset();
        }
        cards.clear();
        cardsMon.reset();
        layoutMon.reset();
        textCacheClear();
        auto& G   = NAwesome::Canvas::inst().gate();
        G.warming      = false;
        G.texStale     = false;
        hovered        = {};
    }

} // namespace NAwesome::Notify
