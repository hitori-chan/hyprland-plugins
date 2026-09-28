// The interaction layer (input.cpp + the reply field's keys): clicks, wheel
// paging, esc and pointer ownership over the popups and the shade.
// Implements the interaction map exactly:
//
//   popup    left = action/link/default → dismiss · right = dismiss ·
//            middle = park the stack into the shade · hover reveals ✕
//   row      a shade row IS its banner: left on the body fires the card's
//            primary (the fd.o `default`) and dismisses unless resident —
//            rows open by default, so the click is spent on acting. The
//            CHEVRON is the only fold target · a link opens · a button
//            acts · right = dismiss · middle = Clear all
//   child    a bundle child is a row without the fold
//   digest   left expands the app's bundle · right dismisses
//   ghead    left collapses · the ✕ / right dismisses the bundle
//   footer   ⊖ = DND · "Clear all" = the global sweep
//   wheel    vertical pages the shade (only inside the panel box);
//            HORIZONTAL AWAY on a row is the phone's swipe-to-dismiss
//   keys     while the shade is open it owns the nav set and nothing else:
//            esc closes · ↑/↓ move the selection · space folds · enter fires
//            the primary · delete dismisses · tab opens the reply field, and
//            while one is armed EVERY key is the field's (notify::reply_key)
//
// Every mutation lands via the hit/key queues + a job drain, never
// synchronously inside the emission (crash class 6); every listener gates
// on sessionLocked() first and resets its half-tracked state there
// (crash class 3), and a native input-capture session, an implicit or seat
// grab, or a native layer surface at the point passes through unintercepted.

use crate::ffi;
use crate::notify::{self, Card, CardKind, Hover};
use crate::probe::State;

/// One queued click (the drain decides what each surface does).
pub struct Hit {
    pub kind: CardKind,
    pub id: u32,
    pub group: String,
    /// a BTN bit: 1 left, 2 right, 4 middle
    pub bit: u32,
    /// the part codes: 0 body, 1 chevron, 2 close, 3 reply field, 4 send
    pub part: u8,
    /// non-empty: a specific action button
    pub action: String,
    /// non-empty: a body hyperlink
    pub href: String,
    /// the click fell outside every surface (closes the shade)
    pub outside: bool,
}

/// One queued key action: verb 1 fold, 2 the primary, 3 dismiss.
pub struct KeyAct {
    pub verb: i32,
    pub id: u32,
    /// non-empty: a bundle
    pub group: String,
}

// ---------------------------------------------------------------------------
// hit testing (most-specific-first: rows/buttons sit on the panel)
// ---------------------------------------------------------------------------

fn card_at(ctx: ffi::Ctx, st: &notify::NotifyState, pos: (f64, f64)) -> Option<&Card> {
    if st.cards.is_empty() {
        return None;
    }
    let mon = st.cards_mon.as_ref()?;
    let (mb, _scale) = ffi::monitor_box(ctx, mon)?;
    if pos.0 < mb.x || pos.0 >= mb.x + mb.w || pos.1 < mb.y || pos.1 >= mb.y + mb.h {
        return None;
    }
    st.cards
        .iter()
        .rev()
        .find(|c| c.rect.contains(pos.0, pos.1))
}

fn button_at(c: &Card, pos: (f64, f64)) -> i32 {
    c.buttons
        .iter()
        .position(|b| b.0.contains(pos.0, pos.1))
        .map_or(-1, |i| i as i32)
}

fn link_at(c: &Card, pos: (f64, f64)) -> bool {
    c.links.iter().any(|l| l.0.contains(pos.0, pos.1))
}

fn part_at(c: &Card, pos: (f64, f64)) -> u8 {
    if !c.chevron.empty() && c.chevron.contains(pos.0, pos.1) {
        return 1;
    }
    if !c.close.empty() && c.close.contains(pos.0, pos.1) {
        return 2;
    }
    if !c.reply_send.empty() && c.reply_send.contains(pos.0, pos.1) {
        return 4;
    }
    if !c.reply_field.empty() && c.reply_field.contains(pos.0, pos.1) {
        return 3;
    }
    0
}

