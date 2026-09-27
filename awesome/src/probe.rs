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
use crate::click;
use crate::ffi;
use crate::max;
use crate::osd;
use crate::pad;
use crate::place;
use crate::snap;

/// The ABI version this build targets (must equal the fork's cabiAbiVersion).
pub const CABI_ABI_VERSION: u32 = 1;

// job kinds (tagged into the ffi::JobArg that is passed as each job's `ud`)
pub const JOB_DEFER: u8 = 1;
pub const JOB_TIMER: u8 = 2;
pub const JOB_PIPE: u8 = 3;
pub const JOB_BAR_TICK: u8 = 4;
pub const JOB_MAX_TOGGLE: u8 = 5;
pub const JOB_MAX_ADOPT: u8 = 6;
pub const JOB_MAX_REFLOW: u8 = 7;
pub const JOB_PLACE: u8 = 8;
pub const JOB_CLICK: u8 = 9;
pub const JOB_PAD_SETTLE: u8 = 10;
pub const JOB_PAD_TOGGLE: u8 = 11;
pub const JOB_SNAP_MAGNET: u8 = 12;
pub const JOB_OSD_DRAIN: u8 = 13;
pub const JOB_OSD_SET_DONE: u8 = 14;
pub const JOB_OSD_GET_OUT: u8 = 15;
pub const JOB_BUS_WAKE: u8 = 16;
pub const JOB_OSD_ORPHAN_TICK: u8 = 17;

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
    // The Phase 2 hyprmax policy (maximize/restore/adopt/reflow/swallow).
    max: Mutex<max::MaxState>,
    // The Phase 2 hyprplace policy (spawn placement + geometry memory).
    place: Mutex<place::PlaceState>,
    // The Phase 2 hyprclick policy (click-to-raise, focus raises, corpse).
    click: Mutex<click::ClickState>,
    // The Phase 2 hyprpad policy (touchpad auto on/off, manual toggle).
    pad: Mutex<pad::PadState>,
    // The Phase 3 hyprsnap policy (magnetism + aerosnap edge zones). `pub`:
    // the snap render callback reads it by the state pointer (the draw is in
    // the snap module, same crate, but the field must be visible there).
    pub snap: Mutex<snap::SnapState>,
    // The Phase 3 hyprosd policy (the volume/brightness OSD chains).
    osd: Mutex<osd::OsdState>,
    // The one bus thread (the plan's D-Bus line). The event-loop handle
    // lives in `crate::bus`'s static (State is leaked and cannot be mutated
    // from teardown); these are the wake pipe's read end (owned by us; the
    // thread owns the write end) + its watch token.
    pub bus_pipe_read: libc::c_int,
    pub bus_reply_token: u64,
}

static STATE: AtomicPtr<State> = AtomicPtr::new(ptr::null_mut());

