// The Phase 1 mini-bar: proves the render pipeline end to end over the cabi
// canvas — a glass strip, one RGBA (CPU-pixel) icon texture, and one text
// (clock) texture that updates on a timer. It is the seed that Phases 2-5
// grow into the full bar.
//
// This module is safe: every raw-pointer / FFI call goes through `ffi`.
//
// Warm/draw gate (crash class 4): textures are only BUILT outside a frame
// (init and the clock timer — both event-loop context, never render context).
// draw() only PAINTS already-cached textures. The fork additionally no-ops a
// paint of a not-yet-uploaded texture (m_texID == 0), so even the frame that
// follows a build is safe.
use std::sync::{Mutex, MutexGuard};

use crate::ffi;
use crate::probe::State;

// Lock, recovering from poisoning. This is single-threaded (draw and tick
// never nest), so a poisoned lock is a spurious state — re-acquire the inner
// rather than panicking on every later frame.
#[allow(clippy::elidable_lifetime_names)]
fn lock<'a>(m: &'a Mutex<BarState>) -> MutexGuard<'a, BarState> {
    match m.lock() {
        Ok(g) => g,
        Err(p) => p.into_inner(),
    }
}

// Bar geometry (monitor-local logical px).
const BAR_H: f64 = 26.0;
const ICON_SIZE: f64 = 18.0;
const MARGIN: f64 = 8.0;
const CLOCK_PT: u32 = 11;
const ICON_PX: u32 = 24; // the icon texture's pixel size (scaled on blit)

// Colors (0..1 floats; XRGB8888 is opaque, so the icon has no alpha).
const STRIP: ffi::hl_color_t = ffi::hl_color_t {
    r: 0.09,
    g: 0.10,
    b: 0.13,
    a: 0.92,
};
const BORDER: ffi::hl_color_t = ffi::hl_color_t {
    r: 0.20,
    g: 0.22,
    b: 0.26,
    a: 0.9,
};
const ACCENT: ffi::hl_color_t = ffi::hl_color_t {
    r: 0.20,
    g: 0.60,
    b: 0.90,
    a: 0.85,
};
const ICON_R: u8 = 51;
const ICON_G: u8 = 153;
const ICON_B: u8 = 230;
const CLOCK_COL: ffi::hl_color_t = ffi::hl_color_t {
    r: 0.92,
    g: 0.94,
    b: 0.97,
    a: 1.0,
};

// The bar's mutable state. Lives behind a `Mutex` in `State`; draw and tick
// never nest (single-threaded compositor), so there is no contention.
pub struct BarState {
    pub mon: Option<ffi::MonitorHandle>,
    pub icon: Option<ffi::TextureHandle>,
    pub clock: Option<ffi::TextureHandle>,
    pub clock_key: String,
    pub bar_w: f64,
    pub bar_h: f64,
}

impl BarState {
    pub fn new() -> Self {
        Self {
            mon: None,
            icon: None,
            clock: None,
            clock_key: String::new(),
            bar_w: 0.0,
            bar_h: 0.0,
        }
    }
}

impl Default for BarState {
    fn default() -> Self {
        Self::new()
    }
}

// ---------------------------------------------------------------------------
// init — query the monitor, build the icon + first clock, register the render
// listener + the clock timer, and damage so the first frame renders.
// ---------------------------------------------------------------------------
pub fn init(ctx: ffi::Ctx, state: &State, state_ptr: *mut State) -> bool {
    // The RGBA icon: a solid colored square (XRGB8888, little-endian B,G,R,X).
    let icon_buf = icon_rgba();
    let icon = ffi::texture_from_rgba(ctx, &icon_buf, ICON_PX, ICON_PX, ICON_PX * 4);

    // The first clock texture (the timer refreshes it once the second rolls).
    let now = ffi::now_hms();
    let clock = ffi::text_texture(ctx, &now, CLOCK_COL, CLOCK_PT, 0, "");

    // The monitor may not be registered at plugin-load time (the nested output
    // comes up later); acquire it best-effort. draw() picks it up lazily from
    // the canvas if it is still missing, so a missing monitor here is not fatal.
    let mon = ffi::first_monitor(ctx);
    let mbox = mon
        .as_ref()
        .and_then(|m| ffi::monitor_box(ctx, m))
        .map(|(b, _s)| b);

    let mut bar = lock(&state.bar);
    bar.mon = mon;
    bar.icon = icon;
    bar.clock = clock;
    bar.clock_key = now;
    bar.bar_w = mbox.map_or(0.0, |b| b.w);
    bar.bar_h = mbox.map_or(0.0, |b| b.h);
    drop(bar);

    // Register the render callback (no monitor needed) and the clock timer.
    // The listener is fatal: without it the bar never paints.
    let ud = state_ptr.cast::<std::ffi::c_void>();
    let rl = ffi::render_listen(ctx, ffi::HL_RND_POST_WINDOWS, ud);
    if rl != 0 {
        ffi::log_str(ctx, ffi::LOG_ERR, "bar: render_listen failed");
        return false;
    }

    let arg = Box::leak(Box::new(ffi::JobArg {
        state: state_ptr,
        kind: crate::probe::JOB_BAR_TICK,
    }));
    ffi::timer(ctx, 1000, 1, arg);

    // Best-effort initial damage (no-ops when the monitor is not up yet); the
    // warmup frames paint the bar regardless.
    damage_strip(ctx, state);
    ffi::log_str(
        ctx,
        ffi::LOG_INFO,
        "bar: mini-bar up (strip + icon + clock, POST_WINDOWS)",
    );
    true
}

