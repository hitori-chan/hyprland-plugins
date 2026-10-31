// awesome/notify/input.cpp — clicks, the hold, wheel paging, esc and
// pointer ownership over the cards, the shade and the card menu. The
// interaction map, exactly:
//
//   any card left = act (on the release, over the card it pressed) ·
//            HOLD left = the card menu (menu.cpp) · right = dismiss ·
//            middle = nothing (swallowed: the card is ours, not the
//            window's under it — a terminal must not get a paste). No
//            buttons ride a card: the hold is the menu, right the dismiss
//   popup    left = link/default → dismiss · in an opened reply field:
//            keep typing · its send circle sends
//   row      a center card IS its banner: left on the body fires the card's
//            primary (the fd.o `default`) and dismisses unless resident,
//            same as the popup — cards open by default, so the click is
//            spent on acting rather than on revealing; a card the budget
//            folded opens instead · a link opens
//   leaving  anything that RAISES something else closes the center with
//            it — the primary, an action, a link (invokeLive below has the
//            AOSP citation). Everything that keeps you here does not: a
//            dismissal, a fold, DND, Clear all, and a `resident` card's
//            actions, which is the spec's own way of saying the action does
//            not take you away.
//   deck     a collapsed stack: left spreads it · right dismisses the
//            stack · the hold is its top card's menu
//   shead    an expanded stack's header: Show Less collapses · right
//            dismisses the stack; its cards are rows
//   silent   the silent fold: left spreads it
//   header   "Clear All" hides the shade's cards for Model::UNDO_WINDOW;
//            the same capsule's Undo brings them back
//   controls its own card: a left press on a level's track drags it until
//            the release · the wheel steps a level as its keys do · the
//            keyboard's glyph puts its light out and back · the speaker
//            mutes · Follow the Room flips · the output pill and the
//            Bluetooth and DND capsules' words open their menus, their
//            circles flip the switch
//   menu     a row is picked on the left RELEASE — press, hold, slide onto
//            a row and let go, or click one on the open menu; any other
//            press closes it, swallowed
//   wheel    vertical pages the shade — captured only inside the panel box
//   keys     esc closes an open menu first; an opened reply field (a
//            banner's too) takes every key. While the center is open it
//            owns the nav set and nothing else:
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
#include "../core/keys.hpp"

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
        uint8_t      part;   // the SHover part codes: 0 body, 1 Show Less, 3 reply field, 4 send
        std::string  href;   // non-empty: a body hyperlink
        uint64_t     rev     = 0;     // the card's revision at the press: its layout's actions are that revision's
        bool         outside = false; // the click fell outside every surface (closes the shade)
        bool         folded  = false; // a center card the budget folded: the click opens it
        bool operator==(const SHit&) const = default;
    };
    static void           drainHits(std::vector<SHit>& batch);
    static void           releaseLeft(const Vector2D& pos);
    // clicks deferred out of the input emission; 32 pending is far past a
    // human's clicks per turn
    static NAwesome::CHopQueue<SHit, 32> hits(drainHits);
    static NAwesome::CHop pendingEsc;

    // most-specific-first: rows are pushed after the panel they sit on
    static const SCard* cardAt(const Vector2D& pos) {
        if (cards.empty())
            return nullptr;
        const auto MON = cardsMon.lock();
        if (!MON || !MON->logicalBox().containsPoint(pos))
            return nullptr;
        for (size_t i = cards.size(); i-- > 0;)
            if (cards[i].kind != SCard::CAPSULE && cards[i].box.containsPoint(pos))
                return &cards[i];
        return nullptr;
    }

    static int linkAt(const SCard& c, const Vector2D& pos) {
        for (size_t i = 0; i < c.links.size(); i++)
            if (c.links[i].box.containsPoint(pos))
                return (int)i;
        return -1;
    }

    // a press this far past a level's track still grabs it, and this far
    // above or below its middle (logical px: the slider's row)
    constexpr double TRACK_SLOP = 4, SLIDER_REACH = 13;

    static uint8_t partAt(const SCard& c, const Vector2D& pos) {
        // a level's section: its slider's row drags along the track (0),
        // the rest is dead (2) — a press on the name or the value never
        // throws the level to an end (its glyph is a button of its own)
        if (c.kind == SCard::CTRL_SLIDER) {
            const double MID = c.track.y + c.track.h / 2;
            if (std::abs(pos.y - MID) > SLIDER_REACH)
                return 2;
            return pos.x >= c.track.x - TRACK_SLOP && pos.x <= c.track.x + c.track.w + TRACK_SLOP ? 0 : 2;
        }
        if (c.less.w > 0 && c.less.containsPoint(pos))
            return 1;
        if (c.replySend.w > 0 && c.replySend.containsPoint(pos))
            return 4;
        if (c.replyField.w > 0 && c.replyField.containsPoint(pos))
            return 3;
        return 0;
    }

    // what a press at `pos` would do: the press and its release each ask,
    // and a click is the two agreeing
    static SHit hitAt(const SCard& c, const Vector2D& pos, uint32_t bit) {
        SHit h;
        h.kind   = c.kind;
        h.id     = c.id;
        h.group  = c.group;
        h.bit    = bit;
        h.part   = partAt(c, pos);
        h.folded = c.folded;
        if (bit == 1u && h.part == 0)
            if (const int L = linkAt(c, pos); L >= 0)
                h.href = c.links[L].href;
        // the revision the press's layout drew, not the model's now: a
        // replace that landed after the last frame must not take the click
        h.rev = c.rev;
        return h;
    }

    // ---- the hold ----
    //
    // A left press on a notification waits: let go and it is a click,
    // hold HOLD and the card menu opens under the pointer. Drifting past
    // SLOP keeps the click and drops the hold. The timer runs on the event
    // loop, never inside an emission; every reset path drops the press.
    static struct {
        SHit     hit;
        Vector2D at;
        bool     live = false;
    } press;
    static SP<CEventLoopTimer> holdTimer;
    constexpr auto             HOLD = std::chrono::milliseconds(450);
    constexpr double           SLOP = 6; // logical px

    static void dropPress() {
        if (!press.live)
            return;
        press.live = false;
        if (holdTimer)
            holdTimer->updateTimeout(std::nullopt);
    }

    static bool holdable(const SCard& c, const SHit& h) {
        switch (c.kind) {
            case SCard::POPUP:
            case SCard::ROW:
            case SCard::DECK: return h.part == 0 && CardMenu::canOpen(c.id); // the reply field is typed in, never held
            default: return false;
        }
    }

    // ---- the deferred drain: what each surface DOES ----

    // Firing a card's primary (or one of its actions) LEAVES: the sender
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
    void invokeLive(uint32_t id, const std::string& actionOverride, std::optional<uint64_t> rev) {
        std::string action   = actionOverride;
        std::string sender; // the X11 activation lookup, captured before close-on-act
        bool        resident = false, live = false;
        for (const auto& N : notifs)
            if (N->id == id) {
                // replaced between the press and this deferred click: the
                // pressed button belonged to what the card WAS (a reused id
                // may be another chat now) — fire nothing
                if (rev && N->rev != *rev)
                    return;
                if (action.empty())
                    action = N->defaultAction;
                else if (std::ranges::none_of(N->actions, [&](const SAction& A) { return A.id == action; }))
                    return;
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
        Bus::invokeAction(id, action, sender, /*raise=*/!resident);
        if (resident)
            return; // the card stays, and so does the shade behind it — and nothing comes up over it
        setCenter(false); // no re-pop: the app the action raises is coming up over the parked stack
        Model::closeOne(id, Model::R_DISMISSED);
    }

    // Deferred out of the input emission: closes reflow the layout and an
    // action can make the client focus/raise itself. Queue+drain so two
    // clicks in one dispatch both land.
    static void queueHit(SHit h) {
        hits.push(std::move(h));
    }

    // an opened reply field's parts: true when the click was the field's
    static bool replyPart(const SHit& H) {
        if (H.part == 3) // inside the field: keep typing
            return true;
        if (H.part != 4)
            return false;
        const auto TX = replyText(); // its send circle
        const auto TG = replyTargetKey();
        replyClose();
        if (!TX.empty() && !Bus::sendReply(H.id, TX, TG))
            replySendRefused(H.id, TX, TG);
        return true;
    }

    static void drainHits(std::vector<SHit>& batch) {
        for (const auto& H : batch) {
            if (H.outside) { // a click off every surface closes the open panel
                setCenter(false, /*repop=*/true);
                setControls(false);
                continue;
            }
            switch (H.kind) {
                case SCard::POPUP: {
                    if (H.bit == 2u) {
                        Model::closeOne(H.id, Model::R_DISMISSED);
                        continue;
                    }
                    if (H.bit != 1u)
                        continue;
                    if (replyPart(H))
                        continue;
                    if (!H.href.empty()) { // left on a hyperlink: open it, keep the card up
                        NAwesome::Jobs::inst().spawn(NAwesome::Jobs::OPEN, {"xdg-open", H.href.c_str(), nullptr});
                        continue;
                    }
                    invokeLive(H.id, "", H.rev);
                    continue;
                }
                case SCard::ROW: {
                    if (H.bit == 2u) {
                        Model::closeOne(H.id, Model::R_DISMISSED);
                        continue;
                    }
                    if (H.bit != 1u)
                        continue;
                    if (H.folded) { // a folded card opens before it acts
                        centerToggleRow(H.id);
                        continue;
                    }
                    if (replyPart(H))
                        continue;
                    if (!H.href.empty()) { // a link in the body: open it, keep the card
                        NAwesome::Jobs::inst().spawn(NAwesome::Jobs::OPEN, {"xdg-open", H.href.c_str(), nullptr});
                        setCenter(false); // but not the center: a browser is coming up over it
                        continue;
                    }
                    invokeLive(H.id, "", H.rev); // the body IS the card's primary, as on the banner
                    continue;
                }
                case SCard::DECK: {
                    if (H.bit == 2u) { // right: the whole stack goes
                        centerDismissStack(H.group);
                        continue;
                    }
                    if (H.bit == 1u)
                        centerToggleGroup(H.group); // left spreads the stack
                    continue;
                }
                case SCard::SHEAD: {
                    if (H.bit == 2u) { // right: the whole stack goes
                        centerDismissStack(H.group);
                        continue;
                    }
                    if (H.bit == 1u && H.part == 1)
                        centerToggleGroup(H.group); // Show Less
                    continue;
                }
                case SCard::SILENT: // the silent fold spreads
                    if (H.bit == 1u)
                        centerToggleGroup(H.group);
                    continue;
                case SCard::BTN_CLEAR: // the heading's: Clear All hides, Undo (as it was drawn) brings back
                    if (H.bit == 1u) {
                        if (H.id)
                            Model::undoClear();
                        else
                            Model::clearLive();
                    }
                    continue;
                case SCard::CTRL_SWITCH:
                    if (H.bit == 1u)
                        controlsSwitch(H.id);
                    continue;
                case SCard::CTRL_SLIDER: continue; // a track's press drags in place
                case SCard::PANEL: continue; // dead panel space swallows silently
                case SCard::CAPSULE: continue; // never hit (cardAt skips it)
                case SCard::MENU: CardMenu::close(); continue; // a press off its rows
                case SCard::MENU_ITEM:
                    if (H.bit == 2u)
                        CardMenu::secondary((int)H.id, H.group);
                    else
                        CardMenu::pick((int)H.id, H.group);
                    continue;
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
        if (NAwesome::sessionLocked() || NAwesome::nativeInputCaptureActive()) {
            swallowRelease = 0;
            heldButtons    = 0;
            return;
        }

        const uint32_t BIT = e.button == BTN_LEFT ? 1u : e.button == BTN_RIGHT ? 2u : e.button == BTN_MIDDLE ? 4u : 0u;

        if (e.state == WL_POINTER_BUTTON_STATE_RELEASED) {
            if (BIT && (swallowRelease & BIT)) {
                swallowRelease &= ~BIT;
                info.cancelled = true;
                if (BIT == 1u)
                    releaseLeft(g_pInputManager->getMouseCoordsInternal());
            } else if (!info.cancelled) // a release the shell module swallowed ends a press we never counted
                heldButtons = std::max(0, heldButtons - 1);
            return;
        }
        // the compositor holds nothing: no count can be live (a pointer
        // unplugged mid-press never sends its release)
        if (g_pInputManager && !g_pInputManager->hasHeldButtons())
            heldButtons = 0;

        // the shell module runs first: a press it swallowed (strip click, open tray
        // menu over the card region) was never ours — and never reached an
        // app, so there is no grab to count
        if (info.cancelled)
            return;

        const auto COORDS = g_pInputManager->getMouseCoordsInternal();
        const auto CARD   = BIT ? cardAt(COORDS) : nullptr;
        const bool MENU   = CardMenu::isOpen();

        // A live implicit or seat grab, or a native layer surface at the
        // point, stays authoritative over every drawn surface (the bar's
        // strip does the same): the press goes through, the cards only
        // watch — and an open menu closes, as for any press off it
        if (heldButtons > 0 || NAwesome::nativePointerGrabActive()) {
            heldButtons++;
            if (MENU)
                queueHit({.kind = SCard::MENU});
            return;
        }
        if (NAwesome::nativeLayerOwnsPointer()) {
            if (MENU)
                queueHit({.kind = SCard::MENU});
            return;
        }

        // The open menu owns every press: a row is picked on its release,
        // any other press closes it, swallowed like the tray menu's
        if (MENU) {
            if (BIT) {
                info.cancelled = true;
                swallowRelease |= BIT;
            }
            if (!CARD || (CARD->kind != SCard::MENU && CARD->kind != SCard::MENU_ITEM))
                queueHit({.kind = SCard::MENU});
            else if (BIT == 2u && CARD->kind == SCard::MENU_ITEM) // a row's secondary: on the press, as a context menu opens
                queueHit({.kind = SCard::MENU_ITEM, .id = CARD->id, .group = CARD->group, .bit = 2u});
            return;
        }

        if (!CARD) {
            // Android closes the shade on an outside tap; the closing click
            // is swallowed, like the tray menu's. The corner dead-strip
            // (offset_y, the screen edge) is the most common stray click
            // next to a conversation — drainHits re-pops the absorbed stack
            // so the close does not leave the notifications invisible
            if ((centerVisible() || controlsVisible()) && BIT) {
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
        if (BIT == 4u)
            return; // middle: no verb

        auto h = hitAt(*CARD, COORDS, BIT);
        // a level's track: the level follows the pointer from the press to
        // the release (motion and release below), each move a deferred ask
        if (BIT == 1u && CARD->kind == SCard::CTRL_SLIDER && h.part == 0) {
            controlsDragStart(*CARD, COORDS);
            return;
        }
        if (BIT == 1u && holdable(*CARD, h) && holdTimer) {
            dropPress();
            press.hit  = std::move(h);
            press.at   = COORDS;
            press.live = true;
            holdTimer->updateTimeout(HOLD);
            return;
        }
        queueHit(std::move(h));
    }

    // The left release: a pending press clicks if it ends where it began
    // (the same card, the same button or link); a menu row under the
    // release is picked — the press-hold-slide-release gesture, or a plain
    // click on the open menu.
    static void releaseLeft(const Vector2D& pos) {
        if (controlsDragging()) {
            controlsDragEnd();
            return;
        }
        if (press.live) {
            const SHit H = std::move(press.hit);
            dropPress();
            if (const auto CARD = cardAt(pos); CARD && hitAt(*CARD, pos, 1u) == H)
                queueHit(H);
            return;
        }
        if (CardMenu::isOpen())
            if (const auto CARD = cardAt(pos); CARD && CARD->kind == SCard::MENU_ITEM)
                queueHit({.kind = SCard::MENU_ITEM, .id = CARD->id, .group = CARD->group, .bit = 1u});
    }

    // ---- wheel: page the center, only inside the panel box ----

    static double scrollAcc = 0, levelAcc = 0, menuAcc = 0;

    void          onMouseAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) {
        if (NAwesome::sessionLocked() || NAwesome::nativeInputCaptureActive()) {
            scrollAcc = levelAcc = menuAcc = 0;
            return;
        }
        // the cheap tests first: every wheel notch and touchpad frame lands
        // here, and the native hit-test walks the layer and popup stacks
        const bool MENU = CardMenu::isOpen();
        if ((!centerVisible() && !controlsVisible() && !MENU) || cards.empty() || info.cancelled || heldButtons > 0)
            return;
        if (NAwesome::nativePointerGrabActive() || NAwesome::nativeLayerOwnsPointer())
            return;
        const auto POS  = g_pInputManager->getMouseCoordsInternal();
        const auto CARD = cardAt(POS);
        if (!CARD)
            return; // outside the panel: windows scroll normally
        // under the control center the banners keep their own wheel (none):
        // only the panel takes it
        if (!centerVisible() && !MENU && CARD->kind != SCard::CTRL_SLIDER && CARD->kind != SCard::CTRL_SWITCH && CARD->kind != SCard::PANEL)
            return;
        info.cancelled = true;
        if (e.axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
            return;
        const double DELTA = e.delta != 0.0 ? e.delta : e.deltaDiscrete / 120.0 * 15.0;
        // under an open menu the shade holds still (a page would slide its
        // card out from under it); what a scan found scrolls in its window
        if (MENU) {
            menuAcc += DELTA;
            if (const int STEP = (int)(menuAcc / 15.0); STEP != 0) {
                menuAcc -= STEP * 15.0;
                CardMenu::wheel(POS, STEP);
            }
            return;
        }
        // over a level the wheel moves it (up raises); over a switch, nothing
        if (CARD->kind == SCard::CTRL_SLIDER) {
            levelAcc += DELTA;
            if (const int STEP = (int)(levelAcc / 15.0); STEP != 0) {
                levelAcc -= STEP * 15.0;
                controlsWheel((Controls::eLevel)CARD->id, -STEP);
            }
            return;
        }
        if (CARD->kind == SCard::CTRL_SWITCH || !centerVisible())
            return;
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
        std::string group; // non-empty: a stack
        std::optional<uint64_t> rev; // enter: the revision the selected row drew
    };
    static void drainKeys(std::vector<SKeyAct>& batch) {
        for (const auto& A : batch) {
            const bool GROUP = !A.group.empty();
            if (A.verb == 1 || (A.verb == 2 && GROUP)) { // space, and enter on a stack: spread or collapse
                if (GROUP)
                    centerToggleGroup(A.group);
                else
                    centerToggleRow(A.id);
            } else if (A.verb == 2)
                invokeLive(A.id, "", A.rev); // enter on a card: its primary, the body click's twin
            else if (A.verb == 3) {
                if (GROUP)
                    centerDismissStack(A.group);
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
        replyKeyEvent(e); // a press or the held key's release ends its repeat
        const bool MENU = CardMenu::isOpen();
        if ((!centerVisible() && !controlsVisible() && !MENU && !replyHeld()) || info.cancelled)
            return;
        // input-capture-v1 is fed after the plugin emissions: while a client
        // owns the physical stream, the shade must not cancel a key first
        if (NAwesome::nativeInputCaptureActive())
            return;
        // releases pass untouched (crash class 3: never cancel key releases)
        if (e.state != WL_KEYBOARD_KEY_STATE_PRESSED)
            return;
        // an exclusive-keyboard layer (a launcher a bind opened over the
        // shade) owns typing natively: the shade and its field step aside,
        // the field keeps its text for when the layer goes
        if (NAwesome::Keys::exclusiveLayer())
            return;
        // the keyboard that typed it, with its own layout and modifiers
        const auto KB = NAwesome::Keys::keyboardFor(e.keycode);
        if (!KB || !KB->m_xkbState)
            return;

        // An open menu peels first: Esc closes it before the field or the
        // shade see the key; nothing else is the menu's
        if (MENU) {
            bool chord = false;
            for (const char* M : {XKB_MOD_NAME_CTRL, XKB_MOD_NAME_ALT, XKB_MOD_NAME_LOGO})
                chord = chord || xkb_state_mod_name_is_active(KB->m_xkbState, M, XKB_STATE_MODS_EFFECTIVE) > 0;
            const auto SYM = xkb_state_key_get_one_sym(KB->m_xkbState, e.keycode + 8);
            if (!chord && SYM == XKB_KEY_Escape) {
                info.cancelled = true;
                pendingEsc.arm([]() { CardMenu::close(); });
                return;
            }
            if (!centerVisible() && !controlsVisible() && !replyHeld())
                return;
        }

        // An armed reply field owns the keyboard first: the user is typing a
        // sentence, and every nav key below would otherwise steal a letter.
        const bool HELD = replyHeld();
        if (replyArmed()) {
            if (replyKey(KB, e.keycode))
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
        if (!centerVisible() && !controlsVisible())
            return; // a banner's field, gone: the rest is the panels'

        // a modified chord is a user bind passing through, never the shade's
        for (const char* M : {XKB_MOD_NAME_CTRL, XKB_MOD_NAME_ALT, XKB_MOD_NAME_LOGO})
            if (xkb_state_mod_name_is_active(KB->m_xkbState, M, XKB_STATE_MODS_EFFECTIVE) > 0)
                return;

        const auto SYM = xkb_state_key_get_one_sym(KB->m_xkbState, e.keycode + 8);
        if (SYM == XKB_KEY_Escape) {
            info.cancelled = true;
            // deferred: the close reflows and refocuses
            pendingEsc.arm([]() {
                setCenter(false, /*repop=*/true);
                setControls(false);
            });
            return;
        }
        if (!centerVisible())
            return; // the control center takes Esc alone
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
            // the shade's rows, not the model: DND-held and OSD cards are
            // cards but never rows, so with only those there is nothing to
            // select and the keys stay the focused window's
            if (!centerHasRows())
                return;
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
        // enter acts on what the row drew, like a click: a replace that
        // landed since the last frame must not take it
        if (a.verb == 2 && a.group.empty())
            if (const auto IT = std::ranges::find_if(cards, [&](const SCard& c) { return c.id == a.id && c.kind == SCard::ROW; }); IT != cards.end())
                a.rev = IT->rev;

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
        scrollAcc = levelAcc = menuAcc = 0;
        dropPress();
        controlsDragEnd(); // its release may never come back up
        CardMenu::close(); // half-tracked input state: its press may never come back up
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
        if (NAwesome::sessionLocked() || NAwesome::nativeInputCaptureActive()) {
            setHovered({});
            releasePointer();
            return;
        }

        // a level being dragged follows the pointer wherever it goes, the
        // bar included, until the release
        if (controlsDragging()) {
            controlsDragMove(g_pInputManager ? g_pInputManager->getMouseCoordsInternal() : pos);
            info.cancelled = true;
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
        const auto P = g_pInputManager ? g_pInputManager->getMouseCoordsInternal() : pos;
        if (press.live && holdTimer && P.distance(press.at) > SLOP)
            holdTimer->updateTimeout(std::nullopt); // drifted: still a click, never a hold
        const auto CARD = cardAt(P);
        if (!CARD || heldButtons > 0 || NAwesome::nativePointerGrabActive() || NAwesome::nativeLayerOwnsPointer() ||
            (g_layoutManager && g_layoutManager->dragController()->target())) {
            setHovered({});
            releasePointer();
            return;
        }

        // per motion: the hover (and its group key's copy) is rebuilt only
        // when it changed
        const uint8_t PART = partAt(*CARD, P);
        if (hovered.kind != CARD->kind || hovered.id != CARD->id || hovered.part != PART || hovered.group != CARD->group) {
            SHover h;
            h.kind  = CARD->kind;
            h.id    = CARD->id;
            h.group = CARD->group;
            h.part  = PART;
            setHovered(h);
        }
        info.cancelled = true;

        const bool ONLINK = PART == 0 && linkAt(*CARD, P) >= 0; // a hyperlink shows the hand (GTK convention)

        const bool ENTERING = !pointerOwned;
        if (ENTERING || !g_pSeatManager->m_state.pointerFocus.expired()) {
            pointerOwned = true;
            // the app under the surface gets its leave — on entry, and again
            // after a native re-send (a map, a group add) entered it beneath
            g_pSeatManager->setPointerFocus(nullptr, {});
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
            h.part  = partAt(*CARD, COORDS);
            setHovered(h);
        } else
            setHovered({});
        if (!pointerOwned || CARD)
            return;
        releasePointer();
        g_pInputManager->simulateMouseMovement(); // the window beneath gets its enter back
    }

    void inputInit() {
        holdTimer = makeShared<CEventLoopTimer>(
            std::nullopt,
            [](SP<CEventLoopTimer>, void*) {
                // the press it was armed for, still down; a lock or a native
                // capture since dropped it (onInputBlocked)
                if (!press.live)
                    return;
                press.live = false;
                CardMenu::open(press.hit.id, press.at);
            },
            nullptr);
        g_pEventLoopManager->addTimer(holdTimer);
        CardMenu::init(); // its tick; exit() takes it out
    }

    void inputExit() {
        hits.reset();
        pendingEsc.reset();
        keys.reset();
        dropPress();
        if (holdTimer && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(holdTimer);
        holdTimer.reset();
        CardMenu::exit();
        controlsDragEnd();
        swallowRelease = 0;
        heldButtons    = 0;
        scrollAcc = levelAcc = menuAcc = 0;
        releasePointer();
    }

} // namespace NAwesome::Notify
