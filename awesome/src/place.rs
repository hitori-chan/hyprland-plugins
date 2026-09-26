// hyprplace — spawn placement for floating windows, ported to the cabi.
//
// 1. an app reopens where its last window closed (per class, persisted):
//    a new window is born at the remembered size, and the remembered spot
//    lands when it's free — a sibling sitting on it sends the newcomer to 2
// 2. otherwise the least-overlap spot (KWin's default): a lone window keeps
//    the centered spot, a busy screen fills the gaps top-left first, a full
//    one hides where it can. No cascade, no center pile.
//
// A fixed-size native toplevel (min == max — a dialog, a splash) keeps the
// compositor's centered spot and never reads or writes the class row (either
// direction). X11 and parent-anchored windows keep their own spot while it's
// free. A maximized (hyprmax) or workarea-filling window consumes no free
// space. The result is clamped fully on-screen, border included, unless the
// window is too big to fit.
//
// Placement is deferred out of the map emission (queue + one-shot job):
// several windows can map in one dispatch and the queue survives a re-arm.
// The close-box is remembered synchronously on window.close (a strong ref,
// still live) — a maximized/fullscreen/workarea-filling close-box is the
// workarea, not a spot, and is not remembered.

use std::collections::HashMap;

use crate::ffi;
use crate::probe::State;

const MAX_PLACE_QUEUE: usize = 256;
const MAX_RESTORE_AXIS: f64 = 16384.0;

// Mirrors max.rs's Box4 (a full window box: position + size). Kept local so
// the two policy modules stay independent.
#[derive(Clone, Copy, PartialEq, Debug)]
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
    fn size(self) -> (f64, f64) {
        (self.w, self.h)
    }
    fn is_real(&self) -> bool {
        self.w > 5.0 && self.h > 5.0
    }
}

pub struct PlaceState {
    // app class -> last windowed box (persisted)
    last_spot: HashMap<String, Box4>,
    // windows that mapped in one dispatch (queue + drain in one job)
    queue: Vec<ffi::WindowHandle>,
    queued: bool,
}

impl PlaceState {
    pub fn new() -> Self {
        Self {
            last_spot: HashMap::new(),
            queue: Vec::new(),
            queued: false,
        }
    }
}

// ---------------------------------------------------------------------------
// persistence (TSV: x\ty\tw\th\tclass — the C++ hyprplace format)
// ---------------------------------------------------------------------------

fn store_path() -> std::path::PathBuf {
    let base = std::env::var("XDG_STATE_HOME").unwrap_or_else(|_| {
        std::env::var("HOME")
            .map(|h| format!("{h}/.local/state"))
            .unwrap_or_default()
    });
    std::path::Path::new(&base)
        .join("hyprplace")
        .join("lastspot.tsv")
}

fn load_spots(state: &mut PlaceState) {
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
            state.last_spot.insert(cls.to_string(), b);
        }
    }
}

