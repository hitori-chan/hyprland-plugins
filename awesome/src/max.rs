// hyprmax — awesome's per-window maximize, ported to the cabi.
//
// maximized is a PER-WINDOW flag here (any number at once), not the
// compositor's one-per-workspace internal maximize. A plugin-maximized window
// is a client-only state: the client is told (xdg set_maximized) and the
// window is sized to the workarea — it never enters compositor fullscreen. A
// compositor-granted maximize (born-maximized at map, app request) is ADOPTED
// into this model on sight. The last windowed box is remembered per app class
// (persisted to $XDG_STATE_HOME/hyprmax), and maximized windows are immovable
// (a Super+press on one is swallowed whole).
//
// The maximized map is keyed by the window's stable address (ffi::window_id);
// entries are dropped on window destroy (the address must not alias a fresh
// window). Deferred work (toggles, adopts, reflowns) is queue+drain through a
// one-shot job — two events in one dispatch must not cancel each other.

use std::collections::HashMap;

use crate::ffi;
use crate::probe::State;

const DEFAULT_MIN: f64 = 20.0;

#[derive(Clone, Copy, PartialEq)]
struct Box4 {
    x: f64,
    y: f64,
    w: f64,
    h: f64,
}

impl Box4 {
    const fn new(x: f64, y: f64, w: f64, h: f64) -> Self {
        Self { x, y, w, h }
    }
    fn from_hl(b: ffi::hl_box_t) -> Self {
        Self::new(b.x, b.y, b.w, b.h)
    }
    /// A real windowed size (a legacy/empty row carries none).
    fn is_real(self) -> bool {
        self.w > 5.0 && self.h > 5.0
    }
}

pub struct MaxState {
    // window id -> (handle keeping the ref, restore box, app class)
    maximized: HashMap<u64, (ffi::WindowHandle, Box4, String)>,
    // app class -> last windowed box (persisted)
    last_windowed: HashMap<String, Box4>,
    // deferred work (queue+drain; the *_queued flag arms one job at a time)
    toggles: Vec<ffi::WindowHandle>,
    toggle_queued: bool,
    adopts: Vec<ffi::WindowHandle>,
    adopt_queued: bool,
    reflow_queued: bool,
    // swallowed left/right/middle button mask (immovable-maximized swallow)
    swallowed: u32,
}

impl MaxState {
    pub fn new() -> Self {
        Self {
            maximized: HashMap::new(),
            last_windowed: HashMap::new(),
            toggles: Vec::new(),
            toggle_queued: false,
            adopts: Vec::new(),
            adopt_queued: false,
            reflow_queued: false,
            swallowed: 0,
        }
    }
}

// ---------------------------------------------------------------------------
// persistence (TSV: x\ty\tw\th\tclass — the C++ hyprmax format)
// ---------------------------------------------------------------------------

fn store_path() -> std::path::PathBuf {
    let base = std::env::var("XDG_STATE_HOME").unwrap_or_else(|_| {
        std::env::var("HOME")
            .map(|h| format!("{h}/.local/state"))
            .unwrap_or_default()
    });
    std::path::Path::new(&base)
        .join("hyprmax")
        .join("windowed.tsv")
}

fn load_windowed(state: &mut MaxState) {
    let path = store_path();
    let Ok(contents) = std::fs::read_to_string(&path) else {
        return;
    };
    if contents.len() > 1_000_000 {
        return; // oversized hostile state: reject as a unit
    }
    for line in contents.lines() {
        let mut parts = line.split('\t');
        let (Some(px), Some(py), Some(pw), Some(ph), Some(cls)) = (
            parts.next(),
            parts.next(),
            parts.next(),
            parts.next(),
            parts.next(),
        ) else {
            continue;
        };
        let (Ok(px), Ok(py), Ok(pw), Ok(ph)) = (
            px.parse::<f64>(),
            py.parse::<f64>(),
            pw.parse::<f64>(),
            ph.parse::<f64>(),
        ) else {
            continue;
        };
        if cls.is_empty() || cls.len() > 256 || cls.contains(['\t', '\r', '\n', '\0']) {
            continue;
        }
        let b = Box4::new(px, py, pw, ph);
        if b.is_real() {
            state.last_windowed.insert(cls.to_string(), b);
        }
    }
}

