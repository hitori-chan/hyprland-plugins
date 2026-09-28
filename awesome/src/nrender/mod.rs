// The drawing units: the banner column (popups.cpp) and the shade
// (center.cpp + row.cpp). ONE `layout` pass runs in two modes — the warm
// builds every texture and measures the heights, the draw paints from what
// the warm stored. The texture rule (crash class 4) runs through everything:
// rasters are requested unconditionally in layout, only the PAINTING is
// gated on the mode, and a draw that misses a raster flags a re-warm for
// after the frame.
//
// The C++ SPaint painted through the renderer; here the canvas calls are
// monitor-local logical px (the fork scales). Motion (the arrival springs,
// the shade open) is computed from steady time, so it needs no per-frame
// state.

use crate::ffi;
use crate::nicons;
use crate::notify::{self, Card, CardKind, NColors, NConfig, NotifyState, RBox};
use crate::nparse;
use crate::probe::State;

// the type roles off the font size (paint.cpp typeScale), physical pt
#[derive(Clone, Copy)]
struct T {
    header: i32,
    title: i32,
    body: i32,
    small: i32,
    action: i32,
    bar: i32,
}

fn type_scale(cfg: &NConfig, scale: f64) -> T {
    let fs = cfg.font_size as f64;
    let pt = |logical: f64| -> i32 { (logical * scale).round().max(1.0) as i32 };
    T {
        header: pt(fs - 1.0),
        title: pt(fs + 1.5),
        body: pt(fs + 0.5),
        small: pt(fs - 1.5),
        action: pt(fs + 0.5),
        bar: pt(fs + 0.5),
    }
}

// the glass·ink static chip fills (common/glass.hpp; theme.hpp tokens)
const FILL: NColors = NColors {
    r: 1.0,
    g: 1.0,
    b: 1.0,
    a: 0.0431,
}; // 0x0bffffff
const FILL2: NColors = NColors {
    r: 1.0,
    g: 1.0,
    b: 1.0,
    a: 0.0902,
}; // 0x17ffffff
const ACCENT_DIM: NColors = NColors {
    r: 0.1961,
    g: 0.8392,
    b: 1.0,
    a: 0.1608,
}; // 0x2932d6ff
const ON_ACCENT: NColors = NColors {
    r: 0.0275,
    g: 0.0863,
    b: 0.1098,
    a: 1.0,
}; // 0xff07161c
const BADGE_RIM: NColors = NColors {
    r: 0.9569,
    g: 0.9647,
    b: 0.9725,
    a: 1.0,
}; // 0xfff4f6f8

/// `animations:enabled` (the shell's motion kill switch).
pub fn animations_on(ctx: ffi::Ctx) -> bool {
    ffi::config_int(ctx, "animations:enabled").is_none_or(|v| v != 0)
}

/// The decoration blur (the glass samples it live).
pub fn blur_on(ctx: ffi::Ctx) -> bool {
    ffi::config_int(ctx, "decoration:blur:enabled").is_some_and(|v| v != 0)
}

// ---- motion curves (paint.cpp) ----

fn ease_out_cubic(t: f64) -> f64 {
    let u = 1.0 - t;
    1.0 - u * u * u
}
fn ease_out_back(t: f64) -> f64 {
    let u = t - 1.0;
    1.0 + 2.2 * u * u * u + 1.2 * u * u
}
/// 0..1 clamped progress of a spring that started at `at` (steady ms).
fn anim_t(at: u64, now: u64, ms: u32) -> f64 {
    ((now.saturating_sub(at)) as f64 / ms as f64).clamp(0.0, 1.0)
}

// the radius family (paint.cpp): physical px
fn r_panel(cfg: &NConfig, scale: f64) -> u32 {
    (((cfg.rounding.max(0)) as f64 + 6.0) * scale).round() as u32
}
fn r_row(cfg: &NConfig, scale: f64) -> u32 {
    ((cfg.rounding.max(0) as f64 - 2.0) * scale)
        .round()
        .max(0.0) as u32
}
fn r_joint(scale: f64) -> u32 {
    (notify::STACK_GAP * scale).round() as u32
}

// ---------------------------------------------------------------------------
// raster snapshots (the C++ drew through raw pointers; Rust copies the Arc
// out of the cache so a later cache write can't fight a live reader)
// ---------------------------------------------------------------------------

#[derive(Clone)]
struct Raster {
    tex: Option<std::sync::Arc<ffi::TextureHandle>>,
    w: u32,
    h: u32,
    links: Vec<(String, (f64, f64, f64, f64))>,
}

