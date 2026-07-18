// hyprbar/input.cpp — clicks, scrolls and pointer ownership over the strip

#include "hyprbar.hpp"

namespace NHyprbar {

    std::map<uint64_t, std::vector<SHit>> hitboxes;
    static uint32_t                       swallowRelease = 0; // bits: 1 left, 2 right, 4 middle, 8 other

    // ---- click dispatch ----
    //
    // What a cell does lives with its widget (IWidget::onHit/onScroll);
    // this unit owns swallowing, the deferral out of the input emission,
    // and the notch coalescing.

    static UP<SEventLoopDoLaterLock> pendingHit;

    // ---- input ----

    static PHLMONITOR monAt(const Vector2D& pos) {
        for (const auto& M : State::monitorState()->monitors())
            if (M->logicalBox().containsPoint(pos))
                return M;
        return State::monitorState()->query().vec(pos).run(); // off every monitor: the query's nearest-match
    }

    // Presses that reached apps (nothing swallowed them) — while one is held an
    // implicit grab may be live, and the strip must not steal the pointer from
    // it. Tracked here because CInputManager's own held-buttons list is private.
    static int heldButtons = 0;

    void       onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
        // emissions precede the compositor's own lock handling: locked input
        // belongs to the lockscreen, and half-tracked state must not survive it
        if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked()) {
            swallowRelease = 0;
            heldButtons    = 0;
            return;
        }

        // untracked buttons (BTN_SIDE...) share a catch-all bit: their
        // swallowed presses must round-trip through swallowRelease too, or
        // the release decrements heldButtons for a press that never counted
        const uint32_t BIT = e.button == BTN_LEFT ? 1u : e.button == BTN_RIGHT ? 2u : e.button == BTN_MIDDLE ? 4u : 8u;

        if (e.state == WL_POINTER_BUTTON_STATE_RELEASED) {
            if (BIT && (swallowRelease & BIT)) {
                swallowRelease &= ~BIT;
                info.cancelled = true;
            } else
                heldButtons = std::max(0, heldButtons - 1); // a press the apps saw ends
            return;
        }

        const auto POS = g_pInputManager->getMouseCoordsInternal();
        const auto MON = monAt(POS);
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
                                pendingHit = g_pEventLoopManager->doLaterLock([li, IDX]() { Menu::openSub(li, IDX); });
                                return;
                            }
                            const auto EN = L.entries[IDX];
                            pendingHit    = g_pEventLoopManager->doLaterLock([EN]() { Menu::activate(EN); });
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

        if (BIT == 8u)
            return; // no side-button actions on the bar
        const auto KB    = g_pSeatManager ? g_pSeatManager->m_keyboard.lock() : nullptr;
        const bool SUPER = KB && (KB->m_modifiersState.depressed & HL_MODIFIER_META);
        for (const auto& HIT : IT->second) {
            if (HIT.box.containsPoint(POS)) {
                SHit hc   = HIT;
                hc.mon    = MON;
                hc.clickX = POS.x;
                // Deferred out of the input emission: workspace/focus changes
                // mid-button-event bite code that still holds pre-click state.
                if (hc.widget)
                    pendingHit = g_pEventLoopManager->doLaterLock([hc, BIT, SUPER]() { hc.widget->onHit(hc, BIT, SUPER); });
                break;
            }
        }
    }

    // awesome's wibar scroll bindings live with their widgets; the strip
    // swallows every scroll it owns.
    static UP<SEventLoopDoLaterLock> pendingScroll;

    // Batched notches ACCUMULATE into one deferred hop: axis events can
    // arrive several per dispatch, and overwriting the doLaterLock cancels
    // the unfired hop — fast wheel spins were collapsing to a single step.
    static std::unordered_map<IWidget*, int> scrollAcc; // reused; main thread only
    static bool                              scrollQueued = false;

    static void                              queueScrollHop(PHLMONITOR mon) {
        if (scrollQueued)
            return;
        scrollQueued  = true;
        pendingScroll = g_pEventLoopManager->doLaterLock([mon]() {
            scrollQueued = false;
            for (auto& [W, STEPS] : scrollAcc)
                if (STEPS)
                    W->onScrollSteps(STEPS, mon);
            scrollAcc.clear();
        });
    }

    void onMouseAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) {
        if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked())
            return;

        const auto POS = g_pInputManager->getMouseCoordsInternal();
        const auto MON = monAt(POS);
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

    static bool barOwnsPoint(const Vector2D& pos) {
        const auto MON = monAt(pos);
        if (!MON)
            return false;

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
            return false;

        const auto WS = MON->m_activeWorkspace;
        if (WS && Fullscreen::controller()->getFullscreenModes(WS).internal == Fullscreen::FSMODE_FULLSCREEN)
            return Menubar::isOpen && Menubar::mon.lock() == MON; // hidden bar — only the open menubar floats above fullscreen
        return true;
    }

    void releasePointer() {
        if (!pointerOwned)
            return;
        pointerOwned = false;
        Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
    }

    void onMouseMove(const Vector2D& pos, Event::SCallbackInfo& info) {
        if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked()) {
            releasePointer(); // no cursor pinned over an invisible strip
            return;
        }

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

        if (!barOwnsPoint(pos) || heldButtons > 0 || (g_layoutManager && g_layoutManager->dragController()->target())) {
            releasePointer();
            return;
        }

        info.cancelled = true;
        if (!pointerOwned) {
            pointerOwned = true;
            g_pSeatManager->setPointerFocus(nullptr, {}); // the app under the strip gets its leave
            Pointer::Cursor::overrideController->setOverride("left_ptr", Pointer::Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
        }
    }

    void inputExit() {
        pendingHit.reset();
        pendingScroll.reset();
        scrollAcc.clear();
        scrollQueued = false;
        hitboxes.clear();
        releasePointer();
    }

} // namespace NHyprbar
