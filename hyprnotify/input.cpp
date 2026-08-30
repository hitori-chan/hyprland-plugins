// hyprnotify/input.cpp — clicks, wheel paging, esc and pointer ownership
// over the popups and the shade. Implements the interaction map exactly:
//
//   popup    left = action/link/default → dismiss · right = dismiss ·
//            middle = park the stack into the shade · hover reveals ✕
//   row      a shade row IS its banner: left on the body fires the card's
//            primary (the fd.o `default`) and dismisses unless resident,
//            same as the popup — rows open by default, so the click is
//            spent on acting rather than on revealing. The CHEVRON is the
//            only fold target · a link opens · a button acts · right =
//            dismiss · middle = Clear all · the ⋮ beside the chevron turns
//            the row into its manage panel
//   manage   the row's verbs, named and at row width: snooze durations, mute
//            durations (iOS's "for 1 hour" / "today" / "always"), priority on
//            a chat, dismiss. Acting on one leaves the panel; the ⋮, right and
//            esc all close it. This replaced a three-glyph hover strip whose
//            targets were 20px at 4px separation, unlabelled, with the one
//            irreversible verb in the middle.
//   leaving  anything that RAISES something else closes the shade with it —
//            the primary, an action button, a link (invokeLive below has
//            the AOSP citation). Everything that keeps you here does not:
//            a dismissal, a fold, the manage panel, DND, Clear all, and a
//            `resident` card's actions, which is the spec's own way of
//            saying the action does not take you away.
//   snooze   the undo row a ◷ leaves behind: left on "Undo" puts the card
//            back, left on ˅ cycles the duration, right dismisses for good
//   child    a bundle child is a row without the fold: body, links, buttons
//   digest   left expands the app's bundle · its ⊘ silences · right dismisses
//   ghead    left collapses · ⊘ silences · the ✕ / right dismisses the bundle
//   footer   ⊖ = DND · "⊘ N" = the silences in force, and one click out of
//            all of them · "Clear all" = the global sweep
//   wheel    vertical pages the shade — captured only inside the panel box.
//            HORIZONTAL on a row is the phone gesture: away dismisses, back
//            opens the manage panel. Strictly an addition — a mouse without a
//            horizontal wheel never reaches it and loses no verb.
//   keys     while the shade is open it owns the nav set and nothing else:
//            esc closes (the topmost-peel's middle link) · ↑/↓ move the
//            selection · space folds it (the click's twin) · enter fires the
//            primary · delete dismisses · m silences the app · s snoozes
//            the card (and re-picks the duration while its undo row is up) ·
//            u takes that snooze back · p marks the sender · tab opens its
//            reply field, and while
//            one is armed EVERY key is the field's (reply.cpp). A chord with
//            ctrl/alt/super is the user's bind, and a nav key with NOTHING
//            selected (or nothing to do — p on a card that is not a chat)
//            still belongs to whatever holds focus: the shade never grabs a
//            key it has no use for.
//
// Every mutation lands via the hit queue + CHop drain, never synchronously
// inside the emission (crash class 6); every listener gates on
// sessionLocked() first and resets its half-tracked state there (class 7),
// and a native input-capture session, an implicit or seat grab, or a native
// layer surface at the point passes through unintercepted (the
// input-capture-v1 / native hit-test contract, mirrored from the bar).

#include "common/lifecycle.hpp"
#include "common/queries.hpp"

#include "ui.hpp"

