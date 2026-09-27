// The hyprsnap port (Phase 3): awful.mouse.snap, both behaviors.
//
// - magnetism: every motion of a floating move/resize drag pulls the window's
//   edges flush to the screen box, then the workarea, then the other visible
//   floaters' opposite edges, within `snap_distance`. Applied AFTER the
//   DragController has positioned the window for this motion (a deferred
//   job), so it wins the frame without fighting the controller.
// - aerosnap: the cursor within `edge` px of a screen edge arms that edge's
//   half slot; within `edge` of two edges, that corner's quarter. The armed
//   slot is previewed as an outline (drawn in the render pass) and the drop
//   commits it. Slots are halves/quarters of the workarea.
//
// A drag ends on button release OR the next key event (the keybind layer
// tears it down), so the commit hangs off both input streams.

// The geometry helpers mirror hyprsnap/geometry.hpp + snap.cpp, which use
// single-letter names (v/h edges, x/y, g the grabbed box); keep them for
// the line-by-line readability of the port.
#![allow(
    clippy::similar_names,
    clippy::many_single_char_names,
    clippy::cast_precision_loss,
    // the geometry compares exact f64 edge positions (mirrors the C++);
    // tolerance would change the snap behavior.
    clippy::float_cmp,
    clippy::collapsible_if
)]

use crate::ffi;
use crate::ffi::hl_box_t;
use std::ffi::c_void;

const V_NONE: i32 = 0;
const V_TOP: i32 = 1;
const V_BOTTOM: i32 = 2;
const H_NONE: i32 = 0;
const H_LEFT: i32 = 1;
const H_RIGHT: i32 = 2;
const MBIND_MOVE: i32 = 0;
const MBIND_RESIZE: i32 = 1;
const DEFAULT_MIN_WINDOW_SIZE: f64 = 20.0;

#[derive(Clone, Copy, PartialEq)]
struct B {
    x: f64,
    y: f64,
    w: f64,
    h: f64,
}

impl B {
    fn from_box(b: &hl_box_t) -> Self {
        B {
            x: b.x,
            y: b.y,
            w: b.w,
            h: b.h,
        }
    }
    fn to_box(self) -> hl_box_t {
        hl_box_t {
            x: self.x,
            y: self.y,
            w: self.w,
            h: self.h,
        }
    }
}

// The geometry helpers mirror hyprsnap/geometry.hpp + snap.cpp, which use
// single-letter names (v/h edges, x/y, g the grabbed box); keep them for
// the line-by-line readability of the port.
fn screen_edges(mb: &B, pos: (f64, f64), d: f64) -> (i32, i32) {
    let (mut v, mut h) = (V_NONE, H_NONE);
    if pos.0 >= mb.x && pos.0 - mb.x <= d {
        h = H_LEFT;
    } else if (mb.x + mb.w - pos.0).abs() <= d {
        h = H_RIGHT;
    }
    if pos.1 >= mb.y && pos.1 - mb.y <= d {
        v = V_TOP;
    } else if (mb.y + mb.h - pos.1).abs() <= d {
        v = V_BOTTOM;
    }
    (v, h)
}

#[derive(Clone, Copy)]
struct Col {
    r: f32,
    g: f32,
    b: f32,
    a: f32,
}

pub struct SnapState {
    // the armed aerosnap zone (constrained border box, global logical)
    zone_box: Option<B>,
    zone_mon: Option<ffi::MonitorHandle>,
    zone_v: i32,
    zone_h: i32,
    // the resize-drag begin box: tells dragged edges from anchored
    resize_start: Option<B>,
    // the last cursor position (captured in on_mouse_move; the deferred
    // magnetism job reads it — the cursor does not move between the motion
    // and the job, same event-loop iteration)
    cursor: (f64, f64),
    // magnetism is coalesced to one deferred run per dispatch
    magnet_queued: bool,
    // the config handles (registered in init)
    cfg_edge: *mut c_void,
    cfg_snap_dist: *mut c_void,
    cfg_col_frame: *mut c_void,
    // the outline color (computed once per config value, not per frame)
    col_raw: u64,
    col: Col,
    col_fill: Col,
}

