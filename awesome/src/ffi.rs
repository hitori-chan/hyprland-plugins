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
    /// A second handle to the same window (increments the handle's own
    /// refcount; the compositor ref stays weak, so this never keeps a window
    /// alive).
    pub fn clone_handle(&self) -> Self {
        unsafe { hl_window_ref(self.ptr) };
        Self { ptr: self.ptr }
    }
}
impl Drop for WindowHandle {
    fn drop(&mut self) {
        unsafe { hl_window_unref(self.ptr) }
    }
}

pub struct PointerHandle {
    ptr: *mut hl_pointer,
}

impl PointerHandle {
    /// Takes ownership of one reference (the fork handed the plugin a ref).
    pub(crate) unsafe fn from_raw(ptr: *mut hl_pointer) -> Option<Self> {
        if ptr.is_null() {
            return None;
        }
        Some(Self { ptr })
    }
    pub(crate) fn as_raw(&self) -> *mut hl_pointer {
        self.ptr
    }
    /// A second handle to the same pointer (increments the handle's refcount).
    pub fn clone_handle(&self) -> Self {
        unsafe { hl_pointer_ref(self.ptr) };
        Self { ptr: self.ptr }
    }
}
impl Drop for PointerHandle {
    fn drop(&mut self) {
        unsafe { hl_pointer_unref(self.ptr) }
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
    /// mapped && !hidden (the window is live and visible on its workspace).
    pub visible: bool,
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
    let mut visible: u32 = 0;
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
            &raw mut visible,
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
        visible: visible != 0,
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
    pub focus_reason: u32,
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
    // dispatch returns true to CANCEL the event (swallow a tracked button on
    // a plugin-maximized window). The raw ev is needed for the cancel slot.
    let cancel = catch_unwind(AssertUnwindSafe(|| {
        let e = unsafe { &*ev };
        let window = unsafe { WindowHandle::from_raw(e.window) };
        let safe = SafeEvent {
            kind: e.kind,
            x: e.x,
            y: e.y,
            button: e.button,
            state: e.state,
            keycode: e.keycode,
            focus_reason: e.focus_reason,
            window,
        };
        super::probe::dispatch(state, &safe)
    }))
    .unwrap_or(false);
    if cancel {
        unsafe { event_cancel_raw(ev) };
    }
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

/// Register a timer (ms, repeat>0 to repeat). Returns the job token.
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

/// Cancel a pending job (a timer or deferred). No-op if already fired.
pub fn job_cancel(ctx: Ctx, token: u64) {
    if token != 0 {
        unsafe { hl_job_cancel(ctx, token) };
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
// window policy (Phase 2) — queries, writes, input state, Lua
// ---------------------------------------------------------------------------

// compositor fullscreen modes (mirror of eFullscreenMode; the fork's swap:
// 1 = maximized, 2 = real fullscreen — OPPOSITE of the IPC convention).
pub const FS_NONE: u32 = 0;
pub const FS_MAXIMIZED: u32 = 1;
pub const FS_FULLSCREEN: u32 = 2;

pub struct WindowFull {
    pub app_id: String,
    /// position + size (GLOBAL logical px — like a monitor box).
    pub at: hl_box_t,
    pub fullscreen: u32,
    pub floating: bool,
}

impl WindowFull {
    pub fn new(app_id: String, at: hl_box_t, fs: u32, fl: bool) -> Self {
        Self {
            app_id,
            at,
            fullscreen: fs,
            floating: fl,
        }
    }
}

/// The full window query: appID + layout box + fullscreen modes + flags.
/// None if the handle's weak ref has expired.
pub fn window_full(ctx: Ctx, w: &WindowHandle) -> Option<WindowFull> {
    let mut app: hl_str_t = hl_str_t {
        d: std::ptr::null(),
        l: 0,
    };
    let mut at: hl_box_t = unsafe { std::mem::zeroed() };
    let mut fs: u32 = 0;
    let mut floating: u32 = 0;
    let rc = unsafe {
        hl_window_get(
            ctx,
            w.as_raw(),
            &raw mut app,
            std::ptr::null_mut(),
            &raw mut at,
            std::ptr::null_mut(),
            &raw mut fs,
            std::ptr::null_mut(),
            &raw mut floating,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
        )
    };
    if rc != HL_E_OK {
        return None;
    }
    Some(WindowFull::new(str_from(&app), at, fs, floating != 0))
}

/// The window's stable identity (its address); 0 if the handle expired.
pub fn window_id(ctx: Ctx, w: &WindowHandle) -> u64 {
    unsafe { hl_window_id(ctx, w.as_raw()) }
}

/// The focused window (takes ownership of the ref). None if none.
pub fn focus_window(ctx: Ctx) -> Option<WindowHandle> {
    let mut out: *mut hl_window = std::ptr::null_mut();
    let rc = unsafe { hl_focus_window(ctx, &raw mut out) };
    (rc == HL_E_OK).then(|| unsafe { WindowHandle::from_raw(out) }.unwrap())
}

/// The window under a global point (fresh hit-test). Takes ownership of the ref.
pub fn window_at(ctx: Ctx, x: f64, y: f64) -> Option<WindowHandle> {
    let mut out: *mut hl_window = std::ptr::null_mut();
    let rc = unsafe { hl_window_at(ctx, x, y, &raw mut out) };
    (rc == HL_E_OK).then(|| unsafe { WindowHandle::from_raw(out) }.unwrap())
}

/// The monitor containing a global point (nearest one if it lands in a gap).
/// Takes ownership of the ref.
pub fn monitor_at(ctx: Ctx, x: f64, y: f64) -> Option<MonitorHandle> {
    let mut out: *mut hl_monitor = std::ptr::null_mut();
    let rc = unsafe { hl_monitor_at(ctx, x, y, &raw mut out) };
    (rc == HL_E_OK).then(|| unsafe { MonitorHandle::from_raw(out) }.unwrap())
}

/// The monitor's workarea (logical box minus reserved areas, global px).
pub fn monitor_workarea(ctx: Ctx, mon: &MonitorHandle) -> Option<hl_box_t> {
    let mut b: hl_box_t = unsafe { std::mem::zeroed() };
    let rc = unsafe { hl_monitor_workarea(ctx, mon.as_raw(), &raw mut b) };
    (rc == HL_E_OK).then_some(b)
}

/// The toplevel's min/max size (a pinned axis has min == max). None if expired.
pub fn min_max_size(ctx: Ctx, w: &WindowHandle) -> Option<(hl_box_t, hl_box_t)> {
    let mut min: hl_box_t = unsafe { std::mem::zeroed() };
    let mut max: hl_box_t = unsafe { std::mem::zeroed() };
    let rc = unsafe { hl_window_min_max_size(ctx, w.as_raw(), &raw mut min, &raw mut max) };
    (rc == HL_E_OK).then_some((min, max))
}

// ---- placement queries (hyprplace) ----

/// The placement-relevant window state (box + flags + class). None if expired.
pub struct WindowPlace {
    pub app_id: String,
    /// position + size (GLOBAL logical px — like a monitor box).
    pub box_: hl_box_t,
    pub fullscreen: u32,
    pub floating: bool,
    pub pinned: bool,
    pub visible: bool,
}

pub fn window_place(ctx: Ctx, w: &WindowHandle) -> Option<WindowPlace> {
    let mut app: hl_str_t = hl_str_t {
        d: std::ptr::null(),
        l: 0,
    };
    let mut at: hl_box_t = unsafe { std::mem::zeroed() };
    let mut fs: u32 = 0;
    let mut floating: u32 = 0;
    let mut pinned: u32 = 0;
    let mut visible: u32 = 0;
    let rc = unsafe {
        hl_window_get(
            ctx,
            w.as_raw(),
            &raw mut app,
            std::ptr::null_mut(),
            &raw mut at,
            std::ptr::null_mut(),
            &raw mut fs,
            std::ptr::null_mut(),
            &raw mut floating,
            &raw mut pinned,
            &raw mut visible,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
        )
    };
    if rc != HL_E_OK {
        return None;
    }
    Some(WindowPlace {
        app_id: str_from(&app),
        box_: at,
        fullscreen: fs,
        floating: floating != 0,
        pinned: pinned != 0,
        visible: visible != 0,
    })
}

pub fn window_is_x11(ctx: Ctx, w: &WindowHandle) -> bool {
    unsafe { hl_window_is_x11(ctx, w.as_raw()) != 0 }
}
pub fn window_has_parent(ctx: Ctx, w: &WindowHandle) -> bool {
    unsafe { hl_window_has_parent(ctx, w.as_raw()) != 0 }
}
pub fn window_override_redirect(ctx: Ctx, w: &WindowHandle) -> bool {
    unsafe { hl_window_override_redirect(ctx, w.as_raw()) != 0 }
}
/// The window's monitor (takes ownership of the ref). None if expired/detached.
pub fn window_monitor(ctx: Ctx, w: &WindowHandle) -> Option<MonitorHandle> {
    let mut out: *mut hl_monitor = std::ptr::null_mut();
    let rc = unsafe { hl_window_monitor(ctx, w.as_raw(), &raw mut out) };
    (rc == HL_E_OK).then(|| unsafe { MonitorHandle::from_raw(out) }.unwrap())
}
/// The window's border width (0 if expired; the border is drawn outside the box).
pub fn window_border_size(ctx: Ctx, w: &WindowHandle) -> f64 {
    unsafe { hl_window_border_size(ctx, w.as_raw()) }
}
/// True while a fullscreen/maximize grant is in play (the compositor owns the
/// geometry, so the placement must not place it).
pub fn window_grant_exempt(ctx: Ctx, w: &WindowHandle) -> bool {
    unsafe { hl_window_grant_exempt(ctx, w.as_raw()) != 0 }
}
/// True if the xdg toplevel was last told maximized (hyprmax's client-only
/// maximize — never enters compositor fullscreen, so the fullscreen mode
/// alone misses it).
pub fn window_told_maximized(ctx: Ctx, w: &WindowHandle) -> bool {
    unsafe { hl_window_told_maximized(ctx, w.as_raw()) != 0 }
}

// ---- hyprclick: focus-setter + cursor/fullscreen/history queries ----
/// A focus SETTER (fullWindowFocus with an explicit reason). The reason is
/// what picks the raise policy (keyboard/focus reasons raise, a hover pass
/// does not).
pub fn focus_window_set(ctx: Ctx, w: &WindowHandle, reason: u32) -> u32 {
    unsafe { hl_focus_window_set(ctx, w.as_raw(), reason) }
}
/// The window under the pointer (a fresh hit test). None if none.
pub fn window_under_cursor(ctx: Ctx) -> Option<WindowHandle> {
    let mut out: *mut hl_window = std::ptr::null_mut();
    let rc = unsafe { hl_window_under_cursor(ctx, &raw mut out) };
    (rc == HL_E_OK).then(|| unsafe { WindowHandle::from_raw(out) }.unwrap())
}
/// Controller-level fullscreen (internal OR client mode active).
pub fn window_is_fullscreen(ctx: Ctx, w: &WindowHandle) -> bool {
    unsafe { hl_window_is_fullscreen(ctx, w.as_raw()) != 0 }
}
/// Tuck the floaters back behind a fullscreen/maximized window (clear the
/// allowed-over flag on the other windows of its workspace; never `lower()`).
pub fn clear_allowed_over(ctx: Ctx, w: &WindowHandle) {
    unsafe { hl_clear_allowed_over(ctx, w.as_raw()) }
}
/// The window focus history, old -> new (bounded at CAP; dead windows are
/// skipped, so the count can be under the historical length).
pub fn focus_history(ctx: Ctx) -> Vec<WindowHandle> {
    const CAP: u32 = 256;
    let mut ptrs: Vec<*mut hl_window> = vec![std::ptr::null_mut(); CAP as usize];
    let total = unsafe { hl_focus_history(ctx, ptrs.as_mut_ptr(), CAP) } as usize;
    ptrs.truncate(total.min(CAP as usize));
    ptrs.into_iter()
        .filter_map(|p| {
            if p.is_null() {
                None
            } else {
                unsafe { WindowHandle::from_raw(p) }
            }
        })
        .collect()
}
/// A monitor's full logical box, or None if it expired.
pub fn monitor_logical_box(ctx: Ctx, mon: &MonitorHandle) -> Option<hl_box_t> {
    let mut out: hl_box_t = unsafe { std::mem::zeroed() };
    let rc = unsafe { hl_monitor_logical_box(ctx, mon.as_raw(), &raw mut out) };
    (rc == HL_E_OK).then_some(out)
}
/// The workspace's numbered id (0 for special/none — numbered IDs start at 1).
pub fn workspace_number(ctx: Ctx, ws: &WorkspaceHandle) -> u32 {
    unsafe { hl_workspace_number(ctx, ws.as_raw()) }
}

/// Every live window (bounded at CAP). Each returned handle owns one ref,
/// released on drop. A screen has far fewer than CAP windows; the count is
/// the bound, so a pathological overflow degrades to the first CAP (a missed
/// blocker, never UB).
pub fn all_windows(ctx: Ctx) -> Vec<WindowHandle> {
    const CAP: u32 = 4096;
    let mut ptrs: Vec<*mut hl_window> = vec![std::ptr::null_mut(); CAP as usize];
    let total = unsafe { hl_windows(ctx, ptrs.as_mut_ptr(), CAP) } as usize;
    ptrs.truncate(total.min(CAP as usize));
    ptrs.into_iter()
        .filter_map(|p| {
            if p.is_null() {
                None
            } else {
                unsafe { WindowHandle::from_raw(p) }
            }
        })
        .collect()
}

// ---- pointers (the input device list) ----

/// Every connected pointer (bounded at CAP). Each handle owns one ref.
pub fn pointers(ctx: Ctx) -> Vec<PointerHandle> {
    const CAP: u32 = 128;
    let mut ptrs: Vec<*mut hl_pointer> = vec![std::ptr::null_mut(); CAP as usize];
    let total = unsafe { hl_pointers(ctx, ptrs.as_mut_ptr(), CAP) } as usize;
    ptrs.truncate(total.min(CAP as usize));
    ptrs.into_iter()
        .filter_map(|p| {
            if p.is_null() {
                None
            } else {
                unsafe { PointerHandle::from_raw(p) }
            }
        })
        .collect()
}

/// 1 if the pointer is a touchpad (libinput touchpad class), else 0.
pub fn pointer_is_touchpad(ctx: Ctx, p: &PointerHandle) -> bool {
    let v: u32 = unsafe { hl_pointer_is_touchpad(ctx, p.as_raw()) };
    v != 0
}
/// 1 if the pointer is virtual (a composited/synthesized pointer), else 0.
pub fn pointer_is_virtual(ctx: Ctx, p: &PointerHandle) -> bool {
    let v: u32 = unsafe { hl_pointer_is_virtual(ctx, p.as_raw()) };
    v != 0
}
/// 1 if the pointer is connected to the cursor, 0 otherwise.
pub fn pointer_connected(ctx: Ctx, p: &PointerHandle) -> bool {
    let v: u32 = unsafe { hl_pointer_connected(ctx, p.as_raw()) };
    v != 0
}
/// The libinput bus type (BUS_* in linux/input.h). 0 if expired/no device.
pub fn pointer_bus_type(ctx: Ctx, p: &PointerHandle) -> u32 {
    unsafe { hl_pointer_bus_type(ctx, p.as_raw()) }
}
/// The pointer's HL device name (m_hlName). Empty if expired.
pub fn pointer_name(ctx: Ctx, p: &PointerHandle) -> String {
    let mut s: hl_str_t = hl_str_t { d: std::ptr::null(), l: 0 };
    let rc = unsafe { hl_pointer_name(ctx, p.as_raw(), &mut s) };
    if rc != HL_E_OK {
        return String::new();
    }
    str_from(&s)
}
/// The pointer's address (a stable identity). 0 if expired.
pub fn pointer_id(ctx: Ctx, p: &PointerHandle) -> u64 {
    unsafe { hl_pointer_id(ctx, p.as_raw()) }
}

/// Run a Lua snippet on the config manager (the same path as the `hl.` API).
/// Returns true if it ran, false on a Lua error.
pub fn run_lua(ctx: Ctx, code: &str) -> bool {
    let c = format!("{code}\0");
    let rc = unsafe { hl_run_lua(ctx, c.as_ptr().cast::<c_char>()) };
    rc == HL_E_OK
}

// ---- window writes (event-loop thread) ----

pub fn window_set_geom(ctx: Ctx, w: &WindowHandle, x: f64, y: f64, pw: f64, ph: f64) -> u32 {
    unsafe { hl_window_set_geom(ctx, w.as_raw(), x, y, pw, ph) }
}
pub fn window_set_fs_mode(ctx: Ctx, w: &WindowHandle, internal: u32, client: u32) -> u32 {
    unsafe { hl_window_set_fs_mode(ctx, w.as_raw(), internal, client) }
}
pub fn window_set_toplevel_maximized(ctx: Ctx, w: &WindowHandle, on: bool) -> u32 {
    unsafe { hl_window_set_toplevel_maximized(ctx, w.as_raw(), u32::from(on)) }
}
pub fn window_request_client_size(ctx: Ctx, w: &WindowHandle) -> u32 {
    unsafe { hl_window_request_client_size(ctx, w.as_raw()) }
}
pub fn window_send_window_size(ctx: Ctx, w: &WindowHandle, force: bool) -> u32 {
    unsafe { hl_window_send_window_size(ctx, w.as_raw(), u32::from(force)) }
}
pub fn window_raise(ctx: Ctx, w: &WindowHandle) -> u32 {
    unsafe { hl_window_raise(ctx, w.as_raw()) }
}
pub fn window_reset_client_size_grant(ctx: Ctx, w: &WindowHandle) -> u32 {
    unsafe { hl_window_reset_client_size_grant(ctx, w.as_raw()) }
}
pub fn window_set_born_fullscreen(ctx: Ctx, w: &WindowHandle, on: bool) -> u32 {
    unsafe { hl_window_set_born_fullscreen(ctx, w.as_raw(), u32::from(on)) }
}

// ---- Lua (the user-facing bind targets keep their original namespaces) ----

/// A Lua function body (the fork's `PLUGIN_LUA_FN`; returns # of results).
pub type LuaFn = unsafe extern "C" fn(*mut c_void) -> i32;

pub fn lua_register(ctx: Ctx, ns: &str, name: &str, f: LuaFn) -> u32 {
    let n = format!("{ns}\0");
    let m = format!("{name}\0");
    unsafe {
        hl_lua_register(
            ctx,
            n.as_ptr().cast::<c_char>(),
            m.as_ptr().cast::<c_char>(),
            Some(f),
        )
    }
}

/// The `unsafe extern "C"` wrapper for the hyprmax Lua toggle. The `unsafe`
/// boundary lives here (one module); the body is the safe max impl.
pub unsafe extern "C" fn lua_toggle(_lua: *mut c_void) -> i32 {
    crate::max::lua_toggle_impl()
}

/// The `unsafe extern "C"` wrappers for the hyprclick Lua focus helpers. The
/// `unsafe` boundary lives here (one module); the bodies are the safe click
/// impls.
pub unsafe extern "C" fn lua_focus_prev_here(_lua: *mut c_void) -> i32 {
    crate::click::lua_focus_prev_here_impl()
}
pub unsafe extern "C" fn lua_focus_next(_lua: *mut c_void) -> i32 {
    crate::click::lua_focus_next_impl()
}
pub unsafe extern "C" fn lua_focus_prev(_lua: *mut c_void) -> i32 {
    crate::click::lua_focus_prev_impl()
}

/// The `unsafe extern "C"` wrapper for the hyprpad manual toggle.
pub unsafe extern "C" fn lua_pad_toggle(_lua: *mut c_void) -> i32 {
    crate::pad::lua_toggle_impl()
}

/// Wrap a non-null raw pointer in a shared reference (the one place a raw
/// pointer becomes a `&`; the pointee's lifetime is the plugin's). The safe
/// modules call this instead of doing the cast themselves.
pub fn ref_from_ptr<T>(ptr: *mut T) -> Option<&'static T> {
    (!ptr.is_null()).then(|| unsafe { &*ptr })
}

// ---- input state (the compositor-integration gates) ----

pub fn input_capture_active(ctx: Ctx) -> bool {
    unsafe { hl_input_capture_active(ctx) != 0 }
}
pub fn native_pointer_grab(ctx: Ctx) -> bool {
    unsafe { hl_native_pointer_grab(ctx) != 0 }
}
pub fn native_layer_at(ctx: Ctx) -> bool {
    unsafe { hl_native_layer_at(ctx) != 0 }
}
pub fn super_held(ctx: Ctx) -> bool {
    unsafe { hl_super_held(ctx) != 0 }
}

/// Cancel the in-flight event (cancellable kinds only). The fork's
/// `_cancel_slot` is fork-owned; this is the only way to swallow it.
pub unsafe fn event_cancel_raw(ev: *mut hl_event_t) {
    if !ev.is_null() {
        unsafe { hl_event_cancel(ev) };
    }
}

/// A tracked left/right/middle button bit (the only buttons a plugin may
/// swallow; anything else stays native application input).
pub fn tracked_button_bit(button: u32) -> u32 {
    // the standard linux BTN_* codes (the virtual pointer + the real seat both
    // deliver these; an earlier off-by-two mapped BTN_LEFT to the middle slot).
    match button {
        272 /* BTN_LEFT */ => 1,
        274 /* BTN_RIGHT */ => 2,
        273 /* BTN_MIDDLE */ => 4,
        _ => 0,
    }
}

// ---------------------------------------------------------------------------
// render (canvas + textures) — the Phase 1 surface
// ---------------------------------------------------------------------------

// render stages (mirror of the fork's hl_render_stage_t; bindgen prefixes the
// consts with the type name).
pub const HL_RND_POST_WINDOWS: u32 = hl_render_stage_t_HL_RND_POST_WINDOWS;

/// A refcounted GPU texture (the fork handed us one ref; Drop unreffs it).
pub struct TextureHandle {
    ptr: *mut hl_texture,
}
impl TextureHandle {
    pub(crate) unsafe fn from_raw(ptr: *mut hl_texture) -> Option<Self> {
        (!ptr.is_null()).then_some(Self { ptr })
    }
    pub(crate) fn as_raw(&self) -> *mut hl_texture {
        self.ptr
    }
    /// The texture's pixel size (0,0 if not uploaded yet).
    pub fn size(&self) -> (u32, u32) {
        let mut w: u32 = 0;
        let mut h: u32 = 0;
        unsafe { hl_texture_size(self.ptr, &raw mut w, &raw mut h) };
        (w, h)
    }
}
impl Drop for TextureHandle {
    fn drop(&mut self) {
        unsafe { hl_texture_unref(self.ptr) };
    }
}

/// A monitor handle (RAII; Drop unreffs).
pub struct MonitorHandle {
    ptr: *mut hl_monitor,
}
impl MonitorHandle {
    pub(crate) unsafe fn from_raw(ptr: *mut hl_monitor) -> Option<Self> {
        (!ptr.is_null()).then_some(Self { ptr })
    }
    pub(crate) fn as_raw(&self) -> *mut hl_monitor {
        self.ptr
    }
}
impl Drop for MonitorHandle {
    fn drop(&mut self) {
        unsafe { hl_monitor_unref(self.ptr) };
    }
}

pub struct WorkspaceHandle {
    ptr: *mut hl_workspace,
}
impl WorkspaceHandle {
    pub(crate) unsafe fn from_raw(ptr: *mut hl_workspace) -> Option<Self> {
        (!ptr.is_null()).then_some(Self { ptr })
    }
    pub(crate) fn as_raw(&self) -> *mut hl_workspace {
        self.ptr
    }
}
impl Drop for WorkspaceHandle {
    fn drop(&mut self) {
        unsafe { hl_workspace_unref(self.ptr) };
    }
}

/// The window's workspace (takes ownership of the ref). None if expired.
pub fn window_workspace(ctx: Ctx, w: &WindowHandle) -> Option<WorkspaceHandle> {
    let mut out: *mut hl_workspace = std::ptr::null_mut();
    let rc = unsafe { hl_window_workspace(ctx, w.as_raw(), &raw mut out) };
    (rc == HL_E_OK).then(|| unsafe { WorkspaceHandle::from_raw(out) }.unwrap())
}
/// The focused monitor (takes ownership of the ref). None if none.
pub fn focus_monitor(ctx: Ctx) -> Option<MonitorHandle> {
    let mut out: *mut hl_monitor = std::ptr::null_mut();
    let rc = unsafe { hl_focus_monitor(ctx, &raw mut out) };
    (rc == HL_E_OK).then(|| unsafe { MonitorHandle::from_raw(out) }.unwrap())
}
/// A monitor's active (numbered) workspace. None if it expired or has none.
pub fn monitor_workspace(ctx: Ctx, mon: &MonitorHandle) -> Option<WorkspaceHandle> {
    let mut out: *mut hl_workspace = std::ptr::null_mut();
    let rc = unsafe { hl_monitor_active_workspace(ctx, mon.as_raw(), &raw mut out) };
    (rc == HL_E_OK).then(|| unsafe { WorkspaceHandle::from_raw(out) }.unwrap())
}

/// The workspace's stable id (0 if expired).
pub fn workspace_id(ctx: Ctx, ws: &WorkspaceHandle) -> u32 {
    let mut id: u32 = 0;
    let rc = unsafe {
        hl_workspace_get(
            ctx,
            ws.as_raw(),
            std::ptr::null_mut(),
            &raw mut id,
            std::ptr::null_mut(),
        )
    };
    if rc == HL_E_OK { id } else { 0 }
}

/// A monitor's logical box (x, y, w, h) + scale, or None if it expired.
pub fn monitor_box(ctx: Ctx, mon: &MonitorHandle) -> Option<(hl_box_t, f32)> {
    let mut b: hl_box_t = unsafe { std::mem::zeroed() };
    let mut s: f32 = 1.0;
    let rc = unsafe {
        hl_monitor_get(
            ctx,
            mon.as_raw(),
            std::ptr::null_mut(),
            &raw mut b,
            &raw mut s,
            std::ptr::null_mut(),
        )
    };
    (rc == HL_E_OK).then_some((b, s))
}

/// The first monitor in the compositor's list (takes ownership of the ref).
pub fn first_monitor(ctx: Ctx) -> Option<MonitorHandle> {
    let mut out: *mut hl_monitor = std::ptr::null_mut();
    let n = unsafe { hl_monitors(ctx, &raw mut out, 1) };
    if n == 0 {
        return None;
    }
    unsafe { MonitorHandle::from_raw(out) }
}

/// Register a render callback at `stage`; the fork invokes `draw_trampoline`
/// (with `ud`) once per rendered frame at that stage. Returns the C error.
/// The fork requires a non-null out-handle slot; we discard the handle (the
/// plugin never unregisters individual callbacks — teardown clears all).
pub fn render_listen(ctx: Ctx, stage: u32, ud: *mut c_void) -> u32 {
    let mut h: *mut c_void = std::ptr::null_mut();
    unsafe { hl_render_listen(ctx, stage, Some(draw_trampoline), ud, &raw mut h) }
}

// The extern "C" draw callback. Owns the raw canvas pointer; hands the bar a
// fully-safe view. A panic can never unwind across the C boundary. The canvas
// is a stack object in the fork's trampoline, valid only for this call.
unsafe extern "C" fn draw_trampoline(cv: *mut hl_canvas, ud: *mut c_void) {
    if cv.is_null() || ud.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let state = unsafe { &*(ud as *const super::probe::State) };
        super::bar::draw(cv, state);
    }));
}