fn save_windowed(state: &mut MaxState) {
    let path = store_path();
    if let Some(parent) = path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    let mut out = String::new();
    for (cls, b) in &state.last_windowed {
        if out.lines().count() >= 512 {
            break;
        }
        if !b.is_real() || cls.is_empty() || cls.contains(['\t', '\r', '\n', '\0']) {
            continue;
        }
        out.push_str(&round64(b.x).to_string());
        out.push('\t');
        out.push_str(&round64(b.y).to_string());
        out.push('\t');
        out.push_str(&round64(b.w).to_string());
        out.push('\t');
        out.push_str(&round64(b.h).to_string());
        out.push('\t');
        out.push_str(cls);
        out.push('\n');
    }
    let tmp = path.with_extension("tmp");
    let _ = std::fs::write(&tmp, out);
    let _ = std::fs::rename(&tmp, &path);
}

// Pixel coordinates are small; clamping to a safe domain makes the `as i64`
// a non-truncating cast (the C++ side uses llround).
#[allow(clippy::cast_possible_truncation)]
fn round64(v: f64) -> i64 {
    v.clamp(-1e15, 1e15).round() as i64
}

// ---------------------------------------------------------------------------
// geometry (ported from hyprmax/geometry.hpp)
// ---------------------------------------------------------------------------

#[allow(clippy::similar_names)]
fn bounded_restore(box_: Box4, wa: ffi::hl_box_t, min: ffi::hl_box_t, max: ffi::hl_box_t) -> Box4 {
    let minx = (1.0_f64).max(if min.w.is_finite() { min.w } else { 1.0 });
    let miny = (1.0_f64).max(if min.h.is_finite() { min.h } else { 1.0 });
    let max_of = |raw: f64, lo: f64, wa_extent: f64| -> f64 {
        let up = if raw.is_finite() { raw } else { f64::MAX };
        lo.max(up.min((1.0_f64).max(wa_extent)))
    };
    let maxx = max_of(max.w, minx, wa.w);
    let maxy = max_of(max.h, miny, wa.h);
    let mut b = box_;
    b.w = b.w.clamp(minx, maxx);
    b.h = b.h.clamp(miny, maxy);
    b.x = b.x.clamp(wa.x, wa.x.max(wa.x + wa.w - b.w));
    b.y = b.y.clamp(wa.y, wa.y.max(wa.y + wa.h - b.h));
    b
}

fn remember(state: &mut MaxState, cls: &str, b: Box4) {
    if cls.is_empty() || !b.is_real() {
        return;
    }
    if state.last_windowed.get(cls).is_none_or(|old| *old != b) {
        state.last_windowed.insert(cls.to_string(), b);
        save_windowed(state);
    }
}

/// The toplevel's min/max size (with sane fallbacks).
fn minmax(ctx: ffi::Ctx, w: &ffi::WindowHandle) -> (ffi::hl_box_t, ffi::hl_box_t) {
    ffi::min_max_size(ctx, w).unwrap_or((
        ffi::hl_box_t {
            x: 0.0,
            y: 0.0,
            w: DEFAULT_MIN,
            h: DEFAULT_MIN,
        },
        ffi::hl_box_t {
            x: 0.0,
            y: 0.0,
            w: 1e9,
            h: 1e9,
        },
    ))
}

// ---------------------------------------------------------------------------
// init / exit
// ---------------------------------------------------------------------------

pub fn init(state: &State) {
    let mut m = crate::probe::max_lock(state);
    load_windowed(&mut m);
}

pub fn exit(state: &State) {
    let mut m = crate::probe::max_lock(state);
    save_windowed(&mut m);
}

// ---------------------------------------------------------------------------
// window helpers
// ---------------------------------------------------------------------------