impl SnapState {
    pub fn new() -> Self {
        Self {
            zone_box: None,
            zone_mon: None,
            zone_v: V_NONE,
            zone_h: H_NONE,
            resize_start: None,
            cursor: (0.0, 0.0),
            magnet_queued: false,
            cfg_edge: std::ptr::null_mut(),
            cfg_snap_dist: std::ptr::null_mut(),
            cfg_col_frame: std::ptr::null_mut(),
            col_raw: u64::MAX,
            col: Col {
                r: 1.0,
                g: 1.0,
                b: 1.0,
                a: 1.0,
            },
            col_fill: Col {
                r: 1.0,
                g: 1.0,
                b: 1.0,
                a: 0.10,
            },
        }
    }

    fn refresh_color(&mut self, ctx: ffi::Ctx) {
        if self.cfg_col_frame.is_null() {
            return;
        }
        let Some((_, raw, _)) = ffi::config_get(ctx, self.cfg_col_frame) else {
            return;
        };
        // the color is a packed ABGR64 (a small positive integer); the f64
        // read is exact for these magnitudes.
        #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
        let raw = raw as u64;
        if raw == self.col_raw {
            return;
        }
        self.col_raw = raw;
        // the packed ABGR64 (little-endian: A<<24 | B<<16 | G<<8 | R)
        let r = ((raw & 0xff) as f32) / 255.0;
        let g = (((raw >> 8) & 0xff) as f32) / 255.0;
        let b = (((raw >> 16) & 0xff) as f32) / 255.0;
        let a = (((raw >> 24) & 0xff) as f32) / 255.0;
        self.col = Col { r, g, b, a };
        self.col_fill = Col {
            r,
            g,
            b,
            a: a * 0.10,
        };
    }
}

// ---------------------------------------------------------------------------
// geometry helpers (mirrors hyprsnap/geometry.hpp + snap.cpp)
// ---------------------------------------------------------------------------

fn slot_for(v: i32, h: i32, wa: &B) -> Option<B> {
    let (hw, hh) = (wa.w / 2.0, wa.h / 2.0);
    if v != V_NONE && h != H_NONE {
        Some(B {
            x: if h == H_LEFT { wa.x } else { wa.x + hw },
            y: if v == V_TOP { wa.y } else { wa.y + hh },
            w: hw,
            h: hh,
        })
    } else if h != H_NONE {
        Some(B {
            x: if h == H_LEFT { wa.x } else { wa.x + hw },
            y: wa.y,
            w: hw,
            h: wa.h,
        })
    } else if v != V_NONE {
        Some(B {
            x: wa.x,
            y: if v == V_TOP { wa.y } else { wa.y + hh },
            w: wa.w,
            h: hh,
        })
    } else {
        None
    }
}

fn anchored_pos(start: f64, extent: f64, size: f64, to_end: bool) -> f64 {
    if to_end { start + extent - size } else { start }
}

fn constrained_slot(slot: &B, min: (f64, f64), max: (f64, f64), border: f64, h: i32, v: i32) -> B {
    let border = border.max(0.0);
    let (minx, miny) = (min.0.max(1.0), min.1.max(1.0));
    let (maxx, maxy) = (max.0.max(minx), max.1.max(miny));
    let wantw = (slot.w - 2.0 * border).max(1.0);
    let wanth = (slot.h - 2.0 * border).max(1.0);
    let width = wantw.clamp(minx, maxx) + 2.0 * border;
    let height = wanth.clamp(miny, maxy) + 2.0 * border;
    B {
        x: anchored_pos(slot.x, slot.w, width, h == H_RIGHT),
        y: anchored_pos(slot.y, slot.h, height, v == V_BOTTOM),
        w: width,
        h: height,
    }
}

#[derive(Clone, Copy)]
struct Nearest {
    found: bool,
    distance: f64,
    value: f64,
}

const NEAREST_INIT: Nearest = Nearest {
    found: false,
    distance: 0.0,
    value: 0.0,
};

fn consider_nearest(best: &mut Nearest, value: f64, current: f64, d: f64) {
    let dist = (value - current).abs();
    if dist >= d {
        return;
    }
    if best.found && (dist > best.distance || (dist == best.distance && value >= best.value)) {
        return;
    }
    best.found = true;
    best.distance = dist;
    best.value = value;
}

