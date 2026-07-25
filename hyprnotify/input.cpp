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
//            dismiss · middle = Clear all
//   child    a bundle child is a row without the fold: body, links, buttons
//   digest   left expands the app's bundle · right dismisses it
//   ghead    left collapses · the ✕ / right dismisses the bundle
//   footer   ⊖ = DND · "Clear all" = the global sweep
//   wheel    pages the shade — captured only inside the panel box
//   keys     while the shade is open it owns the nav set and nothing else:
//            esc closes (the topmost-peel's middle link) · ↑/↓ move the
//            selection · space folds it (the click's twin) · enter fires the
//            primary · delete dismisses · tab opens the selected card's
//            reply field, and while one is armed EVERY key is the field's
//            (reply.cpp). A chord with ctrl/alt/super is the user's bind, and
//            space/enter/delete with NOTHING selected still belong to
//            whatever holds focus — the shade never grabs a key it has no
//            use for.
//
// Every mutation lands via the hit queue + CHop drain, never synchronously
// inside the emission (crash class 6); every listener gates on
// sessionLocked() first and resets its half-tracked state there (class 7).

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
        uint8_t      part;   // 0 body, 1 chevron, 2 close
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
            Model::closeOne(id, Model::R_DISMISSED);
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
        h.group = CARD->group;
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

    // ---- keys: esc peels the center (tray menu > center > menubar: load
    //      order puts hyprbar's menu first, we're next), and the shade drives
    //      its selection ----

    // Same shape as the click queue, and for the same reason: an action can
    // make the client focus itself, so nothing runs inside the emission.
    struct SKeyAct {
        int         verb = 0; // 1 fold, 2 the primary, 3 dismiss
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
            }
        }
    }

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
            default: return;
        }
        // nothing selected: the shade has not taken the keyboard, so a bare
        // space still belongs to whatever holds focus
        if (!centerSelection(a.id, a.group))
            return;

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
