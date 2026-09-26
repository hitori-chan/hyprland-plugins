// The Phase 0 probe: safe behavior over the ffi boundary. It exercises the
// whole ABI surface — version handshake, log, events (with window queries),
// a counter, a deferred job, a repeating timer (a job always pending at
// teardown), a bus pipe (fd-watch), and one config keyword — and logs
// distinctive lines the gate's ffi battery greps for.
//
// This module is safe: every raw-pointer / libc / Box-reclaim operation is a
// safe wrapper in `ffi`.
use std::ptr;
use std::sync::Mutex;
use std::sync::atomic::{AtomicPtr, AtomicU64, Ordering};

use crate::bar;
use crate::ffi;

/// The ABI version this build targets (must equal the fork's cabiAbiVersion).
pub const CABI_ABI_VERSION: u32 = 1;

// job kinds (tagged into the ffi::JobArg that is passed as each job's `ud`)
pub const JOB_DEFER: u8 = 1;
pub const JOB_TIMER: u8 = 2;
pub const JOB_PIPE: u8 = 3;
pub const JOB_BAR_TICK: u8 = 4;

// how often a TICK logs (TICK fires every frame; keep the log readable)
const TICK_LOG_EVERY: u64 = 240;

pub struct State {
    pub ctx: ffi::Ctx,
    tick: AtomicU64,
    events: AtomicU64,
    pipe_read: libc::c_int,
    pipe_write: libc::c_int,
    // The Phase 1 mini-bar (render + textures). Behind a Mutex: the render
    // callback and the clock timer both touch it (single-threaded, no nesting).
    pub bar: Mutex<bar::BarState>,
}

static STATE: AtomicPtr<State> = AtomicPtr::new(ptr::null_mut());

/// Set up the probe: pipe, config, subscription, and jobs. Returns 0 on
/// success (init ok), non-zero to eject.
pub fn init(ctx: ffi::Ctx) -> i32 {
    let Some((read_fd, write_fd)) = ffi::pipe() else {
        ffi::log_str(ctx, ffi::LOG_ERR, "eject: pipe() failed");
        return -1;
    };

    // one config keyword (registered during init, read live)
    let Some(cfg) = ffi::config_register(
        ctx,
        "plugin:awesome:probe_tick",
        "probe: tick log throttle",
        ffi::HL_CFG_INT,
        0.0,
        "",
    ) else {
        ffi::log_str(ctx, ffi::LOG_ERR, "eject: config register failed");
        ffi::close_fd(read_fd);
        ffi::close_fd(write_fd);
        return -1;
    };
    if let Some((t, n, _)) = ffi::config_get(ctx, cfg) {
        ffi::log_str(
            ctx,
            ffi::LOG_INFO,
            &format!("config probe_tick type={t} val={n}"),
        );
    }

    let mut state = Box::new(State {
        ctx,
        tick: AtomicU64::new(0),
        events: AtomicU64::new(0),
        pipe_read: read_fd,
        pipe_write: write_fd,
        bar: Mutex::new(bar::BarState::new()),
    });
    // The pointer the (leaked) Box will live at. We use it for every job's
    // `ud` and the render callback; the Box is leaked at the end of init.
    let state_ptr: *mut State = Box::as_mut_ptr(&mut state);
    STATE.store(state_ptr, Ordering::SeqCst);

    // subscribe to the Phase 0 event set (no input is ever cancelled)
    let mask = ffi::HL_EV_TICK
        | ffi::HL_EV_KEY
        | ffi::HL_EV_MOUSE_BUTTON
        | ffi::HL_EV_WINDOW_ACTIVE
        | ffi::HL_EV_WINDOW_OPEN
        | ffi::HL_EV_WINDOW_DESTROY;
    if ffi::subscribe(ctx, mask, state_ptr.cast::<std::ffi::c_void>()) != 0 {
        ffi::log_str(ctx, ffi::LOG_ERR, "eject: subscribe failed");
        return -1;
    }

    // bus pipe: watch the read end; the deferred job writes one byte
    let pipe_arg = Box::leak(Box::new(ffi::JobArg {
        state: state_ptr,
        kind: JOB_PIPE,
    }));
    ffi::watch_fd(ctx, read_fd, pipe_arg);

    let defer_arg = Box::leak(Box::new(ffi::JobArg {
        state: state_ptr,
        kind: JOB_DEFER,
    }));
    ffi::defer(ctx, defer_arg);

    // a repeating timer keeps a job pending at teardown (expired-no-op test)
    let timer_arg = Box::leak(Box::new(ffi::JobArg {
        state: state_ptr,
        kind: JOB_TIMER,
    }));
    ffi::timer(ctx, 1000, 1, timer_arg);

    // The Phase 1 mini-bar (render + textures). Non-fatal if it can't come up
    // (e.g. no monitor): the probe's event/job/config behavior is independent.
    // Called with a safe `&State` (the Box is still owned here); it captures
    // `state_ptr` for its render callback and timer.
    if !bar::init(ctx, &state, state_ptr) {
        ffi::log_str(ctx, ffi::LOG_WARN, "bar: mini-bar did not start");
    }

    // Leak the Box now that every callback holds `state_ptr` (unchanged by
    // the leak). The pointer already lives in the STATE static; after this,
    // `state` must not be dropped.
    let _ = Box::into_raw(state);

    ffi::log_str(
        ctx,
        ffi::LOG_INFO,
        &format!("init ok (abi {CABI_ABI_VERSION})"),
    );
    0
}

