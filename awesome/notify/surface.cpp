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
    double             lastContentH = 0;
    double             lastContentW = 0;

    static CBox                lastBox; // last damaged layout, global logical (already expanded)
    static SP<CEventLoopTimer> ageTick; // 30s: re-buckets the age lines
    static SP<CEventLoopTimer> motionTick; // ~16ms while something animates
    static NAwesome::CHop      pendingWarm;

    static PHLMONITOR focusedMon() {
        return Desktop::focusState() ? Desktop::focusState()->monitor() : nullptr;
    }

    // Residency keeps quiet cards in the model with NOTHING on screen: only
    // the open center or a live banner actually draws. Everything frame-rate
    // gates on this — the pass element, the scanout inhibit, the age tick —
    // or two resident cards would composite fullscreen video forever.
    static bool anythingToDraw() {
        if (centerVisible())
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

        cards.clear(); // capacity retained: no per-frame allocations
        cardsMon = mon;

        // the center and popups never coexist: opening the center folds the
        // live cards into the panel (same textures, different layout)
        if (centerVisible())
            renderCenter(P, T);
        else
            renderPopups(P, T);
    }

    // ---- damage ----

    static CBox contentBox() {
        if (cards.empty())
            return {};
        double x0 = cards.front().box.x, y0 = cards.front().box.y, x1 = x0, y1 = y0;
        for (const auto& C : cards) {
            x0 = std::min(x0, C.box.x);
            y0 = std::min(y0, C.box.y);
            x1 = std::max(x1, C.box.x + C.box.w);
            y1 = std::max(y1, C.box.y + C.box.h);
        }
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

    // the hover affordance repaints exactly the boxes whose fill changed;
    // no textures move, so no warm — plain damage from the motion listener
    void setHovered(const SHover& h) {
        if (h == hovered)
            return;
        Model::holdBanner(h.kind == SCard::POPUP ? h.id : 0); // reading a banner stops its clock
        if (g_pHyprRenderer) {
            const auto   M      = cardsMon.lock();
            const double MARGIN = (M ? std::ceil(M->m_scale) : 1.0) + 1.0;
            for (const auto& C : cards) {
                const bool WAS = C.kind == hovered.kind && C.id == hovered.id && C.group == hovered.group;
                const bool IS  = C.kind == h.kind && C.id == h.id && C.group == h.group;
                // popups repaint on any enter/leave (the ✕ reveals)
                const bool POPHOV = C.kind == SCard::POPUP && (C.id == hovered.id || C.id == h.id);
                if (WAS || IS || POPHOV)
                    g_pHyprRenderer->damageBox(CBox{C.box}.expand(MARGIN));
            }
        }
        hovered = h;
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
        const auto MON = anythingToDraw() ? focusedMon() : nullptr;
        decodeWarmBegin();
        if (!MON) {
            // no content — or no monitor (disconnect transition): stale boxes
            // must not linger to swallow clicks over nothing
            cards.clear();
            lastContentH = 0;
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
            if (const auto MON = focusedMon(); MON && g_pHyprRenderer && (MON->m_directScanoutIsActive || !MON->m_solitaryClient.expired()))
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

        // The canvas bracket is already open here (its warmAll owns it)
        void warm(PHLMONITOR mon) override {
            (void)mon;
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
            const auto   MB  = mon->logicalBox();
            const double PAD = damageMargin(mon);
            const double W   = std::max(lastContentW, std::max((double)NAwesome::cfg().getI("plugin:awesome:notify:width"), CENTER_W)) + EDGE;
            return CBox{MB.w - W - PAD, (double)NAwesome::cfg().getI("plugin:awesome:notify:offset_y") - PAD, W + 2 * PAD, std::max(lastContentH, 0.0) + 2 * PAD};
        }

        bool needsBlur(PHLMONITOR mon) const override {
            // the glass samples what's beneath, live — only while it paints
            return blurOn() && anythingToDraw() && mon && mon == cardsMon.lock();
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

        NAwesome::Canvas::inst().addLayer(&CSurface::inst());
    }

    void surfaceExit() {
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
        textCacheClear();
        lastContentH = 0;
        lastContentW = 0;
        auto&        G = NAwesome::Canvas::inst().gate();
        G.warming      = false;
        G.texStale     = false;
        hovered        = {};
    }

} // namespace NAwesome::Notify