/// Set up the probe: pipe, config, subscription, and jobs. Returns 0 on
/// success (init ok), non-zero to eject.
#[allow(clippy::too_many_lines)]
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
        max: Mutex::new(max::MaxState::new()),
        place: Mutex::new(place::PlaceState::new()),
        click: Mutex::new(click::ClickState::new()),
        pad: Mutex::new(pad::PadState::new()),
        snap: Mutex::new(snap::SnapState::new()),
        osd: Mutex::new(osd::OsdState::new()),
        bus_pipe_read: -1,
        bus_reply_token: 0,
    });
    // The pointer the (leaked) Box will live at. We use it for every job's
    // `ud` and the render callback; the Box is leaked at the end of init.
    let state_ptr: *mut State = Box::as_mut_ptr(&mut state);
    STATE.store(state_ptr, Ordering::SeqCst);

    // subscribe to the event set. The input events are cancellable: the
    // hyprmax swallow (Super+press on a maximized window) cancels them.
    let mask = ffi::HL_EV_TICK
        | ffi::HL_EV_KEY
        | ffi::HL_EV_MOUSE_BUTTON
        | ffi::HL_EV_MOUSE_MOVE
        | ffi::HL_EV_WINDOW_ACTIVE
        | ffi::HL_EV_WINDOW_OPEN
        | ffi::HL_EV_WINDOW_CLOSE
        | ffi::HL_EV_WINDOW_DESTROY
        | ffi::HL_EV_WINDOW_FULL
        | ffi::HL_EV_WINDOW_WS
        | ffi::HL_EV_WS_ACTIVE
        | ffi::HL_EV_WS_MOVE_MON
        | ffi::HL_EV_MON_RESERVED
        | ffi::HL_EV_MON_LAYOUT
        | ffi::HL_EV_POINTER_CHANGED
        | ffi::HL_EV_CONFIG_RELOAD;
    if ffi::subscribe(ctx, mask, state_ptr.cast::<std::ffi::c_void>()) != 0 {
        ffi::log_str(ctx, ffi::LOG_ERR, "eject: subscribe failed");
        return -1;
    }

    // The no-legacy naming (plan §4): the namespace is the MODULE, not the old
    // plugin name. The user's live `hypr*` binds are migrated old→new at the
    // Phase 6 cutover (the one documented breaking change).
    let _ = ffi::lua_register(ctx, "max", "toggle", ffi::lua_toggle);
    let _ = ffi::lua_register(ctx, "click", "focus_prev_here", ffi::lua_focus_prev_here);
    let _ = ffi::lua_register(ctx, "click", "focus_next", ffi::lua_focus_next);
    let _ = ffi::lua_register(ctx, "click", "focus_prev", ffi::lua_focus_prev);
    let _ = ffi::lua_register(ctx, "pad", "toggle", ffi::lua_pad_toggle);
    let _ = ffi::lua_register(ctx, "osd", "volume_up", ffi::lua_osd_volume_up);
    let _ = ffi::lua_register(ctx, "osd", "volume_down", ffi::lua_osd_volume_down);
    let _ = ffi::lua_register(ctx, "osd", "mute", ffi::lua_osd_mute);
    let _ = ffi::lua_register(ctx, "osd", "mic_mute", ffi::lua_osd_mic_mute);
    let _ = ffi::lua_register(ctx, "osd", "brightness_up", ffi::lua_osd_brightness_up);
    let _ = ffi::lua_register(ctx, "osd", "brightness_down", ffi::lua_osd_brightness_down);

    // The one bus thread (the plan's D-Bus line): cards, logind, and (from
    // Phase 4) the notification service all run there, never on the event
    // loop. The wake pipe wakes this loop when a reply lands.
    if let Some((r, w)) = ffi::pipe2_nonblock() {
        if crate::bus::start(w) {
            state.bus_pipe_read = r;
            state.bus_reply_token = watch_job(ctx, r, JOB_BUS_WAKE, 0);
        } else {
            // the thread never started: close both ends (commands would just
            // drop — the keys keep working, the cards don't)
            ffi::close_fd(r);
            ffi::close_fd(w);
        }
    }

    // bus pipe: watch the read end; the deferred job writes one byte
    let pipe_arg = Box::leak(Box::new(ffi::JobArg {
        state: state_ptr,
        kind: JOB_PIPE,
        extra: 0,
    }));
    ffi::watch_fd(ctx, read_fd, pipe_arg);

    let defer_arg = Box::leak(Box::new(ffi::JobArg {
        state: state_ptr,
        kind: JOB_DEFER,
        extra: 0,
    }));
    ffi::defer(ctx, defer_arg);

    // a repeating timer keeps a job pending at teardown (expired-no-op test)
    let timer_arg = Box::leak(Box::new(ffi::JobArg {
        state: state_ptr,
        kind: JOB_TIMER,
        extra: 0,
    }));
    ffi::timer(ctx, 1000, 1, timer_arg);

    // The Phase 1 mini-bar (render + textures). Non-fatal if it can't come up
    // (e.g. no monitor): the probe's event/job/config behavior is independent.
    // Called with a safe `&State` (the Box is still owned here); it captures
    // `state_ptr` for its render callback and timer.
    if !bar::init(ctx, &state, state_ptr) {
        ffi::log_str(ctx, ffi::LOG_WARN, "bar: mini-bar did not start");
    }

    // The Phase 2 hyprmax policy (loads the persisted windowed boxes).
    max::init(&state);

    // The Phase 2 hyprplace policy (loads the persisted spawn spots).
    place::init(&state);
    // (the hyprclick state is constructed with `State`; it holds no persisted
    // data, only the live gesture timers + the arrival order)

    // The Phase 2 hyprpad policy (arms the initial settle timer).
    pad::init(&state);

    // The Phase 3 hyprsnap policy (registers its config + the render listener).
    {
        let mut s = state.snap.lock().unwrap();
        snap::init(ctx, &mut s, state_ptr);
    }

    // The Phase 3 hyprosd policy (finds the backlight device; the Lua face is
    // registered above, the chains drain from the event loop).
    {
        let mut o = state.osd.lock().unwrap();
        osd::init(&mut o);
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
    // flush the coalesced hyprmax windowed-box write before the ctx is gone
    max::exit(state);
    // flush the coalesced hyprplace spawn-spot write before the ctx is gone
    place::exit(state);
    // reset the hyprclick gesture state (a swallow mask must not survive)
    {
        let mut c = click_lock(state);
        c.reset();
    }
    // cancel the hyprpad settle timer (no job pending at teardown)
    pad::exit(state);
    // the hyprosd chains out (every watch cancelled, every fd closed)
    {
        let mut o = osd_lock(state);
        osd::exit(state.ctx, &mut o);
    }
    // The bus thread: the wake watch out, the read end closed, then the
    // handle dropped — its sender drop ends the thread, which closes the
    // write end (the sources out BEFORE the connections die, as the C++
    // links did).
    if state.bus_reply_token != 0 {
        ffi::job_cancel(state.ctx, state.bus_reply_token);
    }
    if state.bus_pipe_read >= 0 {
        ffi::close_fd(state.bus_pipe_read);
    }
    crate::bus::stop();
}

