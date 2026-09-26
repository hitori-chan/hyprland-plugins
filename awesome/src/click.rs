//! safe `hyprclick` port (Phase 2). awesome's click and focus-raise policy:
//!
//! 1. Click-to-raise: a plain left click (or a Super+right grab) brings the
//!    clicked window to the top; clicking a fullscreen/maximized window tucks
//!    the floaters back behind it. Focus itself is native — the compositor
//!    raises the pointer-focus window on every press; the raise here resolves
//!    a fresh hit test and runs after it, so a stale pointer focus gets
//!    corrected, not compounded.
//! 2. Keyboard focus raises, hover focus doesn't (awesome's rule): only the
//!    keybind / dispatch / switch-to-window focus reasons raise.
//! 3. `focus_prev_here` — the previously focused window on the CURRENT
//!    workspace (Mod+Tab).
//! 4. `focus_next` / `focus_prev` — cycle the workspace's windows in ARRIVAL
//!    order (Mod+J/K); the z-order is useless here because rule 2 rotates it.
//! 5. A click aimed at a window that died under it never retargets: the tail
//!    of a fast double-click on a click-to-close surface is swallowed.
//!
//! No `unsafe` — only the safe `ffi` wrappers. Runs after `max` in the
//! dispatch (a Super-grab swallowed by max is never a raise click).

use std::collections::HashMap;
use std::time::{Duration, Instant};

use crate::ffi;

// the linux BTN_* codes the seat + the virtual pointer both deliver.
const BTN_LEFT: u32 = 272;
const BTN_RIGHT: u32 = 274;

// the focus reasons that raise (everything else — a sloppy FFM, a new-window
// grab — never does; awesome's "keyboard focus raises, hover focus doesn't").
const FOCUS_REASON_KEYBIND: u32 = 1 << 1;
const FOCUS_REASON_DISPATCH_FOCUSWINDOW: u32 = 1 << 2;
const FOCUS_REASON_SWITCH_TO_WINDOW_SOFT: u32 = 1 << 8;
const FOCUS_REASON_SWITCH_TO_WINDOW_HARD: u32 = 1 << 9;
/// the focus jobs use the hard switch reason (it also raises).
const FOCUS_HARD: u32 = FOCUS_REASON_SWITCH_TO_WINDOW_HARD;

// Corpse-guard gesture timing: a close this soon after a press on the window
// reads as click-to-close, and a re-press this soon after that close is the
// tail of the same double-click gesture (Qt and GTK both default the
// double-click interval to 400ms).
const CLICK_KILL: Duration = Duration::from_millis(500);
const GESTURE: Duration = Duration::from_millis(400);

/// A screen box (the corpse guard's guarded area).
#[derive(Clone, Copy, Default)]
struct Box4 {
    x: f64,
    y: f64,
    w: f64,
    h: f64,
}

impl Box4 {
    fn contains(&self, px: f64, py: f64) -> bool {
        px >= self.x && px < self.x + self.w && py >= self.y && py < self.y + self.h
    }
}

/// A queued raise (the window + whether only a fullscreen raise is wanted —
/// the press path needs the allowed-over cleanup, a focus raise doesn't).
struct RaiseJob {
    w: ffi::WindowHandle,
    fullscreen_only: bool,
}

pub struct ClickState {
    // queue + one-shot drain: two raises/focuses can arm in one dispatch
    // (a press plus the focus it caused, a scripted bind pair), so queue
    // rather than overwrite a lone deferred job.
    raise_jobs: Vec<RaiseJob>,
    focus_jobs: Vec<ffi::WindowHandle>,
    raise_queued: bool,
    focus_queued: bool,
    // arrival order (window address -> sequence), minted at map.
    arrival: HashMap<u64, u64>,
    next_seq: u64,
    // the corpse guard.
    press_window: Option<ffi::WindowHandle>,
    press_at: Option<Instant>,
    swallow_release: u32,
    corpse_box: Box4,
    corpse_owner: Option<ffi::WindowHandle>,
    corpse_ws: u32,
    corpse_until: Option<Instant>,
}

impl ClickState {
    pub fn new() -> Self {
        Self {
            raise_jobs: Vec::new(),
            focus_jobs: Vec::new(),
            raise_queued: false,
            focus_queued: false,
            arrival: HashMap::new(),
            next_seq: 0,
            press_window: None,
            press_at: None,
            swallow_release: 0,
            corpse_box: Box4::default(),
            corpse_owner: None,
            corpse_ws: 0,
            corpse_until: None,
        }
    }