fn snap_inside_nearest(g: &mut B, sg: &B, d: f64) {
    let (mut x, mut y) = (NEAREST_INIT, NEAREST_INIT);
    if g.x > sg.x {
        consider_nearest(&mut x, sg.x, g.x, d);
    }
    if ((sg.x + sg.w) - (g.x + g.w)).abs() < d {
        consider_nearest(&mut x, sg.x + sg.w - g.w, g.x, d);
    }
    if g.y > sg.y {
        consider_nearest(&mut y, sg.y, g.y, d);
    }
    if ((sg.y + sg.h) - (g.y + g.h)).abs() < d {
        consider_nearest(&mut y, sg.y + sg.h - g.h, g.y, d);
    }
    if x.found {
        g.x = x.value;
    }
    if y.found {
        g.y = y.value;
    }
}

fn snap_outside_nearest(g: &mut B, others: &[B], d: f64) {
    let (mut x, mut y) = (NEAREST_INIT, NEAREST_INIT);
    for o in others {
        if g.x > o.x + o.w && g.x < o.x + o.w + d {
            consider_nearest(&mut x, o.x + o.w, g.x, d);
        } else if g.x + g.w < o.x && g.x + g.w > o.x - d {
            consider_nearest(&mut x, o.x - g.w, g.x, d);
        }
        if g.y > o.y + o.h && g.y < o.y + o.h + d {
            consider_nearest(&mut y, o.y + o.h, g.y, d);
        } else if g.y + g.h < o.y && g.y + g.h > o.y - d {
            consider_nearest(&mut y, o.y - g.h, g.y, d);
        }
    }
    if x.found {
        g.x = x.value;
    }
    if y.found {
        g.y = y.value;
    }
}

// ---------------------------------------------------------------------------
// the dragged target (mirrors floatingDragTarget)
// ---------------------------------------------------------------------------

fn dragged_target(ctx: ffi::Ctx, resize: bool) -> Option<ffi::WindowHandle> {
    let w = ffi::drag_target(ctx)?;
    let mode = ffi::drag_mode(ctx);
    let thresh = ffi::drag_threshold_reached(ctx);
    let tiled = ffi::drag_dragging_tiled(ctx);
    let drag_thresh = ffi::config_int(ctx, "binds:drag_threshold").unwrap_or(10);
    if !(thresh || drag_thresh <= 0) || tiled {
        return None;
    }
    if resize {
        if mode < MBIND_RESIZE {
            return None;
        }
    } else if mode != MBIND_MOVE {
        return None;
    }
    let info = ffi::window_info(ctx, &w)?;
    if !info.floating {
        return None;
    }
    Some(w)
}

fn client_limits(ctx: ffi::Ctx, w: &ffi::WindowHandle) -> (f64, f64, f64, f64) {
    let (min, max) = ffi::min_max_size(ctx, w).unwrap_or((
        hl_box_t {
            x: DEFAULT_MIN_WINDOW_SIZE,
            y: DEFAULT_MIN_WINDOW_SIZE,
            w: 0.0,
            h: 0.0,
        },
        hl_box_t {
            x: f64::MAX,
            y: f64::MAX,
            w: 0.0,
            h: 0.0,
        },
    ));
    let (minx, miny) = (min.x.max(1.0), min.y.max(1.0));
    (minx, miny, max.x.max(minx), max.y.max(miny))
}

// the other visible floaters on the monitor's active workspace (border boxes),
// excluding fullscreen
fn others_on_workspace(ctx: ffi::Ctx, mon: &ffi::MonitorHandle, border: f64) -> Vec<B> {
    let ws = ffi::monitor_active_workspace(ctx, mon);
    ffi::all_windows(ctx)
        .into_iter()
        .filter(|w| {
            let Some(info) = ffi::window_info(ctx, w) else {
                return false;
            };
            if !info.floating || !info.visible || ffi::window_is_fullscreen(ctx, w) {
                return false;
            }
            // on the monitor's active workspace
            match (&ws, ffi::window_workspace(ctx, w)) {
                (Some(a), Some(b)) => a.as_raw() == b.as_raw(),
                _ => true,
            }
        })
        .filter_map(|w| {
            let pos = ffi::window_target_position(ctx, &w)?;
            let p = B::from_box(&pos);
            Some(B {
                x: p.x - border,
                y: p.y - border,
                w: p.w + 2.0 * border,
                h: p.h + 2.0 * border,
            })
        })
        .collect()
}

// ---------------------------------------------------------------------------
// magnetism (the deferred run, applied after the controller positioned the
// window for this motion)
// ---------------------------------------------------------------------------