/// Take the live State pointer (null if none). Safe: pointer load only.
pub fn take_state() -> *mut State {
    STATE.swap(ptr::null_mut(), Ordering::SeqCst)
}

/// A shared reference to the live State (null-safe). Used by the Lua entry
/// points, which are static `extern "C"` fns with no capture. Single-threaded
/// (the event loop owns the State), so a plain load is sound.
pub fn state() -> Option<&'static State> {
    ffi::ref_from_ptr(STATE.load(Ordering::SeqCst))
}

/// Lock the hyprmax policy state (recovers from a poisoned lock — the whole
/// plugin is single-threaded, so poison only means a prior panic).
pub fn max_lock(state: &State) -> std::sync::MutexGuard<'_, max::MaxState> {
    state
        .max
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

/// Lock the hyprplace policy state (recovers from a poisoned lock).
pub fn click_lock(state: &State) -> std::sync::MutexGuard<'_, click::ClickState> {
    state
        .click
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

pub fn place_lock(state: &State) -> std::sync::MutexGuard<'_, place::PlaceState> {
    state
        .place
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

pub fn osd_lock(state: &State) -> std::sync::MutexGuard<'_, osd::OsdState> {
    state
        .osd
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

pub fn snap_lock(state: &State) -> std::sync::MutexGuard<'_, snap::SnapState> {
    state
        .snap
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

pub fn pad_lock(state: &State) -> std::sync::MutexGuard<'_, pad::PadState> {
    state
        .pad
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

/// Arm a one-shot deferred job for a job kind (leaks its `JobArg`, as every
/// other job does; the fork's `defer` is one-shot, so a fresh arg per arm).
/// Returns false when no state (an unarmable drain must not let a queue grow).
pub fn arm_job(ctx: ffi::Ctx, kind: u8) -> bool {
    let sp = STATE.load(Ordering::SeqCst);
    if sp.is_null() {
        return false;
    }
    let arg = Box::leak(Box::new(ffi::JobArg {
        state: sp,
        kind,
        extra: 0,
    }));
    ffi::defer(ctx, arg) != 0
}