    /// Reset every half-tracked gesture state (session lock, unload). A
    /// swallow mask or corpse guard must never survive into a lock.
    pub fn reset(&mut self) {
        self.raise_jobs.clear();
        self.focus_jobs.clear();
        self.raise_queued = false;
        self.focus_queued = false;
        self.arrival.clear();
        self.next_seq = 0;
        self.press_window = None;
        self.press_at = None;
        self.swallow_release = 0;
        self.corpse_box = Box4::default();
        self.corpse_owner = None;
        self.corpse_ws = 0;
        self.corpse_until = None;
    }

    // ---- arrival order --------------------------------------------------
    /// Mint the window's arrival number on first sight (call from open).
    fn seq_of(&mut self, id: u64) -> u64 {
        if let Some(s) = self.arrival.get(&id).copied() {
            return s;
        }
        let s = self.next_seq;
        self.next_seq += 1;
        self.arrival.insert(id, s);
        s
    }

    fn forget(&mut self, id: u64) {
        self.arrival.remove(&id);
    }
}

// ---- focus helpers -------------------------------------------------------

fn focus_reason_raises(reason: u32) -> bool {
    reason == FOCUS_REASON_KEYBIND
        || reason == FOCUS_REASON_DISPATCH_FOCUSWINDOW
        || reason == FOCUS_REASON_SWITCH_TO_WINDOW_SOFT
        || reason == FOCUS_REASON_SWITCH_TO_WINDOW_HARD
}

/// The workspace's windows in arrival order (mapped + visible). Each handle
/// is a fresh ref the caller owns.
fn arrival_order(ctx: ffi::Ctx, c: &mut ClickState, ws_id: u32) -> Vec<ffi::WindowHandle> {
    let mut out: Vec<(u64, ffi::WindowHandle)> = Vec::new();
    for w in ffi::all_windows(ctx) {
        let id = ffi::window_id(ctx, &w);
        if id == 0 {
            continue;
        }
        let Some(info) = ffi::window_info(ctx, &w) else {
            continue;
        };
        if !info.visible {
            continue;
        }
        let Some(wws) = ffi::window_workspace(ctx, &w) else {
            continue;
        };
        if ffi::workspace_id(ctx, &wws) != ws_id {
            continue;
        }
        c.seq_of(id);
        out.push((id, w));
    }
    // sort by the arrival sequence (the comparator is a pure read — every
    // sequence was minted above, in visit order).
    out.sort_by_key(|(id, _)| c.arrival.get(id).copied().unwrap_or(u64::MAX));
    out.into_iter().map(|(_, w)| w).collect()
}

/// `hl.plugin.hyprclick.focus_prev_here` — the most recently focused OTHER
/// window on the current workspace, focused + raised.
pub fn lua_focus_prev_here(ctx: ffi::Ctx, c: &mut ClickState) {
    let Some(focus_w) = ffi::focus_window(ctx) else {
        return;
    };
    let focus_id = ffi::window_id(ctx, &focus_w);
    let Some(ws_num) = current_workspace_number(ctx) else {
        return;
    };
    // the history is old -> new; the previous window is at the BACK.
    let hist = ffi::focus_history(ctx);
    for h in hist.iter().rev() {
        let hid = ffi::window_id(ctx, h);
        if hid == 0 || hid == focus_id {
            continue;
        }
        let Some(info) = ffi::window_info(ctx, h) else {
            continue;
        };
        if !info.visible {
            continue;
        }
        let Some(hws) = ffi::window_workspace(ctx, h) else {
            continue;
        };
        if ffi::workspace_number(ctx, &hws) != ws_num {
            continue;
        }
        queue_focus(c, ctx, h.clone_handle());
        break;
    }
}

/// The focused monitor's active (numbered) workspace number, or None.
fn current_workspace_number(ctx: ffi::Ctx) -> Option<u32> {
    let mon = ffi::focus_monitor(ctx)?;
    let ws = ffi::monitor_workspace(ctx, &mon)?;
    Some(ffi::workspace_number(ctx, &ws))
}