// ---------------------------------------------------------------------------
// the deferred drains: what each surface DOES
// ---------------------------------------------------------------------------

/// Fire the card's primary (or one of its buttons). Firing LEAVES: the
/// sender raises itself over the very shade the click was made in (the
// AOSP shade collapses on a content click; swaync ships hide-on-action).
/// An action that does NOT start an activity leaves the shade standing —
/// fd.o has no isActivity, but it has `resident`: the spec's way of saying
/// the action keeps you here. A card with no action at all launches
/// nothing: that click is a dismissal, and dismissing never closes the
/// shade.
pub fn invoke_live(
    state: &State,
    st: &mut notify::NotifyState,
    id: u32,
    action_override: &str,
    replies: &mut Vec<crate::bus::BusReply>,
) {
    let (action, resident, sender) = match st.notifs.iter().find(|n| n.id == id) {
        Some(n) => (
            if action_override.is_empty() {
                n.default_action.clone()
            } else {
                action_override.to_owned()
            },
            n.resident,
            n.sender.clone(),
        ),
        None => return,
    };
    if action.is_empty() {
        // nothing to fire: the body click is a dismissal
        notify::close_one(state.ctx, st, id, notify::R_DISMISSED, replies);
        return;
    }
    let token = notify::mint_token(state.ctx);
    if let Some(b) = crate::bus::handle() {
        b.send(crate::bus::BusCmd::EmitAction {
            id,
            action: action.clone(),
            token,
        });
        // the token is for Wayland senders (they spend it through
        // xdg-activation). An X11 sender cannot: its activation arrives as
        // an urgency ping, so the side that saw the click focuses the
        // sender's own window (the pid lookup runs on the bus thread).
        if !sender.is_empty() {
            b.send(crate::bus::BusCmd::ResolvePid { sender });
        }
    }
    if resident {
        return; // the card stays, and so does the shade behind it
    }
    // no re-pop: the app the action raises is coming up over the parked stack
    notify::set_center(state.ctx, st, false, false, replies);
    notify::close_one(state.ctx, st, id, notify::R_DISMISSED, replies);
}

/// Hand a typed reply back (the send pill, Enter in the field): the token
/// precedes the signal, and `resident` holds the card, as ever. The model
/// replies come back to the caller (the emission has no reply queue of its
/// own; the caller folds them into the state's pending replies).
fn send_reply(
    state: &State,
    st: &mut notify::NotifyState,
    id: u32,
    text: &str,
) -> Vec<crate::bus::BusReply> {
    if text.is_empty() {
        return Vec::new();
    }
    let Some(n) = st.notifs.iter().find(|n| n.id == id) else {
        return Vec::new();
    };
    if !n.can_reply {
        return Vec::new();
    }
    let resident = n.resident;
    let token = notify::mint_token(state.ctx);
    if let Some(b) = crate::bus::handle() {
        b.send(crate::bus::BusCmd::EmitReplied {
            id,
            text: text.to_owned(),
            token,
        });
    }
    let mut replies = Vec::new();
    if resident {
        notify::changed(state.ctx, st);
    } else {
        notify::close_one(state.ctx, st, id, notify::R_DISMISSED, &mut replies);
    }
    replies
}