// ---- canvas (non-owning; the pointer is valid only during the draw call) ----

/// The monitor the canvas is painting (takes ownership of the ref).
pub fn canvas_monitor(cv: *mut hl_canvas) -> Option<MonitorHandle> {
    if cv.is_null() {
        return None;
    }
    let mut out: *mut hl_monitor = std::ptr::null_mut();
    unsafe { hl_canvas_monitor(cv, &raw mut out) };
    unsafe { MonitorHandle::from_raw(out) }
}

/// The monitor's logical extent (0,0,w,h in monitor-local logical px) + scale.
pub fn canvas_extent(cv: *mut hl_canvas) -> Option<(hl_box_t, f32)> {
    if cv.is_null() {
        return None;
    }
    let mut b: hl_box_t = unsafe { std::mem::zeroed() };
    let mut s: f32 = 1.0;
    unsafe { hl_canvas_extent(cv, &raw mut b, &raw mut s) };
    (b.w > 0.0 && b.h > 0.0).then_some((b, s))
}

pub fn canvas_rect(cv: *mut hl_canvas, box_: hl_box_t, color: hl_color_t, round: u32, rp: f32) {
    if cv.is_null() {
        return;
    }
    unsafe { hl_canvas_rect(cv, box_, color, round, rp) };
}

