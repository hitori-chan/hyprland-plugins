// hyprnotify/input.cpp — clicks, wheel paging, and pointer ownership
// over the popups and the shade. Implements the interaction map exactly:
//
//   popup    left = action/link/default → dismiss · right = dismiss ·
//            middle = park the stack into the shade
//   row      a compact row whose open form reveals more expands on a body
//            click; once open, the body fires the card's primary (the fd.o
//            `default`) and dismisses unless resident, same as the popup.
//            a link opens · a button acts · right = dismiss · middle passes
//            through natively · long-press manages
//   manage   the row's verbs, named and at row width: snooze durations, mute
//            durations (iOS's "for 1 hour" / "today" / "always"), priority on
//            a chat, dismiss. Acting on one leaves the panel; the long-press
//            target and right-click close it. This replaced a three-glyph
//            hover strip whose
//            targets were 20px at 4px separation, unlabelled, with the one
//            irreversible verb in the middle.
//   leaving  anything that RAISES something else closes the shade with it —
//            the primary, an action button, a link (invokeLive below has
//            the AOSP citation). Everything that keeps you here does not:
//            a dismissal, a fold, the manage panel, DND, Clear all, and a
//            `resident` card's actions, which is the spec's own way of
//            saying the action does not take you away.
//   snooze   the undo row a schedule action leaves behind: left on "Undo" puts
//            the card back, left on the duration control cycles the choice,
//            right dismisses for good
//   child    a bundle child is a row without the fold: body, links, buttons
//   digest   left expands the app's bundle · right dismisses · long-press manages
//   ghead    left collapses · close control / right dismisses the bundle · long-press manages
//   footer   DND control · muted count = the silences in force, and one click out of
//            all of them · "Clear all" = the global sweep
//   wheel    vertical pages the shade — captured only inside the panel box.
//            HORIZONTAL on a row is the phone gesture: away dismisses, back
//            opens the manage panel. Strictly an addition — a mouse without a
//            horizontal wheel never reaches it and loses no verb.
//   keys     the shade never claims keyboard navigation or actions. Only a
//            Reply field explicitly armed by a pointer click receives key
//            events; while it is armed EVERY key is the field's (reply.cpp).
//
// Every mutation lands via the hit queue + CHop drain, never synchronously
// inside the emission (crash class 6); every listener gates on
// sessionLocked() first and resets its half-tracked state there (class 7).

#include "common/input.hpp"
#include "common/lifecycle.hpp"
#include "common/queries.hpp"

