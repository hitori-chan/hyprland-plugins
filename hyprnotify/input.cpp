// hyprnotify/input.cpp — clicks, wheel paging, esc and pointer ownership
// over the popups and the center. Implements the interaction map exactly:
//
//   popup    left = action/link/default → dismiss · right = dismiss ·
//            middle = sweep · hover reveals ✕
//   live row left = default action → dismiss · chevron folds · right =
//            dismiss → history · middle = sweep live → history
//   hist row left = recall (fresh id, original age) · buttons = original
//            actions (best-effort, entry consumed) · right = delete ·
//            middle = clear history
//   digest   left expands · right dismisses/deletes the app's group
//   bottom   ⏱ flips the view · Clear is context-sensitive · ⊖ = DND
//   wheel    pages the center — captured only inside the panel box
//   esc      closes the center (the topmost-peel's middle link)
//
// Every mutation lands via the hit queue + CHop drain, never synchronously
// inside the emission (crash class 6); every listener gates on
// sessionLocked() first and resets its half-tracked state there (class 7).

#include "common/lifecycle.hpp"
#include "common/queries.hpp"

#include "ui.hpp"

#include <xkbcommon/xkbcommon-keysyms.h>

namespace NHyprnotify {

    static uint32_t swallowRelease = 0; // bit 1 = left, 2 = right, 4 = middle
    static int      heldButtons    = 0; // presses that reached apps: an implicit grab may be live
    static bool     pointerOwned   = false;
    static bool     cursorHand     = false; // the override currently shows the link hand

    // clicks accumulate into one drain: two card-clicks in a single dispatch
    // would otherwise clobber the lock and lose the first's action + dismiss
    struct SHit {
        SCard::eKind kind;
        uint32_t     id;
        uint64_t     hseq;
        std::string  group;
        bool         hist;
        uint32_t     bit;
        uint8_t      part;   // 0 body, 1 chevron, 2 close
        std::string  action; // non-empty: a specific action button
        std::string  href;   // non-empty: a body hyperlink
        bool         outside = false; // the click fell outside every surface (closes the center)
    };
    static std::vector<SHit> hitQueue;
    static bool              hitQueued = false;
    static NHyprCommon::CHop pendingHit;
    static NHyprCommon::CHop pendingEsc;

    // most-specific-first: rows/buttons are pushed after the panel they sit on
    static const SCard* cardAt(const Vector2D& pos) {
        if (cards.empty())
            return nullptr;
        const auto MON = cardsMon.lock();
        if (!MON || !MON->logicalBox().containsPoint(pos))
            return nullptr;
        for (size_t i = cards.size(); i-- > 0;)
            if (cards[i].box.containsPoint(pos))
                return &cards[i];
        return nullptr;
    }

    static int buttonAt(const SCard& c, const Vector2D& pos) {
        for (size_t i = 0; i < c.buttons.size(); i++)
            if (c.buttons[i].box.containsPoint(pos))
                return (int)i;
        return -1;
    }

    static int linkAt(const SCard& c, const Vector2D& pos) {
        for (size_t i = 0; i < c.links.size(); i++)
            if (c.links[i].box.containsPoint(pos))
                return (int)i;
        return -1;
    }

    static uint8_t partAt(const SCard& c, const Vector2D& pos) {
        if (c.chevron.w > 0 && c.chevron.containsPoint(pos))
            return 1;
        if (c.close.w > 0 && c.close.containsPoint(pos))
            return 2;
        return 0;
    }

    // ---- the deferred drain: what each surface DOES ----

    static void invokeLive(uint32_t id, const std::string& actionOverride) {
        std::string action = actionOverride;
        bool        resident = false;
        for (const auto& N : notifs)
            if (N->id == id) {
                if (action.empty())
                    action = N->defaultAction;
                resident = N->resident;
                break;
            }
        if (!action.empty())
            Bus::invokeAction(id, action);
        if (!(resident && !action.empty())) // resident keeps the card once an action fired
            Bus::closeOne(id, Bus::R_DISMISSED);
    }

