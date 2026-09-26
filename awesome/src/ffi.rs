// The cabi C ABI boundary — the ONLY module in the crate where unsafe is
// allowed. Everything else calls the safe wrappers / safe types here. Two
// layers:
//   1. raw bindgen bindings (the `hl_*` extern block, included below);
//   2. safe wrappers + RAII handles + the extern "C" entry points and
//      trampoline callbacks (which own the raw-pointer traffic).
//
// No C++ type, reference, exception, or template crosses into the rest of
// the crate: only safe Rust types and the opaque `*mut` handles.
#![allow(unsafe_code)]

// The bindgen output (snake_case C types) lives in a private module with its
// lints suppressed; the pub items are re-exported into this module's scope.
mod bindings {
    #![allow(
        non_camel_case_types,
        non_snake_case,
        non_upper_case_globals,
        non_snake_case,
        dead_code,
        clippy::all,
        clippy::pedantic
    )]
    include!(concat!(env!("OUT_DIR"), "/cabi.rs"));
}
pub use bindings::*;

use std::ffi::c_void;
use std::os::raw::c_char;
use std::panic::{AssertUnwindSafe, catch_unwind};

// log levels (mirror of the fork's hl_log level argument)
pub const LOG_DEBUG: u32 = 0;
pub const LOG_INFO: u32 = 1;
pub const LOG_WARN: u32 = 2;
pub const LOG_ERR: u32 = 3;

// error codes + config types. bindgen prefixes the C-enum consts with the
// type name (hl_error_t_HL_E_OK, …); these clean aliases are what the rest
// of the crate uses. The HL_EV_* event bits are already plain pub consts
// (from the #define macros) and need no alias.
pub const HL_E_OK: hl_error_t = hl_error_t_HL_E_OK;
pub const HL_CFG_INT: hl_cfg_type_t = hl_cfg_type_t_HL_CFG_INT;

pub type Ctx = *mut hl_ctx;

// ---------------------------------------------------------------------------
// version + lifecycle
// ---------------------------------------------------------------------------

/// The ABI version the running fork exports (the plugin's handshake).
pub fn abi_version() -> u32 {
    unsafe { cabiAbiVersion() }
}

/// Shuts the context down: cancels every job, unsubscribes every event.
/// Idempotent; the loader drops the context afterwards.
pub fn shutdown(ctx: Ctx) -> u32 {
    unsafe { hl_shutdown(ctx) }
}

/// Log a pre-formatted message to Hyprland's log.
pub fn log_str(ctx: Ctx, level: u32, msg: &str) {
    let m = format!("{msg}\0");
    unsafe { hl_log_str(ctx, level, m.as_ptr().cast::<c_char>()) }
}

// ---------------------------------------------------------------------------
// RAII handles (the plugin owns the ref the fork handed it; Drop unreffs)
// ---------------------------------------------------------------------------

pub struct WindowHandle {
    ptr: *mut hl_window,
}

impl WindowHandle {
    /// Takes ownership of one reference (the fork handed the plugin a ref).
    pub(crate) unsafe fn from_raw(ptr: *mut hl_window) -> Option<Self> {
        if ptr.is_null() {
            return None;
        }
        Some(Self { ptr })
    }
    pub(crate) fn as_raw(&self) -> *mut hl_window {
        self.ptr
    }
}
impl Drop for WindowHandle {
    fn drop(&mut self) {
        unsafe { hl_window_unref(self.ptr) }
    }
}

// ---------------------------------------------------------------------------
// string model
// ---------------------------------------------------------------------------

/// Read an `hl_str_t` (ptr + len, from the fork's scratch pool) into an owned
/// Rust String. Must be called before the next string-producing query on the
/// same ctx (the pool rotates).
pub fn str_from(s: &hl_str_t) -> String {
    if s.d.is_null() || s.l == 0 {
        return String::new();
    }
    unsafe {
        String::from_utf8_lossy(std::slice::from_raw_parts(s.d.cast::<u8>(), s.l as usize))
            .into_owned()
    }
}

// ---------------------------------------------------------------------------
// queries
// ---------------------------------------------------------------------------

#[derive(Default)]
pub struct WindowInfo {
    pub app_id: String,
    pub title: String,
    pub floating: bool,
    pub pinned: bool,
}

/// Window metadata (appID, title) + a few flags. The strings are copied out
/// before this returns, so they outlive the call.
pub fn window_info(ctx: Ctx, w: &WindowHandle) -> Option<WindowInfo> {
    let mut app: hl_str_t = hl_str_t {
        d: std::ptr::null(),
        l: 0,
    };
    let mut title: hl_str_t = hl_str_t {
        d: std::ptr::null(),
        l: 0,
    };
    let mut floating: u32 = 0;
    let mut pinned: u32 = 0;
    let rc = unsafe {
        hl_window_get(
            ctx,
            w.as_raw(),
            &raw mut app,
            &raw mut title,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            &raw mut floating,
            &raw mut pinned,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
        )
    };
    if rc != HL_E_OK {
        return None;
    }
    Some(WindowInfo {
        app_id: str_from(&app),
        title: str_from(&title),
        floating: floating != 0,
        pinned: pinned != 0,
    })
}

