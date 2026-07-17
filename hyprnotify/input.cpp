// hyprnotify/input.cpp — clicks and pointer ownership over the cards

#include "hyprnotify.hpp"

namespace NHyprnotify {

    static uint32_t                  swallowRelease = 0; // bit 1 = left, 2 = right, 4 = middle
    static int                       heldButtons    = 0; // presses that reached apps: an implicit grab may be live
    static bool                      pointerOwned   = false;

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
            } else
                heldButtons = std::max(0, heldButtons - 1);
            return;
        }

        const auto CARD = BIT ? cardAt(g_pInputManager->getMouseCoordsInternal()) : nullptr;
        if (!CARD) {
            heldButtons++;
            return;
        }

        info.cancelled = true; // the card is ours: the press must not reach the window beneath
        swallowRelease |= BIT;

        // Deferred out of the input emission: the close reflows the stack and
        // an action can make the client focus/raise itself.
        pendingHit = g_pEventLoopManager->doLaterLock([ID = CARD->id, BIT]() {
            if (BIT == 4u) { // middle sweeps the stack, like the old mouse binding
                Bus::closeAll(Bus::R_DISMISSED);
                return;
            }
            if (BIT == 1u) // left invokes the default action when the client sent one
                for (const auto& N : notifs)
                    if (N->id == ID) {
                        if (!N->defaultAction.empty())
                            Bus::invokeAction(ID, N->defaultAction);
                        break;
                    }
            Bus::closeOne(ID, Bus::R_DISMISSED);
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
            releasePointer();
            return;
        }

        // cheap first: almost every motion happens with no notification up
        if (cards.empty()) {
            releasePointer();
            return;
        }

        if (!cardAt(pos) || heldButtons > 0 || (g_layoutManager && g_layoutManager->dragController()->target())) {
            releasePointer();
            return;
        }

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
        if (!pointerOwned || cardAt(g_pInputManager->getMouseCoordsInternal()))
            return;
        releasePointer();
        g_pInputManager->simulateMouseMovement(); // the window beneath gets its enter back
    }

    void inputExit() {
        pendingHit.reset();
        swallowRelease = 0;
        heldButtons    = 0;
        releasePointer();
    }

} // namespace NHyprnotify