/// The click drain (crash class 6): two card-clicks in one dispatch both
/// land, in order.
#[allow(clippy::too_many_lines)]
pub fn drain_hits(
    state: &State,
    st: &mut notify::NotifyState,
    replies: &mut Vec<crate::bus::BusReply>,
) {
    st.hit_pending = false;
    let q = std::mem::take(&mut st.hit_queue);
    for h in q {
        if h.outside {
            // a click off every surface closes the center; the drain
            // re-pops the absorbed stack so the close does not leave the
            // notifications invisible
            notify::set_center(state.ctx, st, false, true, replies);
            continue;
        }
        // a click on the shade keeps it: hover peeks, click pins
        notify::center_pin(state.ctx, st, replies);
        match h.kind {
            CardKind::Popup => {
                if h.bit == 4 {
                    notify::absorb_popped(state.ctx, st, replies);
                    return; // the rest of the queue references now-parked cards
                }
                if h.bit == 2 || h.part == 2 {
                    notify::close_one(state.ctx, st, h.id, notify::R_DISMISSED, replies);
                    continue;
                }
                if !h.href.is_empty() {
                    // left on a hyperlink: open it, keep the card up
                    notify::spawn_detached(state.ctx, st, &["xdg-open", h.href.as_str()]);
                    continue;
                }
                invoke_live(state, st, h.id, &h.action, replies);
            }
            CardKind::Row | CardKind::Child => {
                if h.bit == 4 {
                    notify::dismiss_all_live(state.ctx, st, replies);
                    return; // the rest of the queue references swept cards
                }
                if h.bit == 2 {
                    notify::close_one(state.ctx, st, h.id, notify::R_DISMISSED, replies);
                    continue;
                }
                if h.bit != 1 {
                    continue;
                }
                if h.part == 1 {
                    // the chevron, and only it, folds
                    toggle_row(state.ctx, st, h.id);
                    continue;
                }
                if h.part == 3 {
                    continue; // inside the armed field: keep typing
                }
                if h.part == 4 {
                    // the send pill
                    let tx = notify::reply_text(st).to_owned();
                    notify::reply_close(state.ctx, st);
                    replies.extend(send_reply(state, st, h.id, &tx));
                    continue;
                }
                if h.action == "inline-reply" {
                    notify::reply_open(state.ctx, st, h.id);
                    continue;
                }
                if !h.action.is_empty() {
                    invoke_live(state, st, h.id, &h.action, replies);
                    continue;
                }
                if !h.href.is_empty() {
                    // a link in the body: open it, keep the card — but not
                    // the shade: a browser is coming up over it
                    notify::spawn_detached(state.ctx, st, &["xdg-open", h.href.as_str()]);
                    notify::set_center(state.ctx, st, false, false, replies);
                    continue;
                }
                // the body IS the card's primary, as on the banner
                invoke_live(state, st, h.id, "", replies);
            }
            CardKind::Digest => {
                if h.bit == 1 {
                    toggle_group(state.ctx, st, &h.group);
                    continue;
                }
                if h.bit == 2 {
                    notify::dismiss_app(state.ctx, st, &h.group, replies);
                    continue;
                }
                if h.bit == 4 {
                    notify::dismiss_all_live(state.ctx, st, replies);
                    return;
                }
            }
            CardKind::GHead => {
                if h.part == 2 || h.bit == 2 {
                    notify::dismiss_app(state.ctx, st, &h.group, replies);
                    continue;
                }
                if h.bit == 1 {
                    toggle_group(state.ctx, st, &h.group);
                    continue;
                }
                if h.bit == 4 {
                    notify::dismiss_all_live(state.ctx, st, replies);
                    return;
                }
            }
            CardKind::BtnClear => {
                if h.bit == 1 {
                    notify::dismiss_all_live(state.ctx, st, replies);
                }
            }
            CardKind::BtnDnd => {
                if h.bit == 1 {
                    notify::toggle_suspend(state.ctx, st, replies);
                }
            }
            CardKind::Panel => {
                // dead panel space swallows silently
            }
        }
    }
}

/// The key drain (crash class 6): an action can make the client focus
/// itself, so nothing runs inside the emission.
pub fn drain_keys(
    state: &State,
    st: &mut notify::NotifyState,
    replies: &mut Vec<crate::bus::BusReply>,
) {
    st.key_pending = false;
    let q = std::mem::take(&mut st.key_queue);
    for a in q {
        let group = !a.group.is_empty();
        if a.verb == 1 || (a.verb == 2 && group) {
            // space, and enter on a bundle: fold
            if group {
                toggle_group(state.ctx, st, &a.group);
            } else {
                toggle_row(state.ctx, st, a.id);
            }
        } else if a.verb == 2 {
            // enter on a card: its primary, the body click's twin
            invoke_live(state, st, a.id, "", replies);
        } else if group {
            notify::dismiss_app(state.ctx, st, &a.group, replies);
        } else {
            notify::close_one(state.ctx, st, a.id, notify::R_DISMISSED, replies);
        }
    }
}