    static void drainHits() {
        hitQueued    = false;
        const auto Q = std::move(hitQueue);
        hitQueue.clear();
        for (const auto& H : Q) {
            if (H.outside) { // a click off every surface closes the center
                setCenter(false);
                continue;
            }
            switch (H.kind) {
                case SCard::POPUP: {
                    if (H.bit == 4u) {
                        Bus::dismissAllLive(); // the sweep, like the old middle
                        return;                // the rest reference dismissed cards
                    }
                    if (H.bit == 2u || H.part == 2) {
                        Bus::closeOne(H.id, Bus::R_DISMISSED);
                        continue;
                    }
                    if (!H.href.empty()) { // left on a hyperlink: open it, keep the card up
                        spawnDetached({"xdg-open", H.href.c_str(), nullptr});
                        continue;
                    }
                    invokeLive(H.id, H.action);
                    continue;
                }
                case SCard::ROW:
                case SCard::CHILD: {
                    if (H.part == 1 && H.bit == 1u) { // the chevron folds
                        centerToggleRow(H.id, H.hseq);
                        continue;
                    }
                    if (H.hseq) { // a history row
                        if (H.bit == 4u) {
                            Bus::clearHistory();
                            continue;
                        }
                        if (H.bit == 2u) {
                            Bus::eraseHistory(H.hseq);
                            continue;
                        }
                        if (!H.action.empty()) { // the original action, best effort; entry consumed
                            Bus::invokeHistoryAction(H.hseq, H.action);
                            continue;
                        }
                        Bus::recallAt(H.hseq); // left: recall — fresh id, original age
                        continue;
                    }
                    // a live shade row
                    if (H.bit == 4u) {
                        Bus::dismissAllLive();
                        return;
                    }
                    if (H.bit == 2u) {
                        Bus::closeOne(H.id, Bus::R_DISMISSED);
                        continue;
                    }
                    invokeLive(H.id, H.action);
                    continue;
                }
                case SCard::DIGEST: {
                    if (H.bit == 1u) {
                        centerToggleGroup(H.group, H.hist);
                        continue;
                    }
                    if (H.bit == 2u) {
                        if (H.hist)
                            Bus::eraseHistoryApp(H.group);
                        else
                            Bus::dismissApp(H.group);
                        continue;
                    }
                    if (H.bit == 4u) {
                        if (H.hist)
                            Bus::clearHistory();
                        else
                            Bus::dismissAllLive();
                    }
                    continue;
                }
                case SCard::GHEAD: {
                    if (H.part == 2 || H.bit == 2u) { // the static ✕ / right: the whole group goes
                        if (H.hist)
                            Bus::eraseHistoryApp(H.group);
                        else
                            Bus::dismissApp(H.group);
                        continue;
                    }
                    if (H.bit == 1u) {
                        centerToggleGroup(H.group, H.hist); // collapse
                        continue;
                    }
                    if (H.bit == 4u) {
                        if (H.hist)
                            Bus::clearHistory();
                        else
                            Bus::dismissAllLive();
                    }
                    continue;
                }
                case SCard::BTN_HIST:
                    if (H.bit == 1u)
                        centerFlipView();
                    continue;
                case SCard::BTN_CLEAR:
                    if (H.bit == 1u) {
                        if (centerInHistory())
                            Bus::clearHistory();
                        else
                            Bus::dismissAllLive();
                    }
                    continue;
                case SCard::BTN_DND:
                    if (H.bit == 1u)
                        Bus::toggleSuspend();
                    continue;
                case SCard::PANEL: continue; // dead panel space swallows silently
            }
        }
    }

    void onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
        // emissions precede the compositor's own lock handling: locked input
        // belongs to the lockscreen, and half-tracked state must not survive it
        if (NHyprCommon::sessionLocked()) {
            swallowRelease = 0;
            heldButtons    = 0;
            return;
        }

