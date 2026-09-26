// hyprpad — the old awesome touchpad module, ported to the cabi.
//
// The touchpad turns off while an external (USB/Bluetooth) mouse is present
// and back on when it's unplugged; the manual toggle (hl.plugin.hyprpad.toggle)
// flips it by hand. Fully in-process — no udev, no forks:
//
// - Hotplug rides the compositor's own device signal (HL_EV_POINTER_CHANGED:
//   a pointer added or removed). The device list is stale mid-signal, so the
//   handler only (re)arms a settle timer, which also coalesces one plug's
//   burst into a single re-check.
// - External mouse = a non-virtual, non-touchpad pointer whose libinput bus
//   type is USB or Bluetooth. The touchpad is the m_isTouchpad entry (the
//   compositor's own capability predicate — a pointer with a size), not a
//   name substring; its m_hlName is what hl.device keys on.
// - The flip is hl_run_lua("hl.device({...})") — the code `hyprctl eval`
//   reaches, minus the fork + socket round-trip. It writes the compositor's
//   per-device config store, so nothing fights the next config re-apply. A
//   reload wipes that runtime state: config.reloaded forgets appliedState.
// - Auto re-checks are change-detected against the last applied state: an
//   unrelated hotplug re-checks but applies nothing.
//
// The feedback cards (async D-Bus Notify on the session bus) are deferred to
// the bus-thread subsystem that hyprosd/hyprnotify also need; the flip works
// without them.

use crate::ffi;
use crate::probe::{self, State};

// BUS_* from linux/input.h (the libinput bus type).
const BUS_USB: u32 = 0x03;
const BUS_BLUETOOTH: u32 = 0x05;

// The settle time (coalesces a hotplug burst into a single re-check).
const SETTLE_MS: u32 = 400;

pub struct PadState {
    // The last state this plugin applied (-1 = none, 0 = off, 1 = on) + the
    // touchpad address it applied to. A replacement must receive policy even
    // when the desired boolean matches.
    applied_state: i32,
    applied_touchpad: u64,
    // The settle timer's job token (0 = not armed).
    settle_job: u64,
    // The toggle queue (one entry per flip) + the queued flag.
    toggle_queue: Vec<i32>,
    toggle_queued: bool,
}

impl PadState {
    pub fn new() -> Self {
        Self {
            applied_state: -1,
            applied_touchpad: 0,
            settle_job: 0,
            toggle_queue: Vec::new(),
            toggle_queued: false,
        }
    }
}

// ---------------------------------------------------------------------------
// the device side
// ---------------------------------------------------------------------------

// escape for a double-quoted Lua string literal
fn luaq(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars() {
        if c == '\\' || c == '"' {
            out.push('\\');
        }
        out.push(c);
    }
    out
}

// The touchpad pointer (the m_isTouchpad entry). Returns the handle + its
// stable address (the identity applied-touchpad is tracked by).
fn touchpad(ctx: ffi::Ctx) -> Option<(ffi::PointerHandle, u64)> {
    for p in ffi::pointers(ctx) {
        if ffi::pointer_is_touchpad(ctx, &p) {
            let id = ffi::pointer_id(ctx, &p);
            return Some((p, id));
        }
    }
    None
}

// The touchpad's live enabled state (m_connected). It is updated together with
// libinput's send-events mode, so a manual toggle can read the live state even
// when no automatic pass has populated appliedState yet.
fn touchpad_enabled(ctx: ffi::Ctx) -> Option<bool> {
    for p in ffi::pointers(ctx) {
        if ffi::pointer_is_touchpad(ctx, &p) {
            return Some(ffi::pointer_connected(ctx, &p));
        }
    }
    None
}

fn external_mouse_present(ctx: ffi::Ctx) -> bool {
    for p in ffi::pointers(ctx) {
        if ffi::pointer_is_virtual(ctx, &p) || ffi::pointer_is_touchpad(ctx, &p) {
            continue;
        }
        let bus = ffi::pointer_bus_type(ctx, &p);
        if bus == BUS_USB || bus == BUS_BLUETOOTH {
            return true;
        }
    }
    false
}