/// Arm a one-shot timer job (ms). Returns the job token (0 if no state).
pub fn arm_timer(ctx: ffi::Ctx, ms: u32, kind: u8) -> u64 {
    let sp = STATE.load(Ordering::SeqCst);
    if sp.is_null() {
        return 0;
    }
    let arg = Box::leak(Box::new(ffi::JobArg {
        state: sp,
        kind,
        extra: 0,
    }));
    ffi::timer(ctx, ms, 0, arg)
}

/// Arm a REPEATING timer job (ms). Returns the job token (0 if no state);
/// the caller cancels it with `job_cancel`.
pub fn arm_timer_repeat(ctx: ffi::Ctx, ms: u32, kind: u8) -> u64 {
    let sp = STATE.load(Ordering::SeqCst);
    if sp.is_null() {
        return 0;
    }
    let arg = Box::leak(Box::new(ffi::JobArg {
        state: sp,
        kind,
        extra: 0,
    }));
    ffi::timer(ctx, ms, 1, arg)
}

/// Arm a persistent readable watch that carries a u32 payload (the osd's
/// chain index rides in `extra`). Returns the job token (0 on failure).
pub fn watch_job(ctx: ffi::Ctx, fd: i32, kind: u8, extra: u32) -> u64 {
    let sp = STATE.load(Ordering::SeqCst);
    if sp.is_null() {
        return 0;
    }
    let arg = Box::leak(Box::new(ffi::JobArg {
        state: sp,
        kind,
        extra,
    }));
    ffi::watch_fd_persistent(ctx, fd, arg)
}

// ---------------------------------------------------------------------------
// event dispatch (safe; the ffi trampoline handed us a SafeEvent)
// ---------------------------------------------------------------------------