/// The monitor a window sits on (its center), for the workarea query.
fn window_workarea(ctx: ffi::Ctx, w: &ffi::WindowHandle) -> Option<ffi::hl_box_t> {
    let info = ffi::window_full(ctx, w)?;
    let cx = info.at.x + info.at.w / 2.0;
    let cy = info.at.y + info.at.h / 2.0;
    let mon = ffi::monitor_at(ctx, cx, cy)?;
    ffi::monitor_workarea(ctx, &mon)
}

fn maximized_any(ctx: ffi::Ctx, w: &ffi::WindowHandle) -> bool {
    let Some(info) = ffi::window_full(ctx, w) else {
        return false;
    };
    info.fullscreen != ffi::FS_NONE
}

// ---------------------------------------------------------------------------
// adopt + toggle (the deferred work, drained by the jobs)
// ---------------------------------------------------------------------------

fn adopt(ctx: ffi::Ctx, m: &mut MaxState, w: &ffi::WindowHandle) {
    let Some(info) = ffi::window_full(ctx, w) else {
        return;
    };
    // only a floating, client-only (internal maximized) grant is adopted
    if info.fullscreen != ffi::FS_MAXIMIZED || !info.floating {
        return;
    }
    if m.maximized.contains_key(&ffi::window_id(ctx, w)) {
        return;
    }
    let Some(wa) = window_workarea(ctx, w) else {
        return;
    };
    // dissolve the compositor grant into the plugin model (slot freed)
    ffi::window_set_fs_mode(ctx, w, ffi::FS_NONE, ffi::FS_NONE);
    ffi::window_reset_client_size_grant(ctx, w);
    ffi::window_set_born_fullscreen(ctx, w, false);

    let restore =
        m.last_windowed
            .get(&info.app_id)
            .copied()
            .map_or(Box4::new(0.0, 0.0, 0.0, 0.0), |b| {
                let (mn, mx) = minmax(ctx, w);
                bounded_restore(b, wa, mn, mx)
            });

    m.maximized.insert(
        ffi::window_id(ctx, w),
        (w.clone_handle(), restore, info.app_id.clone()),
    );
    let _ = ffi::window_set_toplevel_maximized(ctx, w, true);
    let _ = ffi::window_set_geom(ctx, w, wa.x, wa.y, wa.w, wa.h);
    let _ = ffi::window_send_window_size(ctx, w, true);
}

fn apply_toggle(ctx: ffi::Ctx, m: &mut MaxState, w: &ffi::WindowHandle) {
    let Some(info) = ffi::window_full(ctx, w) else {
        return;
    };
    let id = ffi::window_id(ctx, w);
    if id == 0 {
        return;
    }
    // maximized and fullscreen are independent: never touch a real fullscreen
    if info.fullscreen == ffi::FS_FULLSCREEN {
        return;
    }

    // Compositor-maximized (born maximized, app request): native unmax. A
    // remembered windowed box beats the client's answer (GTK forgets its
    // normal geometry across restarts).
    if info.fullscreen == ffi::FS_MAXIMIZED {
        ffi::window_set_fs_mode(ctx, w, ffi::FS_NONE, ffi::FS_NONE);
        let wa = info.floating.then(|| window_workarea(ctx, w)).flatten();
        if let (Some(b), Some(wa)) = (m.last_windowed.get(&info.app_id).copied(), wa) {
            let (mn, mx) = minmax(ctx, w);
            let r = bounded_restore(b, wa, mn, mx);
            ffi::window_reset_client_size_grant(ctx, w);
            let _ = ffi::window_set_geom(ctx, w, r.x, r.y, r.w, r.h);
            let _ = ffi::window_send_window_size(ctx, w, true);
        }
        m.maximized.remove(&id);
        return;
    }

    let Some(wa) = window_workarea(ctx, w) else {
        return;
    };

    if let Some((_, stored, cls)) = m.maximized.get(&id) {
        let stored = *stored;
        let cls = cls.clone();
        m.maximized.remove(&id);
        let _ = ffi::window_set_toplevel_maximized(ctx, w, false);
        if stored.is_real() {
            let (mn, mx) = minmax(ctx, w);
            let r = bounded_restore(stored, wa, mn, mx);
            remember(m, &cls, r);
            let _ = ffi::window_set_geom(ctx, w, r.x, r.y, r.w, r.h);
        } else {
            // adopted with no remembered box: the client picks its size
            let _ = ffi::window_request_client_size(ctx, w);
        }
        return;
    }

    // maximize: remember the current windowed box, size to the workarea
    let cur = Box4::from_hl(info.at);
    m.maximized
        .insert(id, (w.clone_handle(), cur, info.app_id.clone()));
    remember(m, &info.app_id, cur);
    let _ = ffi::window_set_toplevel_maximized(ctx, w, true);
    let _ = ffi::window_set_geom(ctx, w, wa.x, wa.y, wa.w, wa.h);
    let _ = ffi::window_raise(ctx, w);
}