/// `hl.plugin.hyprclick.focus_next` / `focus_prev` — cycle the workspace's
/// windows in arrival order, wrapping. When the focus sits off the list
/// (another monitor, a minimized window, or nowhere) the walk starts at the
/// head rather than stepping off it.
fn focus_by_idx(ctx: ffi::Ctx, c: &mut ClickState, next: bool) {
    let Some(ws_num) = current_workspace_number(ctx) else {
        return;
    };
    let list = arrival_order(ctx, c, ws_num);
    let from_id = ffi::focus_window(ctx).map_or(0, |w| ffi::window_id(ctx, &w));
    let n = list.len();
    if n == 0 {
        return;
    }
    let mut idx: Option<usize> = None;
    for (i, w) in list.iter().enumerate() {
        if ffi::window_id(ctx, w) == from_id {
            idx = Some(i);
            break;
        }
    }
    let target = match idx {
        Some(i) => {
            let next_i = if next {
                (i + 1) % n
            } else if i == 0 {
                n - 1
            } else {
                i - 1
            };
            list[next_i].clone_handle()
        }
        None => list[0].clone_handle(),
    };
    queue_focus(c, ctx, target);
}

pub fn lua_focus_next(ctx: ffi::Ctx, c: &mut ClickState) {
    focus_by_idx(ctx, c, true);
}
pub fn lua_focus_prev(ctx: ffi::Ctx, c: &mut ClickState) {
    focus_by_idx(ctx, c, false);
}

// ---- Lua entry points (safe bodies; the extern "C" wrappers live in ffi) ----

// The impls run on the event-loop thread, catch a panic (a panic can never
// unwind across the C boundary), and return 0 (a lua function's success).
pub fn lua_focus_prev_here_impl() -> i32 {
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if let Some(state) = crate::probe::state() {
            let ctx = state.ctx;
            let mut c = crate::probe::click_lock(state);
            lua_focus_prev_here(ctx, &mut c);
            // the focus job is armed inside; drain it off the emission
            crate::probe::arm_job(ctx, crate::probe::JOB_CLICK);
        }
    }));
    0
}
pub fn lua_focus_next_impl() -> i32 {
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if let Some(state) = crate::probe::state() {
            let ctx = state.ctx;
            let mut c = crate::probe::click_lock(state);
            lua_focus_next(ctx, &mut c);
            crate::probe::arm_job(ctx, crate::probe::JOB_CLICK);
        }
    }));
    0
}
pub fn lua_focus_prev_impl() -> i32 {
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if let Some(state) = crate::probe::state() {
            let ctx = state.ctx;
            let mut c = crate::probe::click_lock(state);
            lua_focus_prev(ctx, &mut c);
            crate::probe::arm_job(ctx, crate::probe::JOB_CLICK);
        }
    }));
    0
}

// ---- queue + drain -------------------------------------------------------

fn queue_raise(c: &mut ClickState, ctx: ffi::Ctx, w: &ffi::WindowHandle, fullscreen_only: bool) {
    if c.raise_jobs.len() < 16 {
        c.raise_jobs.push(RaiseJob {
            w: w.clone_handle(),
            fullscreen_only,
        });
    }
    if c.raise_queued {
        return;
    }
    c.raise_queued = true;
    crate::probe::arm_job(ctx, crate::probe::JOB_CLICK);
}

fn queue_focus(c: &mut ClickState, ctx: ffi::Ctx, w: ffi::WindowHandle) {
    if c.focus_jobs.len() < 16 {
        c.focus_jobs.push(w);
    }
    if c.focus_queued {
        return;
    }
    c.focus_queued = true;
    crate::probe::arm_job(ctx, crate::probe::JOB_CLICK);
}

/// The one-shot drain: raises, then focuses, off the input emission. The
/// session lock can engage between the arm and this run — drop the queues.
pub fn drain(ctx: ffi::Ctx, c: &mut ClickState) {
    c.raise_queued = false;
    c.focus_queued = false;
    if ffi::session_locked(ctx) {
        c.raise_jobs.clear();
        c.focus_jobs.clear();
        return;
    }
    let raises = std::mem::take(&mut c.raise_jobs);
    for j in raises {
        let w = j.w;
        if ffi::window_id(ctx, &w) == 0 {
            continue;
        }
        let Some(info) = ffi::window_info(ctx, &w) else {
            continue;
        };
        if !info.visible {
            continue;
        }
        if j.fullscreen_only && !ffi::window_is_fullscreen(ctx, &w) {
            continue;
        }
        raise_window(ctx, &w);
    }
    let focuses = std::mem::take(&mut c.focus_jobs);
    for w in focuses {
        if ffi::window_id(ctx, &w) == 0 {
            continue;
        }
        let Some(info) = ffi::window_info(ctx, &w) else {
            continue;
        };
        if !info.visible {
            continue;
        }
        ffi::focus_window_set(ctx, &w, FOCUS_HARD);
    }
}