// the fold/page verbs: the C++ originals reflow through doLater; here the
// caller's warm arm is the same edge
fn toggle_row(ctx: ffi::Ctx, st: &mut notify::NotifyState, id: u32) {
    notify::center_toggle_row(st, id);
    notify::changed(ctx, st);
}
fn toggle_group(ctx: ffi::Ctx, st: &mut notify::NotifyState, key: &str) {
    notify::center_toggle_group(st, key);
    notify::changed(ctx, st);
}
fn select_move(ctx: ffi::Ctx, st: &mut notify::NotifyState, dir: i32) {
    notify::center_select_move(st, dir);
    notify::changed(ctx, st);
}

// ---------------------------------------------------------------------------
// the input listeners
// ---------------------------------------------------------------------------

/// Is the tracked pointer over any card (the center peek's keep-open test;
/// pure — no ffi call, the position is refreshed on every mouse event).
pub fn pointer_over_cards(st: &notify::NotifyState) -> bool {
    st.cards
        .iter()
        .any(|c| c.rect.contains(st.pointer.0, st.pointer.1))
}

/// A pointer button. Returns whether the event is cancelled.
pub fn on_button(state: &State, st: &mut notify::NotifyState, ev: &ffi::SafeEvent) -> bool {
    st.pointer = (ev.x, ev.y);
    // emissions precede the compositor's own lock handling: locked input
    // belongs to the lockscreen, and half-tracked state must not survive it
    if ffi::session_locked(state.ctx) || ffi::input_capture_active(state.ctx) {
        st.swallow_release = 0;
        st.held_buttons = 0;
        return false;
    }

    let bit = ffi::tracked_button_bit(ev.button);

    if ev.state == 0 {
        // a release: the tracked ones are swallowed (a release we never
        // pressed must not cancel), the rest only unwinds the grab count
        if bit != 0 && (st.swallow_release & bit) != 0 {
            st.swallow_release &= !bit;
            return true;
        }
        if !ev.cancelled {
            st.held_buttons = (st.held_buttons - 1).max(0);
        }
        return false;
    }

    // an earlier listener (the C++ bar's strip) swallowed the press: it was
    // never ours, and never reached an app, so there is no grab to count
    if ev.cancelled {
        return false;
    }

    let (x, y) = ffi::mouse_coords(state.ctx);
    let card = if bit != 0 {
        card_at(state.ctx, st, (x, y))
    } else {
        None
    };

    // a live implicit or seat grab, or a native layer surface at the point,
    // stays authoritative over every drawn surface (the bar's strip does
    // the same): the press goes through, the cards only watch
    if st.held_buttons > 0 || ffi::native_pointer_grab(state.ctx) {
        st.held_buttons += 1;
        return false;
    }
    if ffi::native_layer_at(state.ctx) {
        return false;
    }
    match card {
        None => {
            // Android closes the shade on an outside tap; the closing click is
            // swallowed, like the tray menu's. The corner dead-strip
            // (offset_y, the screen edge) is the most common stray click next
            // to a conversation.
            if st.center_on && bit != 0 {
                st.swallow_release |= bit;
                st.hit_queue.push(Hit {
                    kind: CardKind::Panel,
                    id: 0,
                    group: String::new(),
                    bit,
                    part: 0,
                    action: String::new(),
                    href: String::new(),
                    outside: true,
                });
                notify::queue_hit(state.ctx, st);
                return true;
            }
            st.held_buttons += 1;
            false
        }
        Some(card) => {
            // copy the hit data out before the st mutation (the card is a
            // borrow of st itself)
            let part = part_at(card, (x, y));
            let mut action = String::new();
            let mut href = String::new();
            if bit == 1 && part == 0 {
                if let Some(b) = card.buttons.iter().find(|b| b.0.contains(x, y)) {
                    b.1.clone_into(&mut action);
                } else if let Some(l) = card.links.iter().find(|l| l.0.contains(x, y)) {
                    l.1.clone_into(&mut href);
                }
            }
            let (kind, id, group) = (card.kind, card.id, card.group.clone());
            // the surface is ours: the press must not reach the window beneath
            st.swallow_release |= bit;
            st.hit_queue.push(Hit {
                kind,
                id,
                group,
                bit,
                part,
                action,
                href,
                outside: false,
            });
            notify::queue_hit(state.ctx, st);
            true
        }
    }
}