// ---------------------------------------------------------------------------
// reflow (maximized windows follow the workarea across layout changes)
// ---------------------------------------------------------------------------

fn do_reflow(ctx: ffi::Ctx, m: &mut MaxState) {
    // snapshot the ids (the map may mutate as handles expire)
    let ids: Vec<u64> = m.maximized.keys().copied().collect();
    for id in ids {
        let Some((w, _, _)) = m.maximized.get(&id) else {
            continue;
        };
        let Some(info) = ffi::window_full(ctx, w) else {
            m.maximized.remove(&id);
            continue;
        };
        // a native fullscreen/maximize grant owns the geometry
        if info.fullscreen != ffi::FS_NONE {
            continue;
        }
        let Some(wa) = window_workarea(ctx, w) else {
            continue;
        };
        if (info.at.x - wa.x).abs() < 0.5 && (info.at.y - wa.y).abs() < 0.5 {
            continue;
        }
        let _ = ffi::window_set_geom(ctx, w, wa.x, wa.y, wa.w, wa.h);
    }
}

// ---------------------------------------------------------------------------
// input (the immovable-maximized swallow)
// ---------------------------------------------------------------------------

/// Handles a mouse-button event. Returns true to CANCEL (swallow) it.
pub fn on_button(ctx: ffi::Ctx, m: &mut MaxState, ev: &ffi::SafeEvent) -> bool {
    // emissions precede the compositor's lock handling: locked/captured input
    // belongs elsewhere — reset the half-tracked swallow mask there.
    if ffi::session_locked(ctx) || ffi::input_capture_active(ctx) {
        m.swallowed = 0;
        return false;
    }

    let bit = ffi::tracked_button_bit(ev.button);

    if ev.state == 1
    /* WL_POINTER_BUTTON_STATE_PRESSED */
    {
        if bit == 0 {
            return false;
        }
        // only a Super-grab can move a window, so only that needs swallowing
        if ffi::native_pointer_grab(ctx) || ffi::native_layer_at(ctx) || !ffi::super_held(ctx) {
            return false;
        }
        let Some(w) = ffi::window_at(ctx, ev.x, ev.y) else {
            return false;
        };
        if !maximized_any(ctx, &w) {
            return false;
        }
        // immovable: swallow the press before the keybind layer starts a drag
        m.swallowed |= bit;
        return true;
    }

    if bit != 0 && (m.swallowed & bit) != 0 {
        m.swallowed &= !bit;
        return true;
    }
    false
}

// ---------------------------------------------------------------------------
// events
// ---------------------------------------------------------------------------

/// Drop expired entries (the window was destroyed) and remember their boxes.
/// The destroy event may carry a null window (the ref is mid-destruction), so
/// this scans for expired handles rather than trusting the event's window.
pub fn on_destroy(ctx: ffi::Ctx, m: &mut MaxState) {
    let to_remove: Vec<u64> = m
        .maximized
        .iter()
        .filter(|(_, (w, _, _))| ffi::window_id(ctx, w) == 0)
        .map(|(id, _)| *id)
        .collect();
    for id in to_remove {
        if let Some((_, b, cls)) = m.maximized.remove(&id) {
            remember(m, &cls, b);
        }
    }
}

