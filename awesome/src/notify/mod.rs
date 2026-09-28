// The notify module: Android's notification system over the freedesktop
// spec. The model (what a card is, how it arrives, when it dies), the shade
// (one ranked list with the expansion budget), the warm/draw split, and the
// config snapshot. The drawing units (nrender.rs) paint from what the warm
// measured; the input (ninput.rs) defers every mutation off the emission.
//
// Port of the C++ hyprnotify: model.cpp + center.cpp + render.cpp + the
// config registration in main.cpp. The D-Bus service lives in bus.rs; the
// payload transforms in nparse.rs; the icons in nicons.rs.
//
// RESIDENCY is the model's central rule: a card's BANNER and the card itself
// have separate lifetimes. Expiry takes the banner and leaves the card in
// the shade; only a dismissal, an action or the cap takes the card.

use std::collections::{HashMap, HashSet};
use std::sync::Arc;

use crate::ffi;
use crate::nicons;
use crate::nparse::{self, Notif};

// ---- the OSD band (ids the scripts pin; replace-in-place, never grouped) ----
pub const OSD_LO: u32 = 9990;
pub const OSD_HI: u32 = 9999;
pub fn in_osd_band(id: u32) -> bool {
    (OSD_LO..=OSD_HI).contains(&id)
}

// NotificationClosed reasons (the spec's); 4 = undefined (the eviction)
pub const R_EXPIRED: u32 = 1;
pub const R_DISMISSED: u32 = 2;
pub const R_CLOSED: u32 = 3;
pub const R_UNDEFINED: u32 = 4;

// the shade layout (ui.hpp, logical px)
pub const EDGE: f64 = 10.0;
pub const PADX: f64 = 14.0;
pub const PADY: f64 = 11.0;
pub const ICON_GAP: f64 = 12.0;
pub const HEAD_GAP: f64 = 3.0;
pub const TITLE_GAP: f64 = 4.0;
pub const PROGRESS_H: f64 = 5.0;
pub const PROGRESS_GAP: f64 = 8.0;
pub const HERO_CAP: f64 = 110.0;
pub const HERO_TEXT_MIN: f64 = 60.0;
pub const BTN_H: f64 = 26.0;
pub const BTN_PADX: f64 = 10.0;
pub const BTN_GAP: f64 = 4.0;
pub const BTN_ROW_GAP: f64 = 6.0;
pub const BTN_ICON: f64 = 15.0;
pub const BTN_ICON_GAP: f64 = 5.0;
pub const BODYIMG_H: f64 = 96.0;
pub const IMG_GAP: f64 = 6.0;
pub const IMG_ROW_GAP: f64 = 8.0;
pub const XCIRC: f64 = 20.0;
pub const BADGE_D: f64 = 20.0 / 40.0;
pub const BADGE_PROT: f64 = 2.0 / 40.0;
pub const BADGE_INSET: f64 = 2.0 / 20.0;
pub const CENTER_W: f64 = 360.0;
pub const ROW_PADT: f64 = 9.0;
pub const ROW_PADX: f64 = 12.0;
pub const ROW_PADB: f64 = 10.0;
pub const ROW_ICON: f64 = 40.0;
pub const ROW_ICON_GAP: f64 = 10.0;
pub const CHEV: f64 = 24.0;
pub const CHILD_ICON: f64 = 28.0;
pub const CHILD_GAP: f64 = 2.0;
pub const PREV_ICON: f64 = 16.0;
pub const PILL_H: f64 = 20.0;
pub const BAR_BTN: f64 = 34.0;
pub const BAR_PADT: f64 = 4.0;
pub const BAR_PADX: f64 = 10.0;
pub const BAR_PADB: f64 = 12.0;
pub const BAR_GAP: f64 = 8.0;
pub const BODY_PADT: f64 = 10.0;
pub const BODY_PADX: f64 = 10.0;
pub const BODY_PADB: f64 = 10.0;
pub const STACK_GAP: f64 = 3.0;
pub const AUTOGROUP_AT: u32 = 4;
pub const PEEK_GRACE_MS: u32 = 400;
pub const MOTION_SPATIAL_MS: u32 = 320; // theme.hpp
pub const AGE_TICK_MS: u32 = 30_000;
pub const MOTION_TICK_MS: u32 = 16;
pub const DECODE_POLL_MS: u32 = 16;

// ---- config snapshot (read once per warm; the handles live in State) ----

#[derive(Clone, Copy, Default)]
#[allow(dead_code)] // every field is read by some surface, not all by one
pub struct NColors {
    pub r: f32,
    pub g: f32,
    pub b: f32,
    pub a: f32,
}

impl NColors {
    pub fn ffi(self) -> ffi::hl_color_t {
        ffi::hl_color_t {
            r: self.r,
            g: self.g,
            b: self.b,
            a: self.a,
        }
    }
    pub fn modify_a(self, a: f32) -> NColors {
        NColors { a, ..self }
    }
}

/// The live config snapshot (the C++ SNotifyConfig, read through the handles).
pub struct NConfig {
    pub font: String,
    pub font_size: i32,
    pub width: i32,
    pub max_height: i32,
    pub max_icon: i32,
    pub margin: i32,
    pub offset_y: i32,
    pub timeout_low: i32,
    pub timeout_normal: i32,
    pub coalesce: bool,
    pub rounding: i32,
    pub rounding_power: f32,
    pub max_notifs: i32,
    pub ignore_dbus_close: bool,
    pub col_bg: NColors,
    pub col_fg: NColors,
    pub col_title: NColors,
    pub col_kicker: NColors,
    pub col_frame: NColors,
    pub col_urgent: NColors,
    pub col_highlight: NColors,
    pub col_link: NColors,
    pub sound_command: String,
    pub fallback_icon_dir: String,
    // the hex keys the icon layer carries in its staleness keys
    pub fg_hex: u64,
    pub bg_hex: u64,
}

impl std::ops::Deref for NConfig {
    type Target = ();
    fn deref(&self) -> &() {
        &()
    }
}

