// awesome/notify/input.cpp — clicks, wheel paging, esc and pointer ownership
// over the popups and the shade. Implements the interaction map exactly:
//
//   popup    left = action/link/default → dismiss · right = dismiss ·
//            middle = park the stack into the shade · hover reveals ✕
//   row      a shade row IS its banner: left on the body fires the card's
//            primary (the fd.o `default`) and dismisses unless resident,
//            same as the popup — rows open by default, so the click is
//            spent on acting rather than on revealing. The CHEVRON is the
//            only fold target · a link opens · a button acts · right =
//            dismiss · middle = Clear all
//   leaving  anything that RAISES something else closes the shade with it —
//            the primary, an action button, a link (invokeLive below has
//            the AOSP citation). Everything that keeps you here does not:
//            a dismissal, a fold, DND, Clear all, and a `resident` card's
//            actions, which is the spec's own way of saying the action does
//            not take you away.
//   child    a bundle child is a row without the fold: body, links, buttons
//   digest   left expands the app's bundle · right dismisses
//   ghead    left collapses · the ✕ / right dismisses the bundle
//   footer   ⊖ = DND · "Clear all" = the global sweep
//   wheel    vertical pages the shade — captured only inside the panel box.
//            HORIZONTAL AWAY on a row is the phone's swipe-to-dismiss. Strictly
//            an addition — a mouse without a horizontal wheel never reaches it
//            and loses no verb.
//   keys     while the shade is open it owns the nav set and nothing else:
//            esc closes · ↑/↓ move the selection · space folds it (the click's
//            twin) · enter fires the primary · delete dismisses · tab opens its
//            reply field, and while one is armed EVERY key is the field's
//            (reply.cpp). A chord with ctrl/alt/super is the user's bind, and
//            a nav key with NOTHING selected still belongs to whatever holds
//            focus: the shade never grabs a key it has no use for.
//
// Every mutation lands via the hit queue + CHop drain, never synchronously
// inside the emission (crash class 6); every listener gates on
// sessionLocked() first and resets its half-tracked state there (class 7),
// and a native input-capture session, an implicit or seat grab, or a native
// layer surface at the point passes through unintercepted (the
// input-capture-v1 / native hit-test contract, mirrored from the bar).

#include "ui.hpp"

#include "../core/jobs.hpp"

#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>

#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon-names.h>

namespace NAwesome::Notify {

    static uint32_t swallowRelease = 0; // bit 1 = left, 2 = right, 4 = middle
    static int      heldButtons    = 0; // presses that reached apps: an implicit grab may be live
    static bool     pointerOwned   = false;
    static bool     cursorHand     = false; // the override currently shows the link hand