/// Raising a fullscreen/maximized window = tucking the floaters back behind
/// it (clear the allowed-over flag — never `lower()`, which would bury the
/// opener). An ordinary floating window is raised natively by the press path;
/// a focus raise raises it here.
fn raise_window(ctx: ffi::Ctx, w: &ffi::WindowHandle) {
    if ffi::window_is_fullscreen(ctx, w) {
        ffi::clear_allowed_over(ctx, w);
    } else if let Some(info) = ffi::window_info(ctx, w)
        && info.floating
    {
        ffi::window_raise(ctx, w);
    }
}

// ---- events --------------------------------------------------------------

/// The mouse button handler. Returns true to CANCEL (the corpse guard or a
/// swallowed release). Runs after `max` in the dispatch (a Super-grab max
/// swallowed is never a raise click).
pub fn on_button(ctx: ffi::Ctx, c: &mut ClickState, ev: &ffi::SafeEvent) -> bool {
    // emissions precede the compositor's lock handling: reset the half-tracked
    // swallow + corpse state there (it must not survive into a lock/capture).
    if ffi::session_locked(ctx) || ffi::input_capture_active(ctx) {
        c.swallow_release = 0;
        c.corpse_until = None;
        c.corpse_owner = None;
        c.press_window = None;
        c.press_at = None;
        return false;
    }

    let bit = ffi::tracked_button_bit(ev.button);
    let now = Instant::now();

    if ev.state == 0
    /* RELEASED */
    {
        // the release of a press swallowed below: swallow it too, or the
        // window under the cursor gets a release it never saw pressed.
        if bit != 0 && (c.swallow_release & bit) != 0 {
            c.swallow_release &= !bit;
            return true;
        }
        return false;
    }

    // a press. Buttons outside the shell's policy pass through untouched.
    if bit == 0 {
        return false;
    }

    // the pointer is on a layer surface (a bar) or natively grabbed: that
    // click is the layer's / the grab's, never a raise through to the window
    // underneath. (max already ran and cancelled its own Super-grabs.)
    if ffi::native_pointer_grab(ctx) || ffi::native_layer_at(ctx) {
        return false;
    }

    let w = ffi::window_under_cursor(ctx);

    // The corpse guard: a press inside the box of the window the previous
    // press just killed is the tail of the same gesture. Cancelling here
    // stops focus, raise and delivery at once (this emission precedes all
    // compositor handling). Each swallowed press extends the guard; a press
    // resolving to the corpse's still-living owner passes (a click ON the
    // window, not through where it used to be); the claim ends off the
    // corpse's workspace.
    let on_corpse_ws = |w: &Option<ffi::WindowHandle>| -> bool {
        if let Some(w) = w {
            let Some(ws) = ffi::window_workspace(ctx, w) else {
                return false;
            };
            ffi::workspace_number(ctx, &ws) == c.corpse_ws
        } else {
            let Some(mon) = ffi::monitor_at(ctx, ev.x, ev.y) else {
                return false;
            };
            let Some(ws) = ffi::monitor_workspace(ctx, &mon) else {
                return false;
            };
            ffi::workspace_number(ctx, &ws) == c.corpse_ws
        }
    };
    if let (Some(until), true) = (c.corpse_until, c.corpse_box.contains(ev.x, ev.y)) {
        let owner_matches = match &w {
            Some(w) => match &c.corpse_owner {
                Some(o) => ffi::window_id(ctx, w) == ffi::window_id(ctx, o),
                None => false,
            },
            None => false,
        };
        let ws_ok = on_corpse_ws(&w);
        if until > now && !owner_matches && ws_ok {
            // swallow the press + its release; extend the guard (a burst is
            // one gesture).
            c.swallow_release |= bit;
            c.corpse_until = Some(now + GESTURE);
            return true;
        }
    }

    // awesome's click-to-raise: a plain left, or a Super+right grab.
    if ev.button != BTN_LEFT && !(ev.button == BTN_RIGHT && ffi::super_held(ctx)) {
        return false;
    }

    // Only a press that can invoke the raise policy arms corpse protection on
    // the pressed window's close/fullscreen event.
    if let Some(w) = &w {
        c.press_window = Some(w.clone_handle());
    }
    c.press_at = Some(now);

    // Fullscreen needs the allowed-over cleanup; an ordinary floating window
    // is raised natively by the press path, so schedule a second raise only
    // for the fullscreen case.
    if let Some(w) = &w {
        queue_raise(c, ctx, w, true);
    }
    false
}