/// A pointer axis: the wheel pages the center, only inside the panel box;
/// a horizontal flick away on a row is the phone's swipe-to-dismiss
/// (strictly an addition — a mouse without a horizontal wheel never reaches
/// it and loses no verb).
pub fn on_axis(state: &State, st: &mut notify::NotifyState, ev: &ffi::SafeEvent) -> bool {
    const SWIPE: f64 = 60.0; // a deliberate flick, not a nudge
    st.pointer = (ev.x, ev.y);
    if ffi::session_locked(state.ctx) || ffi::input_capture_active(state.ctx) {
        st.scroll_acc = 0.0;
        st.swipe_acc = 0.0;
        st.swipe_on = 0;
        return false;
    }
    if st.held_buttons > 0 || ffi::native_pointer_grab(state.ctx) || ffi::native_layer_at(state.ctx)
    {
        return false;
    }
    if !st.center_on || st.cards.is_empty() || ev.cancelled {
        return false;
    }
    let (x, y) = ffi::mouse_coords(state.ctx);
    let Some(card) = card_at(state.ctx, st, (x, y)) else {
        return false; // outside the panel: windows scroll normally
    };

    let delta = if ev.delta != 0.0 {
        ev.delta
    } else {
        ev.delta_discrete / 120.0 * 15.0
    };

    if ev.axis == 1 {
        // WL_POINTER_AXIS_HORIZONTAL_SCROLL
        let (kind, id) = (card.kind, card.id);
        let row = matches!(kind, CardKind::Row | CardKind::Child);
        if !row || id == 0 {
            st.swipe_acc = 0.0;
            st.swipe_on = 0;
            return true;
        }
        if st.swipe_on != id {
            // the pointer moved to another row mid-gesture
            st.swipe_on = id;
            st.swipe_acc = 0.0;
        }
        st.swipe_acc += delta;
        if st.swipe_acc >= SWIPE || st.swipe_acc <= -SWIPE {
            let away = st.swipe_acc > 0.0;
            st.swipe_acc = 0.0; // bounded either way; only away acts
            if away {
                // both verbs go through the CLICK queue rather than acting
                // here: a dismissal reflows the layout and emits on the bus
                st.hit_queue.push(Hit {
                    kind,
                    id,
                    group: String::new(),
                    bit: 2, // the swipe IS the right-click
                    part: 0,
                    action: String::new(),
                    href: String::new(),
                    outside: false,
                });
                notify::queue_hit(state.ctx, st);
            }
        }
        return true;
    }
    if ev.axis != 0 {
        return true; // only the vertical axis pages
    }
    st.swipe_acc = 0.0; // a vertical scroll ends any half-made flick
    st.scroll_acc += delta;
    let step = (st.scroll_acc / 15.0) as i32;
    if step != 0 {
        st.scroll_acc -= step as f64 * 15.0;
        notify::center_page(st, step);
        notify::changed(state.ctx, st);
    }
    true
}