/// window.fullscreen: reassert the plugin-max client bit, or adopt a new
/// compositor grant; always queue a reflow.
pub fn on_fullscreen(ctx: ffi::Ctx, m: &mut MaxState, ev: &ffi::SafeEvent) {
    let Some(w) = &ev.window else {
        queue_reflow(m, ctx);
        return;
    };
    let id = ffi::window_id(ctx, w);
    if m.maximized.contains_key(&id) {
        // the compositor recomputes the client bit from ITS fullscreen mode on
        // every change — reassert ours so it never flickers.
        let _ = ffi::window_set_toplevel_maximized(ctx, w, true);
        queue_reflow(m, ctx);
        return;
    }
    if let Some(info) = ffi::window_full(ctx, w)
        && info.floating
        && info.fullscreen == ffi::FS_MAXIMIZED
    {
        queue_adopt(m, ctx, w);
    }
    queue_reflow(m, ctx);
}

/// A layout-affecting event (workspace/monitor change): queue a reflow.
pub fn on_reflow_trigger(m: &mut MaxState, ctx: ffi::Ctx) {
    queue_reflow(m, ctx);
}

// ---------------------------------------------------------------------------
// deferred work (queue+drain; one job per kind at a time)
// ---------------------------------------------------------------------------

fn queue_toggle(m: &mut MaxState, ctx: ffi::Ctx, w: &ffi::WindowHandle) {
    if m.toggles.len() >= 16 {
        return;
    }
    m.toggles.push(w.clone_handle());
    if m.toggle_queued {
        return;
    }
    m.toggle_queued = true;
    crate::probe::arm_job(ctx, crate::probe::JOB_MAX_TOGGLE);
}

fn queue_adopt(m: &mut MaxState, ctx: ffi::Ctx, w: &ffi::WindowHandle) {
    m.adopts.push(w.clone_handle());
    if m.adopt_queued {
        return;
    }
    m.adopt_queued = true;
    crate::probe::arm_job(ctx, crate::probe::JOB_MAX_ADOPT);
}

fn queue_reflow(m: &mut MaxState, ctx: ffi::Ctx) {
    if m.reflow_queued {
        return;
    }
    m.reflow_queued = true;
    crate::probe::arm_job(ctx, crate::probe::JOB_MAX_REFLOW);
}

/// Drain the toggle queue (fired by `JOB_MAX_TOGGLE`).
pub fn drain_toggles(ctx: ffi::Ctx, m: &mut MaxState) {
    m.toggle_queued = false;
    let q = std::mem::take(&mut m.toggles);
    for w in &q {
        apply_toggle(ctx, m, w);
    }
}

/// Drain the adopt queue (fired by `JOB_MAX_ADOPT`).
pub fn drain_adopts(ctx: ffi::Ctx, m: &mut MaxState) {
    m.adopt_queued = false;
    let q = std::mem::take(&mut m.adopts);
    for w in &q {
        adopt(ctx, m, w);
    }
}

/// The reflow (fired by `JOB_MAX_REFLOW`).
pub fn do_reflow_job(ctx: ffi::Ctx, m: &mut MaxState) {
    m.reflow_queued = false;
    do_reflow(ctx, m);
}

/// `hl.plugin.hyprmax.toggle()` — queue a toggle for the focused window.
pub fn toggle_focused(ctx: ffi::Ctx, m: &mut MaxState) {
    let Some(w) = ffi::focus_window(ctx) else {
        return;
    };
    queue_toggle(m, ctx, &w);
}

// The Lua body (safe; the `unsafe extern "C"` wrapper lives in ffi.rs so the
// `unsafe_code = deny` boundary stays in one module). No args (operates on
// the focused window). A panic can never unwind across the C boundary.
pub fn lua_toggle_impl() -> i32 {
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if let Some(state) = crate::probe::state() {
            let ctx = state.ctx;
            let mut m = crate::probe::max_lock(state);
            toggle_focused(ctx, &mut m);
        }
    }));
    0
}