/// Keyboard focus raises, hover focus doesn't. Deferred out of the focus
/// emission (rawWindowFocus is still running when this fires).
pub fn on_window_active(ctx: ffi::Ctx, c: &mut ClickState, ev: &ffi::SafeEvent) {
    if !focus_reason_raises(ev.focus_reason) {
        return;
    }
    if let Some(w) = &ev.window {
        queue_raise(c, ctx, w, false);
    }
}

/// Stamp the arrival number at map (not at first cycle).
pub fn on_open(ctx: ffi::Ctx, c: &mut ClickState, w: &ffi::WindowHandle) {
    let id = ffi::window_id(ctx, w);
    if id != 0 {
        c.seq_of(id);
    }
}

/// Forget the arrival number on destroy (before the address can be reused).
pub fn on_destroy(ctx: ffi::Ctx, c: &mut ClickState, w: &ffi::WindowHandle) {
    let id = ffi::window_id(ctx, w);
    if id != 0 {
        c.forget(id);
    }
}

/// Arm (or grow) the corpse over `box` if the press on `w` was recent enough
/// to have caused its state change. A second arming inside a live gesture
/// unions instead of shrinking the guarded area.
fn arm_corpse(ctx: ffi::Ctx, c: &mut ClickState, w: &ffi::WindowHandle, box_: Box4) {
    let now = Instant::now();
    let recent = c
        .press_at
        .is_some_and(|t| now.duration_since(t) <= CLICK_KILL);
    if !recent {
        return;
    }
    if let Some(until) = c.corpse_until {
        if until > now {
            let x = c.corpse_box.x.min(box_.x);
            let y = c.corpse_box.y.min(box_.y);
            c.corpse_box = Box4 {
                x,
                y,
                w: c.corpse_box.x + c.corpse_box.w.max(box_.x + box_.w) - x,
                h: c.corpse_box.y + c.corpse_box.h.max(box_.y + box_.h) - y,
            };
        } else {
            c.corpse_box = box_;
        }
    } else {
        c.corpse_box = box_;
    }
    c.corpse_owner = Some(w.clone_handle());
    c.corpse_ws = w_workspace_number(ctx, w);
    c.corpse_until = Some(now + GESTURE);
}

fn w_workspace_number(ctx: ffi::Ctx, w: &ffi::WindowHandle) -> u32 {
    ffi::window_workspace(ctx, w).map_or(0, |ws| ffi::workspace_number(ctx, &ws))
}

/// The pressed window died right after the press: click-to-close. Arm the
/// corpse guard over where the user last saw it (a fullscreen viewer's corpse
/// is the whole screen — this fires before the unmap resets fullscreen state).
pub fn on_close(ctx: ffi::Ctx, c: &mut ClickState, ev: &ffi::SafeEvent) {
    let Some(w) = &ev.window else {
        return;
    };
    let is_press = match &c.press_window {
        Some(p) => ffi::window_id(ctx, p) == ffi::window_id(ctx, w),
        None => false,
    };
    if !is_press {
        return;
    }
    // reset only the window (the C++ keeps g_pressAt so arm_corpse's recency
    // check still sees the press that caused this close).
    c.press_window = None;
    if let Some(p) = ffi::window_place(ctx, w) {
        let b = Box4 {
            x: p.box_.x,
            y: p.box_.y,
            w: p.box_.w,
            h: p.box_.h,
        };
        arm_corpse(ctx, c, w, b);
    }
}

/// The pressed window leaving fullscreen right after the press is the
/// click-to-close family too: some viewers exit fullscreen before unmapping,
/// and in that gap the restored box no longer covers what the screen still
/// shows. Guard the monitor box it vacated; the owner lives, so presses
/// resolving to it still pass.
pub fn on_fullscreen(ctx: ffi::Ctx, c: &mut ClickState, ev: &ffi::SafeEvent) {
    let Some(w) = &ev.window else {
        return;
    };
    let is_press = match &c.press_window {
        Some(p) => ffi::window_id(ctx, p) == ffi::window_id(ctx, w),
        None => false,
    };
    if !is_press || ffi::window_is_fullscreen(ctx, w) {
        return;
    }
    if let Some(mon) = ffi::window_monitor(ctx, w)
        && let Some(b) = ffi::monitor_logical_box(ctx, &mon)
    {
        let box_ = Box4 {
            x: b.x,
            y: b.y,
            w: b.w,
            h: b.h,
        };
        arm_corpse(ctx, c, w, box_);
    }
}
