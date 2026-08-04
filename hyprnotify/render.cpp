// hyprnotify/render.cpp — the render skeleton: warm/draw orchestration,
// damage, the tick timers, hover bookkeeping and the pass element. The
// surfaces paint themselves (popups.cpp, center.cpp); text rasters live in
// text.cpp's keyed cache.

#include "common/lifecycle.hpp"
#include "common/queries.hpp"

#include "ui.hpp"

namespace NHyprnotify {

    std::vector<SCard> cards;
    PHLMONITORREF      cardsMon;
    SHover             hovered;
    double             lastContentH = 0;
    double             lastContentW = 0;
    double             centerOsdReserve = 0;

    static CBox                lastBox; // last damaged layout, global logical (already expanded)
    static SP<CEventLoopTimer> ageTick;    // 30s: re-buckets the age lines
    static SP<CEventLoopTimer> motionTick; // ~16ms while something animates
    static NHyprCommon::CHop   pendingWarm;

    static PHLMONITOR          focusedMon() {
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

    static void renderAll(PHLMONITOR mon, bool warm) {
        if (!mon)
            return;

        SPaint P{.mon = mon, .warm = warm, .scale = mon->m_scale};
        P.monPos = mon->m_position;

        const auto T = typeScale(mon->m_scale);

        cards.clear(); // capacity retained: no per-frame allocations
        cardsMon = mon;

        // Ordinary banners yield to the center when it opens. OSD cards keep
        // the normal popup anatomy but move below the panel. Measure them
        // before the center budget is chosen so a full shade never grows into
        // the OSD area.
        if (centerVisible()) {
            SPaint      MEASURE = P;
            MEASURE.warm       = true; // layout only: never paint in the probe
            const double GAP   = std::max((double)cfg.margin->value(), 0.0);
            const double OSDH  = renderPopups(MEASURE, T, true, std::nullopt, true);
            const auto     MB  = mon->logicalBox();
            const double   MIN = BODY_PADT + BODY_PADB + BAR_PADT + BAR_BTN + BAR_PADB;
            const double   OFF = std::clamp((double)cfg.offsetY->value(), 0.0, std::max(0.0, MB.h - MIN));
            const double   ROOM = std::max(0.0, MB.h - OFF - MIN - GAP);
            centerOsdReserve   = std::min(OSDH > 0 ? OSDH + GAP : 0.0, ROOM);
            renderCenter(P, T);
            const double STARTY = MB.y + std::clamp((double)cfg.offsetY->value(), 0.0, std::max(0.0, MB.h - MIN)) + lastContentH + GAP;
            renderPopups(P, T, true, STARTY);
        } else {
            centerOsdReserve = 0;
            renderPopups(P, T);
        }
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
                // popups repaint on any enter/leave (the close control reveals)
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
        const bool WANT = animationsOn() && (centerAnimating() || (!centerVisible() && popupsAnimating()) || (centerVisible() && popupsAnimating(true)));
        motionTick->updateTimeout(WANT ? std::optional{std::chrono::milliseconds(16)} : std::nullopt);
    }

    // ---- warm ----

    void warmNotifs() {
        if (!warmGate.beginWarm())
            return;
        iconsWarmBegin();
        const auto MON = anythingToDraw() ? focusedMon() : nullptr;
        if (!MON) {
            // no content — or no monitor (disconnect transition): stale boxes
            // must not linger to swallow clicks over nothing
            cards.clear();
            lastContentH = 0;
        } else {
            textCacheTick();
            renderAll(MON, true);
            textCacheSweep();
        }
        const bool REWARM = iconsWarmEnd();
        warmGate.endWarm();
        if (REWARM)
            notifChanged();
    }

    void notifChanged() {
        // one lock: bursts (an OSD volume sweep, a batch of closes) coalesce
        pendingWarm.arm([]() {
            warmNotifs();
            damageNotifs();
            refreshPointerOwnership();
            armAgeTick();
            armMotionTick();
            Bus::emitStateSoon();
            // A card arriving over a solitary/scanned-out fullscreen window
            // may not schedule a frame from its own damage box. Damage the
            // focused monitor once on every visible-state change so the
            // compositor reaches render.preChecks, where requestFullRender()
            // exits the native solitary/direct-scanout path for this frame.
            if (const auto MON = focusedMon(); MON && g_pHyprRenderer && anythingToDraw() && MON->canAttemptDirectScanoutFast())
                g_pHyprRenderer->damageMonitor(MON);
        });
    }

    // ---- pass element ----

    class CNotifyPassElement : public IPassElement {
      public:
        CNotifyPassElement(PHLMONITOR mon) : m_mon(mon) {}
        virtual ~CNotifyPassElement() = default;

        virtual std::vector<UP<IPassElement>> draw() override {
            warmGate.inRender = true;
            renderAll(m_mon.lock(), false);
            warmGate.inRender = false;
            warmGate.rewarmIfStale([]() {
                warmNotifs();
                damageNotifs();
            });
            return {};
        }
        virtual bool needsLiveBlur() override {
            return blurOn(); // the glass samples what's beneath, live
        }
        virtual bool needsPrecomputeBlur() override {
            return false;
        }
        virtual std::optional<CBox> boundingBox() override {
            const auto MON = m_mon.lock();
            if (!MON)
                return std::nullopt;
            const auto   MB  = MON->logicalBox();
            const double PAD = damageMargin(MON);
            const double W   = std::max(lastContentW, std::max((double)cfg.width->value(), CENTER_W)) + EDGE;
            // monitor-local LOGICAL px — the pass scales by m_scale itself
            return CBox{MB.w - W - PAD, (double)cfg.offsetY->value() - PAD, W + 2 * PAD, std::max(lastContentH, 0.0) + 2 * PAD};
        }
        virtual const char* passName() override {
            return "CNotifyPassElement";
        }
        virtual ePassElementType type() override {
            return EK_CUSTOM;
        }

      private:
        PHLMONITORREF m_mon;
    };

    // A solitary fullscreen client (mpv) makes the compositor skip the whole
    // workspace render for its monitor — direct scanout, or a solitary-only
    // renderWindow — so RENDER_POST_WINDOWS never fires and the card is
    // invisible. Notifications are ontop, so while a VISIBLE card (or the
    // open center) is up the compositor-owned hook requests the normal render
    // path at preChecks, before direct scanout is selected. Once the last card
    // clears, the scheduler's normal solitary recheck restores the fast path.
    void onRenderPreChecks(PHLMONITOR mon) {
        // the cheap gate first: this runs per monitor per frame, and a
        // resident-only model (nothing drawn) must NOT inhibit scanout —
        // two quiet shade cards would composite fullscreen video forever
        if (!anythingToDraw())
            return;
        if (!mon || mon != focusedMon())
            return;
        if (NHyprCommon::sessionLocked())
            return; // never force a card to float over the lockscreen
        mon->requestFullRender();
    }

    void onRenderStage(eRenderStage stage) {
        if (stage != RENDER_POST_WINDOWS || !anythingToDraw())
            return;
        // never above the lockscreen (the built-in overlay leaks there; these
        // are the user's notifications). No unlock watcher needed: textures
        // stay warm through the lock, and the lock surface's unmap damages the
        // whole output — that IS the survivors' repaint.
        if (NHyprCommon::sessionLocked())
            return;
        const auto MON = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!MON || MON != focusedMon())
            return;
        g_pHyprRenderer->addPassElement(makeUnique<CNotifyPassElement>(MON));
    }

    void renderInit() {
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
    }

    void renderExit() {
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
        lastContentH      = 0;
        lastContentW      = 0;
        centerOsdReserve  = 0;
        warmGate.warming  = false;
        warmGate.texStale = false;
        hovered           = {};
    }

} // namespace NHyprnotify