/// Safe probe teardown (called before the context is shut down).
pub fn exit(state: &State) {
    ffi::log_str(state.ctx, ffi::LOG_INFO, "shutdown");
    // the fork owns the read end (closed with the ctx); close only our write end
    ffi::close_fd(state.pipe_write);
}

/// Take the live State pointer (null if none). Safe: pointer load only.
pub fn take_state() -> *mut State {
    STATE.swap(ptr::null_mut(), Ordering::SeqCst)
}

// ---------------------------------------------------------------------------
// event dispatch (safe; the ffi trampoline handed us a SafeEvent)
// ---------------------------------------------------------------------------

pub fn dispatch(state: &State, ev: &ffi::SafeEvent) {
    state.events.fetch_add(1, Ordering::Relaxed);
    match ev.kind {
        ffi::HL_EV_TICK => on_tick(state),
        ffi::HL_EV_KEY => on_key(state, ev),
        ffi::HL_EV_MOUSE_BUTTON => on_button(state, ev),
        ffi::HL_EV_WINDOW_ACTIVE => on_window(state, ev, "WINDOW_ACTIVE"),
        ffi::HL_EV_WINDOW_OPEN => on_window(state, ev, "WINDOW_OPEN"),
        ffi::HL_EV_WINDOW_DESTROY => on_window(state, ev, "WINDOW_DESTROY"),
        _ => {}
    }
}

fn on_tick(state: &State) {
    let t = state.tick.fetch_add(1, Ordering::Relaxed) + 1;
    if t == 1 || t.is_multiple_of(TICK_LOG_EVERY) {
        ffi::log_str(
            state.ctx,
            ffi::LOG_DEBUG,
            &format!(
                "event TICK #{t} (events={})",
                state.events.load(Ordering::Relaxed)
            ),
        );
    }
}

fn on_key(state: &State, ev: &ffi::SafeEvent) {
    let locked = ffi::session_locked(state.ctx);
    ffi::log_str(
        state.ctx,
        ffi::LOG_DEBUG,
        &format!(
            "event KEY code={} state={} session_locked={}",
            ev.keycode, ev.state, locked
        ),
    );
}

fn on_button(state: &State, ev: &ffi::SafeEvent) {
    ffi::log_str(
        state.ctx,
        ffi::LOG_DEBUG,
        &format!(
            "event BUTTON b={} state={} x={} y={}",
            ev.button, ev.state, ev.x, ev.y
        ),
    );
}

fn on_window(state: &State, ev: &ffi::SafeEvent, what: &str) {
    match ev
        .window
        .as_ref()
        .and_then(|w| ffi::window_info(state.ctx, w))
    {
        Some(i) => ffi::log_str(
            state.ctx,
            ffi::LOG_DEBUG,
            &format!(
                "event {what} app={} title={} floating={} pinned={}",
                i.app_id, i.title, i.floating, i.pinned
            ),
        ),
        None => ffi::log_str(
            state.ctx,
            ffi::LOG_DEBUG,
            &format!("event {what} (no window info)"),
        ),
    }
}

// ---------------------------------------------------------------------------
// jobs (safe; the ffi trampoline decoded the JobArg)
// ---------------------------------------------------------------------------

pub fn on_job(state: &State, kind: u8) {
    match kind {
        JOB_DEFER => {
            ffi::log_str(
                state.ctx,
                ffi::LOG_INFO,
                "job deferred fired: writing bus-pipe byte 0x42",
            );
            let b: [u8; 1] = [0x42];
            if ffi::write_fd(state.pipe_write, &b).is_err() {
                ffi::log_str(state.ctx, ffi::LOG_WARN, "job deferred: pipe write failed");
            }
        }
        JOB_TIMER => {
            ffi::log_str(state.ctx, ffi::LOG_DEBUG, "job timer tick");
        }
        JOB_PIPE => match ffi::read_fd(state.pipe_read, &mut [0u8; 16]) {
            Some(n) if n > 0 => ffi::log_str(state.ctx, ffi::LOG_INFO, "job bus-pipe byte=0x42"),
            _ => ffi::log_str(state.ctx, ffi::LOG_WARN, "job bus-pipe: readable but empty"),
        },
        JOB_BAR_TICK => bar::tick(state.ctx, state),
        _ => {}
    }
}