#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon-names.h>

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
        uint8_t      part;   // the SHover part codes: 0 body, 1 chevron, 2 close, 3 reply field, 4 send, 5 silence, 6 priority, 7 snooze, 8 undo, 9 duration
        std::string  action; // non-empty: a specific action button
        std::string  href;   // non-empty: a body hyperlink
        bool         outside = false; // the click fell outside every surface (closes the shade)
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

    bool       pointerOverCards() {
        return g_pInputManager && cardAt(g_pInputManager->getMouseCoordsInternal()) != nullptr;
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

    // The two per-app rules behind the KEYS m and p — the pointer reaches them
    // through the row's manage panel instead. Both are keyed on something the
    // card carries, so the selected card is only here to supply the key.
    static void muteApp(uint32_t id) {
        if (const auto N = Model::byId(id))
            Policy::toggleSilence(N->appKey);
    }
    static void markSender(uint32_t id) {
        if (const auto N = Model::byId(id))
            Policy::togglePriority(N->appKey, N->summary);
    }

    // one entry of a row's manage panel, by the index its hit rect carried
    static void manageEntry(uint32_t id, size_t idx) {
        const auto N = Model::byId(id);
        if (!N)
            return;
        const auto EN = menuEntries(N);
        if (idx >= EN.size())
            return;
        const auto& E = EN[idx];
        switch (E.verb) {
            case 1: Model::snoozeFor(id, E.arg); break;
            case 2: Policy::silenceFor(N->appKey, E.arg); break;
            case 3: Policy::unsilence(N->appKey); break;
            case 4: Policy::togglePriority(N->appKey, N->summary); break;
            case 5: Model::closeOne(id, Model::R_DISMISSED); return; // the card is gone; so is its panel
        }
        // Acting on a rule LEAVES the panel — you came for one verb. The
        // dismissal above never gets here, and a snooze hands the slot to its
        // own undo row.
        centerToggleManage(id);
    }

    // Deferred out of the input emission: closes reflow the layout and an
    // action can make the client focus/raise itself. Queue+drain so two
    // clicks in one dispatch both land.
    static void drainHits();
    static void queueHit(SHit h) {
        hitQueue.push_back(std::move(h));
        if (hitQueued)
            return;
        hitQueued = true;
        pendingHit.arm(drainHits);
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
            centerPin(); // a click on the shade keeps it: hover peeks, click pins
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
                    if (H.part == 10) { // the ⋮ turns the row into its manage panel
                        centerToggleManage(H.id);
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
                        spawnDetached({"xdg-open", H.href.c_str(), nullptr});
                        setCenter(false); // but not the shade: a browser is coming up over it
                        continue;
                    }
                    invokeLive(H.id, ""); // the body IS the card's primary, as on the banner
                    continue;
                }
                case SCard::DIGEST: {
                    if (H.bit == 1u && H.part == 5) { // the ⊘ silences the app, it does not expand
                        Policy::toggleSilence(H.group);
                        continue;
                    }
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
                    if (H.bit == 1u && H.part == 5) {
                        Policy::toggleSilence(H.group);
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
                        centerToggleManage(H.id);
                        continue;
                    }
                    if (H.bit != 1u)
                        continue;
                    if (H.part == 10)
                        centerToggleManage(H.id);
                    else if (H.part >= 16)
                        manageEntry(H.id, H.part - 16);
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
            swallowRelease = 0;
            heldButtons    = 0;
            return;
        }
        if (NHyprCommon::nativeInputCaptureActive()) {
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

        // A live implicit or seat grab, or a native layer surface at the
        // point, stays authoritative over every drawn surface (the bar's
        // strip does the same): the press goes through, the cards only watch
        if (heldButtons > 0 || NHyprCommon::nativePointerGrabActive()) {
            heldButtons++;
            return;
        }
        if (NHyprCommon::nativeLayerOwnsPointer())
            return;

        if (!CARD) {
            // Android closes the shade on an outside tap; the closing click
            // is swallowed, like the tray menu's
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
        if (NHyprCommon::sessionLocked()) {
            scrollAcc = swipeAcc = 0;
            swipeOn              = 0;
            return;
        }
        if (NHyprCommon::nativeInputCaptureActive()) {
            scrollAcc = swipeAcc = 0;
            swipeOn              = 0;
            return;
        }
        if (heldButtons > 0 || NHyprCommon::nativePointerGrabActive() || NHyprCommon::nativeLayerOwnsPointer())
            return;
        if (!centerVisible() || cards.empty() || info.cancelled)
            return;
        const auto POS  = g_pInputManager->getMouseCoordsInternal();
        const auto CARD = cardAt(POS);
        if (!CARD)
            return; // outside the panel: windows scroll normally
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
                h.bit  = AWAY ? 2u : 1u;  // right-swipe IS the right-click
                h.part = AWAY ? 0 : 10;   // the other way opens the ⋮'s panel
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

    // ---- keys: esc peels the center (tray menu > center > menubar: load
    //      order puts hyprbar's menu first, we're next), and the shade drives
    //      its selection ----

    // Same shape as the click queue, and for the same reason: an action can
    // make the client focus itself, so nothing runs inside the emission.
    struct SKeyAct {
        int         verb = 0; // 1 fold, 2 the primary, 3 dismiss, 4 silence, 5 mark, 6 snooze, 7 undo, 8 re-pick the duration
        uint32_t    id   = 0;
        std::string group; // non-empty: a bundle
    };
    static std::vector<SKeyAct> keyQueue;
    static bool                 keyQueued = false;
    static NHyprCommon::CHop    pendingKey;

    static void                 drainKeys() {
        keyQueued    = false;
        const auto Q = std::move(keyQueue);
        keyQueue.clear();
        for (const auto& A : Q) {
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
            } else if (A.verb == 4) {
                if (GROUP)
                    Policy::toggleSilence(A.group);
                else
                    muteApp(A.id);
            } else if (A.verb == 5)
                markSender(A.id);
            else if (A.verb == 6 && !GROUP)
                Model::snooze(A.id);
            else if (A.verb == 7)
                Model::snoozeUndo(A.id);
            else if (A.verb == 8)
                Model::snoozeCycle(A.id);
        }
    }

    void onKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info) {
        if (NHyprCommon::sessionLocked()) {
            // a lock discards every half-tracked input state (crash class 3):
            // a queued shade action or pending esc must not drain after unlock,
            // and an armed reply field must not keep owning the keyboard with
            // text collected while the lock was up
            pendingEsc.reset();
            pendingKey.reset();
            keyQueue.clear();
            keyQueued = false;
            replyExit();
            return;
        }
        if (!centerVisible() || info.cancelled)
            return;
        // input-capture-v1 is fed after the plugin emissions: while a client
        // owns the physical stream, the shade must not cancel a key first
        if (NHyprCommon::nativeInputCaptureActive())
            return;
        // releases pass untouched (crash class 3: never cancel key releases)
        if (e.state != WL_KEYBOARD_KEY_STATE_PRESSED)
            return;
        const auto KB = g_pSeatManager ? g_pSeatManager->m_keyboard.lock() : nullptr;
        if (!KB || !KB->m_xkbState)
            return;

        // An armed reply field owns the keyboard first: the user is typing a
        // sentence, and every nav key below would otherwise steal a letter.
        if (replyArmed()) {
            if (replyKey(KB->m_xkbState, e.keycode + 8))
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
            // one more peel before the shade itself: an open manage panel is a
            // menu, and esc closes the innermost thing first
            if (const auto MID = centerManageRow(); MID != 0) {
                pendingEsc.arm([MID]() { centerToggleManage(MID); });
                return;
            }
            pendingEsc.arm([]() { setCenter(false); }); // deferred: the close reflows and refocuses
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
            case XKB_KEY_m: a.verb = 4; break; // mute the app
            case XKB_KEY_p: a.verb = 5; break; // mark the sender
            case XKB_KEY_s: a.verb = 6; break; // snooze the card
            case XKB_KEY_u: a.verb = 7; break; // take a snooze back, while its row is up
            default: return;
        }
        // nothing selected: the shade has not taken the keyboard, so a bare
        // space still belongs to whatever holds focus
        if (!centerSelection(a.id, a.group))
            return;

        // An undo row is a different keyboard surface with two verbs: u takes
        // the card back, s re-picks the duration (there is nothing left to
        // snooze). It has no fold and no primary, so those keys stay with
        // whatever holds focus; delete still means "go, and for good".
        const auto SEL     = a.group.empty() ? Model::byId(a.id) : nullptr;
        const bool UNDOROW = SEL && Model::snoozeConfirming(SEL);
        if (a.verb == 7 && !UNDOROW)
            return;
        if (UNDOROW) {
            if (a.verb == 1 || a.verb == 2)
                return;
            if (a.verb == 6)
                a.verb = 8;
        }
        // and a bare letter with nothing to do belongs to focus too: a bundle
        // has no one sender to mark and no one card to put away, and neither
        // does a card that is not a chat
        if (a.verb == 6 && !a.group.empty())
            return;
        if (a.verb == 5) {
            bool conv = false;
            if (a.group.empty())
                for (const auto& N : notifs)
                    if (N->id == a.id) {
                        conv = N->conversation;
                        break;
                    }
            if (!conv)
                return;
        }

        info.cancelled = true;
        keyQueue.push_back(std::move(a));
        if (keyQueued)
            return;
        keyQueued = true;
        pendingKey.arm(drainKeys);
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
        if (NHyprCommon::nativeInputCaptureActive()) {
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
        if (!CARD || heldButtons > 0 || NHyprCommon::nativePointerGrabActive() || NHyprCommon::nativeLayerOwnsPointer() ||
            (g_layoutManager && g_layoutManager->dragController()->target())) {
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
        pendingHit.reset();
        pendingEsc.reset();
        pendingKey.reset();
        hitQueued = keyQueued = false;
        hitQueue.clear();
        keyQueue.clear();
        swallowRelease = 0;
        heldButtons    = 0;
        scrollAcc      = 0;
        releasePointer();
    }

} // namespace NHyprnotify