pub fn queue_magnet(ctx: ffi::Ctx, state: &mut SnapState) {
    let d = cfg_int(ctx, state.cfg_snap_dist, 8);
    if state.magnet_queued || d <= 0 {
        return;
    }
    state.magnet_queued = true;
    crate::probe::arm_job(ctx, crate::probe::JOB_SNAP_MAGNET);
}

#[allow(clippy::too_many_lines)] // the port keeps the C++ structure
pub fn do_magnet(ctx: ffi::Ctx, state: &mut SnapState) {
    state.magnet_queued = false;
    if ffi::session_locked(ctx) {
        return;
    }
    let d = cfg_int(ctx, state.cfg_snap_dist, 8) as f64;
    if d <= 0.0 {
        return;
    }
    let (cx, cy) = state.cursor;
    let Some(mon) = ffi::monitor_at(ctx, cx, cy) else {
        return;
    };
    let Some(screen_b) = ffi::monitor_logical_box(ctx, &mon) else {
        return;
    };
    let screen = B::from_box(&screen_b);
    let Some(wa_b) = ffi::monitor_workarea(ctx, &mon) else {
        return;
    };
    let workarea = B::from_box(&wa_b);
    let border = ffi::config_int(ctx, "general:border_size")
        .unwrap_or(2)
        .max(0) as f64;
    let others = others_on_workspace(ctx, &mon, border);

    if let Some(w) = dragged_target(ctx, false) {
        let Some(cur_b) = ffi::window_target_position(ctx, &w) else {
            return;
        };
        let cur = B::from_box(&cur_b);
        let mut g = B {
            x: cur.x - border,
            y: cur.y - border,
            w: cur.w + 2.0 * border,
            h: cur.h + 2.0 * border,
        };
        snap_inside_nearest(&mut g, &screen, d);
        snap_inside_nearest(&mut g, &workarea, d);
        snap_outside_nearest(&mut g, &others, d);
        g.x += border;
        g.y += border;
        if g.x != cur.x || g.y != cur.y {
            ffi::window_set_position_global(
                ctx,
                &w,
                hl_box_t {
                    x: g.x,
                    y: g.y,
                    w: cur.w,
                    h: cur.h,
                },
            );
            ffi::window_warp_position_size(ctx, &w);
        }
        return;
    }

    if let Some(w) = dragged_target(ctx, true) {
        let Some(resize_start) = state.resize_start else {
            return;
        };
        let Some(cur_b) = ffi::window_target_position(ctx, &w) else {
            return;
        };
        let cur = B::from_box(&cur_b);
        let el = cur.x != resize_start.x;
        let er = cur.x + cur.w != resize_start.x + resize_start.w;
        let et = cur.y != resize_start.y;
        let eb = cur.y + cur.h != resize_start.y + resize_start.h;
        if !(el || er || et || eb) {
            return;
        }
        let mut g = B {
            x: cur.x - border,
            y: cur.y - border,
            w: cur.w + 2.0 * border,
            h: cur.h + 2.0 * border,
        };
        let pull_inside = |g: &mut B, sg: &B| {
            if el && (g.x - sg.x).abs() < d {
                g.w += g.x - sg.x;
                g.x = sg.x;
            }
            if er && ((sg.x + sg.w) - (g.x + g.w)).abs() < d {
                g.w = sg.x + sg.w - g.x;
            }
            if et && (g.y - sg.y).abs() < d {
                g.h += g.y - sg.y;
                g.y = sg.y;
            }
            if eb && ((sg.y + sg.h) - (g.y + g.h)).abs() < d {
                g.h = sg.y + sg.h - g.y;
            }
        };
        pull_inside(&mut g, &screen);
        pull_inside(&mut g, &workarea);
        let (minx, miny, maxx, maxy) = client_limits(ctx, &w);
        let res = B {
            x: g.x + border,
            y: g.y + border,
            w: (g.w - 2.0 * border).max(1.0),
            h: (g.h - 2.0 * border).max(1.0),
        };
        let (size_w, size_h) = (res.w.clamp(minx, maxx), res.h.clamp(miny, maxy));
        let mut newpos = res;
        if el && !er {
            newpos.x += res.w - size_w;
        }
        if et && !eb {
            newpos.y += res.h - size_h;
        }
        let final_box = hl_box_t {
            x: newpos.x,
            y: newpos.y,
            w: size_w,
            h: size_h,
        };
        if final_box.x != cur.x
            || final_box.y != cur.y
            || final_box.w != cur.w
            || final_box.h != cur.h
        {
            ffi::window_set_position_global(ctx, &w, final_box);
            ffi::window_warp_position_size(ctx, &w);
        }
    }
}