/// A key: the shade's nav set, and — while a reply field is armed — the
/// field owns the keyboard first (the user is typing a sentence, and every
/// nav key would otherwise steal a letter). Releases always pass (crash
// class 3: never cancel key releases).
#[allow(clippy::too_many_lines)]
pub fn on_key(state: &State, st: &mut notify::NotifyState, ev: &ffi::SafeEvent) -> bool {
    if ffi::session_locked(state.ctx) {
        // a lock discards every half-tracked input state (crash class 3):
        // a queued shade action or pending esc must not drain after unlock,
        // and an armed reply field must not keep owning the keyboard with
        // text collected while the lock was up
        st.esc_pending = false;
        st.key_pending = false;
        st.key_queue.clear();
        notify::reply_exit(st);
        return false;
    }
    if !st.center_on || ev.cancelled {
        return false;
    }
    // input-capture-v1 is fed after the plugin emissions: while a client
    // owns the physical stream, the shade must not cancel a key first
    if ffi::input_capture_active(state.ctx) {
        return false;
    }
    if ev.state != 1 {
        return false; // releases pass untouched
    }
    let Some((sym, ctrl, alt, logo, utf8)) = ffi::keyboard_key(state.ctx, ev.keycode, 8) else {
        return false; // no keyboard connected
    };

    // the armed field owns the keyboard first
    if notify::reply_armed(st) {
        let id = st.reply_id;
        let len0 = st.reply_text.len();
        let owned = match notify::reply_key(st, &sym, ctrl, alt, logo, &utf8) {
            Some(tx) => {
                let r = send_reply(state, st, id, &tx);
                st.pending_replies.extend(r);
                true
            }
            None => false,
        };
        // a character, a backspace, a kill, or a close: the row's layout
        // changed, so reflow
        if st.reply_id == 0 || st.reply_text.len() != len0 {
            notify::changed(state.ctx, st);
        }
        return owned;
    }

    // a modified chord is a user bind passing through, never the shade's
    if ctrl || alt || logo {
        return false;
    }

    if sym == "Escape" {
        // deferred: the close reflows and refocuses (drain_deferred)
        notify::queue_esc(state.ctx, st);
        return true;
    }
    // Tab moves into the selected card's reply field, the way Tab moves
    // into any other control — the pointer has the chip, the keyboard needs
    // a way in that is not one of the acting keys.
    if sym == "Tab" {
        let mut id = 0u32;
        let mut group = String::new();
        if !notify::center_selection(st, &mut id, &mut group) || !group.is_empty() {
            return false;
        }
        let can = st.notifs.iter().any(|n| n.id == id && n.can_reply);
        if !can {
            return false;
        }
        notify::reply_open(state.ctx, st, id);
        return true;
    }
    if sym == "Up" || sym == "Down" {
        select_move(state.ctx, st, if sym == "Down" { 1 } else { -1 });
        return true;
    }

    let verb = match sym.as_str() {
        "space" => 1,
        "Return" | "KP_Enter" => 2,
        "Delete" => 3,
        _ => return false,
    };
    // nothing selected: the shade has not taken the keyboard, so a bare
    // space still belongs to whatever holds focus
    let mut id = 0u32;
    let mut group = String::new();
    if !notify::center_selection(st, &mut id, &mut group) {
        return false;
    }
    st.key_queue.push(KeyAct { verb, id, group });
    notify::queue_key(state.ctx, st);
    true
}

// ---------------------------------------------------------------------------
// pointer ownership
// ---------------------------------------------------------------------------