        const uint32_t BIT = e.button == BTN_LEFT ? 1u : e.button == BTN_RIGHT ? 2u : e.button == BTN_MIDDLE ? 4u : 0u;

        if (e.state == WL_POINTER_BUTTON_STATE_RELEASED) {
            if (BIT && (swallowRelease & BIT)) {
                swallowRelease &= ~BIT;
                info.cancelled = true;
            } else if (!info.cancelled) // a release hyprbar swallowed ends a press we never counted
                heldButtons = std::max(0, heldButtons - 1);
            return;
        }

        // hyprbar runs first: a press it swallowed (strip click, open tray
        // menu over the card region) was never ours — and never reached an
        // app, so there is no grab to count
        if (info.cancelled)
            return;

        const auto COORDS = g_pInputManager->getMouseCoordsInternal();
        const auto CARD   = BIT ? cardAt(COORDS) : nullptr;

        if (!CARD) {
            // Android closes the shade on an outside tap; the closing click
            // is swallowed, like the tray menu's
            if (centerVisible() && BIT) {
                info.cancelled = true;
                swallowRelease |= BIT;
                hitQueue.push_back({.outside = true});
                if (!hitQueued) {
                    hitQueued = true;
                    pendingHit.arm(drainHits);
                }
                return;
            }
            heldButtons++;
            return;
        }

        info.cancelled = true; // the surface is ours: the press must not reach the window beneath
        swallowRelease |= BIT;

        SHit h;
        h.kind  = CARD->kind;
        h.id    = CARD->id;
        h.hseq  = CARD->hseq;
        h.group = CARD->group;
        h.hist  = CARD->hist;
        h.bit   = BIT;
        h.part  = partAt(*CARD, COORDS);
        if (BIT == 1u && h.part == 0) {
            if (const int B = buttonAt(*CARD, COORDS); B >= 0)
                h.action = CARD->buttons[B].id;
            else if (const int L = linkAt(*CARD, COORDS); L >= 0)
                h.href = CARD->links[L].href;
        }