fn save_spots(state: &mut PlaceState) {
    let path = store_path();
    if let Some(parent) = path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    let mut out = String::new();
    for (cls, b) in &state.last_spot {
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
// init / exit
// ---------------------------------------------------------------------------

pub fn init(state: &State) {
    let mut p = crate::probe::place_lock(state);
    load_spots(&mut p);
}

pub fn exit(state: &State) {
    let mut p = crate::probe::place_lock(state);
    save_spots(&mut p);
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

/// A genuinely user-resizable toplevel (min != max in at least one axis). A
/// fixed-size dialog pins min == max in both. No toplevel (X11, unmapped) =
/// can't tell = treat as fixed.
fn resizable(ctx: ffi::Ctx, w: &ffi::WindowHandle) -> bool {
    let Some((mn, mx)) = ffi::min_max_size(ctx, w) else {
        return false;
    };
    let pinned_x = mx.w > 1.0 && mn.w >= mx.w;
    let pinned_y = mx.h > 1.0 && mn.h >= mx.h;
    !(pinned_x && pinned_y)
}

/// The window's workarea (via its monitor).
fn workarea_for(ctx: ffi::Ctx, w: &ffi::WindowHandle) -> Option<ffi::hl_box_t> {
    let mon = ffi::window_monitor(ctx, w)?;
    ffi::monitor_workarea(ctx, &mon)
}

fn covers_workarea(b: Box4, wa: ffi::hl_box_t) -> bool {
    b.x <= wa.x && b.y <= wa.y && b.x + b.w >= wa.x + wa.w && b.y + b.h >= wa.y + wa.h
}

/// Clamp the restored size to `[min, max]` bounded by the workarea (the
/// remembered-size restore). Position is clamped separately (`clamp_to_wa`).
fn bounded_restore_size(
    ctx: ffi::Ctx,
    w: &ffi::WindowHandle,
    desired: Box4,
    wa: Option<ffi::hl_box_t>,
) -> Box4 {
    let (mn, mx) = ffi::min_max_size(ctx, w).unwrap_or((
        ffi::hl_box_t {
            x: 0.0,
            y: 0.0,
            w: 1.0,
            h: 1.0,
        },
        ffi::hl_box_t {
            x: 0.0,
            y: 0.0,
            w: 1e9,
            h: 1e9,
        },
    ));
    let mut d = desired;
    if !d.w.is_finite() || !d.h.is_finite() {
        d = Box4::new(0.0, 0.0, 0.0, 0.0);
    }
    let min_w = 1.0_f64.max(if mn.w.is_finite() { mn.w } else { 1.0 });
    let min_h = 1.0_f64.max(if mn.h.is_finite() { mn.h } else { 1.0 });
    // xdg-shell represents an unconstrained maximum as zero; do not mistake
    // that sentinel for a one-pixel upper bound.
    let max_of = |raw: f64, lo: f64, wa_extent: f64| -> f64 {
        let up = if raw.is_finite() && raw > 1.0 {
            raw.min(MAX_RESTORE_AXIS)
        } else {
            MAX_RESTORE_AXIS
        };
        let cap = if wa_extent.is_finite() {
            1.0_f64.max(wa_extent)
        } else {
            MAX_RESTORE_AXIS
        };
        lo.max(up.min(cap))
    };
    let (wa_w, wa_h) = match wa {
        Some(b) => (b.w, b.h),
        None => (f64::MAX, f64::MAX),
    };
    let max_w = max_of(mx.w, min_w, wa_w);
    let max_h = max_of(mx.h, min_h, wa_h);
    d.w = d.w.clamp(min_w, max_w);
    d.h = d.h.clamp(min_h, max_h);
    d
}

// ---------------------------------------------------------------------------
// placement
// ---------------------------------------------------------------------------

/// The window's workspace id (0 if expired/detached).
fn ws_id(ctx: ffi::Ctx, w: &ffi::WindowHandle) -> u32 {
    ffi::window_workspace(ctx, w).map_or(0, |x| ffi::workspace_id(ctx, &x))
}

#[allow(clippy::too_many_lines)] // a faithful port of the C++ placeWindow
fn place_window(ctx: ffi::Ctx, p: &mut PlaceState, w: &ffi::WindowHandle) {
    // X11 override-redirect surfaces (menus, tooltips) place themselves.
    if ffi::window_override_redirect(ctx, w) {
        return;
    }
    let Some(info) = ffi::window_place(ctx, w) else {
        return;
    };
    if !info.visible || !info.floating || info.fullscreen != ffi::FS_NONE {
        return;
    }
    // the open emission precedes the initial fullscreen/maximize application:
    // do not place a float while a grant is pending.
    if ffi::window_grant_exempt(ctx, w) {
        return;
    }

    let Some(wa) = workarea_for(ctx, w) else {
        return;
    };
    let cur = Box4::from_hl(info.box_);

    // a client-maximized (hyprmax) or workarea-filling window is not ours to
    // place or resize (symmetric with the close path).
    if ffi::window_told_maximized(ctx, w) || covers_workarea(cur, wa) {
        return;
    }

    let is_x11 = ffi::window_is_x11(ctx, w);
    let has_parent = ffi::window_has_parent(ctx, w);
    let cls = info.app_id.clone();
    let ws = ws_id(ctx, w);
    let mon = ffi::window_monitor(ctx, w);
    let w_id = ffi::window_id(ctx, w);

    // the visible floating windows to stay clear of; maximized/fullscreen and
    // workarea-filling ones cover no free space.
    let mut blockers: Vec<Box4> = Vec::new();
    for o in ffi::all_windows(ctx) {
        // the handle pointer differs per makeWindow, so identity is the window
        // address, not the handle (a self-block made the remembered spot
        // "occupied" and every spawn fell to least-overlap).
        if ffi::window_id(ctx, &o) == w_id {
            continue;
        }
        let Some(oi) = ffi::window_place(ctx, &o) else {
            continue;
        };
        if !oi.visible || !oi.floating {
            continue;
        }
        let same_ws = ws_id(ctx, &o);
        let o_mon = ffi::window_monitor(ctx, &o);
        let on_same_mon = match (&mon, &o_mon) {
            (Some(a), Some(b)) => a.as_raw() == b.as_raw(),
            _ => false,
        };
        if same_ws != ws && !(oi.pinned && on_same_mon) {
            continue;
        }
        if oi.fullscreen != ffi::FS_NONE || ffi::window_told_maximized(ctx, &o) {
            continue;
        }
        let ob = Box4::from_hl(oi.box_);
        if covers_workarea(ob, wa) {
            continue;
        }
        blockers.push(ob);
    }

    // the size the window spawns at: the client's own, unless this app is
    // resizable and a real size was remembered — then the remembered box is
    // applied whole, once.
    let resizable = resizable(ctx, w);
    // a fixed-size native toplevel (min == max) is a dialog/splash: it keeps
    // the compositor's native placement and stays out of the class memory in
    // both directions.
    if !resizable && !is_x11 && !has_parent {
        return;
    }
    let stored = if !is_x11 && !has_parent {
        p.last_spot.get(&cls).copied()
    } else {
        None
    };
    let (cur_w, cur_h) = cur.size();
    let mut size_w = cur_w;
    let mut size_h = cur_h;
    if let Some(s) = stored
        && resizable
        && s.w > 5.0
        && s.h > 5.0
    {
        let r = bounded_restore_size(ctx, w, s, Some(wa));
        size_w = r.w;
        size_h = r.h;
    }

    // no_offscreen: nudge the box fully into the workarea AND leave a border's
    // width of margin (the border is drawn outside the box). A window too big
    // to fit even without the margin drops it on that axis.
    let border = ffi::window_border_size(ctx, w);
    let mx = if size_w + 2.0 * border <= wa.w {
        border
    } else {
        0.0
    };
    let my = if size_h + 2.0 * border <= wa.h {
        border
    } else {
        0.0
    };
    let clamp_to_wa = |px: f64, py: f64| -> (f64, f64) {
        let lo_x = wa.x + mx;
        let hi_x = wa.x + wa.w - mx - size_w;
        let lo_y = wa.y + my;
        let hi_y = wa.y + wa.h - my - size_h;
        (
            px.clamp(lo_x, lo_x.max(hi_x)),
            py.clamp(lo_y, lo_y.max(hi_y)),
        )
    };
    let fits = |bx: f64, by: f64| -> bool {
        if bx < wa.x || by < wa.y || bx + size_w > wa.x + wa.w || by + size_h > wa.y + wa.h {
            return false;
        }
        for b in &blockers {
            if bx < b.x + b.w && bx + size_w > b.x && by < b.y + b.h && by + size_h > b.y {
                return false;
            }
        }
        true
    };

    let mut pos: Option<(f64, f64)> = None;

    if is_x11 || has_parent {
        // the window chose this spot (X11 geometry, parent-anchored dialog):
        // keep it while it's free.
        if fits(cur.x, cur.y) {
            return;
        }
    } else if let Some(s) = stored {
        // 1: where this app's last window closed, clamped on-screen so a spot
        // that ran past an edge is honored (against the edge) rather than lost.
        let (px, py) = clamp_to_wa(s.x, s.y);
        if fits(px, py) {
            pos = Some((px, py));
        }
    }

    // Memory missed (or its spot is taken): least-overlap placement. A
    // least-overlap top-left always sits at a grid point of the windows' own
    // edges (and the workarea corner), so score the window there and keep the
    // clearest — starting from, and so preferring, its current centered spot.
    if pos.is_none() {
        let overlap_at = |px: f64, py: f64, cutoff: f64| -> f64 {
            let mut sum = 0.0;
            for b in &blockers {
                let ix = (px + size_w).min(b.x + b.w) - px.max(b.x);
                let iy = (py + size_h).min(b.y + b.h) - py.max(b.y);
                if ix > 0.0 && iy > 0.0 {
                    sum += ix * iy;
                    if sum >= cutoff {
                        return sum;
                    }
                }
            }
            sum
        };

        let mut xs = vec![wa.x];
        let mut ys = vec![wa.y];
        for b in &blockers {
            xs.push(b.x);
            xs.push(b.x + b.w);
            ys.push(b.y);
            ys.push(b.y + b.h);
        }
        xs.sort_by(f64::total_cmp);
        ys.sort_by(f64::total_cmp);
        dedup(&mut xs);
        dedup(&mut ys);

        let (best_x, best_y) = clamp_to_wa(cur.x, cur.y);
        let mut best_ov = overlap_at(best_x, best_y, f64::INFINITY);
        for &x in &xs {
            if best_ov <= 1.0 {
                break; // a zero-overlap gap — nothing beats it
            }
            for &y in &ys {
                let (px, py) = clamp_to_wa(x, y);
                let ov = overlap_at(px, py, best_ov);
                if ov < best_ov - 1.0 {
                    best_ov = ov;
                    pos = Some((px, py));
                }
            }
        }
        if pos.is_none() {
            pos = Some((best_x, best_y));
        }
    }

    let (nx, ny) = clamp_to_wa(
        pos.unwrap_or((cur.x, cur.y)).0,
        pos.unwrap_or((cur.x, cur.y)).1,
    );

    if (nx - cur.x).abs() < 1e-9
        && (ny - cur.y).abs() < 1e-9
        && (size_w - cur_w).abs() < 1e-9
        && (size_h - cur_h).abs() < 1e-9
    {
        return;
    }
    // through the layout so the floating algorithm's lastBox tracking follows
    // the placement; the size change goes out as one ordinary configure.
    ffi::window_set_geom(ctx, w, nx, ny, size_w, size_h);
    if (size_w - cur_w).abs() > 1e-9 || (size_h - cur_h).abs() > 1e-9 {
        ffi::window_send_window_size(ctx, w, true);
    }
}

fn dedup(v: &mut Vec<f64>) {
    v.dedup_by(|a, b| (*a - *b).abs() < 1e-9);
}

// ---------------------------------------------------------------------------
// events
// ---------------------------------------------------------------------------

/// Queue a freshly-mapped window for placement (deferred out of the emission;
/// the queue survives a re-arm, so several maps in one dispatch all place).
pub fn on_open(ctx: ffi::Ctx, p: &mut PlaceState, w: &ffi::WindowHandle) {
    if p.queue.len() >= MAX_PLACE_QUEUE {
        return; // overload falls back to the compositor's native placement
    }
    p.queue.push(w.clone_handle());
    if p.queued {
        return;
    }
    p.queued = true;
    crate::probe::arm_job(ctx, crate::probe::JOB_PLACE);
}

/// Remember the close-box (synchronous — the window.close ref is still live).
pub fn on_close(ctx: ffi::Ctx, p: &mut PlaceState, w: &ffi::WindowHandle) {
    // X11 and dialogs place themselves and never consult the memory; a
    // fixed-size native window never owns the row, or its transient box would
    // clobber the app's remembered close-box.
    if ffi::window_is_x11(ctx, w) || ffi::window_has_parent(ctx, w) || !resizable(ctx, w) {
        return;
    }
    let Some(info) = ffi::window_place(ctx, w) else {
        return;
    };
    if ffi::window_told_maximized(ctx, w) || info.fullscreen != ffi::FS_NONE {
        return;
    }
    let Some(wa) = workarea_for(ctx, w) else {
        return;
    };
    if covers_workarea(Box4::from_hl(info.box_), wa) {
        return;
    }
    if !info.app_id.is_empty() {
        let b = Box4::from_hl(info.box_);
        if b.is_real() && p.last_spot.get(&info.app_id).is_none_or(|old| *old != b) {
            p.last_spot.insert(info.app_id.clone(), b);
            save_spots(p);
        }
    }
}

/// Drain the placement queue (fired by `JOB_PLACE`).
pub fn drain(ctx: ffi::Ctx, p: &mut PlaceState) {
    p.queued = false;
    let q = std::mem::take(&mut p.queue);
    for w in &q {
        place_window(ctx, p, w);
    }
}