// ---------------------------------------------------------------------------
// aerosnap (the edge zones + the preview)
// ---------------------------------------------------------------------------

// read one INT config value by handle (def when unregistered / unreadable).
#[allow(clippy::cast_possible_truncation)]
fn cfg_int(ctx: ffi::Ctx, h: *mut c_void, def: i64) -> i64 {
    if h.is_null() {
        return def;
    }
    match ffi::config_get(ctx, h) {
        Some((_, n, _)) => n as i64,
        None => def,
    }
}

fn damage_zone(ctx: ffi::Ctx, state: &SnapState) {
    let (Some(box_), Some(mon)) = (state.zone_box, state.zone_mon.as_ref()) else {
        return;
    };
    ffi::damage(
        ctx,
        mon,
        hl_box_t {
            x: box_.x - 2.0,
            y: box_.y - 2.0,
            w: box_.w + 4.0,
            h: box_.h + 4.0,
        },
    );
}

fn reset_zone(ctx: ffi::Ctx, state: &mut SnapState) {
    if state.zone_box.is_some() {
        damage_zone(ctx, state);
    }
    state.zone_box = None;
    state.zone_mon = None;
    state.zone_v = V_NONE;
    state.zone_h = H_NONE;
    state.resize_start = None;
}

fn constrained_zone(
    ctx: ffi::Ctx,
    w: &ffi::WindowHandle,
    mon: &ffi::MonitorHandle,
    v: i32,
    h: i32,
) -> Option<B> {
    let wa = ffi::monitor_workarea(ctx, mon).map(|b| B::from_box(&b))?;
    let slot = slot_for(v, h, &wa)?;
    let (minx, miny, maxx, maxy) = client_limits(ctx, w);
    let border = ffi::config_int(ctx, "general:border_size")
        .unwrap_or(2)
        .max(0) as f64;
    Some(constrained_slot(
        &slot,
        (minx, miny),
        (maxx, maxy),
        border,
        h,
        v,
    ))
}

pub fn on_mouse_move(ctx: ffi::Ctx, state: &mut SnapState, x: f64, y: f64) {
    state.cursor = (x, y);
    if ffi::session_locked(ctx) || ffi::input_capture_active(ctx) {
        reset_zone(ctx, state);
        return;
    }
    let Some(t) = dragged_target(ctx, false) else {
        if state.zone_box.is_some() {
            reset_zone(ctx, state);
        }
        if let Some(rt) = dragged_target(ctx, true) {
            if state.resize_start.is_none() {
                if let Some(pos) = ffi::window_target_position(ctx, &rt) {
                    state.resize_start = Some(B::from_box(&pos));
                }
            }
            queue_magnet(ctx, state);
        } else {
            state.resize_start = None;
        }
        return;
    };
    state.resize_start = None;

    let Some(mon) = ffi::monitor_containing(ctx, x, y) else {
        if state.zone_box.is_some() {
            reset_zone(ctx, state);
        }
        queue_magnet(ctx, state);
        return;
    };

    let Some(screen_b) = ffi::monitor_logical_box(ctx, &mon) else {
        queue_magnet(ctx, state);
        return;
    };
    let screen = B::from_box(&screen_b);
    let edge = cfg_int(ctx, state.cfg_edge, 16).max(1) as f64;
    let (v, h) = screen_edges(&screen, (x, y), edge);
    let slot = constrained_zone(ctx, &t, &mon, v, h);
    let changed = v != state.zone_v
        || h != state.zone_h
        || slot.is_some() != state.zone_box.is_some()
        || (slot.as_ref() != state.zone_box.as_ref());
    if changed {
        damage_zone(ctx, state);
        state.zone_box = slot;
        state.zone_mon = Some(mon.clone_handle());
        state.zone_v = v;
        state.zone_h = h;
        state.refresh_color(ctx);
        damage_zone(ctx, state);
    }
    queue_magnet(ctx, state);
}