/// Dispatch one event. Returns true to CANCEL it (the hyprmax swallow on a
/// maximized window; every other path passes through).
#[allow(clippy::too_many_lines)]
pub fn dispatch(state: &State, ev: &ffi::SafeEvent) -> bool {
    state.events.fetch_add(1, Ordering::Relaxed);
    match ev.kind {
        ffi::HL_EV_TICK => {
            on_tick(state);
            false
        }
        ffi::HL_EV_KEY => {
            // the snap drag end (a key event tears down the drag too — the
            // keybind layer runs after the plugin emission, drag state intact)
            {
                let mut s = snap_lock(state);
                snap::on_input_ending_drag(state.ctx, &mut s);
            }
            on_key(state, ev);
            false
        }
        ffi::HL_EV_MOUSE_BUTTON => {
            let ctx = state.ctx;
            // the snap drag end (commit the armed zone / reset the resize
            // state) runs before max/click — the drag is being torn down.
            {
                let mut s = snap_lock(state);
                snap::on_input_ending_drag(ctx, &mut s);
            }
            // max runs first (a Super-grab it swallows is never a raise
            // click — the C++ load order hyprmax -> hyprclick, now in-plugin).
            let max_cancel = {
                let mut m = max_lock(state);
                max::on_button(ctx, &mut m, ev)
            };
            if max_cancel {
                true
            } else {
                let mut c = click_lock(state);
                click::on_button(ctx, &mut c, ev)
            }
        }
        ffi::HL_EV_MOUSE_MOVE => {
            // the hyprsnap aerosnap arming + magnetism (the mouse move is
            // cancellable, but snap never cancels it)
            let mut s = snap_lock(state);
            snap::on_mouse_move(state.ctx, &mut s, ev.x, ev.y);
            false
        }
        ffi::HL_EV_WINDOW_ACTIVE => {
            on_window(state, ev, "WINDOW_ACTIVE");
            {
                let mut c = click_lock(state);
                click::on_window_active(state.ctx, &mut c, ev);
            }
            false
        }
        ffi::HL_EV_WINDOW_OPEN => {
            on_window(state, ev, "WINDOW_OPEN");
            if let Some(w) = &ev.window {
                let mut p = place_lock(state);
                place::on_open(state.ctx, &mut p, w);
                let mut c = click_lock(state);
                click::on_open(state.ctx, &mut c, w);
            }
            false
        }
        ffi::HL_EV_WINDOW_CLOSE => {
            if let Some(w) = &ev.window {
                let mut p = place_lock(state);
                place::on_close(state.ctx, &mut p, w);
            }
            {
                let mut c = click_lock(state);
                click::on_close(state.ctx, &mut c, ev);
            }
            false
        }
        ffi::HL_EV_WINDOW_DESTROY => {
            on_window(state, ev, "WINDOW_DESTROY");
            let ctx = state.ctx;
            let mut m = max_lock(state);
            max::on_destroy(ctx, &mut m);
            if let Some(w) = &ev.window {
                let mut c = click_lock(state);
                click::on_destroy(ctx, &mut c, w);
            }
            false
        }
        ffi::HL_EV_WINDOW_FULL => {
            let ctx = state.ctx;
            let mut m = max_lock(state);
            max::on_fullscreen(ctx, &mut m, ev);
            let mut c = click_lock(state);
            click::on_fullscreen(ctx, &mut c, ev);
            false
        }
        ffi::HL_EV_WINDOW_WS
        | ffi::HL_EV_WS_ACTIVE
        | ffi::HL_EV_WS_MOVE_MON
        | ffi::HL_EV_MON_RESERVED
        | ffi::HL_EV_MON_LAYOUT => {
            let ctx = state.ctx;
            let mut m = max_lock(state);
            max::on_reflow_trigger(&mut m, ctx);
            false
        }
        ffi::HL_EV_POINTER_CHANGED => {
            let mut p = pad_lock(state);
            pad::on_pointer_changed(state, &mut p);
            false
        }
        ffi::HL_EV_CONFIG_RELOAD => {
            let mut p = pad_lock(state);
            pad::on_config_reloaded(state, &mut p);
            false
        }
        _ => false,
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

pub fn on_job(state: &State, kind: u8, extra: u32) {
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
        JOB_MAX_TOGGLE => {
            let mut m = max_lock(state);
            max::drain_toggles(state.ctx, &mut m);
        }
        JOB_MAX_ADOPT => {
            let mut m = max_lock(state);
            max::drain_adopts(state.ctx, &mut m);
        }
        JOB_MAX_REFLOW => {
            let mut m = max_lock(state);
            max::do_reflow_job(state.ctx, &mut m);
        }
        JOB_PLACE => {
            let mut p = place_lock(state);
            place::drain(state.ctx, &mut p);
        }
        JOB_CLICK => {
            let mut c = click_lock(state);
            click::drain(state.ctx, &mut c);
        }
        JOB_PAD_SETTLE => {
            let mut p = pad_lock(state);
            pad::drain_settle(state, &mut p);
        }
        JOB_PAD_TOGGLE => {
            let mut p = pad_lock(state);
            pad::drain_toggles(state, &mut p);
        }
        JOB_SNAP_MAGNET => {
            let mut s = snap_lock(state);
            snap::do_magnet(state.ctx, &mut s);
        }
        JOB_OSD_DRAIN => {
            let mut o = osd_lock(state);
            osd::drain(state.ctx, &mut o);
        }
        JOB_OSD_SET_DONE => {
            let mut o = osd_lock(state);
            osd::on_set_done(state.ctx, &mut o, extra);
        }
        JOB_OSD_GET_OUT => {
            let mut o = osd_lock(state);
            osd::on_get_out(state.ctx, &mut o, extra);
        }
        JOB_OSD_ORPHAN_TICK => {
            let mut o = osd_lock(state);
            osd::drain_orphan_tick(state.ctx, &mut o);
        }
        JOB_BUS_WAKE => {
            // drain the wake pipe, then the reply queue (the queue is the
            // source of truth; the byte is only the nudge)
            let _ = ffi::read_fd(state.bus_pipe_read, &mut [0u8; 64]);
            let replies = crate::bus::handle()
                .map(|b| b.drain_replies())
                .unwrap_or_default();
            for crate::bus::BusReply::BrightnessFailed { gen_id } in replies {
                let mut o = osd_lock(state);
                osd::on_brightness_failed(&mut o, gen_id);
            }
        }
        _ => {}
    }
}