#include "ui.hpp"

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
        std::string  group;
        uint32_t     bit;
        uint8_t      part;               // 0 body, 2 close, 3 reply field, 4 send, 8 undo, 9 duration, 10 gesture-manage
        std::string  action;             // non-empty: a specific action button
        std::string  href;               // non-empty: a body hyperlink
        bool         expanded   = false; // exact ROW state from the painted hit record
        bool         expandable = false;
        bool         outside    = false; // the click fell outside every surface (closes the shade)
    };

    struct SLongPress {
        bool     active = false;
        bool     fired  = false;
        SHit     click{};
        Vector2D origin;
    };
    static std::vector<SHit> hitQueue;
    static bool              hitQueued = false;
    static NHyprCommon::CHop pendingHit;
    static SLongPress        longPress;
    static SP<CEventLoopTimer> longPressTimer;
    constexpr size_t         MAX_HIT_QUEUE = 128;
    constexpr int64_t         LONG_PRESS_MS = 500;
    constexpr double          LONG_PRESS_MOVE = 8.0;

    static void drainHits();
    static bool queueHit(SHit h);

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
        for (const auto& M : c.manage)
            if (M.box.containsPoint(pos))
                return M.part;
        if (c.close.w > 0 && c.close.containsPoint(pos))
            return 2;
        if (c.replySend.w > 0 && c.replySend.containsPoint(pos))
            return 4;
        if (c.replyField.w > 0 && c.replyField.containsPoint(pos))
            return 3;
        return 0;
    }

    static bool longPressKind(SCard::eKind kind) {
        return kind == SCard::ROW || kind == SCard::CHILD || kind == SCard::DIGEST || kind == SCard::GHEAD;
    }

    static bool longPressTargetAlive(const SHit& h) {
        if (!h.group.empty())
            return std::ranges::any_of(notifs, [&](const auto& N) { return !N->waiting && !N->snoozed && N->appKey == h.group; });
        const auto N = h.id != 0 ? Model::byId(h.id) : nullptr;
        return N && !N->waiting && !N->snoozed;
    }

    static void cancelLongPress() {
        longPress.active = false;
        longPress.fired  = false;
        longPress.click  = {};
        if (longPressTimer)
            longPressTimer->updateTimeout(std::nullopt);
    }

    void inputCancelLongPress() {
        cancelLongPress();
    }

    static void fireLongPress() {
        if (!longPress.active || !centerVisible() || NHyprCommon::sessionLocked() || NHyprCommon::nativeInputCaptureActive() || NHyprCommon::nativePointerGrabActive() ||
            NHyprCommon::nativeLayerOwnsPointer() || !longPressTargetAlive(longPress.click)) {
            cancelLongPress();
            return;
        }
        const auto POS  = g_pInputManager ? g_pInputManager->getMouseCoordsInternal() : Vector2D{};
        const auto CARD = cardAt(POS);
        if (!CARD || CARD->kind != longPress.click.kind || CARD->id != longPress.click.id || CARD->group != longPress.click.group) {
            cancelLongPress();
            return;
        }
        const auto H = longPress.click;
        longPress.active = false;
        longPress.fired  = true;
        if (longPressTimer)
            longPressTimer->updateTimeout(std::nullopt);
        if (!H.group.empty())
            centerToggleManageGroup(H.group);
        else
            centerToggleManage(H.id);
    }

    static bool armLongPress(SHit click, const Vector2D& origin) {
        if (!g_pEventLoopManager)
            return false;
        if (!longPressTimer) {
            longPressTimer = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { fireLongPress(); }, nullptr);
            g_pEventLoopManager->addTimer(longPressTimer);
        }
        longPress.active = true;
        longPress.fired  = false;
        longPress.click  = std::move(click);
        longPress.origin = origin;
        longPressTimer->updateTimeout(std::chrono::milliseconds(LONG_PRESS_MS));
        return true;
    }

    static void releaseLongPress() {
        if (!longPress.active && !longPress.fired)
            return;
        const bool FIRED = longPress.fired;
        const auto H     = longPress.click;
        cancelLongPress();
        // A short, stationary press is still the normal click. A management
        // long press consumes the release, and a card that vanished cannot be
        // safely replayed against a stale layout.
        if (!FIRED && centerVisible() && longPressTargetAlive(H))
            queueHit(H);
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
        bool        resident = false;
        for (const auto& N : notifs)
            if (N->id == id) {
                if (action.empty())
                    action = N->defaultAction;
                resident = N->resident;
                break;
            }
        if (action.empty()) { // nothing to fire: the body click is a dismissal
            Model::closeOne(id, Model::R_DISMISSED);
            return;
        }
        Bus::invokeAction(id, action);
        if (resident)
            return; // the card stays, and so does the shade behind it
        setCenter(false);
        Model::closeOne(id, Model::R_DISMISSED);
    }

    // one entry of a row's manage panel, by the index its hit rect carried
    static void manageEntry(uint32_t id, size_t idx, const std::string& group) {
        const auto N = Model::byId(id);
        if (!N)
            return;
        const bool BUNDLE = !group.empty();
        const auto EN = menuEntries(N, BUNDLE);
        if (idx >= EN.size())
            return;
        const auto& E = EN[idx];
        switch (E.verb) {
            case 1: Model::snoozeFor(id, E.arg); break;
            case 2: Policy::silenceFor(N->appKey, E.arg); break;
            case 3: Policy::unsilence(N->appKey); break;
            case 4: Policy::togglePriority(N->appKey, N->summary); break;
            case 5:
                if (BUNDLE)
                    Model::dismissApp(group);
                else
                    Model::closeOne(id, Model::R_DISMISSED);
                return; // the card or bundle is gone; so is its panel
        }
        // Acting on a rule LEAVES the panel — you came for one verb. The
        // dismissal above never gets here, and a snooze hands the slot to its
        // own undo row.
        if (BUNDLE)
            centerToggleManageGroup(group);
        else
            centerToggleManage(id);
    }

    // Deferred out of the input emission: closes reflow the layout and an
    // action can make the client focus/raise itself. Queue+drain so two
    // clicks in one dispatch both land.
    static void drainHits();
    static bool queueHit(SHit h) {
        if (hitQueue.size() >= MAX_HIT_QUEUE)
            return false;
        hitQueue.push_back(std::move(h));
        if (hitQueued)
            return true;
        hitQueued = true;
        pendingHit.arm(drainHits);
        return true;
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
                        Model::absorbPopped(); // middle: park the stack into the shade (no dismiss)
                        return;              // the rest reference now-parked cards
                    }
                    if (H.bit == 2u || H.part == 2) {
                        Model::closeOne(H.id, Model::R_DISMISSED);
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
                    if (H.bit == 2u) {
                        Model::closeOne(H.id, Model::R_DISMISSED);
                        continue;
                    }
                    if (H.bit != 1u)
                        continue;
                    if (H.part == 10) { // the horizontal gesture opens management
                        if (!H.group.empty())
                            centerToggleManageGroup(H.group);
                        else
                            centerToggleManage(H.id);
                        continue;
                    }
                    if (H.part == 3) // inside the armed field: keep typing
                        continue;
                    if (H.part == 4) { // its send pill
                        const auto TX = replyText();
                        if (TX.empty())
                            continue; // disabled Send keeps the draft surface armed
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
                        spawnDetached({"xdg-open", H.href.c_str(), nullptr});
                        setCenter(false); // but not the shade: a browser is coming up over it
                        continue;
                    }
                    if (H.kind == SCard::ROW && H.expandable && !H.expanded) {
                        centerToggleRow(H.id); // reveal hidden content before the body can act
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
                    if (H.part == 2 || H.bit == 2u) { // the static close / right: the whole bundle goes
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
                case SCard::MANAGE: {
                    // right closes the panel rather than dismissing the card:
                    // you are looking at a menu, and the card's own Dismiss is
                    // one of the rows in front of you
                    if (H.bit == 2u) {
                        if (!H.group.empty())
                            centerToggleManageGroup(H.group);
                        else
                            centerToggleManage(H.id);
                        continue;
                    }
                    if (H.bit != 1u)
                        continue;
                    if (H.part == 2) {
                        if (!H.group.empty())
                            centerToggleManageGroup(H.group);
                        else
                            centerToggleManage(H.id);
                    } else if (H.part == 10) {
                        if (!H.group.empty())
                            centerToggleManageGroup(H.group);
                        else
                            centerToggleManage(H.id);
                    }
                    else if (H.part >= 16)
                        manageEntry(H.id, H.part - 16, H.group);
                    continue;
                }
                case SCard::SNOOZE: {
                    // the undo window. Its two verbs both KEEP you here, so
                    // neither closes the shade; right still dismisses, which
                    // is how you say "no, actually go away for good".
                    if (H.bit == 2u) {
                        Model::closeOne(H.id, Model::R_DISMISSED);
                        continue;
                    }
                    if (H.bit != 1u)
                        continue;
                    if (H.part == 8)
                        Model::snoozeUndo(H.id);
                    else if (H.part == 9)
                        Model::snoozeCycle(H.id);
                    continue;
                }
                case SCard::BTN_CLEAR: // the footer: the global sweep
                    if (H.bit == 1u)
                        Model::dismissAllLive();
                    continue;
                case SCard::BTN_RULES: // the count is the affordance; the click is the way out
                    if (H.bit == 1u)
                        Policy::unsilenceAll();
                    continue;
                case SCard::BTN_DND:
                    if (H.bit == 1u)
                        Model::toggleSuspend();
                    continue;
                case SCard::PANEL: continue; // dead panel space swallows silently
            }
        }
    }

    void onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
        // emissions precede the compositor's own lock handling: locked input
        // belongs to the lockscreen, and half-tracked state must not survive it
        if (NHyprCommon::sessionLocked()) {
            cancelLongPress();
            swallowRelease = 0;
            heldButtons    = 0;
            return;
        }
        if (NHyprCommon::nativeInputCaptureActive()) {
            cancelLongPress();
            swallowRelease = 0;
            heldButtons    = 0;
            return;
        }

        const uint32_t BIT = NHyprCommon::trackedPointerButtonBit(e.button);

        if (e.state == WL_POINTER_BUTTON_STATE_RELEASED) {
            if (BIT && (swallowRelease & BIT)) {
                swallowRelease &= ~BIT;
                if (BIT == 1u)
                    releaseLongPress();
                info.cancelled = true;
            } else if (!info.cancelled) // a release hyprbar swallowed ends a press we never counted
                heldButtons = std::max(0, heldButtons - 1);
            return;
        }

        // Buttons without a notification action remain native app input.
        if (!BIT) {
            cancelLongPress();
            heldButtons++;
            return;
        }

        if (BIT != 1u)
            cancelLongPress();

        // hyprbar runs first: a press it swallowed (strip click, open tray
        // menu over the card region) was never ours — and never reached an
        // app, so there is no grab to count
        if (info.cancelled)
            return;

        if (heldButtons > 0 || NHyprCommon::nativePointerGrabActive()) {
            heldButtons++;
            return;
        }
        if (NHyprCommon::nativeLayerOwnsPointer())
            return;

        const auto COORDS = g_pInputManager->getMouseCoordsInternal();
        const auto CARD   = BIT ? cardAt(COORDS) : nullptr;

        if (!CARD) {
            // Android closes the shade on an outside tap; the closing click
            // is swallowed, like the tray menu's
            if (centerVisible() && BIT) {
                if (!queueHit({.outside = true})) {
                    heldButtons++;
                    return;
                }
                info.cancelled = true;
                swallowRelease |= BIT;
                return;
            }
            heldButtons++;
            return;
        }

        // Middle-click remains a native desktop gesture over all shade
        // content. The popup exception deliberately parks banners in the
        // shade, preserving its existing Android-style shortcut.
        if (BIT == 4u && CARD->kind != SCard::POPUP) {
            heldButtons++;
            setHovered({});
            releasePointer();
            // The shade clears native pointer focus while it owns hover. Put
            // it back before the compositor processes this native button.
            if (g_pInputManager)
                g_pInputManager->simulateMouseMovement();
            return;
        }

        SHit h;
        h.kind       = CARD->kind;
        h.id         = CARD->id;
        h.group      = CARD->group;
        h.bit        = BIT;
        h.part       = partAt(*CARD, COORDS);
        h.expanded   = CARD->expanded;
        h.expandable = CARD->expandable;
        if (BIT == 1u && h.part == 0) {
            if (const int B = buttonAt(*CARD, COORDS); B >= 0)
                h.action = CARD->buttons[B].id;
            else if (const int L = linkAt(*CARD, COORDS); L >= 0)
                h.href = CARD->links[L].href;
        }

        const bool LONG_BODY = BIT == 1u && h.part == 0 && h.action.empty() && h.href.empty() && longPressKind(h.kind);
        if (LONG_BODY) {
            if (!armLongPress(h, COORDS)) {
                if (!queueHit(std::move(h))) {
                    heldButtons++;
                    return;
                }
            }
            info.cancelled = true;
            swallowRelease |= BIT;
            return;
        }

        if (!queueHit(std::move(h))) {
            heldButtons++; // overload falls back to the native implicit grab
            return;
        }
        info.cancelled = true; // the surface is ours: the press must not reach the window beneath
        swallowRelease |= BIT;
    }

    // ---- wheel: page the center, only inside the panel box ----

    static double scrollAcc = 0, swipeAcc = 0;
    static uint32_t swipeOn = 0; // the row the current horizontal gesture belongs to

    void            onMouseAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) {
        if (NHyprCommon::sessionLocked()) {
            cancelLongPress();
            scrollAcc = swipeAcc = 0;
            swipeOn              = 0;
            return;
        }
        if (NHyprCommon::nativeInputCaptureActive()) {
            cancelLongPress();
            scrollAcc = swipeAcc = 0;
            swipeOn              = 0;
            return;
        }
        // A long press is a stationary pointer gesture. Any axis event means
        // the user is doing something else, even when the current button
        // state would otherwise make the wheel a no-op.
        if (longPress.active)
            cancelLongPress();
        if (!centerVisible() || cards.empty() || info.cancelled || heldButtons > 0 || NHyprCommon::nativePointerGrabActive() || NHyprCommon::nativeLayerOwnsPointer())
            return;
        const auto POS  = g_pInputManager->getMouseCoordsInternal();
        const auto CARD = cardAt(POS);
        if (!CARD)
            return; // outside the panel: windows scroll normally
        if (CARD->kind == SCard::POPUP) {
            // An OSD popup sits below the open shade. Keep its wheel from
            // paging the panel underneath; popup cards have no wheel verb.
            info.cancelled = true;
            scrollAcc = swipeAcc = 0;
            swipeOn              = 0;
            return;
        }
        info.cancelled = true;
        const double DELTA = e.delta != 0.0 ? e.delta : e.deltaDiscrete / 120.0 * 15.0;

        // HORIZONTAL is the phone gesture, and the vertical wheel already had
        // the panel — a trackpad gets swipe-to-dismiss and swipe-to-manage for
        // free. It is strictly an ADDITION: a mouse with no horizontal wheel
        // never reaches here and loses nothing, so neither verb may be the
        // only way to do its job.
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
                swipeAcc        = 0;
                SHit h;
                h.kind = CARD->kind;
                h.id   = CARD->id;
                h.group = CARD->group;
                h.bit  = AWAY ? 2u : 1u;  // right-swipe IS the right-click
                h.part = AWAY ? 0 : 10;   // the other way opens the manage panel
                if (!queueHit(std::move(h)))
                    info.cancelled = false; // overload leaves the wheel native
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

    void onKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info) {
        if (NHyprCommon::sessionLocked())
            return;
        if (NHyprCommon::nativeInputCaptureActive())
            return;
        if (info.cancelled || !centerVisible() || !replyArmed())
            return;
        // releases pass untouched (crash class 3: never cancel key releases)
        if (e.state != WL_KEYBOARD_KEY_STATE_PRESSED)
            return;
        const auto KB = g_pSeatManager ? g_pSeatManager->m_keyboard.lock() : nullptr;
        if (!KB || !KB->m_xkbState)
            return;

        // Keyboard input is intentionally armed only by the visible Reply chip.
        // The center itself has no navigation or action key map, so all other
        // keys continue through to compositor bindings and the focused client.
        if (replyKey(KB->m_xkbState, e.keycode + 8))
            info.cancelled = true;
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
            cancelLongPress();
            setHovered({});
            releasePointer();
            return;
        }
        if (NHyprCommon::nativeInputCaptureActive()) {
            cancelLongPress();
            setHovered({});
            releasePointer();
            return;
        }

        if (longPress.active) {
            const double DX = pos.x - longPress.origin.x;
            const double DY = pos.y - longPress.origin.y;
            if (DX * DX + DY * DY > LONG_PRESS_MOVE * LONG_PRESS_MOVE || info.cancelled || NHyprCommon::nativePointerGrabActive() || NHyprCommon::nativeLayerOwnsPointer())
                cancelLongPress();
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
        if (!CARD || heldButtons > 0 || NHyprCommon::nativePointerGrabActive() || (g_layoutManager && g_layoutManager->dragController()->target())) {
            setHovered({});
            releasePointer();
            return;
        }

        if (NHyprCommon::nativeLayerOwnsPointer()) {
            setHovered({});
            releasePointer();
            return;
        }

        SHover h;
        h.kind  = CARD->kind;
        h.id    = CARD->id;
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
        if (!centerVisible() || !CARD || (longPress.active && (!longPressTargetAlive(longPress.click) || longPress.click.kind != CARD->kind || longPress.click.id != CARD->id || longPress.click.group != CARD->group)))
            cancelLongPress();
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
        cancelLongPress();
        if (longPressTimer && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(longPressTimer);
        longPressTimer.reset();
        pendingHit.reset();
        hitQueued = false;
        hitQueue.clear();
        swallowRelease = 0;
        heldButtons    = 0;
        scrollAcc      = 0;
        swipeAcc       = 0;
        swipeOn        = 0;
        releasePointer();
    }

} // namespace NHyprnotify