    // clicks accumulate into one drain: two card-clicks in a single dispatch
    // would otherwise clobber the lock and lose the first's action + dismiss
    struct SHit {
        SCard::eKind kind;
        uint32_t     id;
        std::string  group;
        uint32_t     bit;
        uint8_t      part;   // the SHover part codes: 0 body, 1 chevron, 2 close, 3 reply field, 4 send
        std::string  action; // non-empty: a specific action button
        std::string  href;   // non-empty: a body hyperlink
        bool         outside = false; // the click fell outside every surface (closes the shade)
    };
    static void           drainHits(std::vector<SHit>& batch);
    // clicks deferred out of the input emission; 32 pending is far past a
    // human's clicks per turn
    static NAwesome::CHopQueue<SHit, 32> hits(drainHits);
    static NAwesome::CHop pendingEsc;

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
        if (c.replySend.w > 0 && c.replySend.containsPoint(pos))
            return 4;
        if (c.replyField.w > 0 && c.replyField.containsPoint(pos))
            return 3;
        return 0;
    }

    // ---- the deferred drain: what each surface DOES ----

    // Firing a card's primary (or one of its buttons) LEAVES: the sender
    // raises itself over the very shade the click was made in, so the shade
    // must get out of the way. Android does exactly this —
    // StatusBarNotificationActivityStarter collapses the shade on a
    // content-intent click, and handleRemoteViewClick closes it for any
    // action that starts an activity ("close the shade if it was open and
    // maybe wait for activity start") — and swaync ships the same as
    // hide-on-action, default on.
    //
    // An action that does NOT start an activity leaves Android's shade
    // standing. fd.o has no isActivity, but it has `resident`: "the server
    // will not automatically remove the notification when an action has been
    // invoked" — the spec's way of saying this action keeps you here. So the
    // shade goes exactly when the card does. A card with no action at all
    // launches nothing: that click is a dismissal, and dismissing never
    // closes the shade, here or on a phone.
    static void invokeLive(uint32_t id, const std::string& actionOverride) {
        std::string action   = actionOverride;
        std::string sender; // the X11 activation lookup, captured before close-on-act
        bool        resident = false, live = false;
        for (const auto& N : notifs)
            if (N->id == id) {
                if (action.empty())
                    action = N->defaultAction;
                resident = N->resident;
                sender   = N->sender;
                live     = true;
                break;
            }
        // the card went between the press and this deferred click (closed by
        // its sender, expired): its action must not fire for a gone id
        if (!live)
            return;
        if (action.empty()) { // nothing to fire: the body click is a dismissal
            Model::closeOne(id, Model::R_DISMISSED);
            return;
        }
        Bus::invokeAction(id, action, sender);
        if (resident)
            return; // the card stays, and so does the shade behind it
        setCenter(false); // no re-pop: the app the action raises is coming up over the parked stack
        Model::closeOne(id, Model::R_DISMISSED);
    }

    // Deferred out of the input emission: closes reflow the layout and an
    // action can make the client focus/raise itself. Queue+drain so two
    // clicks in one dispatch both land.
    static void queueHit(SHit h) {
        hits.push(std::move(h));
    }

    static void drainHits(std::vector<SHit>& batch) {
        for (const auto& H : batch) {
            if (H.outside) { // a click off every surface closes the center
                setCenter(false, /*repop=*/true);
                continue;
            }
            switch (H.kind) {
                case SCard::POPUP: {
                    if (H.bit == 4u) {
                        Model::absorbPopped(); // middle: park the stack into the shade (no dismiss)
                        return;              // the rest reference now-parked cards
                    }
                    if (H.bit == 2u || H.part == 2) {
                        Model::closeOne(H.id, Model::R_DISMISSED);
                        continue;
                    }
                    if (!H.href.empty()) { // left on a hyperlink: open it, keep the card up
                        NAwesome::Jobs::inst().spawn(NAwesome::Jobs::OPEN, {"xdg-open", H.href.c_str(), nullptr});
                        continue;
                    }
                    invokeLive(H.id, H.action);
                    continue;
                }
                case SCard::ROW:
                case SCard::CHILD: {
                    if (H.bit == 4u) {
                        Model::dismissAllLive();
                        return; // the rest of the queue references swept cards
                    }
                    if (H.bit == 2u) {
                        Model::closeOne(H.id, Model::R_DISMISSED);
                        continue;
                    }
                    if (H.bit != 1u)
                        continue;
                    if (H.part == 1) { // the chevron, and only it, folds
                        centerToggleRow(H.id);
                        continue;
                    }
                    if (H.part == 3) // inside the armed field: keep typing
                        continue;
                    if (H.part == 4) { // its send pill
                        const auto TX = replyText();
                        replyClose();
                        Bus::sendReply(H.id, TX);
                        continue;
                    }
                    if (H.action == "inline-reply") { // the chip arms the field
                        replyOpen(H.id);
                        continue;
                    }
                    if (!H.action.empty()) {
                        invokeLive(H.id, H.action);
                        continue;
                    }
                    if (!H.href.empty()) { // a link in the body: open it, keep the card
                        NAwesome::Jobs::inst().spawn(NAwesome::Jobs::OPEN, {"xdg-open", H.href.c_str(), nullptr});
                        setCenter(false); // but not the shade: a browser is coming up over it
                        continue;
                    }
                    invokeLive(H.id, ""); // the body IS the card's primary, as on the banner
                    continue;
                }
                case SCard::DIGEST: {
                    if (H.bit == 1u) { // left expands the app's bundle
                        centerToggleGroup(H.group);
                        continue;
                    }
                    if (H.bit == 2u) { // right: the whole bundle goes
                        Model::dismissApp(H.group);
                        continue;
                    }
                    if (H.bit == 4u) {
                        Model::dismissAllLive();
                        return;
                    }
                    continue;
                }
                case SCard::GHEAD: {
                    if (H.part == 2 || H.bit == 2u) { // the static ✕ / right: the whole bundle goes
                        Model::dismissApp(H.group);
                        continue;
                    }
                    if (H.bit == 1u) {
                        centerToggleGroup(H.group); // collapse
                        continue;
                    }
                    if (H.bit == 4u) {
                        Model::dismissAllLive();
                        return;
                    }
                    continue;
                }
                case SCard::BTN_CLEAR: // the footer: the global sweep
                    if (H.bit == 1u)
                        Model::dismissAllLive();
                    continue;
                case SCard::BTN_DND:
                    if (H.bit == 1u)
                        Model::toggleSuspend();
                    continue;
                case SCard::PANEL: continue; // dead panel space swallows silently
                case SCard::PAGE_UP:
                case SCard::PAGE_DOWN:
                    if (H.bit == 1u)
                        centerPageScreen(H.kind == SCard::PAGE_DOWN ? 1 : -1);
                    continue;
            }
        }
    }

    void onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
        // emissions precede the compositor's own lock handling: locked input
        // belongs to the lockscreen, and half-tracked state must not survive it
        if (NAwesome::sessionLocked()) {
            swallowRelease = 0;
            heldButtons    = 0;
            return;
        }
        if (NAwesome::nativeInputCaptureActive()) {
            swallowRelease = 0;
            heldButtons    = 0;
            return;
        }

        const uint32_t BIT = e.button == BTN_LEFT ? 1u : e.button == BTN_RIGHT ? 2u : e.button == BTN_MIDDLE ? 4u : 0u;

        if (e.state == WL_POINTER_BUTTON_STATE_RELEASED) {
            if (BIT && (swallowRelease & BIT)) {
                swallowRelease &= ~BIT;
                info.cancelled = true;
            } else if (!info.cancelled) // a release the shell module swallowed ends a press we never counted
                heldButtons = std::max(0, heldButtons - 1);
            return;
        }

        // the shell module runs first: a press it swallowed (strip click, open tray
        // menu over the card region) was never ours — and never reached an
        // app, so there is no grab to count
        if (info.cancelled)
            return;

        const auto COORDS = g_pInputManager->getMouseCoordsInternal();
        const auto CARD   = BIT ? cardAt(COORDS) : nullptr;

        // A live implicit or seat grab, or a native layer surface at the
        // point, stays authoritative over every drawn surface (the bar's
        // strip does the same): the press goes through, the cards only watch
        if (heldButtons > 0 || NAwesome::nativePointerGrabActive()) {
            heldButtons++;
            return;
        }
        if (NAwesome::nativeLayerOwnsPointer())
            return;

        if (!CARD) {
            // Android closes the shade on an outside tap; the closing click
            // is swallowed, like the tray menu's. The corner dead-strip
            // (offset_y, the screen edge) is the most common stray click
            // next to a conversation — drainHits re-pops the absorbed stack
            // so the close does not leave the notifications invisible
            if (centerVisible() && BIT) {
                info.cancelled = true;
                swallowRelease |= BIT;
                queueHit({.outside = true});
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
        h.group = CARD->group;
        h.bit   = BIT;
        h.part  = partAt(*CARD, COORDS);
        if (BIT == 1u && h.part == 0) {
            if (const int B = buttonAt(*CARD, COORDS); B >= 0)
                h.action = CARD->buttons[B].id;
            else if (const int L = linkAt(*CARD, COORDS); L >= 0)
                h.href = CARD->links[L].href;
        }

        queueHit(std::move(h));
    }

    // ---- wheel: page the center, only inside the panel box ----

    static double scrollAcc = 0, swipeAcc = 0;
    static uint32_t swipeOn = 0; // the row the current horizontal gesture belongs to

    void            onMouseAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) {
        if (NAwesome::sessionLocked()) {
            scrollAcc = swipeAcc = 0;
            swipeOn              = 0;
            return;
        }
        if (NAwesome::nativeInputCaptureActive()) {
            scrollAcc = swipeAcc = 0;
            swipeOn              = 0;
            return;
        }
        if (heldButtons > 0 || NAwesome::nativePointerGrabActive() || NAwesome::nativeLayerOwnsPointer())
            return;
        if (!centerVisible() || cards.empty() || info.cancelled)
            return;
        const auto POS  = g_pInputManager->getMouseCoordsInternal();
        const auto CARD = cardAt(POS);
        if (!CARD)
            return; // outside the panel: windows scroll normally
        info.cancelled = true;
        const double DELTA = e.delta != 0.0 ? e.delta : e.deltaDiscrete / 120.0 * 15.0;

        // HORIZONTAL AWAY on a row is the phone's swipe-to-dismiss. It is
        // strictly an ADDITION: a mouse with no horizontal wheel never
        // reaches here, and right-click is the same verb without the gesture.
        if (e.axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
            const bool ROW = CARD->kind == SCard::ROW || CARD->kind == SCard::CHILD;
            if (!ROW || CARD->id == 0) {
                swipeAcc = 0;
                swipeOn  = 0;
                return;
            }
            if (swipeOn != CARD->id) { // the pointer moved to another row mid-gesture
                swipeOn  = CARD->id;
                swipeAcc = 0;
            }
            swipeAcc += DELTA;
            constexpr double SWIPE = 60.0; // a deliberate flick, not a nudge
            // Both verbs go through the CLICK queue rather than acting here:
            // a dismissal reflows the layout and emits on the bus, and nothing
            // may do that inside an input emission (crash class 6). A swipe is
            // an alias for a click that already exists, so it drains down the
            // very same path.
            if (swipeAcc >= SWIPE || swipeAcc <= -SWIPE) {
                const bool AWAY = swipeAcc > 0;
                swipeAcc        = 0; // bounded either way; only away acts
                if (AWAY) {
                    SHit h;
                    h.kind = CARD->kind;
                    h.id   = CARD->id;
                    h.bit  = 2u; // the swipe IS the right-click
                    h.part = 0;
                    queueHit(std::move(h));
                }
            }
            return;
        }
        if (e.axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
            return;
        swipeAcc = 0; // a vertical scroll ends any half-made flick
        scrollAcc += DELTA;
        if (const int STEP = (int)(scrollAcc / 15.0); STEP != 0) {
            scrollAcc -= STEP * 15.0;
            centerPage(STEP);
        }
    }

    // ---- keys: esc peels the center (tray menu > center > menubar: load
    //      order puts the shell's menu first, we're next), and the shade drives
    //      its selection ----

    // Same shape as the click queue, and for the same reason: an action can
    // make the client focus itself, so nothing runs inside the emission.
    struct SKeyAct {
        int      verb = 0; // 1 fold, 2 the primary, 3 dismiss
        uint32_t    id   = 0;
        std::string group; // non-empty: a bundle
    };
    static void drainKeys(std::vector<SKeyAct>& batch) {
        for (const auto& A : batch) {
            const bool GROUP = !A.group.empty();
            if (A.verb == 1 || (A.verb == 2 && GROUP)) { // space, and enter on a bundle: fold
                if (GROUP)
                    centerToggleGroup(A.group);
                else
                    centerToggleRow(A.id);
            } else if (A.verb == 2)
                invokeLive(A.id, ""); // enter on a card: its primary, the body click's twin
            else if (A.verb == 3) {
                if (GROUP)
                    Model::dismissApp(A.group);
                else
                    Model::closeOne(A.id, Model::R_DISMISSED);
            }
        }
    }

    // shade key actions, deferred like the clicks
    static NAwesome::CHopQueue<SKeyAct, 32> keys(drainKeys);

    void onKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info) {
        if (NAwesome::sessionLocked()) {
            // a lock discards every half-tracked input state (crash class 3):
            // a queued shade action or pending esc must not drain after unlock,
            // and an armed reply field must not keep owning the keyboard with
            // text collected while the lock was up
            pendingEsc.reset();
            keys.reset();
            replyExit();
            return;
        }
        if (!centerVisible() || info.cancelled)
            return;
        // input-capture-v1 is fed after the plugin emissions: while a client
        // owns the physical stream, the shade must not cancel a key first
        if (NAwesome::nativeInputCaptureActive())
            return;
        // releases pass untouched (crash class 3: never cancel key releases)
        if (e.state != WL_KEYBOARD_KEY_STATE_PRESSED)
            return;
        const auto KB = g_pSeatManager ? g_pSeatManager->m_keyboard.lock() : nullptr;
        if (!KB || !KB->m_xkbState)
            return;

        // An armed reply field owns the keyboard first: the user is typing a
        // sentence, and every nav key below would otherwise steal a letter.
        const bool HELD = replyHeld();
        if (replyArmed()) {
            if (replyKey(KB->m_xkbState, e.keycode + 8))
                info.cancelled = true;
            return;
        }
        // The field's card died mid-sentence (the sender closed it: the chat
        // was read elsewhere) and the field disarmed itself. This key was
        // typed for the field, and the selection's index now names the card
        // that took the dead one's place: swallow the key, drop the
        // selection — never a space that folds, or a Return that fires,
        // someone else's card.
        if (HELD) {
            centerDeselect();
            const auto SYM = xkb_state_key_get_one_sym(KB->m_xkbState, e.keycode + 8);
            if (SYM < XKB_KEY_Shift_L || SYM > XKB_KEY_Hyper_R) // a bare modifier passes, as in the field
                info.cancelled = true;
            return;
        }

        // a modified chord is a user bind passing through, never the shade's
        for (const char* M : {XKB_MOD_NAME_CTRL, XKB_MOD_NAME_ALT, XKB_MOD_NAME_LOGO})
            if (xkb_state_mod_name_is_active(KB->m_xkbState, M, XKB_STATE_MODS_EFFECTIVE) > 0)
                return;

        const auto SYM = xkb_state_key_get_one_sym(KB->m_xkbState, e.keycode + 8);
        if (SYM == XKB_KEY_Escape) {
            info.cancelled = true;
            pendingEsc.arm([]() { setCenter(false, /*repop=*/true); }); // deferred: the close reflows and refocuses
            return;
        }
        // Tab moves into the selected card's reply field, the way Tab moves
        // into any other control — the pointer has the chip, the keyboard
        // needs a way in that is not one of the acting keys.
        if (SYM == XKB_KEY_Tab) {
            uint32_t    id = 0;
            std::string group;
            if (!centerSelection(id, group) || !group.empty())
                return;
            bool can = false;
            for (const auto& N : notifs)
                if (N->id == id) {
                    can = N->canReply;
                    break;
                }
            if (!can)
                return;
            info.cancelled = true;
            replyOpen(id);
            return;
        }
        if (SYM == XKB_KEY_Up || SYM == XKB_KEY_Down) {
            info.cancelled = true;
            centerSelectMove(SYM == XKB_KEY_Down ? 1 : -1); // local state + a deferred warm: safe here
            return;
        }

        SKeyAct a;
        switch (SYM) {
            case XKB_KEY_space: a.verb = 1; break;
            case XKB_KEY_Return:
            case XKB_KEY_KP_Enter: a.verb = 2; break;
            case XKB_KEY_Delete: a.verb = 3; break;
            default: return;
        }
        // nothing selected: the shade has not taken the keyboard, so a bare
        // space still belongs to whatever holds focus
        if (!centerSelection(a.id, a.group))
            return;

        info.cancelled = true;
        keys.push(std::move(a));
    }

    // the pipeline's blocked branch: the handlers never fire while the
    // session is locked or a native capture owns the stream, so the
    // resets the handlers used to do at their heads happen HERE instead
    void onInputBlocked() {
        const bool LOCK = NAwesome::sessionLocked();
        swallowRelease = 0;
        heldButtons    = 0;
        scrollAcc = swipeAcc = 0;
        swipeOn = 0;
        if (LOCK) {
            // a lock discards every half-tracked input state (crash class 7):
            // a queued shade action or pending esc must not drain after unlock,
            // and an armed reply field must not keep owning the keyboard with
            // text collected while the lock was up
            hits.reset();
            pendingEsc.reset();
            keys.reset();
            replyExit();
        }
        setHovered({});
        releasePointer();
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
        if (NAwesome::sessionLocked()) {
            setHovered({});
            releasePointer();
            return;
        }
        if (NAwesome::nativeInputCaptureActive()) {
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

        // info.cancelled: an earlier listener (the shell's strip or an open
        // menu) owns the point — and just set the shared SPECIAL_ACTION
        // cursor slot. Drop ownership WITHOUT unsetting it: releasePointer's
        // unset would strip the bar's override for its whole visit.
        if (info.cancelled) {
            setHovered({});
            pointerOwned = false;
            return;
        }

        // the exact position, as the press tests it: the event's is floored,
        // and a card edge at a fractional scale falls between the two
        const auto P    = g_pInputManager ? g_pInputManager->getMouseCoordsInternal() : pos;
        const auto CARD = cardAt(P);
        if (!CARD || heldButtons > 0 || NAwesome::nativePointerGrabActive() || NAwesome::nativeLayerOwnsPointer() ||
            (g_layoutManager && g_layoutManager->dragController()->target())) {
            setHovered({});
            releasePointer();
            return;
        }

        SHover h;
        h.kind  = CARD->kind;
        h.id    = CARD->id;
        h.group = CARD->group;
        h.btn   = buttonAt(*CARD, P);
        h.part  = h.btn >= 0 ? 0 : partAt(*CARD, P);
        setHovered(h);
        info.cancelled = true;

        const bool ONLINK = h.btn < 0 && h.part == 0 && linkAt(*CARD, P) >= 0; // a hyperlink shows the hand (GTK convention)

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
        hits.reset();
        pendingEsc.reset();
        keys.reset();
        swallowRelease = 0;
        heldButtons    = 0;
        scrollAcc      = 0;
        releasePointer();
    }

} // namespace NAwesome::Notify
