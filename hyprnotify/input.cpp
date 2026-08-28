// hyprnotify/input.cpp — clicks, the long-press hold, the drag-to-dismiss,
// wheel paging, and pointer ownership over the popups and the shade.
//
//   popup    left = action/link/default → leaves · right = dismiss ·
//            middle = park the stack into the shade · the chevron jumps to
//            the shade (a HUN never expands in place)
//   row      tap body = contentIntent · the expand chip OR the icon column
//            toggle the open state (AOSP's alternate_expand_target) · a link
//            opens · a button acts · right = dismiss · long-press (450 ms) =
//            the hold menu · drag down > 90 px = swipe-to-dismiss into the
//            history (on release; no live lift — ledger A-138)
//   child    body = that child's contentIntent · its chevron = the kid's own
//            level-2 expand · the card-level gestures ride the parent
//   digest   tap / chip / icon = toggle the children, NOT open the app
//   ghead    tap / chip / icon = collapse the children
//   manage   stages the importance choice + the snooze list; Done commits,
//            Dismiss removes the target, right-click closes without changing
//            anything
//   snooze   the undo row a schedule action leaves behind: left on "Undo" puts
//            the card back, right dismisses for good
//   footer   history pill (opens the dismissed-card panel) · muted count =
//            the silences in force, one click out of all of them · "Clear
//            all" = the global sweep · DND
//   wheel    vertical pages the shade (panel box only) · a horizontal swipe
//            (> 90 px) flips the history panel
//   keys     the shade never claims keyboard navigation. Only a Reply field
//            armed by a pointer takes keys; while it is armed EVERY key is
//            the field's (reply.cpp)
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
        std::string  childKey; // CHILD: the kid's expansion key
        uint32_t     bit;
        uint8_t      part; // see SHover in hyprnotify.hpp
        std::string  action; // non-empty: a specific action button
        std::string  href; // non-empty: a body hyperlink
        bool         expandable = false;
        bool         outside    = false; // the click fell outside every surface (closes the shade)
    };

    // one press on a card body: long-press arms the hold menu, a downward
    // drag > 90 px dismisses on release, a short stationary press is the tap
    struct SPress {
        bool     active   = false;
        bool     fired    = false; // the hold menu opened: the release is consumed
        bool     dragging = false; // down > LONG_PRESS_MOVE: it is a drag, not a tap
        SHit     click{};
        Vector2D origin;
        double   dragY = 0; // downward travel
    };

    static std::deque<SHit> hitQueue;
    static bool              hitQueued = false;
    static NHyprCommon::CHop pendingHit;
    static SPress            press;
    static SP<CEventLoopTimer> longPressTimer;
    constexpr size_t         MAX_HIT_QUEUE = 128;

    static void drainHits();
    static void queueHit(SHit h);

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
        if (c.expansionButton && c.expandButton.containsPoint(pos))
            return 5;
        for (const auto& M : c.manage)
            if (M.box.containsPoint(pos))
                return M.part;
        if (c.replySend.w > 0 && c.replySend.containsPoint(pos))
            return 4;
        if (c.replyField.w > 0 && c.replyField.containsPoint(pos))
            return 3;
        return 0;
    }

    // the card-body press surface: the shade cards own the long-press and the
    // drag. The HUN is armed at the PRESS_BODY site instead (its kind is
    // shared with the OSD band, which taps immediately); small controls tap
    // immediately.
    static bool draggableKind(SCard::eKind kind) {
        return kind == SCard::ROW || kind == SCard::CHILD || kind == SCard::DIGEST || kind == SCard::GHEAD;
    }

    static bool pressTargetAlive(const SHit& h) {
        if (!h.group.empty())
            return std::ranges::any_of(notifs,
                                       [&](const auto& N) { return !N->waiting && !N->snoozed && Pixel::displayGroupKey(N->appKey, N->declaredGroupKey, N->section) == h.group; });
        const auto N = h.id != 0 ? Model::byId(h.id) : nullptr;
        return N && !N->waiting && !N->snoozed;
    }

    static void cancelPress(bool firedHeld = false) {
        if (firedHeld)
            press.fired = true; // the hold menu is open: consume the release
        press.active   = false;
        press.dragging = false;
        press.click    = {};
        press.dragY    = 0;
        if (longPressTimer)
            longPressTimer->updateTimeout(std::nullopt);
    }

    void inputCancelLongPress() {
        cancelPress();
    }

    static void fireLongPress() {
        // The HUN fires while the shade is CLOSED (it is about to open it); a
        // shade card already lives in an open center.
        const bool IN_CENTER = centerVisible() || press.click.kind == SCard::POPUP;
        if (!press.active || press.fired || !IN_CENTER || NHyprCommon::sessionLocked() || NHyprCommon::nativeInputCaptureActive() ||
            NHyprCommon::nativePointerGrabActive() || NHyprCommon::nativeLayerOwnsPointer() || !pressTargetAlive(press.click)) {
            cancelPress();
            return;
        }
        const auto POS  = g_pInputManager ? g_pInputManager->getMouseCoordsInternal() : Vector2D{};
        const auto CARD = cardAt(POS);
        if (!CARD || CARD->kind != press.click.kind || CARD->id != press.click.id || CARD->group != press.click.group) {
            cancelPress();
            return;
        }
        const auto H = press.click;
        if (longPressTimer)
            longPressTimer->updateTimeout(std::nullopt);
        press.fired = true; // the release is consumed by the menu, not the tap
        if (!H.group.empty())
            centerToggleManageGroup(H.group);
        else {
            centerToggleManage(H.id);
            if (H.kind == SCard::POPUP) // the menu lives in the shade's card slot
                setCenter(true);
        }
    }

    static void armPress(SHit click, const Vector2D& origin) {
        press.active   = false;
        press.fired    = false;
        press.dragging = false;
        press.click    = click;
        press.origin   = origin;
        press.dragY    = 0;
        press.active   = true; // armPress is only reached through PRESS_BODY
        if (!g_pEventLoopManager)
            return;
        if (!longPressTimer) {
            longPressTimer = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { fireLongPress(); }, nullptr);
            g_pEventLoopManager->addTimer(longPressTimer);
        }
        longPressTimer->updateTimeout(std::chrono::milliseconds(LONG_PRESS_MS));
    }

    // the left release over a card body: the press decided what it was
    static void releasePress() {
        if (!press.active) {
            return;
        }
        const bool   FIRED    = press.fired;
        const bool   DRAGGING = press.dragging;
        const double DRAGY    = press.dragY;
        const auto   H        = press.click;
        cancelPress();
        if (FIRED)
            return; // the hold menu is open; nothing else happens
        if (DRAGGING) {
            // past the threshold it is a swipe-to-dismiss, stashed into the
            // history on release (the demo lifts live; the compositor card
            // does not reflow per motion — ledger A-138)
            if (DRAGY > DRAG_DISMISS_PX) {
                SHit D = H;
                D.bit  = 2u;
                D.part = 10; // the drain dismisses the TOP-LEVEL target
                queueHit(std::move(D));
            }
            return; // a short drag suppresses the tap (the demo's capture)
        }
        // a short, stationary press is still the normal click; a card that
        // vanished cannot be safely replayed against a stale layout. A HUN tap
        // must work while the shade is closed (its card layout is alive then)
        if (pressTargetAlive(H) && (H.kind == SCard::POPUP || centerVisible()))
            queueHit(std::move(H));
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

    // the drag-dismiss (part 10) drops the TOP-LEVEL target: a bundle kid's
    // drag takes the bundle with it, the demo's setPointerCapture included
    static void dragDismiss(const SHit& H) {
        if (!H.group.empty())
            Model::dismissGroup(H.group);
        else
            Model::closeOne(H.id, Model::R_DISMISSED);
    }

    // Deferred out of the input emission: closes reflow the layout and an
    // action can make the client focus/raise itself. Queue+drain so two
    // clicks in one dispatch both land.
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
                        return; // the rest reference now-parked cards
                    }
                    if (H.bit == 2u) {
                        Model::closeOne(H.id, Model::R_DISMISSED);
                        continue;
                    }
                    if (!H.href.empty()) { // left on a hyperlink: open it, keep the card up
                        spawnDetached({"xdg-open", H.href.c_str(), nullptr});
                        continue;
                    }
                    if (H.bit == 1u && H.part == 5) { // the chevron: a HUN never expands in place
                        setCenter(true);
                        continue;
                    }
                    // HUN actions invoke, never arm an inline field: the ROM
                    // opens the app's own reply UI for a heads-up Reply
                    invokeLive(H.id, H.action);
                    continue;
                }
                case SCard::ROW:
                case SCard::CHILD: {
                    if (H.part == 10) { // the drag reached the threshold
                        dragDismiss(H);
                        continue;
                    }
                    if (H.bit == 2u) {
                        Model::closeOne(H.id, Model::R_DISMISSED);
                        continue;
                    }
                    if (H.bit != 1u)
                        continue;
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
                    if (H.kind == SCard::CHILD && H.part == 1) // the kid's own chevron
                    {
                        centerToggleChild(H.childKey);
                        continue;
                    }
                    if (H.kind == SCard::ROW && (H.part == 1 || H.part == 2) && H.expandable) // the chip OR the icon
                    {
                        centerToggleRow(H.id);
                        continue;
                    }
                    // a kid's lead disc is row surface: it invokes, never
                    // toggles (the kid's chevron is the only kid toggle)
                    if (!H.action.empty() && H.action == "inline-reply") {
                        // v13: the inline field lives on a conversation kid —
                        // the card's Reply (or its newest kid's) arms it; a
                        // plain card's Reply invokes the app's own UI
                        const auto TN = Model::byId(H.id);
                        if (H.kind == SCard::CHILD || (H.kind == SCard::ROW && TN && TN->conversation)) {
                            setCenter(true);
                            replyOpen(H.id);
                        } else
                            invokeLive(H.id, H.action);
                        continue;
                    }
                    if (!H.action.empty()) { // a specific button: fires and closes it
                        invokeLive(H.id, H.action);
                        continue;
                    }
                    if (!H.href.empty()) {
                        spawnDetached({"xdg-open", H.href.c_str(), nullptr});
                        setCenter(false); // a link opens in the app: the shade leaves with it
                        continue;
                    }
                    invokeLive(H.id, ""); // body left: the content intent
                    continue;
                }
                case SCard::DIGEST:
                case SCard::GHEAD: {
                    if (H.part == 10) { // the drag reached the threshold
                        dragDismiss(H);
                        continue;
                    }
                    if (H.bit == 2u) { // right on the summary/header: the whole bundle
                        Model::dismissGroup(H.group);
                        continue;
                    }
                    if (H.bit != 1u)
                        continue;
                    // tap / chip / icon all toggle the children — the bundle
                    // summary is never an app launcher (demo wireCard)
                    centerToggleGroup(H.group);
                    continue;
                }
                case SCard::MANAGE: {
                    if (H.bit == 2u) { // right: close, changing nothing
                        centerToggleManage(H.id);
                        continue;
                    }
                    if (H.bit != 1u)
                        continue;
                    if (H.part <= 2) {
                        centerChooseManageMode((Policy::eAlertingMode)H.part); // staged; Done commits
                        continue;
                    }
                    if (H.part == 3) {
                        centerToggleManageSnooze(); // bundles never paint the section
                        continue;
                    }
                    if (H.part >= 4 && H.part <= 7) {
                        const int SECS = (int)SNOOZE_OPTS[H.part - 4];
                        centerChooseManageSnooze(SECS); // staged; Done commits
                        continue;
                    }
                    if (H.part == 8) { // Done: save (or commit a staged snooze)
                        if (!centerManageBundle())
                            centerCommitManage(H.id, "");
                        else
                            centerCommitManage(0, H.group);
                        continue;
                    }
                    if (H.part == 9) { // Dismiss
                        if (!H.group.empty())
                            Model::dismissGroup(H.group);
                        else
                            Model::closeOne(H.id, Model::R_DISMISSED);
                        if (centerVisible())
                            centerToggleManage(0);
                        continue;
                    }
                    continue;
                }
                case SCard::SNOOZE: {
                    if (H.bit == 2u) {
                        Model::closeOne(H.id, Model::R_DISMISSED); // Undo the undo
                        continue;
                    }
                    if (H.bit == 1u && H.part == 8)
                        Model::snoozeUndo(H.id); // part 8 = the Undo button
                    continue;
                }
                case SCard::BTN_CLEAR:
                    if (H.bit == 1u)
                        Model::dismissAllLive();
                    continue;
                case SCard::BTN_RULES:
                    if (H.bit == 1u)
                        Policy::unsilenceAll();
                    continue;
                case SCard::BTN_DND:
                    if (H.bit == 1u)
                        Model::toggleSuspend();
                    continue;
                case SCard::BTN_HISTORY:
                    if (H.bit == 1u)
                        centerToggleHistory();
                    continue;
                case SCard::HIST_CLEAR:
                    if (H.bit == 1u)
                        centerHistoryClear();
                    continue;
                case SCard::PANEL:
                case SCard::HIST_BOX: // the panel body and history items: swallow
                    continue;
                default:
                    continue;
            }
        }
        if (!hitQueue.empty())
            queueHit({}); // something landed while draining: keep the chain alive
    }

    static void queueHit(SHit h) {
        if (hitQueue.size() >= MAX_HIT_QUEUE)
            hitQueue.pop_front(); // bounded: drop the oldest, never fail
        hitQueue.push_back(std::move(h));
        if (hitQueued || NHyprCommon::sessionLocked() || !g_pEventLoopManager)
            return;
        hitQueued = true;
        pendingHit.arm(drainHits);
    }

    // ---- pointer button: the press state machine + the immediate verbs ----

    void onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
        // emissions precede the compositor's own lock handling: locked input
        // belongs to the lockscreen, and half-tracked state must not survive it
        if (NHyprCommon::sessionLocked() || NHyprCommon::nativeInputCaptureActive()) {
            cancelPress();
            swallowRelease = 0;
            heldButtons    = 0;
            return;
        }

        const uint32_t BIT = NHyprCommon::trackedPointerButtonBit(e.button);

        if (e.state == WL_POINTER_BUTTON_STATE_RELEASED) {
            if (BIT && (swallowRelease & BIT)) {
                swallowRelease &= ~BIT;
                if (BIT == 1u)
                    releasePress();
                info.cancelled = true;
            } else if (!info.cancelled) // a release hyprbar swallowed ends a press we never counted
                heldButtons = std::max(0, heldButtons - 1);
            return;
        }

        // Buttons without a notification action remain native app input.
        if (!BIT) {
            cancelPress();
            heldButtons++;
            return;
        }

        if (BIT != 1u)
            cancelPress();

        // hyprbar runs first: a press it swallowed (strip click, open tray
        // menu over the card region) was never ours — and never reached an
        // app, so there is no grab to count
        if (info.cancelled) {
            return;
        }

        if (heldButtons > 0 || NHyprCommon::nativePointerGrabActive()) {
            heldButtons++;
            return;
        }
        if (NHyprCommon::nativeLayerOwnsPointer()) {
            return;
        }

        const auto COORDS = g_pInputManager->getMouseCoordsInternal();
        const auto CARD   = cardAt(COORDS);

        if (!CARD) {
            // Android closes the shade on an outside tap; the closing click
            // is swallowed, like the tray menu's
            if (centerVisible() && BIT) {
                queueHit({.outside = true});
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
        h.childKey   = CARD->childKey;
        h.bit        = BIT;
        h.part       = partAt(*CARD, COORDS);
        h.expandable = CARD->expandable;
        if (BIT == 1u && h.part == 0) {
            if (const int B = buttonAt(*CARD, COORDS); B >= 0)
                h.action = CARD->buttons[B].id;
            else if (const int L = linkAt(*CARD, COORDS); L >= 0)
                h.href = CARD->links[L].href;
        }

        // body, the expand chip and the icon column: the press may still
        // become a hold or a drag, so the tap is decided at release. The HUN
        // arms too (long-press -> hold menu in the shade, drag -> dismiss, tap
        // -> contentIntent at release); the OSD band and small controls keep
        // the instant verb.
        const bool HUN_BODY   = h.kind == SCard::POPUP && !inOsdBand(h.id);
        const bool PRESS_BODY = BIT == 1u && h.part <= 2 && h.action.empty() && h.href.empty() && (draggableKind(h.kind) || HUN_BODY);
        if (PRESS_BODY) {
            armPress(std::move(h), COORDS);
            info.cancelled = true;
            swallowRelease |= BIT;
            return;
        }

        queueHit(std::move(h));
        info.cancelled = true; // the surface is ours: the press must not reach the window beneath
        swallowRelease |= BIT;
    }

    // ---- wheel: page the center, and the horizontal history swipe ----

    static double  scrollAcc = 0, swipeAcc = 0;
    static uint32_t swipeOn = 0; // the card the current horizontal gesture belongs to

    void onMouseAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) {
        if (NHyprCommon::sessionLocked()) {
            cancelPress();
            scrollAcc = swipeAcc = 0;
            swipeOn   = 0;
            return;
        }
        if (NHyprCommon::nativeInputCaptureActive()) {
            cancelPress();
            scrollAcc = swipeAcc = 0;
            swipeOn   = 0;
            return;
        }
        // A long press is a stationary pointer gesture. Any axis event means
        // the user is doing something else, even when the current button
        // state would otherwise make the wheel a no-op.
        if (press.active)
            cancelPress();
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
            swipeOn   = 0;
            return;
        }
        info.cancelled = true;
        const double DELTA = e.delta != 0.0 ? e.delta : e.deltaDiscrete / 120.0 * 15.0;

        // HORIZONTAL is the phone's shade-to-history flick, translated to the
        // wheel: a deliberate swipe (> 90 px, the spec's threshold) flips the
        // history panel, from any shade surface. It goes through the CLICK
        // queue rather than acting here — the flip reflows the layout, and
        // nothing may do that inside an input emission (crash class 6).
        if (e.axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
            if (CARD->id == 0 && CARD->kind != SCard::PANEL) { // footer/history surfaces: no swipe verb
                swipeAcc = 0;
                swipeOn  = 0;
                return;
            }
            const uint32_t OWNER = CARD->id != 0 ? CARD->id : 0x7FFFFFFFu;
            if (swipeOn != OWNER) { // the pointer moved to another card mid-gesture
                swipeOn  = OWNER;
                swipeAcc = 0;
            }
            swipeAcc += DELTA;
            if (swipeAcc >= SWIPE_THRESHOLD || swipeAcc <= -SWIPE_THRESHOLD) {
                swipeAcc = 0;
                SHit h;
                h.kind = SCard::BTN_HISTORY;
                h.bit  = 1u;
                queueHit(std::move(h));
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

        // Keyboard input is intentionally armed only by the visible Reply
        // field. The center itself has no navigation or action key map, so
        // all other keys continue through to compositor bindings and the
        // focused client.
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
            cancelPress();
            setHovered({});
            releasePointer();
            return;
        }
        if (NHyprCommon::nativeInputCaptureActive()) {
            cancelPress();
            setHovered({});
            releasePointer();
            return;
        }

        if (press.active) {
            const double DX = pos.x - press.origin.x;
            const double DY = pos.y - press.origin.y;
            if (press.dragging) {
                if (DY > 0)
                    press.dragY = DY; // downward travel, global coordinates
            } else if (DY > LONG_PRESS_MOVE) {
                press.dragging = true; // it is a drag now, not a hold
                if (longPressTimer)
                    longPressTimer->updateTimeout(std::nullopt);
            } else if (DX * DX + DY * DY > LONG_PRESS_MOVE * LONG_PRESS_MOVE || info.cancelled || NHyprCommon::nativePointerGrabActive() || NHyprCommon::nativeLayerOwnsPointer())
                cancelPress(); // a lateral wander is neither hold nor dismiss
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
        h.kind     = CARD->kind;
        h.id       = CARD->id;
        h.group    = CARD->group;
        h.childKey = CARD->childKey;
        h.btn      = buttonAt(*CARD, pos);
        h.part     = h.btn >= 0 ? 0 : partAt(*CARD, pos);
        setHovered(h);
        info.cancelled = true;

        const bool ONLINK = h.btn < 0 && h.part == 0 && linkAt(*CARD, pos) >= 0; // a hyperlink shows the hand (GTK convention)

        const bool ENTERING = !pointerOwned;
        if (ENTERING) {
            pointerOwned = true;
            g_pSeatManager->setPointerFocus(nullptr, {}); // the app under the surface gets its leave
        }
        // set the shape on entry, and re-set only when it flips (a still
        // stream of motion must not re-assert the override every event)
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
        // pendingWarm can fire long after the change was queued — never poke
        // the pointer at a lock screen or capture session
        if (NHyprCommon::sessionLocked() || NHyprCommon::nativeInputCaptureActive())
            return;
        const auto COORDS = g_pInputManager->getMouseCoordsInternal();
        const auto CARD   = cardAt(COORDS);
        if (!centerVisible() || !CARD || (press.active && (!pressTargetAlive(press.click) || press.click.kind != CARD->kind || press.click.id != CARD->id || press.click.group != CARD->group)))
            cancelPress();
        if (CARD) { // a reflow can slide another surface under the still pointer
            SHover h;
            h.kind     = CARD->kind;
            h.id       = CARD->id;
            h.group    = CARD->group;
            h.childKey = CARD->childKey;
            h.btn      = buttonAt(*CARD, COORDS);
            h.part     = h.btn >= 0 ? 0 : partAt(*CARD, COORDS);
            setHovered(h);
        } else
            setHovered({});
        if (!pointerOwned || CARD)
            return;
        releasePointer();
        g_pInputManager->simulateMouseMovement(); // the window beneath gets its enter back
    }

    void inputExit() {
        cancelPress();
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
