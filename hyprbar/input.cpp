// hyprbar/input.cpp — clicks, scrolls and pointer ownership over the strip

#include "common/input.hpp"
#include "common/lifecycle.hpp"
#include "common/queries.hpp"

#include "hyprbar.hpp"

namespace NHyprbar {

    std::map<uint64_t, std::vector<SHit>> hitboxes;
    static uint32_t                       swallowRelease = 0; // bits: 1 left, 2 right, 4 middle

    // ---- click dispatch ----
    //
    // What a cell does lives with its widget (IWidget::onHit/onScroll);
    // this unit owns swallowing, the deferral out of the input emission,
    // and the notch coalescing.

    static NHyprCommon::CHop         pendingHit;

    // Batched hits drain in one hop: two button presses can land in one
    // dispatch (a simultaneous left+right tap, a scripted client) and
    // overwriting a lone doLaterLock cancels the unfired click
    struct SHitJob {
        enum eKind : uint8_t {
            WIDGET,
            MENU_ACTIVATE,
            MENU_SUBMENU
        } kind = WIDGET;
        SHit         hit; // WIDGET
        Menu::SEntry entry; // MENU_ACTIVATE
        size_t       subLevel = 0; // MENU_SUBMENU
        int          subIdx = -1;
        uint32_t     bit = 0;
        bool         super = false;
    };
    static std::vector<SHitJob> hitJobs;
    static bool                 hitQueued = false;

    static void queueHitJob(SHitJob job) {
        if (hitJobs.size() < 16)
            hitJobs.push_back(std::move(job));
        if (hitQueued)
            return;
        hitQueued = true;
        pendingHit.arm([]() {
            hitQueued = false;
            if (NHyprCommon::sessionLocked()) {
                hitJobs.clear(); // the lock can engage between the click and this hop
                return;
            }
            for (auto& J : hitJobs) {
                switch (J.kind) {
                case SHitJob::WIDGET:
                    if (J.hit.widget)
                        J.hit.widget->onHit(J.hit, J.bit, J.super); // a widget onHit can change workspace/focus — never under a lock
                    break;
                case SHitJob::MENU_ACTIVATE:
                    Menu::activate(J.entry);
                    break;
                case SHitJob::MENU_SUBMENU:
                    Menu::openSub(J.subLevel, J.subIdx);
                    break;
                }
            }
            hitJobs.clear();
        });
    }

    // ---- input ----

    using NHyprCommon::monitorAt; // common/queries.hpp: allocation-free, runs per pointer motion

    // Presses that reached apps (nothing swallowed them) — while one is held an
    // implicit grab may be live, and the strip must not steal the pointer from
    // it. Tracked here because CInputManager's own held-buttons list is private.
    static int heldButtons = 0;

    void       onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
        // emissions precede the compositor's own lock handling: locked input
        // belongs to the lockscreen, and half-tracked state must not survive it
        if (NHyprCommon::sessionLocked()) {
            swallowRelease = 0;
            heldButtons    = 0;
            return;
        }
        if (NHyprCommon::nativeInputCaptureActive()) {
            swallowRelease = 0;
            heldButtons    = 0;
            return;
        }

        const uint32_t BIT = NHyprCommon::trackedPointerButtonBit(e.button);

        if (e.state == WL_POINTER_BUTTON_STATE_RELEASED) {
            if (swallowRelease & BIT) {
                swallowRelease &= ~BIT;
                info.cancelled = true;
            } else
                heldButtons = std::max(0, heldButtons - 1); // a press the apps saw ends
            return;
        }

        // Side/extra buttons have no shell action. Pass them through as native
        // application input instead of making one catch-all swallow bit.
        if (!BIT) {
            heldButtons++;
            return;
        }

        // An application implicit grab remains authoritative until all of its
        // buttons are released, even if the pointer crosses the strip.
        if (heldButtons > 0 || NHyprCommon::nativePointerGrabActive()) {
            heldButtons++;
            return;
        }
        if (NHyprCommon::nativeLayerOwnsPointer())
            return;

        const auto POS = g_pInputManager->getMouseCoordsInternal();
        const auto MON = monitorAt(POS);
        if (!MON) {
            heldButtons++;
            return;
        }