impl Raster {
    fn from(e: Option<&notify::CachedText>) -> Self {
        match e {
            Some(e) => Self {
                tex: e.tex.clone(),
                w: e.w,
                h: e.h,
                links: e.links.clone(),
            },
            None => Self {
                tex: None,
                w: 0,
                h: 0,
                links: Vec::new(),
            },
        }
    }
    /// The texture's height in logical px (0 for a missing raster — the C++
    /// `texH(null)` is 0).
    fn h_px(&self, scale: f64) -> f64 {
        self.tex.as_ref().map_or(0.0, |_| self.h as f64 / scale)
    }
    fn w_px(&self, scale: f64) -> f64 {
        self.tex.as_ref().map_or(0.0, |_| self.w as f64 / scale)
    }
}

// ---- the paint context ----

#[derive(Clone, Copy)]
struct P {
    ctx: ffi::Ctx,
    cv: *mut ffi::hl_canvas,
    mb: ffi::hl_box_t, // the monitor's GLOBAL logical box
    scale: f64,
    warm: bool, // the real warm: builds allowed, nothing paints
    alpha: f32,
    dy: f64,
    blur: bool,
    stale: bool, // a draw missed a warm-built raster
}

impl P {
    fn col(&self, c: NColors) -> ffi::hl_color_t {
        ffi::hl_color_t {
            r: c.r,
            g: c.g,
            b: c.b,
            a: (c.a * self.alpha).min(1.0),
        }
    }
    fn local(&self, g: &RBox) -> ffi::hl_box_t {
        ffi::hl_box_t {
            x: g.x - self.mb.x,
            y: g.y - self.mb.y + self.dy,
            w: g.w,
            h: g.h,
        }
    }
    fn rect(&self, g: RBox, c: NColors, round: u32, rp: f32) {
        if self.warm {
            return;
        }
        ffi::canvas_rect(self.cv, self.local(&g), self.col(c), round, rp);
    }
    fn glass(&self, g: RBox, c: NColors, round: u32, rp: f32) {
        if self.warm {
            return;
        }
        ffi::canvas_glass(self.cv, self.local(&g), self.col(c), round, rp, self.blur);
    }
    fn shadow(&self, g: RBox, round: u32, rp: f32, range: f64) {
        if self.warm {
            return;
        }
        ffi::canvas_shadow(
            self.cv,
            self.local(&g),
            round,
            rp,
            (range * self.scale).round().max(1.0) as u32,
            self.alpha,
        );
    }
    fn ring(&self, g: RBox, c: NColors, round: u32, rp: f32, px: f64) {
        if self.warm {
            return;
        }
        ffi::canvas_border(
            self.cv,
            self.local(&g),
            self.col(c),
            round,
            rp,
            (px * self.scale).round().max(1.0) as u32,
        );
    }
    /// A texture at its NATIVE pixel size (descaled to logical) at a logical
    /// position — the C++ `tex()`.
    fn tex(&self, r: &Raster, gx: f64, gy: f64) {
        if self.warm {
            return;
        }
        let Some(t) = r.tex.as_ref() else {
            return;
        };
        let (w, h) = t.size();
        if w == 0 || h == 0 {
            return;
        }
        ffi::canvas_texture(
            self.cv,
            t,
            ffi::hl_box_t {
                x: gx - self.mb.x,
                y: gy - self.mb.y + self.dy,
                w: w as f64 / self.scale,
                h: h as f64 / self.scale,
            },
            0,
            2.0,
            self.alpha,
        );
    }
    /// A texture fitted into a logical cell (optionally rounded).
    fn texfit(&self, r: &Raster, cell: RBox, round: u32, rp: f32) {
        if self.warm {
            return;
        }
        let Some(t) = r.tex.as_ref() else {
            return;
        };
        ffi::canvas_texture(self.cv, t, self.local(&cell), round, rp, self.alpha);
    }
}

// ---- the keyed raster lookups (text.cpp) ----

/// One cached-text lookup: the warm builds on a miss, the draw reads (a miss
/// flags the re-warm and paints nothing for that piece).
#[allow(clippy::needless_pass_by_value)] // key is an inline temporary at every call site; a move beats 25 reborrows
#[allow(clippy::too_many_arguments)]
fn tget(
    p: &mut P,
    cache: &mut notify::TextCache,
    font: &str,
    key: String,
    text: &str,
    col: NColors,
    pt: i32,
    max_w: i32,
    max_h: i32,
    line_sp: f32,
    weight: i32,
    link: Option<NColors>,
) -> Raster {
    let warm = p.warm;
    let ctx = p.ctx;
    let found = cache.get(&key, warm, || {
        let mt = ffi::markup_text(
            ctx,
            text,
            col.ffi(),
            pt as u32,
            font,
            max_w.max(1) as u32,
            max_h,
            line_sp,
            weight,
            link.map(super::notify::NColors::ffi),
            64,
        );
        match mt {
            Some(m) => {
                let links = m
                    .links
                    .iter()
                    .zip(m.hrefs.iter())
                    .map(|(r, h)| {
                        (
                            h.clone(),
                            (
                                r.x0 as f64,
                                r.y0 as f64,
                                (r.x1 - r.x0) as f64,
                                (r.y1 - r.y0) as f64,
                            ),
                        )
                    })
                    .collect();
                Some(notify::CachedText {
                    tex: Some(std::sync::Arc::new(m.tex)),
                    links,
                    w: m.w,
                    h: m.h,
                })
            }
            None => None,
        }
    });
    if found.is_none() && !warm {
        p.stale = true;
    }
    Raster::from(found)
}

