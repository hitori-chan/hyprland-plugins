// hyprnotify/input.cpp — clicks, wheel paging, esc and pointer ownership
// over the popups and the shade. Implements the interaction map exactly:
//
//   popup    left = action/link/default → dismiss · right = dismiss ·
//            middle = park the stack into the shade · hover reveals ✕
//   row      LEFT READS: anywhere on the row (the chevron included) folds it
//            open ⇄ shut and nothing else — the shade's most common intent
//            gets its biggest target, and none of it is destructive. A
//            button acts (the primary included) → dismiss unless resident ·
//            right = dismiss · middle = Clear all
//   child    a bundle child is already open: only its buttons and right act
//   digest   left expands the app's bundle · right dismisses it
//   ghead    left collapses · the ✕ / right dismisses the bundle
//   footer   ⊖ = DND · "Clear all" = the global sweep
//   wheel    pages the shade — captured only inside the panel box
//   esc      closes the shade (the topmost-peel's middle link)
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
                        Bus::absorbPopped(); // middle: park the stack into the shade (no dismiss)
                        return;              // the rest reference now-parked cards
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
                    if (H.bit == 4u) {
                        Bus::dismissAllLive();
                        return; // the rest of the queue references swept cards
                    }
                    if (H.bit == 2u) {
                        Bus::closeOne(H.id, Bus::R_DISMISSED);
                        continue;
                    }
                    if (H.bit != 1u)
                        continue;
                    if (!H.action.empty()) { // a button — the primary included
                        invokeLive(H.id, H.action);
                        continue;
                    }
                    // left on the body or the chevron: read it. A bundle child
                    // is already open, so there is nothing to reveal.
                    if (H.kind == SCard::ROW)
                        centerToggleRow(H.id);
                    continue;
                }
                case SCard::DIGEST: {
                    if (H.bit == 1u) { // left expands the app's bundle
                        centerToggleGroup(H.group);
                        continue;
                    }
                    if (H.bit == 2u) { // right: the whole bundle goes
                        Bus::dismissApp(H.group);
                        continue;
                    }
                    if (H.bit == 4u) {
                        Bus::dismissAllLive();
                        return;
                    }
                    continue;
                }
                case SCard::GHEAD: {
                    if (H.part == 2 || H.bit == 2u) { // the static ✕ / right: the whole bundle goes
                        Bus::dismissApp(H.group);
                        continue;
                    }
                    if (H.bit == 1u) {
                        centerToggleGroup(H.group); // collapse
                        continue;
                    }
                    if (H.bit == 4u) {
                        Bus::dismissAllLive();
                        return;
                    }
                    continue;
                }
                case SCard::BTN_CLEAR: // the footer: the global sweep
                    if (H.bit == 1u)
                        Bus::dismissAllLive();
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
        hitQueued = false;
        hitQueue.clear();
        swallowRelease = 0;
        heldButtons    = 0;
        scrollAcc      = 0;
        releasePointer();
    }

} // namespace NHyprnotify