/// A pointer move: hover the surface under the point, and own the pointer
/// over it (the window beneath must not see enter/leave). Hands off while
/// a button is held or a drag is live — implicit grabs and drags keep
/// flowing, as they would over a real layer-surface daemon.
pub fn on_move(state: &State, st: &mut notify::NotifyState, ev: &ffi::SafeEvent) -> bool {
    st.pointer = (ev.x, ev.y);
    if ffi::session_locked(state.ctx) || ffi::input_capture_active(state.ctx) {
        notify::set_hovered(state.ctx, st, Hover::default());
        release_pointer(state.ctx, st);
        return false;
    }

    // cheap first: almost every motion happens with nothing shown
    if st.cards.is_empty() {
        notify::set_hovered(state.ctx, st, Hover::default());
        release_pointer(state.ctx, st);
        return false;
    }

    // an earlier listener (the C++ bar's strip or an open menu) owns the
    // point — and just set the shared SPECIAL_ACTION cursor slot. Drop
    // ownership WITHOUT unsetting it: release_pointer's unset would strip
    // the bar's override for its whole visit.
    if ev.cancelled {
        notify::set_hovered(state.ctx, st, Hover::default());
        st.pointer_owned = false;
        return false;
    }

    let (x, y) = (ev.x, ev.y);
    if card_at(state.ctx, st, (x, y)).is_none()
        || st.held_buttons > 0
        || ffi::native_pointer_grab(state.ctx)
        || ffi::native_layer_at(state.ctx)
        || ffi::drag_target(state.ctx).is_some()
    {
        notify::set_hovered(state.ctx, st, Hover::default());
        release_pointer(state.ctx, st);
        return false;
    }
    let Some(card) = card_at(state.ctx, st, (x, y)) else {
        return false;
    };
    let btn = button_at(card, (x, y));
    // a hyperlink shows the hand (GTK convention)
    let onlink = btn < 0 && part_at(card, (x, y)) == 0 && link_at(card, (x, y));
    // the hover copies the card's identity; the borrow must not outlive it
    let h = Hover {
        kind: card.kind,
        id: card.id,
        group: card.group.clone(),
        btn,
        part: if btn >= 0 { 0 } else { part_at(card, (x, y)) },
    };
    notify::set_hovered(state.ctx, st, h);
    let entering = !st.pointer_owned;
    if entering {
        st.pointer_owned = true;
        // the app under the surface gets its leave
        ffi::pointer_focus_reset(state.ctx);
    }
    // set the shape on entry, and re-set only when it flips (a still stream
    // of motion must not re-assert the override every event)
    if entering || st.cursor_hand != onlink {
        ffi::cursor_override(
            state.ctx,
            Some(if onlink { "pointer" } else { "left_ptr" }),
            true,
        );
        st.cursor_hand = onlink;
    }
    true
}

/// Leave the surface: unset the override (the cursor reverts with it).
fn release_pointer(ctx: ffi::Ctx, st: &mut notify::NotifyState) {
    if !st.pointer_owned {
        return;
    }
    st.pointer_owned = false;
    st.cursor_hand = false;
    ffi::cursor_override(ctx, None, false);
}

/// A surface can vanish under a motionless pointer (expiry, a dismissal,
/// the center closing): without this the cursor override lingers and the
/// window beneath keeps NO pointer focus until the next motion — dead hover
/// UI. A real layer-surface daemon's unmap triggers the compositor's own
/// refocus; match it. Runs from the warm, never an input emission.
pub fn refresh_ownership(ctx: ffi::Ctx, st: &mut notify::NotifyState) {
    let (x, y) = ffi::mouse_coords(ctx);
    // a reflow can slide another surface under the still pointer: rebuild
    // the hover from the point (its identity is copied out before the
    // borrow of the cards ends)
    let card = card_at(ctx, st, (x, y));
    let had_card = card.is_some();
    let h = match card {
        Some(c) => {
            let btn = button_at(c, (x, y));
            Hover {
                kind: c.kind,
                id: c.id,
                group: c.group.clone(),
                btn,
                part: if btn >= 0 { 0 } else { part_at(c, (x, y)) },
            }
        }
        None => Hover::default(),
    };
    notify::set_hovered(ctx, st, h);
    if !st.pointer_owned || had_card {
        return;
    }
    release_pointer(ctx, st);
    // the window beneath gets its enter back
    ffi::mouse_simulate_move(ctx);
}

/// Teardown: reset every half-tracked input state (crash classes 3/6/7).
pub fn input_exit(state: &State, st: &mut notify::NotifyState) {
    st.esc_pending = false;
    st.hit_pending = false;
    st.key_pending = false;
    st.hit_queue.clear();
    st.key_queue.clear();
    st.swallow_release = 0;
    st.held_buttons = 0;
    st.scroll_acc = 0.0;
    st.swipe_acc = 0.0;
    st.swipe_on = 0;
    release_pointer(state.ctx, st);
}