/// The Material chevron (a stroked 45° pair, not a font glyph).
fn chevron(p: &mut P, cache: &mut notify::TextCache, dir: u32, col: NColors, px: i32) -> Raster {
    if px <= 4 {
        return Raster::from(None);
    }
    let key = format!("chevron|{}|{:x}|{}", dir, notify::color_hex(col), px);
    let warm = p.warm;
    let ctx = p.ctx;
    let found = cache.get(&key, warm, || {
        let t = ffi::chevron_texture(ctx, dir, col.ffi(), px as u32).map(std::sync::Arc::new);
        Some(notify::CachedText {
            tex: t,
            links: Vec::new(),
            w: px as u32,
            h: px as u32,
        })
    });
    if found.is_none() && !warm {
        p.stale = true;
    }
    Raster::from(found)
}

// ---------------------------------------------------------------------------
// the shared card recipes (paint.cpp)
// ---------------------------------------------------------------------------

fn has_lead_icon(n: &nparse::Notif) -> bool {
    (n.icon.tex.is_some() && !n.hero) || n.ident.tex.is_some()
}

/// Android's conversation icon column: the avatar leads, the app identity
/// badges its bottom-right corner.
fn paint_icon_column(p: &P, n: &nparse::Notif, cell: RBox, with_badge: bool, rp: f32) {
    let has_ident = n.ident.tex.is_some();
    let avatar = n.icon.tex.is_some() && !n.hero;
    let lead = if avatar { &n.icon } else { &n.ident };
    if lead.tex.is_none() {
        return;
    }
    // faces are round, app icons are squircles
    let round_face = avatar && n.conversation;
    let r = if round_face {
        cell.w / 2.0
    } else {
        cell.w * 10.0 / 44.0
    };
    let lrp = if round_face { 2.0 } else { rp };
    p.texfit(
        &Raster::from(Some(&lead_arc(lead))),
        cell,
        (r * p.scale).round() as u32,
        lrp,
    );

    if !with_badge || !avatar || !has_ident {
        return;
    }
    let d = cell.w * notify::BADGE_D;
    let in_ = d * notify::BADGE_INSET;
    let bb = RBox::new(
        cell.x + cell.w * (1.0 + notify::BADGE_PROT) - d,
        cell.y + cell.h * (1.0 + notify::BADGE_PROT) - d,
        d,
        d,
    );
    p.rect(bb, BADGE_RIM, (d / 2.0 * p.scale).round() as u32, 2.0);
    if n.ident.tex.is_some() {
        p.texfit(
            &Raster::from(Some(&lead_arc(&n.ident))),
            RBox::new(bb.x + in_, bb.y + in_, d - 2.0 * in_, d - 2.0 * in_),
            ((d / 2.0 - in_) * p.scale).round() as u32,
            2.0,
        );
    }
}

/// An IconTex's raster (a copy, so the icon's Arc stays shared).
fn lead_arc(it: &nparse::IconTex) -> notify::CachedText {
    notify::CachedText {
        tex: it.tex.clone(),
        links: Vec::new(),
        w: it.tex.as_ref().map_or(0, |t| t.size().0),
        h: it.tex.as_ref().map_or(0, |t| t.size().1),
    }
}

fn paint_progress(p: &P, cfg: &NConfig, x: f64, y: f64, w: f64, pct: i32, critical: bool) {
    let pr = (notify::PROGRESS_H / 2.0 * p.scale).round() as u32;
    p.rect(RBox::new(x, y, w, notify::PROGRESS_H), FILL2, pr, 2.0);
    if pct > 0 {
        let c = if critical {
            cfg.col_urgent
        } else {
            cfg.col_highlight
        };
        p.rect(
            RBox::new(
                x,
                y,
                (w * pct as f64 / 100.0).max(notify::PROGRESS_H),
                notify::PROGRESS_H,
            ),
            c,
            pr,
            2.0,
        );
    }
}