pub fn canvas_glass(
    cv: *mut hl_canvas,
    box_: hl_box_t,
    color: hl_color_t,
    round: u32,
    rp: f32,
    blur: bool,
) {
    if cv.is_null() {
        return;
    }
    unsafe { hl_canvas_glass(cv, box_, color, round, rp, u32::from(blur)) };
}

pub fn canvas_border(
    cv: *mut hl_canvas,
    box_: hl_box_t,
    color: hl_color_t,
    round: u32,
    rp: f32,
    size_px: u32,
) {
    if cv.is_null() {
        return;
    }
    unsafe { hl_canvas_border(cv, box_, color, round, rp, size_px) };
}

pub fn canvas_texture(cv: *mut hl_canvas, tex: &TextureHandle, box_: hl_box_t) {
    if cv.is_null() {
        return;
    }
    unsafe { hl_canvas_texture(cv, tex.as_raw(), box_) };
}

// ---- textures (built OUTSIDE a frame — the warm/draw gate, crash class 4) ----

/// Build a text texture (the compositor rasterizes `text` at `pt` pt).
pub fn text_texture(
    ctx: Ctx,
    text: &str,
    color: hl_color_t,
    pt: u32,
    max_width: u32,
    font: &str,
) -> Option<TextureHandle> {
    let t = format!("{text}\0");
    let f = format!("{font}\0");
    let mut out: *mut hl_texture = std::ptr::null_mut();
    let rc = unsafe {
        hl_text_texture(
            ctx,
            t.as_ptr().cast::<c_char>(),
            color,
            pt,
            max_width,
            f.as_ptr().cast::<c_char>(),
            &raw mut out,
        )
    };
    if rc != HL_E_OK {
        return None;
    }
    unsafe { TextureHandle::from_raw(out) }
}