/// Whether the session is locked (native input state).
pub fn session_locked(ctx: Ctx) -> bool {
    unsafe { hl_session_locked(ctx) != 0 }
}

// ---------------------------------------------------------------------------
// events
// ---------------------------------------------------------------------------

/// A safe, owned view of one dispatched event (the fork's refcounted window
/// ref is wrapped in a `WindowHandle`).
pub struct SafeEvent {
    pub kind: u32,
    pub x: f64,
    pub y: f64,
    pub button: u32,
    pub state: u32,
    pub keycode: u32,
    pub window: Option<WindowHandle>,
}

/// Subscribe to a mask with a single dispatcher. The `ud` (opaque, passed
/// back to every dispatch/job callback) is the probe's State pointer.
pub fn subscribe(ctx: Ctx, mask: u32, ud: *mut c_void) -> u32 {
    unsafe { hl_subscribe(ctx, mask, Some(dispatch_trampoline), ud) }
}

// The extern "C" event dispatcher. Owns the raw-pointer traffic; hands the
// probe a fully-safe SafeEvent. A panic can never unwind across the C
// boundary.
unsafe extern "C" fn dispatch_trampoline(ev: *mut hl_event_t, ud: *mut c_void) {
    if ev.is_null() || ud.is_null() {
        return;
    }
    let state = unsafe { &*(ud as *const super::probe::State) };
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let e = unsafe { &*ev };
        let window = unsafe { WindowHandle::from_raw(e.window) };
        let safe = SafeEvent {
            kind: e.kind,
            x: e.x,
            y: e.y,
            button: e.button,
            state: e.state,
            keycode: e.keycode,
            window,
        };
        super::probe::dispatch(state, &safe);
    }));
}

// ---------------------------------------------------------------------------
// jobs
// ---------------------------------------------------------------------------

/// A job argument (the probe's State + a kind tag). Leaked; freed at exit.
pub struct JobArg {
    pub state: *const super::probe::State,
    pub kind: u8,
}

/// Register a one-shot deferred job (next idle).
pub fn defer(ctx: Ctx, arg: &JobArg) -> u64 {
    unsafe {
        hl_defer(
            ctx,
            Some(job_trampoline),
            std::ptr::from_ref::<JobArg>(arg) as *mut c_void,
        )
    }
}

/// Register a timer (ms, repeat>0 to repeat).
pub fn timer(ctx: Ctx, ms: u32, repeat: u32, arg: &JobArg) -> u64 {
    unsafe {
        hl_timer(
            ctx,
            ms,
            repeat,
            Some(job_trampoline),
            std::ptr::from_ref::<JobArg>(arg) as *mut c_void,
        )
    }
}

/// Watch an fd for readability (the fork takes ownership of the fd).
pub fn watch_fd(ctx: Ctx, fd: i32, arg: &JobArg) -> u64 {
    unsafe {
        hl_watch_fd(
            ctx,
            fd,
            Some(job_trampoline),
            std::ptr::from_ref::<JobArg>(arg) as *mut c_void,
        )
    }
}

// The extern "C" job callback. Owns the raw ud (a leaked JobArg).
unsafe extern "C" fn job_trampoline(ud: *mut c_void) {
    if ud.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let arg = unsafe { &*(ud as *const JobArg) };
        super::probe::on_job(unsafe { &*arg.state }, arg.kind);
    }));
}

// ---------------------------------------------------------------------------
// config
// ---------------------------------------------------------------------------

/// Register a config value; returns the opaque handle (kept by the fork).
pub fn config_register(
    ctx: Ctx,
    key: &str,
    desc: &str,
    r#type: u32,
    num_default: f64,
    str_default: &str,
) -> Option<*mut c_void> {
    let k = format!("{key}\0");
    let d = format!("{desc}\0");
    let s = format!("{str_default}\0");
    let mut out: *mut c_void = std::ptr::null_mut();
    let rc = unsafe {
        hl_config_register(
            ctx,
            k.as_ptr().cast::<c_char>(),
            d.as_ptr().cast::<c_char>(),
            r#type,
            num_default,
            s.as_ptr().cast::<c_char>(),
            &raw mut out,
        )
    };
    (rc == HL_E_OK).then_some(out)
}