// ---------------------------------------------------------------------------
// the text cache (the keyed raster cache; the C++ CGenCache with 24 warms of
// grace)
// ---------------------------------------------------------------------------

/// A cached raster: the texture (Arc-shared; the draw reads it, the LRU keeps
/// it), its links (physical px, texture-relative), and its pixel size.
pub struct CachedText {
    pub tex: Option<Arc<ffi::TextureHandle>>,
    pub links: Vec<(String, (f64, f64, f64, f64))>,
    pub w: u32,
    pub h: u32,
}

struct CacheEntry {
    entry: CachedText,
    last_warm: u64, // the warm generation that last wanted it
}

pub struct TextCache {
    map: HashMap<String, CacheEntry>,
    warm_gen: u64,
}

impl TextCache {
    const GRACE: u64 = 24;
    const CAP: usize = 512;

    /// The cache key: the text plus every parameter that changes the raster.
    #[allow(clippy::too_many_arguments)]
    pub fn key(
        text: &str,
        col: u64,
        pt: u32,
        max_w: u32,
        max_h: i32,
        line_sp: f32,
        markup: bool,
        weight: i32,
        link: Option<u64>,
    ) -> String {
        format!(
            "{text}|{col:x}|{pt}|{max_w}|{max_h}|{line_sp:.2}|{}|{weight}|{}",
            i32::from(markup),
            link.unwrap_or(0u64)
        )
    }

    pub fn new() -> Self {
        Self {
            map: HashMap::new(),
            warm_gen: 0,
        }
    }

    /// Advance the grace generation (a full warm begins).
    pub fn tick(&mut self) {
        self.warm_gen += 1;
        // sweep what no recent warm wanted (evict at the cap too)
        self.map
            .retain(|_, e| e.last_warm + Self::GRACE >= self.warm_gen);
        while self.map.len() > Self::CAP {
            // evict the coldest (fewest recent wants)
            let cold = self
                .map
                .iter()
                .min_by_key(|(_, e)| e.last_warm)
                .map(|(k, _)| k.clone());
            if let Some(k) = cold {
                self.map.remove(&k);
            } else {
                break;
            }
        }
    }

    pub fn clear(&mut self) {
        self.map.clear();
        self.warm_gen = 0;
    }

    /// Look up (and refresh) a cached raster. `build` runs on a MISS while
    /// the warm allows a build (it creates the texture); a draw-side miss
    /// simply returns None (the re-warm rebuilds it).
    pub fn get<F>(&mut self, key: &str, want: bool, mut build: F) -> Option<&CachedText>
    where
        F: FnMut() -> Option<CachedText>,
    {
        let warm_g = self.warm_gen;
        if self.map.contains_key(key) {
            // a hit refreshes the grace (warm and draw alike)
            if let Some(e) = self.map.get_mut(key) {
                e.last_warm = warm_g;
            }
        } else if want {
            // a miss builds (warm only; a draw-side miss reads as stale)
            if let Some(entry) = build() {
                if self.map.len() >= Self::CAP {
                    let cold = self
                        .map
                        .iter()
                        .min_by_key(|(_, e)| e.last_warm)
                        .map(|(k, _)| k.clone());
                    if let Some(cold) = cold {
                        self.map.remove(&cold);
                    }
                }
                self.map.insert(
                    key.to_string(),
                    CacheEntry {
                        entry,
                        last_warm: warm_g,
                    },
                );
            }
        }
        self.map.get(key).map(|e| &e.entry)
    }
}

// ---------------------------------------------------------------------------
// hit rects and hover (render.cpp)
// ---------------------------------------------------------------------------

/// One hit rect of the last layout (global logical px).
#[derive(Clone, Copy, Debug, Default)]
pub struct RBox {
    pub x: f64,
    pub y: f64,
    pub w: f64,
    pub h: f64,
}