/// Build a texture from CPU RGBA (XRGB8888) pixels.
pub fn texture_from_rgba(
    ctx: Ctx,
    data: &[u8],
    w: u32,
    h: u32,
    stride: u32,
) -> Option<TextureHandle> {
    let mut out: *mut hl_texture = std::ptr::null_mut();
    let rc = unsafe { hl_texture_from_rgba(ctx, data.as_ptr(), w, h, stride, &raw mut out) };
    if rc != HL_E_OK {
        return None;
    }
    unsafe { TextureHandle::from_raw(out) }
}

// ---- damage ----

/// Damage a monitor-local logical box (schedules a repaint of that region).
pub fn damage(ctx: Ctx, mon: &MonitorHandle, box_: hl_box_t) {
    unsafe { hl_damage(ctx, mon.as_raw(), box_) };
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

/// The current local time as "HH:MM:SS" (via libc; no std-time dependency).
pub fn now_hms() -> String {
    let mut t: libc::time_t = 0;
    let mut tm: libc::tm = unsafe { std::mem::zeroed() };
    let mut buf = [0i8; 16];
    unsafe {
        if libc::time(&raw mut t) == libc::time_t::MAX {
            return String::new();
        }
        if libc::localtime_r(&raw const t, &raw mut tm).is_null() {
            return String::new();
        }
        let n = libc::strftime(
            buf.as_mut_ptr(),
            buf.len(),
            c"%H:%M:%S".as_ptr().cast(),
            &raw const tm,
        );
        if n == 0 {
            return String::new();
        }
        let bytes = std::slice::from_raw_parts(buf.as_ptr().cast::<u8>(), n as usize);
        String::from_utf8_lossy(bytes).into_owned()
    }
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
            // env! is a Rust &str (not NUL-terminated); the fork copies the C
            // string until NUL, so terminate it explicitly (the &str's bytes
            // sit mid-\u{2e}rodata, adjacent to other literals, so an unterminated
            // read would swallow them).
            *version = concat!(env!("CARGO_PKG_VERSION"), "\0")
                .as_bytes()
                .as_ptr()
                .cast::<c_char>();
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