        // the menubar prompt closes on any press, like clicking away in awesome;
        // a press ON its strip must not fall through to the window beneath it
        if (Menubar::isOpen) {
            const auto MBM = Menubar::mon.lock();
            Menubar::close();
            if (MBM == MON && POS.y > MON->logicalBox().y + barHeight() && POS.y <= MON->logicalBox().y + barHeight() * 2) {
                info.cancelled = true;
                swallowRelease |= BIT;
                return;
            }
        }

        // menu first: it owns every click while open. Panels are hit-tested
        // deepest first — a left-flipped cascade can overlap its parent.
        if (Menu::isOpen) {
            info.cancelled = true;
            swallowRelease |= BIT;
            if (Menu::mon.lock() == MON) {
                for (size_t li = Menu::levels.size(); li-- > 0;) {
                    auto& L = Menu::levels[li];
                    if (!L.box.containsPoint(POS))
                        continue;
                    for (const auto& [ROW, IDX] : L.rows) {
                        if (!ROW.containsPoint(POS))
                            continue;
                        if (IDX == Menu::SCROLL_UP || IDX == Menu::SCROLL_DOWN) { // arrow strip: step like a wheel notch
                            if (BIT == 1u) {
                                L.scrollTop = std::clamp(L.scrollTop + (IDX == Menu::SCROLL_UP ? -3 : 3), 0, L.maxScroll);
                                Menu::closeDeeperThan(li); // its cascade anchored to rows that just moved
                                Menu::damageMenu();
                            }
                            return;
                        }
                        if ((BIT == 1u || BIT == 2u) && (size_t)IDX < L.entries.size() && L.entries[IDX].enabled && !L.entries[IDX].separator) {
                            if (L.entries[IDX].submenu) { // a click cascades too, like GTK
                                queueHitJob(SHitJob{.kind = SHitJob::MENU_SUBMENU, .subLevel = li, .subIdx = IDX});
                                return;
                            }
                            queueHitJob(SHitJob{.kind = SHitJob::MENU_ACTIVATE, .entry = L.entries[IDX]});
                        }
                        return;
                    }
                    return; // inside the panel but on no row
                }
            }
            Menu::close();
            return;
        }

        // bar hidden under real fullscreen: the strip belongs to the window then
        // (swallowing here would make the top rows of fullscreen apps click-dead)
        if (const auto WS = MON->m_activeWorkspace; WS && Fullscreen::controller()->getFullscreenModes(WS).internal == Fullscreen::FSMODE_FULLSCREEN) {
            heldButtons++;
            return;
        }

        const auto MB = MON->logicalBox();
        if (POS.y > MB.y + barHeight()) {
            heldButtons++;
            return;
        }

        info.cancelled = true; // the strip is ours: every button, even between hitboxes
        swallowRelease |= BIT;

        const auto IT = hitboxes.find(MON->m_id);
        if (IT == hitboxes.end())
            return;