impl RBox {
    pub fn new(x: f64, y: f64, w: f64, h: f64) -> Self {
        Self { x, y, w, h }
    }
    pub fn empty(&self) -> bool {
        self.w <= 0.0
    }
    pub fn contains(&self, x: f64, y: f64) -> bool {
        x >= self.x && x <= self.x + self.w && y >= self.y && y <= self.y + self.h
    }
    /// A copy translated along x.
    pub fn shift_x(&self, dx: f64) -> Self {
        Self {
            x: self.x + dx,
            ..*self
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum CardKind {
    #[default]
    Popup = 0,
    Row = 1,
    Digest = 2,
    GHead = 3,
    Child = 4,
    BtnClear = 5,
    BtnDnd = 6,
    Panel = 7,
}

/// A hit card (popups, rows, bundle faces, footer buttons, the panel).
#[derive(Clone, Debug, Default)]
pub struct Card {
    pub kind: CardKind,
    pub rect: RBox,
    pub id: u32,
    pub group: String,
    pub chevron: RBox,
    pub close: RBox,
    pub reply_field: RBox,
    pub reply_send: RBox,
    pub buttons: Vec<(RBox, String)>,
    pub links: Vec<(RBox, String)>,
}

/// The hover affordance (rows/buttons warm under the pointer). `btn` -1 =
/// the surface itself; `part`: 0 body, 1 chevron, 2 close, 3 reply field,
/// 4 send.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Hover {
    pub id: u32,
    pub group: String,
    pub kind: CardKind,
    pub btn: i32,
    pub part: u8,
}

impl Default for Hover {
    fn default() -> Self {
        Self {
            id: 0,
            group: String::new(),
            kind: CardKind::Popup,
            // -1 = no button (the C++ hover defaults)
            btn: -1,
            part: 0,
        }
    }
}

/// A display item: one card, or an app's bundle of them (newest first).
#[derive(Clone)]
pub struct Disp {
    pub items: Vec<usize>, // indices into notifs (newest first)
    pub key: String,       // the (app, group) bundle identity
}

// ---------------------------------------------------------------------------
// the state
// ---------------------------------------------------------------------------

pub struct NotifyState {
    // the model
    pub notifs: Vec<Notif>, // newest first
    pub suspended: bool,    // DND
    pub held_banner: u32,
    pub next_id: u32,
    // the shade
    pub center_on: bool,
    pub peek: bool,
    pub peek_bell: bool,
    pub skip: usize,
    pub items: usize,
    pub opened_rows: HashSet<u32>,
    pub folded_rows: HashSet<u32>,
    pub opened_groups: HashSet<String>,
    pub folded_groups: HashSet<String>,
    pub row_state: HashMap<u32, bool>,
    pub group_state: HashMap<String, bool>,
    pub opened_at: u64,
    pub animating: bool,
    pub sel: i32,
    pub last_vis: usize,
    pub disp: Vec<Disp>,
    pub item_h: Vec<f64>,
    pub item_open: Vec<bool>,
    pub item_more: Vec<bool>,
    pub child_h: Vec<Vec<f64>>,
    // the frame
    pub cards: Vec<Card>,
    pub cards_mon: Option<ffi::MonitorHandle>,
    pub hovered: Hover,
    pub last_content_h: f64,
    pub last_content_w: f64,
    pub last_box: RBox, // the last damaged layout (monitor-local logical, expanded)
    pub warm_gen: u64,  // the warm's layout generation (draw misses re-warm)
    // the keyed text cache
    pub text_cache: TextCache,
    // the icons
    pub decode_jobs: Vec<nicons::DecodeJob>,
    pub desktop_pending: bool, // the .desktop index is still scanning
    // the reply field
    pub reply_id: u32,
    pub reply_text: String,
    // input (ninput.rs)
    pub swallow_release: u32,
    pub held_buttons: i32,
    pub pointer_owned: bool,
    pub cursor_hand: bool,
    pub scroll_acc: f64,
    pub swipe_acc: f64,
    /// The last known pointer position (global logical px, refreshed on
    /// every move/button/axis): the center peek's "pointer over a card" test
    /// reads it without an ffi call (the tracked value is fresh enough for
    /// a 400 ms peek window).
    pub pointer: (f64, f64),
    pub swipe_on: u32,
    pub hit_queue: Vec<crate::ninput::Hit>,
    pub key_queue: Vec<crate::ninput::KeyAct>,
    // replies minted inside an emission (the reply field's send): the bus
    // wake drain folds them in, so the NotificationClosed signal is not
    // lost when the model changes off the job path
    pub pending_replies: Vec<crate::bus::BusReply>,
    // deferred work (crash class 6: nothing acts inside an emission)
    pub warm_pending: bool,
    pub hit_pending: bool,
    pub esc_pending: bool,
    pub key_pending: bool,
    pub tex_stale: bool, // a draw missed a warm-built raster: re-warm after
    pub suspend_presses: u32,
    pub center_presses: u32,
    pub peek_want: i32, // -1 = nothing pending
    // timers (job tokens; 0 = disarmed)
    pub peek_out_token: u64,
    pub decode_poll_token: u64,
    pub age_token: u64,
    pub motion_token: u64,
    pub expiry_token: u64,
    pub expiry_at: u64,       // the nearest due banner (steady ms); 0 = none
    pub expiry_armed_ms: u64, // the delay the pending one-shot was armed with
    // the detached children (hyperlink open, sounds); the cap bounds a
    // hostile sender's fork rate
    pub live_children: u32,
    // config handles (the registered values, read live per warm)
    pub cfg_font: *mut std::ffi::c_void,
    pub cfg_font_size: *mut std::ffi::c_void,
    pub cfg_width: *mut std::ffi::c_void,
    pub cfg_max_height: *mut std::ffi::c_void,
    pub cfg_max_icon: *mut std::ffi::c_void,
    pub cfg_margin: *mut std::ffi::c_void,
    pub cfg_offset_y: *mut std::ffi::c_void,
    pub cfg_timeout_low: *mut std::ffi::c_void,
    pub cfg_timeout_normal: *mut std::ffi::c_void,
    pub cfg_coalesce: *mut std::ffi::c_void,
    pub cfg_rounding: *mut std::ffi::c_void,
    pub cfg_rounding_power: *mut std::ffi::c_void,
    pub cfg_max_notifs: *mut std::ffi::c_void,
    pub cfg_ignore_dbus_close: *mut std::ffi::c_void,
    pub cfg_col_bg: *mut std::ffi::c_void,
    pub cfg_col_fg: *mut std::ffi::c_void,
    pub cfg_col_title: *mut std::ffi::c_void,
    pub cfg_col_kicker: *mut std::ffi::c_void,
    pub cfg_col_frame: *mut std::ffi::c_void,
    pub cfg_col_urgent: *mut std::ffi::c_void,
    pub cfg_col_highlight: *mut std::ffi::c_void,
    pub cfg_col_link: *mut std::ffi::c_void,
    pub cfg_sound_command: *mut std::ffi::c_void,
    pub cfg_fallback_icon_dir: *mut std::ffi::c_void,
}

impl NotifyState {
    pub fn new() -> Self {
        Self {
            notifs: Vec::new(),
            suspended: false,
            held_banner: 0,
            next_id: 1,
            center_on: false,
            peek: false,
            peek_bell: false,
            skip: 0,
            items: 0,
            opened_rows: HashSet::new(),
            folded_rows: HashSet::new(),
            opened_groups: HashSet::new(),
            folded_groups: HashSet::new(),
            row_state: HashMap::new(),
            group_state: HashMap::new(),
            opened_at: 0,
            animating: false,
            sel: -1,
            last_vis: 0,
            disp: Vec::new(),
            item_h: Vec::new(),
            item_open: Vec::new(),
            item_more: Vec::new(),
            child_h: Vec::new(),
            cards: Vec::new(),
            cards_mon: None,
            hovered: Hover::default(),
            last_content_h: 0.0,
            last_content_w: 0.0,
            last_box: RBox::default(),
            warm_gen: 0,
            text_cache: TextCache::new(),
            decode_jobs: Vec::new(),
            desktop_pending: true,
            reply_id: 0,
            reply_text: String::new(),
            swallow_release: 0,
            held_buttons: 0,
            pointer_owned: false,
            cursor_hand: false,
            scroll_acc: 0.0,
            swipe_acc: 0.0,
            pointer: (0.0, 0.0),
            swipe_on: 0,
            hit_queue: Vec::new(),
            key_queue: Vec::new(),
            pending_replies: Vec::new(),
            warm_pending: false,
            hit_pending: false,
            esc_pending: false,
            key_pending: false,
            tex_stale: false,
            suspend_presses: 0,
            center_presses: 0,
            peek_want: -1,
            peek_out_token: 0,
            decode_poll_token: 0,
            age_token: 0,
            motion_token: 0,
            expiry_token: 0,
            expiry_at: 0,
            expiry_armed_ms: 0,
            live_children: 0,
            cfg_font: std::ptr::null_mut(),
            cfg_font_size: std::ptr::null_mut(),
            cfg_width: std::ptr::null_mut(),
            cfg_max_height: std::ptr::null_mut(),
            cfg_max_icon: std::ptr::null_mut(),
            cfg_margin: std::ptr::null_mut(),
            cfg_offset_y: std::ptr::null_mut(),
            cfg_timeout_low: std::ptr::null_mut(),
            cfg_timeout_normal: std::ptr::null_mut(),
            cfg_coalesce: std::ptr::null_mut(),
            cfg_rounding: std::ptr::null_mut(),
            cfg_rounding_power: std::ptr::null_mut(),
            cfg_max_notifs: std::ptr::null_mut(),
            cfg_ignore_dbus_close: std::ptr::null_mut(),
            cfg_col_bg: std::ptr::null_mut(),
            cfg_col_fg: std::ptr::null_mut(),
            cfg_col_title: std::ptr::null_mut(),
            cfg_col_kicker: std::ptr::null_mut(),
            cfg_col_frame: std::ptr::null_mut(),
            cfg_col_urgent: std::ptr::null_mut(),
            cfg_col_highlight: std::ptr::null_mut(),
            cfg_col_link: std::ptr::null_mut(),
            cfg_sound_command: std::ptr::null_mut(),
            cfg_fallback_icon_dir: std::ptr::null_mut(),
        }
    }
}

impl Default for NotifyState {
    fn default() -> Self {
        Self::new()
    }
}

// ---------------------------------------------------------------------------
// init / exit
// ---------------------------------------------------------------------------

// the theme defaults (common/theme.hpp glass-ink tokens; the C++ config defaults)
const FONT_DEFAULT: &str = "IBM Plex Sans"; // theme.hpp FONT
const TH_GLASS: NColors = NColors {
    r: 0.0549,
    g: 0.0706,
    b: 0.0941,
    a: 0.6196,
}; // 0x9E0F_1218
const TH_INK: NColors = NColors {
    r: 0.8941,
    g: 0.9098,
    b: 0.9333,
    a: 1.0,
}; // 0xFFE4_E8EE
const TH_TITLE: NColors = NColors {
    r: 0.9333,
    g: 0.9451,
    b: 0.9608,
    a: 1.0,
}; // 0xFFEE_F1F5
const TH_SUB: NColors = NColors {
    r: 0.5961,
    g: 0.6353,
    b: 0.6745,
    a: 1.0,
}; // 0xFF98_A2AC
const TH_LINE: NColors = NColors {
    r: 0.8627,
    g: 0.9216,
    b: 1.0,
    a: 0.0902,
}; // 0x17DC_EBFF
const TH_URGENT: NColors = NColors {
    r: 1.0,
    g: 0.5412,
    b: 0.3608,
    a: 1.0,
}; // 0xFFFF_8A5C
const TH_ACCENT: NColors = NColors {
    r: 0.1961,
    g: 0.8392,
    b: 1.0,
    a: 1.0,
}; // 0xFF32_D6FF
const TH_LINK: NColors = NColors {
    r: 0.4902,
    g: 0.7059,
    b: 1.0,
    a: 1.0,
}; // 0xFF7D_B4FF

fn cfg_int(ctx: ffi::Ctx, key: &str, desc: &str, def: f64) -> Option<*mut std::ffi::c_void> {
    ffi::config_register(ctx, key, desc, ffi::HL_CFG_INT, def, "")
}

fn cfg_str(ctx: ffi::Ctx, key: &str, desc: &str, def: &str) -> Option<*mut std::ffi::c_void> {
    ffi::config_register(ctx, key, desc, ffi::HL_CFG_STRING, 0.0, def)
}

fn cfg_color(ctx: ffi::Ctx, key: &str, desc: &str, def: u32) -> Option<*mut std::ffi::c_void> {
    ffi::config_register(ctx, key, desc, ffi::HL_CFG_COLOR, def as f64, "")
}

pub fn color_hex(c: NColors) -> u64 {
    let r = (c.r * 255.0).round().clamp(0.0, 255.0) as u32;
    let g = (c.g * 255.0).round().clamp(0.0, 255.0) as u32;
    let b = (c.b * 255.0).round().clamp(0.0, 255.0) as u32;
    let a = (c.a * 255.0).round().clamp(0.0, 255.0) as u32;
    ((a << 24) | (r << 16) | (g << 8) | b) as u64
}

/// Register the config handles (the `plugin:notify:*` keys).
#[allow(clippy::too_many_lines)]
pub fn init(ctx: ffi::Ctx, st: &mut NotifyState) {
    st.cfg_font = cfg_str(ctx, "plugin:notify:font", "font family", FONT_DEFAULT)
        .unwrap_or(std::ptr::null_mut());
    st.cfg_font_size = cfg_int(
        ctx,
        "plugin:notify:font_size",
        "body text size in logical px",
        12.0,
    )
    .unwrap_or(std::ptr::null_mut());
    st.cfg_width = cfg_int(
        ctx,
        "plugin:notify:width",
        "popup card width in logical px",
        348.0,
    )
    .unwrap_or(std::ptr::null_mut());
    st.cfg_max_height = cfg_int(
        ctx,
        "plugin:notify:max_height",
        "popup card height cap in logical px",
        300.0,
    )
    .unwrap_or(std::ptr::null_mut());
    st.cfg_max_icon = cfg_int(
        ctx,
        "plugin:notify:max_icon",
        "popup icon column in logical px",
        44.0,
    )
    .unwrap_or(std::ptr::null_mut());
    st.cfg_margin = cfg_int(
        ctx,
        "plugin:notify:margin",
        "inter-card gap in logical px",
        6.0,
    )
    .unwrap_or(std::ptr::null_mut());
    st.cfg_offset_y = cfg_int(
        ctx,
        "plugin:notify:offset_y",
        "distance from the monitor top",
        34.0,
    )
    .unwrap_or(std::ptr::null_mut());
    st.cfg_timeout_low = cfg_int(
        ctx,
        "plugin:notify:timeout_low",
        "ephemeral timeout in ms",
        4000.0,
    )
    .unwrap_or(std::ptr::null_mut());
    st.cfg_timeout_normal = cfg_int(
        ctx,
        "plugin:notify:timeout_normal",
        "normal-urgency banner timeout in ms (0 = sticky)",
        5000.0,
    )
    .unwrap_or(std::ptr::null_mut());
    st.cfg_coalesce = cfg_int(
        ctx,
        "plugin:notify:coalesce_popups",
        "1 = at most one live popup per app",
        1.0,
    )
    .unwrap_or(std::ptr::null_mut());
    st.cfg_rounding = cfg_int(
        ctx,
        "plugin:notify:rounding",
        "card radius in logical px (panel +6 and rows -2 derive)",
        16.0,
    )
    .unwrap_or(std::ptr::null_mut());
    st.cfg_rounding_power = cfg_int(
        ctx,
        "plugin:notify:rounding_power",
        "corner superellipse exponent",
        3.0,
    )
    .unwrap_or(std::ptr::null_mut());
    st.cfg_max_notifs =
        cfg_int(ctx, "plugin:notify:max_notifs", "model cap", 50.0).unwrap_or(std::ptr::null_mut());
    st.cfg_ignore_dbus_close = cfg_int(
        ctx,
        "plugin:notify:ignore_dbusclose",
        "ignore app-initiated CloseNotification",
        0.0,
    )
    .unwrap_or(std::ptr::null_mut());
    st.cfg_col_bg = cfg_color(ctx, "plugin:notify:col_bg", "glass fill", 0x9E0F_1218)
        .unwrap_or(std::ptr::null_mut());
    st.cfg_col_fg = cfg_color(ctx, "plugin:notify:col_fg", "body text", 0xFFE4_E8EE)
        .unwrap_or(std::ptr::null_mut());
    st.cfg_col_title = cfg_color(ctx, "plugin:notify:col_title", "card titles", 0xFFEE_F1F5)
        .unwrap_or(std::ptr::null_mut());
    st.cfg_col_kicker = cfg_color(
        ctx,
        "plugin:notify:col_kicker",
        "header/age/secondary text",
        0xFF98_A2AC,
    )
    .unwrap_or(std::ptr::null_mut());
    st.cfg_col_frame = cfg_color(ctx, "plugin:notify:col_frame", "hairlines", 0x17DC_EBFF)
        .unwrap_or(std::ptr::null_mut());
    st.cfg_col_urgent = cfg_color(
        ctx,
        "plugin:notify:col_urgent",
        "critical ring/progress/urgent fills",
        0xFFFF_8A5C,
    )
    .unwrap_or(std::ptr::null_mut());
    st.cfg_col_highlight = cfg_color(
        ctx,
        "plugin:notify:col_highlight",
        "progress, actions, selections",
        0xFF32_D6FF,
    )
    .unwrap_or(std::ptr::null_mut());
    st.cfg_col_link = cfg_color(
        ctx,
        "plugin:notify:col_link",
        "body hyperlinks",
        0xFF7D_B4FF,
    )
    .unwrap_or(std::ptr::null_mut());
    st.cfg_sound_command = cfg_str(
        ctx,
        "plugin:notify:sound_command",
        "libcanberra player; empty disables sound",
        "canberra-gtk-play",
    )
    .unwrap_or(std::ptr::null_mut());
    st.cfg_fallback_icon_dir = cfg_str(
        ctx,
        "plugin:notify:fallback_icon_dir",
        "iconless cards draw a random face from here",
        "",
    )
    .unwrap_or(std::ptr::null_mut());
    nicons::reset_icon_cache();
    nicons::reset_fallback_cache();
    nicons::start_desktop_index();
    st.desktop_pending = true;
}

/// Teardown: cancel every timer, drop every decode, clear the caches.
#[allow(clippy::too_many_lines)]
pub fn exit(ctx: ffi::Ctx, st: &mut NotifyState) {
    for t in [
        &mut st.peek_out_token,
        &mut st.decode_poll_token,
        &mut st.age_token,
        &mut st.motion_token,
        &mut st.expiry_token,
    ] {
        if *t != 0 {
            ffi::job_cancel(ctx, *t);
            *t = 0;
        }
    }
    nicons::drop_all_decodes(ctx, &mut st.decode_jobs);
    st.notifs.clear();
    st.cards.clear();
    st.text_cache.clear();
    nicons::clear_avatars();
    st.hovered = Hover::default();
    st.swallow_release = 0;
    st.held_buttons = 0;
    st.hit_queue.clear();
    st.key_queue.clear();
}

// ---------------------------------------------------------------------------
// the config snapshot
// ---------------------------------------------------------------------------

fn read_int(ctx: ffi::Ctx, h: *mut std::ffi::c_void, def: i32) -> i32 {
    ffi::config_get(ctx, h).map_or(def, |(_t, n, _)| n as i32)
}

fn read_str(ctx: ffi::Ctx, h: *mut std::ffi::c_void, def: &str) -> String {
    ffi::config_get(ctx, h).map_or_else(
        || def.to_owned(),
        |(_t, _n, s)| if s.is_empty() { def.to_owned() } else { s },
    )
}

fn read_color(ctx: ffi::Ctx, h: *mut std::ffi::c_void, def: NColors) -> NColors {
    let v = match ffi::config_get(ctx, h) {
        Some((_t, n, _)) if (n as i64) != 0 => n as i64,
        _ => return def,
    };
    let a = ((v >> 24) & 0xff) as f32 / 255.0;
    let r = ((v >> 16) & 0xff) as f32 / 255.0;
    let g = ((v >> 8) & 0xff) as f32 / 255.0;
    let b = (v & 0xff) as f32 / 255.0;
    NColors { r, g, b, a }
}

/// Read the live config once (per warm / per arrival).
pub fn config(ctx: ffi::Ctx, st: &NotifyState) -> NConfig {
    let font = read_str(ctx, st.cfg_font, FONT_DEFAULT);
    let glass = read_color(ctx, st.cfg_col_bg, TH_GLASS);
    let ink = read_color(ctx, st.cfg_col_fg, TH_INK);
    NConfig {
        col_frame: read_color(ctx, st.cfg_col_frame, TH_LINE),
        font,
        font_size: read_int(ctx, st.cfg_font_size, 12),
        width: read_int(ctx, st.cfg_width, 348),
        max_height: read_int(ctx, st.cfg_max_height, 300),
        max_icon: read_int(ctx, st.cfg_max_icon, 44),
        margin: read_int(ctx, st.cfg_margin, 6),
        offset_y: read_int(ctx, st.cfg_offset_y, 34),
        timeout_low: read_int(ctx, st.cfg_timeout_low, 4000),
        timeout_normal: read_int(ctx, st.cfg_timeout_normal, 5000),
        coalesce: read_int(ctx, st.cfg_coalesce, 1) != 0,
        rounding: read_int(ctx, st.cfg_rounding, 16),
        rounding_power: read_int(ctx, st.cfg_rounding_power, 3) as f32,
        max_notifs: read_int(ctx, st.cfg_max_notifs, 50),
        ignore_dbus_close: read_int(ctx, st.cfg_ignore_dbus_close, 0) != 0,
        col_bg: glass,
        col_fg: ink,
        col_title: read_color(ctx, st.cfg_col_title, TH_TITLE),
        col_kicker: read_color(ctx, st.cfg_col_kicker, TH_SUB),
        col_urgent: read_color(ctx, st.cfg_col_urgent, TH_URGENT),
        col_highlight: read_color(ctx, st.cfg_col_highlight, TH_ACCENT),
        col_link: read_color(ctx, st.cfg_col_link, TH_LINK),
        sound_command: read_str(ctx, st.cfg_sound_command, "canberra-gtk-play"),
        fallback_icon_dir: read_str(ctx, st.cfg_fallback_icon_dir, ""),
        fg_hex: color_hex(ink),
        bg_hex: color_hex(glass),
    }
}

/// The model changed: schedule the one warm (coalesced). `replies` collects
/// the coalesced State signal.
pub fn changed(ctx: ffi::Ctx, st: &mut NotifyState) {
    if st.warm_pending {
        return;
    }
    st.warm_pending = true;
    let _ = crate::probe::arm_job_n(crate::probe::JOB_N_WARM);
    let _ = ctx;
}

/// The model changed AND the state signal must go out (the coalesced
/// emitStateSoon).
pub fn changed_st(ctx: ffi::Ctx, st: &mut NotifyState, replies: &mut Vec<crate::bus::BusReply>) {
    changed(ctx, st);
    replies.push(crate::bus::BusReply::StateChanged);
}

// ---------------------------------------------------------------------------
// the warm (build textures + measure the layout) and the damage
// ---------------------------------------------------------------------------

/// The focused monitor (the C++ focusedMon).
fn focused_mon(ctx: ffi::Ctx) -> Option<ffi::MonitorHandle> {
    ffi::focus_monitor(ctx)
}

/// Anything actually draws? Only the open center or a live banner — a
/// resident-only model with the center closed must warm and damage nothing.
pub fn anything_to_draw(st: &NotifyState) -> bool {
    if st.center_on {
        return true;
    }
    st.notifs.iter().any(|n| !n.waiting && n.banner)
}

/// The one warm: build every texture the next frame will paint, measure the
/// layout, and damage the old + new boxes. Event loop only.
#[allow(clippy::too_many_lines)]
pub fn warm(ctx: ffi::Ctx, st: &mut NotifyState) {
    // the pointer-release refocus re-dispatches the move into our own
    // dispatch; the section guard keeps that re-entry standing down
    let _sect = crate::probe::section_enter();
    st.warm_pending = false;
    let cfg = config(ctx, st);
    let mon = if anything_to_draw(st) {
        focused_mon(ctx)
    } else {
        None
    };
    st.text_cache.tick();
    st.warm_gen += 1;
    if mon.is_none() {
        st.cards.clear();
        st.last_content_h = 0.0;
        st.last_content_w = 0.0;
    } else {
        // the decode poll rides the warm: the ensure* recipes re-derive a
        // ready icon in this very pass
        nicons_poll(ctx, st);
        crate::nrender::layout(ctx, st, &cfg, std::ptr::null_mut());
        arm_decode_poll(ctx, st);
    }
    damage(ctx, st);
    refresh_pointer_ownership(ctx, st);
    // the age tick (re-bucket the age lines) and the motion tick (springs)
    arm_age_tick(ctx, st);
    arm_motion_tick(ctx, st);
}

/// Damage the previous layout and the fresh one (monitor-local logical).
pub fn damage(ctx: ffi::Ctx, st: &mut NotifyState) {
    let Some(mon) = st.cards_mon.as_ref() else {
        return;
    };
    let cur = content_box(st);
    let margin = blur_margin(mon_scale(ctx, mon));
    let new_box = if cur.w > 0.0 {
        RBox::new(
            cur.x - margin,
            cur.y - margin,
            cur.w + 2.0 * margin,
            cur.h + 2.0 * margin,
        )
    } else {
        RBox::default()
    };
    if st.last_box.w > 0.0 {
        ffi::damage(ctx, mon, ffi_box(&st.last_box));
    }
    if new_box.w > 0.0 {
        ffi::damage(ctx, mon, ffi_box(&new_box));
    }
    st.last_box = new_box;
}

fn ffi_box(b: &RBox) -> ffi::hl_box_t {
    ffi::hl_box_t {
        x: b.x,
        y: b.y,
        w: b.w,
        h: b.h,
    }
}

fn content_box(st: &NotifyState) -> RBox {
    if st.cards.is_empty() {
        return RBox::default();
    }
    let mut x0 = st.cards[0].rect.x;
    let mut y0 = st.cards[0].rect.y;
    let mut x1 = x0;
    let mut y1 = y0;
    for c in &st.cards {
        x0 = x0.min(c.rect.x);
        y0 = y0.min(c.rect.y);
        x1 = x1.max(c.rect.x + c.rect.w);
        y1 = y1.max(c.rect.y + c.rect.h);
    }
    RBox::new(x0, y0, x1 - x0, y1 - y0)
}

/// The monitor's scale (for damage margins); 1.0 if it expired.
fn mon_scale(ctx: ffi::Ctx, mon: &ffi::MonitorHandle) -> f64 {
    ffi::monitor_box(ctx, mon).map_or(1.0, |(_b, s)| s as f64)
}

// damage margins: hairline + the glass blur reach (the C++ damageMargin)
fn blur_margin(scale: f64) -> f64 {
    scale.ceil() + 1.0 + 26.0
}

/// The hover affordance repaints exactly the boxes whose fill changed.
pub fn set_hovered(ctx: ffi::Ctx, st: &mut NotifyState, h: Hover) {
    if h == st.hovered {
        return;
    }
    hold_banner(ctx, st, if h.kind == CardKind::Popup { h.id } else { 0 });
    if let Some(mon) = st.cards_mon.as_ref() {
        let margin = mon_scale(ctx, mon);
        for c in &st.cards {
            let was =
                c.kind == st.hovered.kind && c.id == st.hovered.id && c.group == st.hovered.group;
            let is = c.kind == h.kind && c.id == h.id && c.group == h.group;
            let pop = c.kind == CardKind::Popup && (c.id == st.hovered.id || c.id == h.id);
            if was || is || pop {
                ffi::damage(
                    ctx,
                    mon,
                    ffi_box(&RBox::new(
                        c.rect.x - margin,
                        c.rect.y - margin,
                        c.rect.w + 2.0 * margin,
                        c.rect.h + 2.0 * margin,
                    )),
                );
            }
        }
    }
    st.hovered = h;
}

fn arm_age_tick(ctx: ffi::Ctx, st: &mut NotifyState) {
    let want = anything_to_draw(st);
    if want && st.age_token == 0 {
        st.age_token = crate::probe::arm_timer_repeat(ctx, AGE_TICK_MS, crate::probe::JOB_N_AGE);
    } else if !want && st.age_token != 0 {
        ffi::job_cancel(ctx, st.age_token);
        st.age_token = 0;
    }
}

fn arm_motion_tick(ctx: ffi::Ctx, st: &mut NotifyState) {
    let want = crate::nrender::animating(ctx, st);
    if want && st.motion_token == 0 {
        st.motion_token =
            crate::probe::arm_timer_repeat(ctx, MOTION_TICK_MS, crate::probe::JOB_N_MOTION);
    } else if !want && st.motion_token != 0 {
        ffi::job_cancel(ctx, st.motion_token);
        st.motion_token = 0;
    }
}

/// A surface can vanish under a motionless pointer (expiry, a dismissal, the
/// center closing): re-hover or release, and the window beneath gets its
/// focus back. Runs from the warm, never an input emission.
pub fn refresh_pointer_ownership(ctx: ffi::Ctx, st: &mut NotifyState) {
    crate::ninput::refresh_ownership(ctx, st);
}

// ---------------------------------------------------------------------------
// the shade (center.cpp)
// ---------------------------------------------------------------------------

// the deferred queue drains (crash class 6) — the input module owns the hit
// and key queues; these just arm the drains.
pub fn queue_hit(ctx: ffi::Ctx, st: &mut NotifyState) {
    if st.hit_pending {
        return;
    }
    st.hit_pending = true;
    let _ = crate::probe::arm_job_n(crate::probe::JOB_N_HITS);
    let _ = ctx;
}

pub fn queue_key(ctx: ffi::Ctx, st: &mut NotifyState) {
    if st.key_pending {
        return;
    }
    st.key_pending = true;
    let _ = crate::probe::arm_job_n(crate::probe::JOB_N_KEYS);
    let _ = ctx;
}

pub fn queue_esc(ctx: ffi::Ctx, st: &mut NotifyState) {
    if st.esc_pending {
        return;
    }
    st.esc_pending = true;
    let _ = crate::probe::arm_job_n(crate::probe::JOB_N_ESC);
    let _ = ctx;
}

// ---- the deferred entry points (every surface funnels through these) ----

/// The shade toggle (F12's bind, the bar's bell over the bus, `notify center`
/// via the Lua face). Deferred and accumulating.
pub fn queue_center_toggle(ctx: ffi::Ctx, st: &mut NotifyState) {
    st.center_presses = st.center_presses.saturating_add(1);
    if st.center_presses > 1 {
        return;
    }
    if !crate::probe::arm_job_n(crate::probe::JOB_N_CENTER) {
        st.center_presses = 0;
    }
    let _ = ctx;
}

/// The bell's hover (the bar's Peek over the bus); only the newest state.
pub fn queue_center_peek(ctx: ffi::Ctx, st: &mut NotifyState, on_bell: bool) {
    st.peek_want = i32::from(on_bell);
    if !crate::probe::arm_job_n(crate::probe::JOB_N_PEEK) {
        st.peek_want = -1;
    }
    let _ = ctx;
}

/// The DND chord (the bar's suspend over the bus). Deferred, accumulating.
pub fn queue_suspend(ctx: ffi::Ctx, st: &mut NotifyState) {
    st.suspend_presses = st.suspend_presses.saturating_add(1);
    if st.suspend_presses > 1 {
        return;
    }
    if !crate::probe::arm_job_n(crate::probe::JOB_N_SUSPEND) {
        st.suspend_presses = 0;
    }
    let _ = ctx;
}

/// Drain the deferred entry points (the job trampoline). `replies` collects
/// the outbound bus signals.
pub fn drain_deferred(
    ctx: ffi::Ctx,
    st: &mut NotifyState,
    replies: &mut Vec<crate::bus::BusReply>,
) {
    if st.center_presses & 1 == 1 {
        st.center_presses = 0;
        if st.peek {
            center_pin(ctx, st, replies);
        } else if st.center_on {
            set_center(ctx, st, false, true, replies);
        } else {
            set_center(ctx, st, true, false, replies);
        }
    } else {
        st.center_presses = 0;
    }
    if st.suspend_presses & 1 == 1 {
        st.suspend_presses = 0;
        toggle_suspend(ctx, st, replies);
    } else {
        st.suspend_presses = 0;
    }
    if st.esc_pending {
        st.esc_pending = false;
        set_center(ctx, st, false, true, replies);
    }
    if st.peek_want >= 0 {
        let want = st.peek_want == 1;
        st.peek_want = -1;
        center_peek(ctx, st, want, replies);
    }
}

// the decode poll (the icons' async front door)
fn nicons_poll(ctx: ffi::Ctx, st: &mut NotifyState) -> (bool, bool) {
    let mut became_ready = false;
    let mut i = 0usize;
    while i < st.decode_jobs.len() {
        let token = st.decode_jobs[i].token;
        if token != 0
            && let Some(s) = ffi::image_token_status(ctx, token)
        {
            became_ready |= s == 1;
        }
        i += 1;
    }
    // the desktop index may have finished: upgrade the identity stand-ins
    if st.desktop_pending && nicons::desktop_done() {
        st.desktop_pending = false;
        let icon_px = (config(ctx, st).max_icon.max(8)) as u32;
        let mut identity_changed = false;
        for n in &mut st.notifs {
            if !n.identity_from_desktop {
                continue;
            }
            let icon = nicons::resolve_desktop_entry_icon(&n.desktop_entry, icon_px);
            if icon.is_empty() || icon == n.identity {
                continue;
            }
            n.identity = icon;
            n.ident.tex = None;
            n.ident.for_.clear();
            n.ident.px = 0;
            n.ident.settled = false;
            n.identity_from_desktop = false;
            identity_changed = true;
        }
        became_ready |= identity_changed;
    }
    let pending = st
        .decode_jobs
        .iter()
        .any(|j| j.token != 0 && ffi::image_token_status(ctx, j.token) != Some(2));
    (became_ready, pending)
}

/// The decode poll's armer (the C++ 16 ms repeating timer): it stands while
/// any decode or the desktop index is in flight and drops out when nothing
/// is left to watch.
pub fn arm_decode_poll(ctx: ffi::Ctx, st: &mut NotifyState) {
    let pending = st
        .decode_jobs
        .iter()
        .any(|j| j.token != 0 && ffi::image_token_status(ctx, j.token) != Some(2));
    if pending && st.decode_poll_token == 0 {
        st.decode_poll_token =
            crate::probe::arm_timer_repeat(ctx, DECODE_POLL_MS, crate::probe::JOB_N_DECODE);
    } else if !pending && st.decode_poll_token != 0 {
        ffi::job_cancel(ctx, st.decode_poll_token);
        st.decode_poll_token = 0;
    }
}

/// The decode tick (the repeating timer): a ready decode re-warms (the
/// ensure* recipes re-derive it); the poll re-arms itself while pending.
pub fn decode_tick(ctx: ffi::Ctx, st: &mut NotifyState, replies: &mut Vec<crate::bus::BusReply>) {
    let (ready, _) = nicons_poll(ctx, st);
    arm_decode_poll(ctx, st);
    if ready {
        changed(ctx, st);
    }
    let _ = replies;
}

/// Raise + hard-focus the topmost mapped X11 window of the pid (the C++
/// activateAppWindow: only X11 windows need it — Wayland self-activates with
/// the token).
pub fn activate_app_window(ctx: ffi::Ctx, pid: u32) -> bool {
    let ws = ffi::windows(ctx);
    let mut best: Option<ffi::WindowHandle> = None;
    for w in &ws {
        // Z-order: the LAST match is topmost
        let Some(info) = ffi::window_info(ctx, w) else {
            continue;
        };
        if info.visible && ffi::window_is_x11(ctx, w) && ffi::window_pid(ctx, w) == pid {
            best = Some(w.clone_handle());
        }
    }
    let Some(w) = best else {
        return false;
    };
    if let Some(info) = ffi::window_info(ctx, &w)
        && info.floating
    {
        let _ = ffi::window_raise(ctx, &w);
    }
    let _ = ffi::focus_window_set(ctx, &w, 512); // FOCUS_REASON_SWITCH_TO_WINDOW_HARD
    true
}

/// Mint an activation token and emit it before the action (spec 1.3).
pub fn mint_token(ctx: ffi::Ctx) -> Option<String> {
    ffi::activation_token(ctx)
}

/// The `hl.plugin.notify.center` Lua entry (the F12 bind): deferred and
/// accumulating like the C++ original — the drain decides between peek,
/// pin, re-pop, and open.
pub fn lua_center_impl() -> i32 {
    if let Some(st) = crate::probe::state()
        && let Ok(mut n) = st.notify.lock()
    {
        queue_center_toggle(st.ctx, &mut n);
    }
    0
}

/// The `hl.plugin.notify.suspend` Lua entry (DND): deferred and
/// accumulating (two presses in one dispatch net zero, as ever).
pub fn lua_suspend_impl() -> i32 {
    if let Some(st) = crate::probe::state()
        && let Ok(mut n) = st.notify.lock()
    {
        queue_suspend(st.ctx, &mut n);
    }
    0
}

pub(crate) mod center;
pub(crate) mod model;
pub(crate) mod reply;
// the module face: the submodules' pub items are the module's items (the
// input/render/probe modules call `notify::close_one`, `notify::warm`, …)
pub use center::*;
pub use model::*;
pub use reply::*;