// the alt-line reservation, shared by the layout and the paint
fn alt_res(a: &Raster, scale: f64) -> f64 {
    if a.tex.is_some() {
        14.0_f64.max(a.h_px(scale))
    } else {
        14.0
    }
}

// a row's style: singles and bundle children are the same row in different

// ---------------------------------------------------------------------------
// the entries
// ---------------------------------------------------------------------------

/// One layout pass: the warm builds and measures; the draw paints (through
/// `cv`, non-null only in the draw).
pub fn layout(ctx: ffi::Ctx, st: &mut NotifyState, cfg: &NConfig, cv: *mut ffi::hl_canvas) {
    // the monitor: the canvas's in the draw, the focused one in the warm
    // (the draw paints only the monitor the warm measured)
    let mon = if cv.is_null() {
        if let Some(m) = ffi::focus_monitor(ctx) {
            m
        } else {
            st.cards.clear();
            st.last_content_h = 0.0;
            st.last_content_w = 0.0;
            return;
        }
    } else {
        match ffi::canvas_monitor(cv) {
            Some(m) => m,
            None => return,
        }
    };
    let Some((mb, scale)) = ffi::monitor_box(ctx, &mon) else {
        st.cards.clear();
        st.last_content_h = 0.0;
        st.last_content_w = 0.0;
        return;
    };
    let mut p = P {
        ctx,
        cv,
        mb,
        scale: scale as f64,
        warm: cv.is_null(),
        alpha: 1.0,
        dy: 0.0,
        blur: blur_on(ctx),
        stale: false,
    };
    st.cards.clear();
    st.cards_mon = Some(mon.clone_handle());
    if st.center_on {
        center::center(&mut p, st, cfg);
    } else {
        popups::popups(&mut p, st, cfg);
    }
    st.tex_stale = p.stale;
}

/// The draw entry (the render trampoline). `cv` is valid for this call only.
pub fn draw(cv: *mut ffi::hl_canvas, state: &State) {
    // a re-entered pass (forced by our own hold) paints nothing: the outer
    // pass is drawing the same state and holds the notify lock
    if crate::probe::in_section() {
        return;
    }
    let _sect = crate::probe::section_enter();
    let Some(mon) = ffi::canvas_monitor(cv) else {
        return;
    };
    let Ok(mut st) = state.notify.lock() else {
        return;
    };
    if !notify::anything_to_draw(&st) {
        return;
    }
    // the layout must have run on THIS monitor (the warm tracks it); a
    // monitor swap re-warms before painting anything
    if st
        .cards_mon
        .as_ref()
        .map_or(std::ptr::null_mut(), super::ffi::MonitorHandle::as_raw)
        != mon.as_raw()
    {
        st.warm_pending = true;
        return;
    }
    let cfg = notify::config(state.ctx, &st);
    layout(state.ctx, &mut st, &cfg, cv);
    if st.tex_stale {
        st.tex_stale = false;
        st.warm_pending = true;
    }
}

/// The pre-scanout hook (render.cpp onRenderPreChecks): while a visible card
/// (or the open center) is up, hold the workspace render so the card
/// composites over a fullscreen client. Self-healing: once the last card
/// clears, the compositor re-latches solitary and scanout re-engages.
pub fn prechecks(mon_raw: *mut ffi::hl_monitor, state: &State) {
    if crate::probe::in_section() {
        return;
    }
    let _sect = crate::probe::section_enter();
    let Ok(st) = state.notify.lock() else {
        return;
    };
    if !notify::anything_to_draw(&st) {
        return;
    }
    if ffi::session_locked(state.ctx) {
        return;
    }
    // the callback's monitor handle carries one ref; take ownership (Drop
    // releases it) and compare against the focused monitor
    let Some(mon) = ffi::monitor_from_raw(mon_raw) else {
        return;
    };
    let Some(fmon) = ffi::focus_monitor(state.ctx) else {
        return;
    };
    if fmon.as_raw() != mon.as_raw() {
        return;
    }
    drop(st);
    ffi::monitor_force_render(state.ctx, &fmon);
}

/// Anything animating? (the motion tick's armer)
pub fn animating(ctx: ffi::Ctx, st: &NotifyState) -> bool {
    if !animations_on(ctx) {
        return false;
    }
    let now = ffi::steady_ms();
    if st.center_on && st.animating {
        return true;
    }
    if !st.center_on {
        for n in &st.notifs {
            if !n.waiting
                && n.banner
                && now.saturating_sub(n.born) < notify::MOTION_SPATIAL_MS as u64
            {
                return true;
            }
        }
    }
    false
}

pub(crate) mod center;
pub(crate) mod popups;
pub(crate) mod row;