        const bool SUPER = NHyprCommon::superHeld();
        for (const auto& HIT : IT->second) {
            if (HIT.box.containsPoint(POS)) {
                SHit hc   = HIT;
                hc.mon    = MON;
                hc.clickX = POS.x;
                hc.clickY = POS.y;
                // Deferred out of the input emission: workspace/focus changes
                // mid-button-event bite code that still holds pre-click state.
                if (hc.widget)
                    queueHitJob(SHitJob{.kind = SHitJob::WIDGET, .hit = hc, .bit = BIT, .super = SUPER});
                break;
            }
        }
    }

    // awesome's wibar scroll bindings live with their widgets; the strip
    // swallows every scroll it owns.
    static NHyprCommon::CHop         pendingScroll;

    // Batched notches ACCUMULATE into one deferred hop: axis events can
    // arrive several per dispatch, and overwriting the doLaterLock cancels
    // the unfired hop — fast wheel spins were collapsing to a single step.
    static std::unordered_map<IWidget*, int> scrollAcc; // reused; main thread only
    static bool                              scrollQueued = false;

    static void                              queueScrollHop(PHLMONITOR mon) {
        if (scrollQueued)
            return;
        scrollQueued  = true;
        pendingScroll.arm([mon]() {
            scrollQueued = false;
            if (NHyprCommon::sessionLocked()) {
                scrollAcc.clear(); // reset the half-tracked accumulator, act on nothing
                return;
            }
            for (auto& [W, STEPS] : scrollAcc)
                if (STEPS)
                    W->onScrollSteps(STEPS, mon);
            scrollAcc.clear();
        });
    }

    void onMouseAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) {
        if (NHyprCommon::sessionLocked())
            return;
        if (NHyprCommon::nativeInputCaptureActive()) {
            scrollAcc.clear();
            scrollQueued = false;
            pendingScroll.reset();
            return;
        }
        if (heldButtons > 0 || NHyprCommon::nativePointerGrabActive() || NHyprCommon::nativeLayerOwnsPointer())
            return;

        const auto POS = g_pInputManager->getMouseCoordsInternal();
        const auto MON = monitorAt(POS);
        if (!MON)
            return;

        if (Menu::isOpen && Menu::mon.lock() == MON) {
            for (size_t li = Menu::levels.size(); li-- > 0;) {
                auto& L = Menu::levels[li];
                if (!L.box.containsPoint(POS))
                    continue;
                info.cancelled = true; // the panel owns its scroll, nothing below may see it
                // a panel taller than the screen scrolls — GTK's menus did, under
                // X11. One wheel notch (delta 15) = 3 rows; touchpad deltas accumulate.
                if (L.overflow && e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
                    static double acc = 0; // main thread only, remainder is < one row
                    acc += e.delta != 0.0 ? e.delta : e.deltaDiscrete / 120.0 * 15.0;
                    if (const int STEP = (int)(acc / 5.0); STEP != 0) {
                        acc -= STEP * 5.0;
                        L.scrollTop = std::clamp(L.scrollTop + STEP, 0, L.maxScroll);
                        Menu::closeDeeperThan(li); // its cascade anchored to rows that just moved
                        Menu::damageMenu();
                    }
                }
                return;
            }
        }
        if (Menubar::isOpen && Menubar::mon.lock() == MON && POS.y <= MON->logicalBox().y + barHeight() * 2) {
            info.cancelled = true; // the prompt strip swallows scroll, no action
            return;
        }
        if (const auto WS = MON->m_activeWorkspace; WS && Fullscreen::controller()->getFullscreenModes(WS).internal == Fullscreen::FSMODE_FULLSCREEN)
            return; // bar hidden, strip belongs to the fullscreen window
        if (POS.y > MON->logicalBox().y + barHeight())
            return;

        info.cancelled = true;

        if (e.axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
            return;
        const double D = e.delta != 0.0 ? e.delta : (double)e.deltaDiscrete;
        if (D == 0.0)
            return;
        const bool UP = D < 0; // wheel up: awesome binds button 4 = next/byidx(1)

        const auto IT = hitboxes.find(MON->m_id);
        if (IT == hitboxes.end())
            return;
        for (const auto& HIT : IT->second) {
            if (!HIT.box.containsPoint(POS))
                continue;
            if (!HIT.widget)
                break;
            if (HIT.widget->accumulatesScroll()) {
                scrollAcc[HIT.widget] += UP ? 1 : -1;
                queueScrollHop(MON);
            } else
                HIT.widget->onScroll(HIT, UP ? 1 : -1);
            break;
        }
    }

    // The strip (and the open menu) is compositor-drawn, not a surface — the
    // pointer over it logically belongs to whatever window pokes underneath.
    // Left alone, a terminal parked under the bar shows its I-beam and takes
    // hover focus while the mouse is visually ON the bar. So: over bar
    // territory, cancel the move before any hover/focus processing runs, drop
    // the app's pointer focus once on entry, and pin the default cursor. Hands
    // off while a button is held or a drag is live — implicit grabs (text
    // selection sweeping through the strip) and drags must keep flowing, same
    // as they would over a real layer-surface bar.
    static bool pointerOwned = false;

    // the monitor whose strip owns this point, null when none does (the caller
    // needs the monitor anyway, and finding it walks every one of them)
    static PHLMONITOR barOwnsPoint(const Vector2D& pos) {
        const auto MON = monitorAt(pos);
        if (!MON)
            return nullptr;

        // the cheap geometry first: this runs per pointer motion, and almost
        // every motion is far below the strip
        bool over = pos.y <= MON->logicalBox().y + barHeight();
        if (!over && Menu::isOpen && Menu::mon.lock() == MON)
            for (const auto& L : Menu::levels)
                if (L.box.containsPoint(pos)) {
                    over = true;
                    break;
                }
        if (!over && Menubar::isOpen && Menubar::mon.lock() == MON && pos.y <= MON->logicalBox().y + barHeight() * 2)
            over = true; // the prompt strip below the bar is ours too
        if (!over)
            return nullptr;

        if (NHyprCommon::nativeLayerOwnsPointer())
            return nullptr;

        const auto WS = MON->m_activeWorkspace;
        if (WS && Fullscreen::controller()->getFullscreenModes(WS).internal == Fullscreen::FSMODE_FULLSCREEN)
            return Menubar::isOpen && Menubar::mon.lock() == MON ? MON : nullptr; // hidden bar — only the open menubar floats above fullscreen
        return MON;
    }

    // ---- cell hover ----
    //
    // Widgets that want it get enter/leave on their cells. Tracked per WIDGET,
    // not per hit: moving between two of the same widget's cells is not a leave.
    static IWidget* hoverWidget = nullptr;

    static void     setHoverWidget(IWidget* w) {
        if (w == hoverWidget)
            return;
        if (hoverWidget)
            hoverWidget->onHover(false);
        hoverWidget = w;
        if (hoverWidget)
            hoverWidget->onHover(true);
    }

    void releasePointer() {
        if (!pointerOwned)
            return;
        pointerOwned = false;
        Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
    }

    void onMouseMove(const Vector2D& pos, Event::SCallbackInfo& info) {
        if (NHyprCommon::sessionLocked()) {
            releasePointer(); // no cursor pinned over an invisible strip
            setHoverWidget(nullptr);
            return;
        }
        if (NHyprCommon::nativeInputCaptureActive()) {
            releasePointer();
            setHoverWidget(nullptr);
            return;
        }

        const bool GRABBED = heldButtons > 0 || NHyprCommon::nativePointerGrabActive() || (g_layoutManager && g_layoutManager->dragController()->target());

        if (Menu::isOpen) {
            // the pointer's (level, row), deepest panel first
            size_t pl = Menu::levels.size();
            int    pr = -1;
            for (size_t li = Menu::levels.size(); li-- > 0;) {
                if (!Menu::levels[li].box.containsPoint(pos))
                    continue;
                pl = li;
                for (const auto& [ROW, IDX] : Menu::levels[li].rows)
                    if (ROW.containsPoint(pos)) {
                        pr = IDX;
                        break;
                    }
                break;
            }
            bool changed = false;
            for (size_t li = 0; li < Menu::levels.size(); li++) {
                const int want = li == pl ? pr : -1;
                if (Menu::levels[li].hover != want) {
                    Menu::levels[li].hover = want;
                    changed                = true;
                }
            }
            if (changed)
                Menu::damageMenu();
            Menu::hoverIntent(pl, pr); // open/close cascades on GTK's popup delay
        }

        // the held-button / live-drag gates first: they are two loads, while
        // barOwnsPoint walks the monitors and fetches the bar height — and a
        // drag sweeping across the screen emits motion the whole way
        const auto MON = GRABBED ? nullptr : barOwnsPoint(pos);
        if (!MON) {
            releasePointer();
            setHoverWidget(nullptr);
            return;
        }

        IWidget* over = nullptr;
        if (const auto IT = hitboxes.find(MON->m_id); IT != hitboxes.end())
            for (const auto& HIT : IT->second)
                if (HIT.box.containsPoint(pos)) {
                    over = HIT.widget;
                    break;
                }
        setHoverWidget(over);

        info.cancelled = true;
        if (!pointerOwned) {
            pointerOwned = true;
            g_pSeatManager->setPointerFocus(nullptr, {}); // the app under the strip gets its leave
            Pointer::Cursor::overrideController->setOverride("left_ptr", Pointer::Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
        }
    }

    void inputExit() {
        setHoverWidget(nullptr);
        pendingHit.reset();
        hitJobs.clear();
        hitQueued = false;
        pendingScroll.reset();
        scrollAcc.clear();
        scrollQueued = false;
        hitboxes.clear();
        swallowRelease = 0;
        heldButtons    = 0;
        releasePointer();
    }

} // namespace NHyprbar