        // Deferred out of the input emission: closes reflow the layout and an
        // action can make the client focus/raise itself. Queue+drain so two
        // clicks in one dispatch both land.
        hitQueue.push_back(std::move(h));
        if (hitQueued)
            return;
        hitQueued = true;
        pendingHit.arm(drainHits);
    }

    // ---- wheel: page the center, only inside the panel box ----

    static double scrollAcc = 0;

    void onMouseAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) {
        if (NHyprCommon::sessionLocked()) {
            scrollAcc = 0;
            return;
        }
        if (!centerVisible() || cards.empty() || info.cancelled)
            return;
        const auto POS  = g_pInputManager->getMouseCoordsInternal();
        const auto CARD = cardAt(POS);
        if (!CARD)
            return; // outside the panel: windows scroll normally
        info.cancelled = true;
        if (e.axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
            return;
        scrollAcc += e.delta != 0.0 ? e.delta : e.deltaDiscrete / 120.0 * 15.0;
        if (const int STEP = (int)(scrollAcc / 15.0); STEP != 0) {
            scrollAcc -= STEP * 15.0;
            centerPage(STEP);
        }
    }

    // ---- esc peels the center (tray menu > center > menubar: load order
    //      puts hyprbar's menu first, we're next) ----

    void onKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info) {
        if (NHyprCommon::sessionLocked())
            return;
        if (!centerVisible() || info.cancelled)
            return;
        // releases pass untouched (crash class 3: never cancel key releases)
        if (e.state != WL_KEYBOARD_KEY_STATE_PRESSED)
            return;
        const auto KB = g_pSeatManager ? g_pSeatManager->m_keyboard.lock() : nullptr;
        if (!KB || !KB->m_xkbState)
            return;
        if (xkb_state_key_get_one_sym(KB->m_xkbState, e.keycode + 8) != XKB_KEY_Escape)
            return;
        info.cancelled = true;
        pendingEsc.arm([]() { setCenter(false); }); // deferred: the close reflows and refocuses
    }

    // ---- pointer ownership ----

    void releasePointer() {
        if (!pointerOwned)
            return;
        pointerOwned = false;
        cursorHand   = false;
        Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
    }

    // The cards own the pointer over them: hover must not leak to the window
    // poking underneath (sloppy focus would flip focus under every popup).
    // Hands off while a button is held or a drag is live — implicit grabs and
    // drags keep flowing, as they would over a real layer-surface daemon.
    void onMouseMove(const Vector2D& pos, Event::SCallbackInfo& info) {
        if (NHyprCommon::sessionLocked()) {
            setHovered({});
            releasePointer();
            return;
        }

        // cheap first: almost every motion happens with nothing shown
        if (cards.empty()) {
            setHovered({});
            releasePointer();
            return;
        }

        // info.cancelled: an earlier listener (hyprbar's strip or an open
        // menu) owns the point — and just set the shared SPECIAL_ACTION
        // cursor slot. Drop ownership WITHOUT unsetting it: releasePointer's
        // unset would strip the bar's override for its whole visit.
        if (info.cancelled) {
            setHovered({});
            pointerOwned = false;
            return;
        }

        const auto CARD = cardAt(pos);
        if (!CARD || heldButtons > 0 || (g_layoutManager && g_layoutManager->dragController()->target())) {
            setHovered({});
            releasePointer();
            return;
        }

        SHover h;
        h.kind  = CARD->kind;
        h.id    = CARD->id;
        h.hseq  = CARD->hseq;
        h.group = CARD->group;
        h.btn   = buttonAt(*CARD, pos);
        h.part  = h.btn >= 0 ? 0 : partAt(*CARD, pos);
        setHovered(h);
        info.cancelled = true;

        const bool ONLINK = h.btn < 0 && h.part == 0 && linkAt(*CARD, pos) >= 0; // a hyperlink shows the hand (GTK convention)

        const bool ENTERING = !pointerOwned;
        if (ENTERING) {
            pointerOwned = true;
            g_pSeatManager->setPointerFocus(nullptr, {}); // the app under the surface gets its leave
        }
        // set the shape on entry, and re-set only when it flips (a still stream
        // of motion must not re-assert the override every event)
        if (ENTERING || cursorHand != ONLINK) {
            Pointer::Cursor::overrideController->setOverride(ONLINK ? "pointer" : "left_ptr", Pointer::Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
            cursorHand = ONLINK;
        }
    }

    // A surface can vanish under a motionless pointer (expiry, a dismissal,
    // the center closing): without this the cursor override lingers and the
    // window beneath keeps NO pointer focus until the next motion — dead
    // hover UI. A real layer-surface daemon's unmap triggers the compositor's
    // own refocus; match it. Runs from the notifChanged doLater, never an
    // input emission.
    void refreshPointerOwnership() {
        const auto COORDS = g_pInputManager->getMouseCoordsInternal();
        const auto CARD   = cardAt(COORDS);
        if (CARD) { // a reflow can slide another surface under the still pointer
            SHover h;
            h.kind  = CARD->kind;
            h.id    = CARD->id;
            h.hseq  = CARD->hseq;
            h.group = CARD->group;
            h.btn   = buttonAt(*CARD, COORDS);
            h.part  = h.btn >= 0 ? 0 : partAt(*CARD, COORDS);
            setHovered(h);
        } else
            setHovered({});
        if (!pointerOwned || CARD)
            return;
        releasePointer();
        g_pInputManager->simulateMouseMovement(); // the window beneath gets its enter back
    }

    void inputExit() {
        pendingHit.reset();
        pendingEsc.reset();
        hitQueued = false;
        hitQueue.clear();
        swallowRelease = 0;
        heldButtons    = 0;
        scrollAcc      = 0;
        releasePointer();
    }

} // namespace NHyprnotify