/// Read a live config value (num for INT/BOOL/FLOAT, str for STRING).
pub fn config_get(ctx: Ctx, h: *mut c_void) -> Option<(u32, f64, String)> {
    let mut r#type: u32 = 0;
    let mut num: f64 = 0.0;
    let mut s: hl_str_t = hl_str_t {
        d: std::ptr::null(),
        l: 0,
    };
    let rc = unsafe { hl_config_get(ctx, h, &raw mut r#type, &raw mut num, &raw mut s) };
    if rc != HL_E_OK {
        return None;
    }
    Some((r#type, num, str_from(&s)))
}

// ---------------------------------------------------------------------------
// sys (pipe + fd io) — safe wrappers over libc
// ---------------------------------------------------------------------------

/// Create a pipe; returns (`read_fd`, `write_fd`).
pub fn pipe() -> Option<(libc::c_int, libc::c_int)> {
    let mut fds: [libc::c_int; 2] = [0; 2];
    let rc = unsafe { libc::pipe(fds.as_mut_ptr()) };
    if rc != 0 {
        return None;
    }
    Some((fds[0], fds[1]))
}

/// Close an fd (no-op if already closed / invalid).
pub fn close_fd(fd: libc::c_int) {
    if fd >= 0 {
        unsafe { libc::close(fd) };
    }
}

/// Read up to `buf.len()` bytes; returns the count (0/None on EOF/error).
pub fn read_fd(fd: libc::c_int, buf: &mut [u8]) -> Option<usize> {
    let n = unsafe { libc::read(fd, buf.as_mut_ptr().cast::<c_void>(), buf.len()) };
    (n >= 0).then_some(n.cast_unsigned())
}

/// Write all of buf; returns Ok(()) or the errno.
pub fn write_fd(fd: libc::c_int, buf: &[u8]) -> Result<(), i32> {
    let n = unsafe { libc::write(fd, buf.as_ptr().cast::<c_void>(), buf.len()) };
    if n < 0 {
        return Err(-1);
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// the loader entry points
// ---------------------------------------------------------------------------

/// C-ABI init. The fork creates the ctx and passes it; we fill the metadata
/// out-params and set up the probe. Returns 0 on success, non-zero to eject.
/// A panic can never unwind across this boundary.
#[unsafe(no_mangle)]
pub extern "C" fn hyprPluginInitC(
    ctx: *mut c_void,
    name: *mut *const c_char,
    version: *mut *const c_char,
    author: *mut *const c_char,
    description: *mut *const c_char,
) -> i32 {
    let result = catch_unwind(AssertUnwindSafe(|| {
        init_impl(ctx.cast::<hl_ctx>(), name, version, author, description)
    }));
    result.unwrap_or(-1)
}

fn init_impl(
    ctx: Ctx,
    name: *mut *const c_char,
    version: *mut *const c_char,
    author: *mut *const c_char,
    description: *mut *const c_char,
) -> i32 {
    // version handshake: eject on mismatch (a clean log, not a crash)
    let abi = abi_version();
    if abi != super::probe::CABI_ABI_VERSION {
        log_str(
            ctx,
            LOG_ERR,
            &format!(
                "eject: abi mismatch (fork {abi}, built for {})",
                super::probe::CABI_ABI_VERSION
            ),
        );
        return -1;
    }

    // fill the metadata out-params (pointers to 'static strings)
    unsafe {
        if !name.is_null() {
            *name = c"awesome".as_ptr();
        }
        if !version.is_null() {
            *version = env!("CARGO_PKG_VERSION").as_ptr().cast::<c_char>();
        }
        if !author.is_null() {
            *author = c"hitori".as_ptr();
        }
        if !description.is_null() {
            *description = c"awesome's modules as one Rust plugin (cabi)".as_ptr();
        }
    }

    super::probe::init(ctx)
}

/// C-ABI exit. Tears the probe down, then shuts the context. The loader
/// drops the context (SP) after we return, so no job can fire into the
/// unmapped .so.
#[unsafe(no_mangle)]
pub extern "C" fn hyprPluginExitC(ctx: *mut c_void) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let ctx = ctx.cast::<hl_ctx>();
        // Take the live State, run the safe probe teardown, then reclaim the
        // Box (unsafe lives here). This happens BEFORE the loader drops the
        // context and dlclose()s the .so, so nothing references freed memory.
        let state_ptr = super::probe::take_state();
        if !state_ptr.is_null() {
            super::probe::exit(unsafe { &*state_ptr });
            free_state(state_ptr);
        }
        shutdown(ctx);
    }));
}

/// Reclaim a leaked State Box (the `Box::into_raw` pair in `probe::init`).
fn free_state(ptr: *mut super::probe::State) {
    if ptr.is_null() {
        return;
    }
    unsafe { drop(Box::from_raw(ptr)) };
}
