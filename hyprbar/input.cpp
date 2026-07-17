// hyprbar/input.cpp — clicks, scrolls and pointer ownership over the strip

#include "hyprbar.hpp"

namespace NHyprbar {

    std::map<uint64_t, std::vector<SHit>> hitboxes;
    static uint32_t                       swallowRelease = 0; // bit 1 = left, 2 = right

    // ---- click dispatch (deferred out of the input emission) ----

    static UP<SEventLoopDoLaterLock> pendingHit;

    static void                      runHit(const SHit& hit, bool right, bool super) {
        switch (hit.kind) {
            case SHit::TAG:
                // awesome's taglist buttons: click views the tag, Mod+click
                // sends the focused window there without following. Right-click
                // (viewtoggle) and Mod+right (toggle_tag) have no analog — a
                // window sits on exactly one workspace here.
                if (right)
                    break;
                if (super) {
                    auto ws = State::workspaceState()->query().id(hit.tag).run();
                    if (!ws)
                        if (const auto M = Desktop::focusState() ? Desktop::focusState()->monitor() : nullptr)
                            ws = State::workspaceState()->create(hit.tag, M->m_id);
                    if (ws)
                        std::ignore = Config::Actions::moveToWorkspace(ws, true); // silent — move_to_tag never followed
                } else
                    std::ignore = Config::Actions::changeWorkspace(std::to_string(hit.tag));
                break;
            case SHit::TASK:
                if (right) { // awesome: the all-clients menu
                    Menu::openClients(hit.anchorX, hit.mon);
                    break;
                }
                {
                    if (const auto W = hit.window.lock(); W && W->m_isMapped) {
                        // Not Actions::focus(): that goes through FocusState with
                        // surface=nullptr, and its already-focused guard compares
                        // (window, surface) == (m_focusWindow, m_focusSurface).
                        // When a popup/layer that held the keyboard dies while the
                        // pointer sits on the bar (moves swallowed = FFM can't
                        // heal), m_focusSurface is left empty with m_focusWindow
                        // still set — nullptr == empty matches, the guard returns
                        // before the raise AND before keyboard focus, and the
                        // click looks dead until some other window gets focused.
                        // So: raise explicitly, then focus with the window's real
                        // surface — the guard can never match a half-focused
                        // window, and a focused-but-obscured one still raises.
                        Desktop::windowState()->raise(W);
                        Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_DISPATCH_FOCUSWINDOW, W->wlSurface()->resource());
                    }
                }
                break;
            case SHit::TRAY: {
                const auto IT = hit.tray.lock();
                if (!IT || !IT->proxy)
                    return;
                const bool HASMENU = !IT->menuPath.empty();
                if (!right && !(IT->itemIsMenu && HASMENU)) {
                    IT->proxy->callMethodAsync("Activate").onInterface(Tray::SNI).withArguments((int32_t)0, (int32_t)0).uponReplyInvoke([](std::optional<sdbus::Error>) {});
                    Tray::pollSoon(); // the activation usually flips the icon right back
                    return;
                }
                if (HASMENU)
                    Menu::openFor(IT, hit.anchorX, hit.mon);
                else if (right) {
                    IT->proxy->callMethodAsync("ContextMenu").onInterface(Tray::SNI).withArguments((int32_t)0, (int32_t)0).uponReplyInvoke([](std::optional<sdbus::Error>) {});
                    Tray::pollSoon();
                }
                break;
            }
        }
    }

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

        const uint32_t BIT = e.button == BTN_LEFT ? 1u : e.button == BTN_RIGHT ? 2u : e.button == BTN_MIDDLE ? 4u : 0u;

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

        if (BIT == 4u) { // middle on a tray icon: the SNI SecondaryActivate call
            for (const auto& HIT : IT->second) {
                if (HIT.kind != SHit::TRAY || !HIT.box.containsPoint(POS))
                    continue;
                if (const auto TI = HIT.tray.lock(); TI && TI->proxy) {
                    TI->proxy->callMethodAsync("SecondaryActivate").onInterface(Tray::SNI).withArguments((int32_t)0, (int32_t)0).uponReplyInvoke([](std::optional<sdbus::Error>) {
                    });
                    Tray::pollSoon();
                }
                break;
            }
            return;
        }
        if (BIT != 1u && BIT != 2u)
            return; // no side-button actions on the bar
        const auto KB    = g_pSeatManager ? g_pSeatManager->m_keyboard.lock() : nullptr;
        const bool SUPER = KB && (KB->m_modifiersState.depressed & HL_MODIFIER_META);
        for (const auto& HIT : IT->second) {
            if (HIT.box.containsPoint(POS)) {
                SHit hc = HIT;
                hc.mon  = MON;
                if (hc.kind == SHit::TASK)
                    hc.anchorX = POS.x; // the client-list menu pops at the click
                // Deferred out of the input emission: workspace/focus changes
                // mid-button-event bite code that still holds pre-click state.
                pendingHit = g_pEventLoopManager->doLaterLock([hc, right = BIT == 2u, SUPER]() { runHit(hc, right, SUPER); });
                break;
            }
        }
    }

    // awesome's wibar scroll bindings: on the taglist wheel-up/down view the
    // next/previous tag (wrapping, like awful.tag.viewnext/viewprev); on a
    // tasklist item they walk focus through the tasks (focus.byidx ±1,
    // wrapping); a tray icon gets the SNI Scroll call (the XEmbed systray let
    // apps see scroll too). The strip swallows every scroll it owns.
    static UP<SEventLoopDoLaterLock> pendingScroll;

    void                             onMouseAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) {
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
            if (HIT.kind == SHit::TAG) {
                auto CUR = MON->m_activeWorkspace ? MON->m_activeWorkspace->m_id : 1;
                if (CUR < 1 || CUR > 9)
                    CUR = 1;
                const int NEXT = UP ? (int)(CUR % 9) + 1 : (int)((CUR + 7) % 9) + 1;
                pendingScroll  = g_pEventLoopManager->doLaterLock([NEXT]() { std::ignore = Config::Actions::changeWorkspace(std::to_string(NEXT)); });
            } else if (HIT.kind == SHit::TASK) {
                pendingScroll = g_pEventLoopManager->doLaterLock([MON, UP]() {
                    const auto WS = MON->m_activeWorkspace;
                    if (!WS)
                        return;
                    static std::vector<std::pair<uint64_t, PHLWINDOW>> tasks; // reused; main thread only
                    tasks.clear();
                    for (const auto& W : Desktop::windowState()->windows()) {
                        if (isTaskOn(W, WS))
                            if (const auto SEQ = winSeq.find(W.get()); SEQ != winSeq.end())
                                tasks.emplace_back(SEQ->second, W);
                    }
                    if (tasks.empty())
                        return;
                    std::sort(tasks.begin(), tasks.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                    const auto FOCUS = Desktop::focusState() ? Desktop::focusState()->window() : nullptr;
                    int        idx   = 0;
                    for (int i = 0; i < (int)tasks.size(); i++)
                        if (tasks[i].second == FOCUS)
                            idx = i;
                    const auto W = tasks[(idx + (UP ? 1 : (int)tasks.size() - 1)) % tasks.size()].second;
                    tasks.clear(); // don't keep strong window refs across scrolls
                    Desktop::windowState()->raise(W);
                    Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_DISPATCH_FOCUSWINDOW, W->wlSurface()->resource());
                });
            } else if (HIT.kind == SHit::TRAY) {
                if (const auto TI = HIT.tray.lock(); TI && TI->proxy) {
                    TI->proxy->callMethodAsync("Scroll")
                        .onInterface(Tray::SNI)
                        .withArguments((int32_t)(UP ? -120 : 120), std::string{"vertical"})
                        .uponReplyInvoke([](std::optional<sdbus::Error>) {});
                    Tray::pollSoon();
                }
            }
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
        hitboxes.clear();
        releasePointer();
    }

} // namespace NHyprbar
