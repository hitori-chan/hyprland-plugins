// hyprnotify/input.cpp — clicks and pointer ownership over the cards

#include "hyprnotify.hpp"

namespace NHyprnotify {

    static uint32_t                  swallowRelease = 0; // bit 1 = left, 2 = right, 4 = middle
    static int                       heldButtons    = 0; // presses that reached apps: an implicit grab may be live
    static bool                      pointerOwned   = false;

    // clicks accumulate into one drain: two card-clicks in a single dispatch
    // would otherwise clobber the lock and lose the first's action + dismiss
    static std::vector<std::pair<uint32_t, uint32_t>> hitQueue; // (card id, button bit)
    static bool                                       hitQueued = false;
    static UP<SEventLoopDoLaterLock>                  pendingHit;

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

        const auto CARD = BIT ? cardAt(g_pInputManager->getMouseCoordsInternal()) : nullptr;
        if (!CARD) {
            heldButtons++;
            return;
        }

        info.cancelled = true; // the card is ours: the press must not reach the window beneath
        swallowRelease |= BIT;

        // Deferred out of the input emission: the close reflows the stack and
        // an action can make the client focus/raise itself. Queue+drain so two
        // clicks in one dispatch both land instead of the second clobbering the first.
        hitQueue.emplace_back(CARD->id, BIT);
        if (hitQueued || !g_pEventLoopManager)
            return;
        hitQueued  = true;
        pendingHit = g_pEventLoopManager->doLaterLock([]() {
            hitQueued    = false;
            const auto Q = std::move(hitQueue);
            hitQueue.clear();
            for (const auto& [ID, BIT] : Q) {
                if (BIT == 4u) { // middle sweeps the stack, like the old mouse binding
                    Bus::closeAll(Bus::R_DISMISSED);
                    break; // the rest reference now-dismissed cards
                }
                if (BIT == 1u) // left invokes the default action when the client sent one
                    for (const auto& N : notifs)
                        if (N->id == ID) {
                            if (!N->defaultAction.empty())
                                Bus::invokeAction(ID, N->defaultAction);
                            break;
                        }
                Bus::closeOne(ID, Bus::R_DISMISSED);
            }
        });
    }

    void releasePointer() {
        if (!pointerOwned)
            return;
        pointerOwned = false;
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

        setHovered(CARD->id);
        info.cancelled = true;
        if (!pointerOwned) {
            pointerOwned = true;
            g_pSeatManager->setPointerFocus(nullptr, {}); // the app under the card gets its leave
            Pointer::Cursor::overrideController->setOverride("left_ptr", Pointer::Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
        }
    }

    // A card can vanish under a motionless pointer (expiry, CloseNotification,
    // a sweep): without this the cursor override lingers and the window
    // beneath keeps NO pointer focus until the next motion — dead hover UI. A
    // real layer-surface daemon's unmap triggers the compositor's own refocus;
    // match it. Runs from the notifChanged doLater, never an input emission.
    void refreshPointerOwnership() {
        const auto CARD = cardAt(g_pInputManager->getMouseCoordsInternal());
        setHovered(CARD ? CARD->id : 0); // a reflow can slide another card under the still pointer
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