fn apply_enabled(
    ctx: ffi::Ctx,
    st: &mut PadState,
    on: bool,
    target: Option<&ffi::PointerHandle>,
) {
    let (tp, id) = match target {
        Some(tp) => (tp.clone_handle(), ffi::pointer_id(ctx, tp)),
        None => match touchpad(ctx) {
            Some(t) => t,
            None => {
                st.applied_state = -1;
                st.applied_touchpad = 0;
                return;
            }
        },
    };
    let name = ffi::pointer_name(ctx, &tp);
    let code = format!("hl.device({{ name = \"{}\", enabled = {} }})", luaq(&name), on);
    if !ffi::run_lua(ctx, &code) {
        return; // appliedState untouched: the next check retries
    }
    st.applied_state = i32::from(on);
    st.applied_touchpad = id;
}

fn auto_apply(ctx: ffi::Ctx, st: &mut PadState) {
    let Some((tp, id)) = touchpad(ctx) else {
        st.applied_state = -1;
        st.applied_touchpad = 0;
        return; // nothing to auto-manage
    };
    let want = i32::from(!external_mouse_present(ctx));
    if want != st.applied_state || st.applied_touchpad != id {
        apply_enabled(ctx, st, want == 1, Some(&tp));
    }
}

// ---------------------------------------------------------------------------
// the settle timer (re-armed on hotplug + config reload; coalesces bursts)
// ---------------------------------------------------------------------------

/// (Re)arm the settle timer: cancel the previous + arm a new one (extend the
/// timeout so a hotplug burst coalesces into a single re-check).
fn arm_settle(state: &State, st: &mut PadState) {
    let ctx = state.ctx;
    ffi::job_cancel(ctx, st.settle_job);
    st.settle_job = probe::arm_timer(ctx, SETTLE_MS, probe::JOB_PAD_SETTLE);
}

/// Fired by JOB_PAD_SETTLE: the settle timer. Re-check the devices (the
/// re-check is change-detected; an unrelated hotplug applies nothing).
pub fn drain_settle(state: &State, st: &mut PadState) {
    st.settle_job = 0;
    auto_apply(state.ctx, st);
}

// ---------------------------------------------------------------------------
// the manual toggle (hl.plugin.hyprpad.toggle)
// ---------------------------------------------------------------------------

/// Queue a flip (queue+drain, never a lone defer: two flips can arm in one
/// dispatch and overwriting the lock cancels the unfired one; only the parity
/// survives the drain, an even batch nets to no change).
fn queue_toggle(state: &State, st: &mut PadState) {
    if st.toggle_queue.len() < 16 {
        st.toggle_queue.push(0);
    }
    if st.toggle_queued {
        return;
    }
    st.toggle_queued = true;
    probe::arm_job(state.ctx, probe::JOB_PAD_TOGGLE);
}

/// Fired by JOB_PAD_TOGGLE: drain the toggle queue.
pub fn drain_toggles(state: &State, st: &mut PadState) {
    let n = st.toggle_queue.len();
    st.toggle_queue.clear();
    st.toggle_queued = false;
    if n % 2 == 0 {
        return;
    }
    // the manual flip cancels a pending auto re-check so it isn't overridden
    // a beat later
    ffi::job_cancel(state.ctx, st.settle_job);
    st.settle_job = 0;
    let ctx = state.ctx;
    let current = touchpad_enabled(ctx).unwrap_or(st.applied_state == 1);
    apply_enabled(ctx, st, !current, None);
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

/// Init (called during plugin init): arm the initial settle timer (by the time
/// it fires the device list is populated).
pub fn init(state: &State) {
    let mut st = probe::pad_lock(state);
    arm_settle(state, &mut st);
}

/// A pointer was added or removed (HL_EV_POINTER_CHANGED): arm the settle timer.
pub fn on_pointer_changed(state: &State, st: &mut PadState) {
    arm_settle(state, st);
}

/// The config reloaded: the runtime hl.device state is wiped, so forget what
/// was applied + re-check.
pub fn on_config_reloaded(state: &State, st: &mut PadState) {
    st.applied_state = -1;
    st.applied_touchpad = 0;
    arm_settle(state, st);
}

/// The safe body for the `hl.plugin.hyprpad.toggle` Lua function.
pub fn lua_toggle_impl() -> i32 {
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if let Some(state) = probe::state() {
            let mut st = probe::pad_lock(state);
            queue_toggle(state, &mut st);
        }
    }));
    0
}

/// Teardown: cancel the pending settle timer (a job pending at teardown is an
/// expired-no-op, but cancel it explicitly so the log is clean).
pub fn exit(state: &State) {
    let mut st = probe::pad_lock(state);
    ffi::job_cancel(state.ctx, st.settle_job);
    st.settle_job = 0;
}
