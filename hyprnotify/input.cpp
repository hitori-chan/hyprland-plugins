// hyprnotify/input.cpp — clicks and pointer ownership over the cards

#include "hyprnotify.hpp"

namespace NHyprnotify {

    static uint32_t                  swallowRelease = 0; // bit 1 = left, 2 = right, 4 = middle
    static int                       heldButtons    = 0; // presses that reached apps: an implicit grab may be live
    static bool                      pointerOwned   = false;
    static bool                      cursorHand     = false; // the override currently shows the link hand

    // clicks accumulate into one drain: two card-clicks in a single dispatch
    // would otherwise clobber the lock and lose the first's action + dismiss
    struct SHit {
        uint32_t    id;
        uint32_t    bit;
        std::string action; // non-empty: a specific action button was clicked
        std::string href;   // non-empty: a body hyperlink was clicked
    };
    static std::vector<SHit>         hitQueue;
    static bool                      hitQueued = false;
    static UP<SEventLoopDoLaterLock> pendingHit;

    static const SCard*              cardAt(const Vector2D& pos) {
        if (cards.empty())
            return nullptr;
        const auto MON = cardsMon.lock();
        if (!MON || !MON->logicalBox().containsPoint(pos))
            return nullptr;
        for (const auto& C : cards)
            if (C.box.containsPoint(pos))
                return &C;
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

    void onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
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
            heldButtons++;
            return;
        }

        info.cancelled = true; // the card is ours: the press must not reach the window beneath
        swallowRelease |= BIT;

        // a left click on a specific action button invokes that action, on a
        // body hyperlink opens it; the body (or any right/middle) falls through
        // to the default/dismiss/sweep logic
        std::string action, href;
        if (BIT == 1u) {
            if (const int B = buttonAt(*CARD, COORDS); B >= 0)
                action = CARD->buttons[B].id;
            else if (const int L = linkAt(*CARD, COORDS); L >= 0)
                href = CARD->links[L].href;
        }

        // Deferred out of the input emission: the close reflows the stack and
        // an action can make the client focus/raise itself. Queue+drain so two
        // clicks in one dispatch both land instead of the second clobbering the first.
        hitQueue.push_back({CARD->id, BIT, action, href});
        if (hitQueued || !g_pEventLoopManager)
            return;
        hitQueued  = true;
        pendingHit = g_pEventLoopManager->doLaterLock([]() {
            hitQueued    = false;
            const auto Q = std::move(hitQueue);
            hitQueue.clear();
            for (const auto& H : Q) {
                if (H.bit == 4u) { // middle sweeps the stack, like the old mouse binding
                    Bus::closeAll(Bus::R_DISMISSED);
                    break; // the rest reference now-dismissed cards
                }
                if (H.bit == 2u) { // right dismisses, no action
                    Bus::closeOne(H.id, Bus::R_DISMISSED);
                    continue;
                }
                if (!H.href.empty()) { // left on a hyperlink: open it, keep the card up
                    spawnDetached({"xdg-open", H.href.c_str(), nullptr});
                    continue;
                }
                // left: a specific button (H.action) or the body's default action
                std::string action   = H.action;
                bool        resident = false;
                for (const auto& N : notifs)
                    if (N->id == H.id) {
                        if (action.empty())
                            action = N->defaultAction;
                        resident = N->resident;
                        break;
                    }
                if (!action.empty())
                    Bus::invokeAction(H.id, action);
                if (!(resident && !action.empty())) // resident keeps the card once an action fired
                    Bus::closeOne(H.id, Bus::R_DISMISSED);
            }
        });
    }

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
        if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked()) {
            setHovered(0);
            releasePointer();
            return;
        }

        // cheap first: almost every motion happens with no notification up
        if (cards.empty()) {
            setHovered(0);
            releasePointer();
            return;
        }

        // info.cancelled: an earlier listener (hyprbar's strip or an open
        // menu) owns the point — and just set the shared SPECIAL_ACTION
        // cursor slot. Drop ownership WITHOUT unsetting it: releasePointer's
        // unset would strip the bar's override for its whole visit.
        if (info.cancelled) {
            setHovered(0);
            pointerOwned = false;
            return;
        }

        const auto CARD = cardAt(pos);
        if (!CARD || heldButtons > 0 || (g_layoutManager && g_layoutManager->dragController()->target())) {
            setHovered(0);
            releasePointer();
            return;
        }

        const int  BTN    = buttonAt(*CARD, pos);
        const bool ONLINK = BTN < 0 && linkAt(*CARD, pos) >= 0; // a hyperlink shows the hand; a button keeps the arrow (GTK convention)
        setHovered(CARD->id, BTN);
        info.cancelled = true;

        const bool ENTERING = !pointerOwned;
        if (ENTERING) {
            pointerOwned = true;
            g_pSeatManager->setPointerFocus(nullptr, {}); // the app under the card gets its leave
        }
        // set the shape on entry, and re-set only when it flips (a still stream
        // of motion must not re-assert the override every event)
        if (ENTERING || cursorHand != ONLINK) {
            Pointer::Cursor::overrideController->setOverride(ONLINK ? "pointer" : "left_ptr", Pointer::Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
            cursorHand = ONLINK;
        }
    }

    // A card can vanish under a motionless pointer (expiry, CloseNotification,
    // a sweep): without this the cursor override lingers and the window
    // beneath keeps NO pointer focus until the next motion — dead hover UI. A
    // real layer-surface daemon's unmap triggers the compositor's own refocus;
    // match it. Runs from the notifChanged doLater, never an input emission.
    void refreshPointerOwnership() {
        const auto COORDS = g_pInputManager->getMouseCoordsInternal();
        const auto CARD   = cardAt(COORDS);
        setHovered(CARD ? CARD->id : 0, CARD ? buttonAt(*CARD, COORDS) : -1); // a reflow can slide another card under the still pointer
        if (!pointerOwned || CARD)
            return;
        releasePointer();
        g_pInputManager->simulateMouseMovement(); // the window beneath gets its enter back
    }

    void inputExit() {
        pendingHit.reset();
        hitQueued = false;
        hitQueue.clear();
        swallowRelease = 0;
        heldButtons    = 0;
        releasePointer();
    }

} // namespace NHyprnotify
