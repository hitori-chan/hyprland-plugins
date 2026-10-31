// awesome/shell/input.cpp — clicks, scrolls and pointer ownership over the strip

#include "shell/shell.hpp"

#include <ranges>

namespace NAwesome::Shell {

    std::map<uint64_t, std::vector<SHit>> hitboxes;
    SBarHover                             barHover;
    static uint32_t                       swallowRelease = 0; // bits: 1 left, 2 right, 4 middle

    // ---- click dispatch ----
    //
    // What a cell does lives with its widget (IWidget::onHit/onScroll);
    // this unit owns swallowing, the deferral out of the input emission,
    // and the notch coalescing.

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
    // a click's action, out of the input emission (a widget's onHit can
    // change workspace or focus); 16 pending is far past a human's clicks
    // per turn
    static NAwesome::CHopQueue<SHitJob, 16> hitJobs([](std::vector<SHitJob>& batch) {
        if (NAwesome::sessionLocked())
            return; // the lock can engage between the click and this hop
        for (auto& J : batch) {
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
    });

    static void queueHitJob(SHitJob job) {
        hitJobs.push(std::move(job));
    }

    // ---- input ----

    using NAwesome::monitorAt; // core/queries.hpp: allocation-free, runs per pointer motion

    // Presses that reached apps (nothing swallowed them) — while one is held an
    // implicit grab may be live, and the strip must not steal the pointer from
    // it. Tracked here because CInputManager's own held-buttons list is private.
    static int heldButtons = 0;

    // the pipeline head guarantees a locked or natively-captured session
    // never dispatches here (it calls onInputBlocked instead); the resets
    // live there, crash class 3/7
    void       onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
        const uint32_t BIT = NAwesome::trackedPointerButtonBit(e.button);

        if (e.state == WL_POINTER_BUTTON_STATE_RELEASED) {
            if (swallowRelease & BIT) {
                swallowRelease &= ~BIT;
                info.cancelled = true;
            } else
                heldButtons = std::max(0, heldButtons - 1); // a press the apps saw ends
            return;
        }
        // the compositor holds nothing: no count can be live (a pointer
        // unplugged mid-press never sends its release, and the strip would
        // pass every later press through as part of a grab)
        if (g_pInputManager && !g_pInputManager->hasHeldButtons())
            heldButtons = 0;

        // Side/extra buttons have no shell action. Pass them through as native
        // application input instead of making one catch-all swallow bit.
        if (!BIT) {
            heldButtons++;
            return;
        }

        // An application implicit grab remains authoritative until all of its
        // buttons are released, even if the pointer crosses the strip.
        if (heldButtons > 0 || NAwesome::nativePointerGrabActive()) {
            heldButtons++;
            return;
        }
        if (NAwesome::nativeLayerOwnsPointer())
            return;

        const auto POS = g_pInputManager->getMouseCoordsInternal();
        const auto MON = monitorAt(POS);
        if (!MON) {
            heldButtons++;
            return;
        }

        // the menubar prompt closes on any press, like clicking away in awesome;
        // a press ON its strip must not fall through to the window beneath it.
        // While it was open here the bar showed even over a fullscreen window
        // (barHidden), so this press still lands on what was drawn.
        const bool MENUBAR_HERE = Menubar::isOpen && Menubar::mon.lock() == MON;
        if (Menubar::isOpen) {
            const auto MBM = Menubar::mon.lock();
            Menubar::close();
            if (MBM == MON && POS.y >= MON->logicalBox().y + barHeight(MON) && POS.y < MON->logicalBox().y + barHeight(MON) * 2) {
                info.cancelled = true;
                swallowRelease |= BIT;
                return;
            }
            // Over a fullscreen window the strip showed only for the prompt:
            // with the prompt gone, a press on it (a task's or a tray icon's
            // menu) would open what the hidden bar's warm closes at once.
            // The press closed the launcher; that is all it does.
            if (MBM == MON && POS.y >= MON->logicalBox().y && POS.y < MON->logicalBox().y + barHeight(MON))
                if (NAwesome::fullscreenOwns(MON)) {
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
        if (!MENUBAR_HERE && NAwesome::fullscreenOwns(MON)) {
            heldButtons++;
            return;
        }

        const auto MB = MON->logicalBox();
        if (POS.y >= MB.y + barHeight(MON)) {
            heldButtons++;
            return;
        }

        info.cancelled = true; // the strip is ours: every button, even between hitboxes
        swallowRelease |= BIT;

        const auto IT = hitboxes.find(MON->m_id);
        if (IT == hitboxes.end())
            return;

        // topmost first: a cell painted later (the bell's badge over its
        // neighbour) is what the pointer is on
        const bool SUPER = NAwesome::superHeld();
        for (const auto& HIT : IT->second | std::views::reverse) {
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
    static NAwesome::CHop         pendingScroll;

    // Batched notches ACCUMULATE into one deferred hop: axis events can
    // arrive several per dispatch, and overwriting the doLaterLock cancels
    // the unfired hop — fast wheel spins were collapsing to a single step.
    static std::unordered_map<IWidget*, int> scrollAcc; // reused; main thread only
    static bool                              scrollQueued = false;

    static void                              queueScrollHop(PHLMONITOR mon) {
        if (scrollQueued)
            return;
        scrollQueued  = true;
        // weak: an output removed between the scroll and the hop must not
        // get a workspace switched onto it
        pendingScroll.arm([monRef = PHLMONITORREF{mon}]() {
            scrollQueued   = false;
            const auto MON = monRef.lock();
            if (NAwesome::sessionLocked() || !MON || !MON->m_enabled) {
                scrollAcc.clear(); // reset the half-tracked accumulator, act on nothing
                return;
            }
            for (auto& [W, STEPS] : scrollAcc)
                if (STEPS)
                    W->onScrollSteps(STEPS, MON);
            scrollAcc.clear();
        });
    }

    // Whole notches, signed like the axis (negative = up): awesome hears
    // button 4/5 once per wheel detent and once per 15 units of touchpad
    // travel (libinput's X11 scroll distance), never once per event. A
    // high-resolution wheel sends a detent as several partial v120 events
    // and a touchpad swipe as dozens of small deltas; both used to step on
    // every one (a notch spun 8 tags, a swipe ran through all of them).
    // The remainder resets on a direction flip, a source change and a
    // touchpad's lift (its axis stop: delta 0).
    static int scrollNotches(const IPointer::SAxisEvent& e) {
        static double                 acc    = 0; // main thread only
        static wl_pointer_axis_source source = WL_POINTER_AXIS_SOURCE_WHEEL;
        if (e.source != source) {
            source = e.source;
            acc    = 0;
        }
        const bool   WHEEL = e.source == WL_POINTER_AXIS_SOURCE_WHEEL;
        const double V     = WHEEL ? (e.deltaDiscrete != 0 ? (double)e.deltaDiscrete : e.delta * 8.0) : e.delta;
        const double UNIT  = WHEEL ? 120.0 : 15.0;
        if (V == 0.0 || (acc != 0.0 && (acc < 0) != (V < 0)))
            acc = 0;
        acc += V;
        const int N = (int)(acc / UNIT);
        acc -= N * UNIT;
        return N;
    }

    void onMouseAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) {
        const auto POS = g_pInputManager->getMouseCoordsInternal();
        const auto MON = monitorAt(POS);
        if (!MON)
            return;
        // the cheap test first: a scroll anywhere off the shell's territory
        // (the bar, the prompt strip, an open menu) is none of ours, and the
        // native hit-test below walks the windows and layers per event
        const bool MENU_HERE = Menu::isOpen && Menu::mon.lock() == MON;
        const bool PROMPT_HERE = Menubar::isOpen && Menubar::mon.lock() == MON;
        if (!MENU_HERE && POS.y >= MON->logicalBox().y + barHeight(MON) * (PROMPT_HERE ? 2 : 1))
            return;
        if (heldButtons > 0 || NAwesome::nativePointerGrabActive() || NAwesome::nativeLayerOwnsPointer())
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
        if (Menubar::isOpen && Menubar::mon.lock() == MON && POS.y < MON->logicalBox().y + barHeight(MON) * 2) {
            info.cancelled = true; // the prompt strip swallows scroll, no action
            return;
        }
        if (NAwesome::fullscreenOwns(MON))
            return; // bar hidden, strip belongs to the fullscreen window
        if (POS.y >= MON->logicalBox().y + barHeight(MON))
            return;

        info.cancelled = true;

        if (e.axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
            return;
        const int STEPS = -scrollNotches(e); // wheel up: awesome binds button 4 = next/byidx(1)
        if (STEPS == 0)
            return;

        const auto IT = hitboxes.find(MON->m_id);
        if (IT == hitboxes.end())
            return;
        for (const auto& HIT : IT->second | std::views::reverse) { // topmost first
            if (!HIT.box.containsPoint(POS))
                continue;
            if (!HIT.widget)
                break;
            if (HIT.widget->accumulatesScroll()) {
                scrollAcc[HIT.widget] += STEPS;
                queueScrollHop(MON);
            } else
                HIT.widget->onScroll(HIT, STEPS);
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
        // every motion is far below the strip. Half-open, like every strip
        // edge test (presses included): the row at top + H is the window's,
        // and with integer edges a floored motion coordinate and an exact
        // press coordinate always fall on the same side.
        bool over = pos.y < MON->logicalBox().y + barHeight(MON);
        if (!over && Menu::isOpen && Menu::mon.lock() == MON)
            for (const auto& L : Menu::levels)
                if (L.box.containsPoint(pos)) {
                    over = true;
                    break;
                }
        if (!over && Menubar::isOpen && Menubar::mon.lock() == MON && pos.y < MON->logicalBox().y + barHeight(MON) * 2)
            over = true; // the prompt strip below the bar is ours too
        if (!over)
            return nullptr;

        if (NAwesome::nativeLayerOwnsPointer())
            return nullptr;

        if (NAwesome::fullscreenOwns(MON))
            return Menubar::isOpen && Menubar::mon.lock() == MON ? MON : nullptr; // hidden bar — only the open menubar floats above fullscreen
        return MON;
    }

    // Hover lifts a tag's or a task's text to a variant the warm already
    // built: the change damages the two cells and warms nothing (crash
    // class 4)
    static void setBarHover(const SBarHover& h) {
        if (h == barHover)
            return;
        if (g_pHyprRenderer) {
            if (barHover.widget)
                g_pHyprRenderer->damageBox(CBox{barHover.box}.expand(1));
            if (h.widget)
                g_pHyprRenderer->damageBox(CBox{h.box}.expand(1));
        }
        barHover = h;
        Battery::hover(h); // its note waits on a rest
    }

    // the cell a hover reacts to: tags and tasks lift their text, the
    // battery names its state after a rest (the tray, the bell and the clock
    // do not react)
    static void barHoverAt(const PHLMONITOR& mon, const Vector2D& pos) {
        SBarHover h;
        if (const auto IT = hitboxes.find(mon->m_id); IT != hitboxes.end() && pos.y < mon->logicalBox().y + barHeight(mon))
            for (const auto& HIT : IT->second | std::views::reverse)
                if (HIT.box.containsPoint(pos)) {
                    if (HIT.widget == &taglistWidget() || HIT.widget == &tasklistWidget() || HIT.widget == &batteryWidget())
                        h = {.mon = mon->m_id, .widget = HIT.widget, .tag = HIT.tag, .window = HIT.window.get(), .box = HIT.box};
                    break;
                }
        setBarHover(h);
    }

    void releasePointer() {
        setBarHover({});
        if (!pointerOwned)
            return;
        pointerOwned = false;
        Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
    }

    void menuHoverAt(const Vector2D& pos) {
        if (!Menu::isOpen)
            return;
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
            menuHoverChanged();
        Menu::hoverIntent(pl, pr); // open/close cascades on GTK's popup delay
    }

    void onMouseMove(const Vector2D& pos, Event::SCallbackInfo& info) {
        const bool GRABBED = heldButtons > 0 || NAwesome::nativePointerGrabActive() || (g_layoutManager && g_layoutManager->dragController()->target());

        menuHoverAt(pos);

        // the held-button / live-drag gates first: they are two loads, while
        // barOwnsPoint walks the monitors and fetches the bar height — and a
        // drag sweeping across the screen emits motion the whole way
        const auto MON = GRABBED ? nullptr : barOwnsPoint(pos);
        if (!MON) {
            releasePointer();
            return;
        }

        info.cancelled = true;
        barHoverAt(MON, pos);
        if (!pointerOwned) {
            pointerOwned = true;
            g_pSeatManager->setPointerFocus(nullptr, {}); // the app under the strip gets its leave
            Pointer::Cursor::overrideController->setOverride("left_ptr", Pointer::Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
        } else if (!g_pSeatManager->m_state.pointerFocus.expired())
            g_pSeatManager->setPointerFocus(nullptr, {}); // a native re-send (a map, a group add) entered the app beneath
    }

    // the pipeline head's reset (crash class 3/7): the session is locked or a
    // native input-capture session owns the stream. Every partial input state
    // goes: the swallow masks, the held count, the scroll accumulator, the
    // pinned cursor, the hover, and the open menus/prompts — a typed query
    // or a cascade left under the lockscreen would survive it.
    void onInputBlocked() {
        swallowRelease = 0;
        heldButtons    = 0;
        hitJobs.reset();
        pendingScroll.reset();
        scrollAcc.clear();
        scrollQueued = false;
        releasePointer();
        if (Menu::isOpen)
            Menu::close();
        if (Menubar::isOpen)
            Menubar::close();
    }

    void inputExit() {
        hitJobs.reset();
        pendingScroll.reset();
        scrollAcc.clear();
        scrollQueued = false;
        hitboxes.clear();
        swallowRelease = 0;
        heldButtons    = 0;
        releasePointer();
    }

} // namespace NAwesome::Shell