// ---------------------------------------------------------------------------
// draw — paints the bar. Called by the fork once per rendered frame at the
// selected stage. Only reads the cache (no texture builds here).
// ---------------------------------------------------------------------------
pub fn draw(cv: *mut ffi::hl_canvas, state: &State) {
    let Some((ext, scale)) = ffi::canvas_extent(cv) else {
        return;
    };

    let mut bar = lock(&state.bar);
    // Lazily acquire the monitor (for later damage) if init did not have it;
    // the canvas knows the monitor being rendered.
    if bar.mon.is_none() {
        bar.mon = ffi::canvas_monitor(cv);
    }
    let w = ext.w;
    // Phase 1: the mini-bar sits at the BOTTOM of the monitor so it never
    // overlaps the C++ hyprbar (top) while both are loaded (Phases 1-5).
    let top = ext.h - BAR_H;

    // The strip: full width, BAR_H tall, at the bottom (glass), with a 1px
    // border and a 2px accent line along the strip's top edge.
    let strip = ffi::hl_box_t {
        x: 0.0,
        y: top,
        w,
        h: BAR_H,
    };
    ffi::canvas_glass(cv, strip, STRIP, 0, 2.0, false);
    ffi::canvas_border(cv, strip, BORDER, 0, 2.0, 1);
    let accent = ffi::hl_box_t {
        x: 0.0,
        y: top,
        w,
        h: 2.0,
    };
    ffi::canvas_rect(cv, accent, ACCENT, 0, 1.0);

    // The icon: left, vertically centered in the strip.
    if let Some(icon) = bar.icon.as_ref() {
        let iy = top + (BAR_H - ICON_SIZE) / 2.0;
        let ibox = ffi::hl_box_t {
            x: MARGIN,
            y: iy,
            w: ICON_SIZE,
            h: ICON_SIZE,
        };
        ffi::canvas_texture(cv, icon, ibox);
    }

    // The clock: right-aligned.
    if let Some(clock) = bar.clock.as_ref() {
        let (tw, th) = clock.size();
        if tw > 0 && th > 0 {
            let scale = f64::from(scale);
            let twl = f64::from(tw) / scale; // logical width from the pixel width
            let thl = f64::from(th) / scale;
            let cx = w - MARGIN - twl;
            let cy = top + (BAR_H - thl) / 2.0;
            ffi::canvas_texture(
                cv,
                clock,
                ffi::hl_box_t {
                    x: cx,
                    y: cy,
                    w: twl,
                    h: thl,
                },
            );
        }
    }

    // Remember the size so the timer can damage the same region.
    bar.bar_w = w;
    bar.bar_h = ext.h;
}

// ---------------------------------------------------------------------------
// tick — the 1 s clock timer. Rebuilds the clock texture only when the string
// changes, then damages the strip so the next frame repaints it. Runs in
// event-loop context (never render), so building the texture is safe.
// ---------------------------------------------------------------------------
pub fn tick(ctx: ffi::Ctx, state: &State) {
    let now = ffi::now_hms();
    let mut bar = lock(&state.bar);
    if now == bar.clock_key {
        return; // the second hasn't rolled; nothing to repaint
    }
    // Build the texture from `now` (borrow) before moving it into clock_key.
    let tex = ffi::text_texture(ctx, &now, CLOCK_COL, CLOCK_PT, 0, "");
    bar.clock = tex;
    bar.clock_key = now;
    // Read-only after the rebuild; the guard is held across the (non-reentrant)
    // damage call, which is safe since neither re-enters the bar.
    if let Some(m) = bar.mon.as_ref() {
        let y = bar.bar_h - BAR_H;
        ffi::damage(
            ctx,
            m,
            ffi::hl_box_t {
                x: 0.0,
                y,
                w: bar.bar_w,
                h: BAR_H,
            },
        );
    }
}

// Damage the strip region (used at init before the first draw knows the width
// falls back to the monitor's logical width).
fn damage_strip(ctx: ffi::Ctx, state: &State) {
    let bar = lock(&state.bar);
    if let Some(m) = bar.mon.as_ref() {
        let y = bar.bar_h - BAR_H;
        ffi::damage(
            ctx,
            m,
            ffi::hl_box_t {
                x: 0.0,
                y,
                w: bar.bar_w,
                h: BAR_H,
            },
        );
    }
    drop(bar);
}

// A solid colored square (XRGB8888, little-endian: B,G,R,X per pixel).
fn icon_rgba() -> Vec<u8> {
    let mut buf = vec![0u8; (ICON_PX * ICON_PX * 4) as usize];
    for i in 0..(ICON_PX * ICON_PX) as usize {
        let o = i * 4;
        buf[o] = ICON_B;
        buf[o + 1] = ICON_G;
        buf[o + 2] = ICON_R;
        buf[o + 3] = 255;
    }
    buf
}
