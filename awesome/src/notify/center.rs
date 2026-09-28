// The shade state machine (center.cpp): open/peek/pin/page/select.

use super::*;

pub fn center_pin(ctx: ffi::Ctx, st: &mut NotifyState, replies: &mut Vec<crate::bus::BusReply>) {
    if !st.peek {
        return;
    }
    st.peek = false;
    st.peek_bell = false;
    disarm_peek_out(ctx, st);
    absorb_popped(ctx, st, replies);
    changed_st(ctx, st, replies);
}

pub fn center_peek(
    ctx: ffi::Ctx,
    st: &mut NotifyState,
    on_bell: bool,
    replies: &mut Vec<crate::bus::BusReply>,
) {
    st.peek_bell = on_bell;
    if on_bell {
        disarm_peek_out(ctx, st);
        if st.center_on {
            return;
        }
        st.peek = true;
        set_center(ctx, st, true, false, replies);
        return;
    }
    if st.peek {
        let over = crate::ninput::pointer_over_cards(st);
        arm_peek_out(ctx, st, !over);
    }
}

pub fn center_page(st: &mut NotifyState, dir: i32) {
    if st.items <= 1 {
        return;
    }
    st.skip = ((st.skip as i32) + dir).clamp(0, (st.items - 1) as i32) as usize;
    // a local change: re-warm (the budget re-fits the page)
    let _ = st.skip;
}

pub fn center_toggle_row(st: &mut NotifyState, id: u32) {
    let open = st.row_state.get(&id).copied().unwrap_or(false);
    if open {
        st.opened_rows.remove(&id);
        st.folded_rows.insert(id);
    } else {
        st.folded_rows.remove(&id);
        st.opened_rows.insert(id);
    }
}

pub fn center_toggle_group(st: &mut NotifyState, app_key: &str) {
    let open = st.group_state.get(app_key).copied().unwrap_or(false);
    if open {
        st.opened_groups.remove(app_key);
        st.folded_groups.insert(app_key.to_owned());
    } else {
        st.folded_groups.remove(app_key);
        st.opened_groups.insert(app_key.to_owned());
    }
}

pub fn center_select_move(st: &mut NotifyState, dir: i32) {
    if st.disp.is_empty() {
        st.sel = -1;
        return;
    }
    let last = st.disp.len() as i32 - 1;
    if st.sel < 0 {
        st.sel = if dir > 0 {
            (st.skip as i32).min(last)
        } else {
            (st.last_vis as i32).min(last)
        };
    } else {
        st.sel = (st.sel + dir).clamp(0, last);
    }
    if st.sel < st.skip as i32 {
        st.skip = st.sel as usize;
    } else if st.sel > st.last_vis as i32 {
        st.skip += (st.sel - st.last_vis as i32) as usize;
    }
}

pub fn center_selection(st: &NotifyState, id: &mut u32, group: &mut String) -> bool {
    if st.sel < 0 || st.sel as usize >= st.disp.len() {
        return false;
    }
    let d = &st.disp[st.sel as usize];
    let first = d.items[0];
    *group = if d.items.len() > 1 {
        d.key.clone()
    } else {
        String::new()
    };
    *id = st.notifs[first].id;
    true
}

/// Everything one visit accumulates; the close resets it (Android parity).
fn reset_visit(st: &mut NotifyState) {
    st.skip = 0;
    st.items = 0;
    st.opened_rows.clear();
    st.folded_rows.clear();
    st.opened_groups.clear();
    st.folded_groups.clear();
    st.row_state.clear();
    st.group_state.clear();
    st.animating = false;
    st.sel = -1;
    st.last_vis = 0;
    st.peek = false;
    st.peek_bell = false;
    st.disp.clear();
    st.item_h.clear();
    st.item_open.clear();
    st.item_more.clear();
    st.child_h.clear();
}

/// Open/close the shade. `repop` (an explicit close) returns the absorbed
/// stack to banners; an action that closes on its way out does not.
pub fn set_center(
    ctx: ffi::Ctx,
    st: &mut NotifyState,
    on: bool,
    repop: bool,
    replies: &mut Vec<crate::bus::BusReply>,
) {
    if on == st.center_on {
        return;
    }
    st.center_on = on;
    if !on {
        reset_visit(st);
        reply_exit(st);
        if repop {
            repop_absorbed(ctx, st, replies);
        }
    } else {
        if !st.peek {
            absorb_popped(ctx, st, replies);
        }
        st.opened_at = ffi::steady_ms();
        st.animating = true;
    }
    changed_st(ctx, st, replies);
}

fn arm_peek_out(ctx: ffi::Ctx, st: &mut NotifyState, on: bool) {
    if on {
        if st.peek_out_token == 0 {
            st.peek_out_token =
                crate::probe::arm_timer(ctx, PEEK_GRACE_MS, crate::probe::JOB_N_PEEK_OUT);
        }
    } else {
        disarm_peek_out(ctx, st);
    }
}

fn disarm_peek_out(ctx: ffi::Ctx, st: &mut NotifyState) {
    if st.peek_out_token != 0 {
        ffi::job_cancel(ctx, st.peek_out_token);
        st.peek_out_token = 0;
    }
}

/// The peek's grace timer fired: close the peek unless the pointer is on the
/// bell or the panel.
pub fn peek_out(ctx: ffi::Ctx, st: &mut NotifyState, replies: &mut Vec<crate::bus::BusReply>) {
    st.peek_out_token = 0;
    if st.peek && !st.peek_bell && !crate::ninput::pointer_over_cards(st) {
        set_center(ctx, st, false, true, replies);
    }
}