pub fn on_input_ending_drag(ctx: ffi::Ctx, state: &mut SnapState) {
    if ffi::session_locked(ctx) || ffi::input_capture_active(ctx) {
        reset_zone(ctx, state);
        return;
    }
    let Some(t) = dragged_target(ctx, false) else {
        state.resize_start = None;
        return;
    };
    if state.zone_box.is_some() {
        let border = ffi::config_int(ctx, "general:border_size")
            .unwrap_or(2)
            .max(0) as f64;
        if let Some(mon) = state
            .zone_mon
            .as_ref()
            .map(ffi::MonitorHandle::clone_handle)
        {
            if let Some(final_box) = constrained_zone(ctx, &t, &mon, state.zone_v, state.zone_h) {
                ffi::window_set_position_global(
                    ctx,
                    &t,
                    hl_box_t {
                        x: final_box.x + border,
                        y: final_box.y + border,
                        w: final_box.w - 2.0 * border,
                        h: final_box.h - 2.0 * border,
                    },
                );
                ffi::window_warp_position_size(ctx, &t);
            }
        }
    }
    reset_zone(ctx, state);
}

// ---------------------------------------------------------------------------
// the render (the preview outline)
// ---------------------------------------------------------------------------

fn to_hl(c: Col) -> ffi::hl_color_t {
    ffi::hl_color_t {
        r: c.r,
        g: c.g,
        b: c.b,
        a: c.a,
    }
}

pub fn draw(cv: *mut ffi::hl_canvas, state: &crate::probe::State) {
    let ctx = state.ctx;
    let snap = state.snap.lock().unwrap();
    let Some(box_) = snap.zone_box else {
        return;
    };
    if ffi::session_locked(ctx) {
        return;
    }
    // the zone must be on the monitor being rendered
    let Some(cv_mon) = ffi::canvas_monitor(cv) else {
        return;
    };
    let Some(zmon) = snap.zone_mon.as_ref() else {
        return;
    };
    if cv_mon.as_raw() != zmon.as_raw() {
        return;
    }
    let box_ = box_.to_box();
    let (col, fill) = (to_hl(snap.col), to_hl(snap.col_fill));
    let bw = 1.0;
    ffi::canvas_rect(cv, box_, fill, 0, 1.0);
    ffi::canvas_rect(
        cv,
        hl_box_t {
            x: box_.x,
            y: box_.y,
            w: box_.w,
            h: bw,
        },
        col,
        0,
        1.0,
    );
    ffi::canvas_rect(
        cv,
        hl_box_t {
            x: box_.x,
            y: box_.y + box_.h - bw,
            w: box_.w,
            h: bw,
        },
        col,
        0,
        1.0,
    );
    ffi::canvas_rect(
        cv,
        hl_box_t {
            x: box_.x,
            y: box_.y + bw,
            w: bw,
            h: box_.h - 2.0 * bw,
        },
        col,
        0,
        1.0,
    );
    ffi::canvas_rect(
        cv,
        hl_box_t {
            x: box_.x + box_.w - bw,
            y: box_.y + bw,
            w: bw,
            h: box_.h - 2.0 * bw,
        },
        col,
        0,
        1.0,
    );
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------

pub fn init(ctx: ffi::Ctx, state: &mut SnapState, state_ptr: *mut crate::probe::State) {
    if let Some(h) = ffi::config_register(
        ctx,
        "plugin:snap:edge",
        "px from a screen edge that arms an aerosnap zone",
        ffi::HL_CFG_INT,
        16.0,
        "",
    ) {
        state.cfg_edge = h;
    }
    if let Some(h) = ffi::config_register(
        ctx,
        "plugin:snap:snap_distance",
        "px of magnetic pull between window and screen/client edges",
        ffi::HL_CFG_INT,
        8.0,
        "",
    ) {
        state.cfg_snap_dist = h;
    }
    // 0x77ffffff (a translucent white; the C++ default Theme::PRIMARY's frame
    // role) as the packed ABGR64, small enough to be exact in an f64.
    if let Some(h) = ffi::config_register(
        ctx,
        "plugin:snap:col_frame",
        "armed snap zone outline",
        ffi::HL_CFG_COLOR,
        2_013_265_919.0,
        "",
    ) {
        state.cfg_col_frame = h;
    }
    state.refresh_color(ctx);
    // the render listener (registered once; draw() no-ops when no zone armed)
    let _ = ffi::render_listen_snap(ctx, ffi::HL_RND_POST_WINDOWS, state_ptr.cast::<c_void>());
}
