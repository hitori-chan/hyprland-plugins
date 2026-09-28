// The notify port's icon layer: freedesktop icon-NAME resolution (port of
// common/icons.hpp — the bar reuses this module at Phase 5), the async file
// decode over the fork's worker thread, the .desktop entry index, the
// fallback face dir, and the generated (initials) avatars.
//
// Texture rule (crash class 4): file icons DECODE on the fork's worker
// thread and are DERIVED (scaled/uploaded) here, on the event loop, only
// from the warm pass. Nothing decodes or uploads inside a render frame.

use std::collections::HashMap;
use std::path::Path;
use std::sync::Arc;

use crate::ffi;
use crate::nparse;

// ---- bounds (icons.cpp) ----
const MAX_PENDING_IMAGE_TOKENS: usize = 24;
const MAX_IMAGE_FILE_BYTES: u64 = 32 * 1024 * 1024;
const MAX_DECODED_IMAGE_PIXELS: f64 = 16.0 * 1024.0 * 1024.0;
const MAX_DESKTOP_FILES: usize = 4096;
const MAX_DESKTOP_VISITED: usize = 16384;
const MAX_DESKTOP_FILE_BYTES: u64 = 512 * 1024;
const MAX_FALLBACK_VISITS: usize = 65536;
const MAX_GENERATED_AVATARS: usize = 128;

// ---------------------------------------------------------------------------
// freedesktop icon-NAME resolution (common/icons.hpp, verbatim port)
// ---------------------------------------------------------------------------

pub fn is_svg_icon_path(source: &str) -> bool {
    source.len() >= 4 && source.to_ascii_lowercase().ends_with(".svg")
}

/// The XDG data dirs in precedence order (per-user first, then $XDG_DATA_DIRS).
pub fn xdg_data_dirs() -> Vec<String> {
    let mut dirs = Vec::new();
    match std::env::var("XDG_DATA_HOME") {
        Ok(x) if !x.is_empty() => dirs.push(x),
        _ => match std::env::var("HOME") {
            Ok(h) if !h.is_empty() => dirs.push(format!("{h}/.local/share")),
            _ => {}
        },
    }
    let data =
        std::env::var("XDG_DATA_DIRS").unwrap_or_else(|_| "/usr/local/share:/usr/share".to_owned());
    for d in data.split(':') {
        let d = d.trim_end_matches('/');
        if !d.is_empty() {
            dirs.push(d.to_owned());
        }
    }
    dirs
}

/// Where icon THEMES live: the data dirs' icons/ plus the legacy ~/.icons
/// (right after the per-user data dir — the spec's order).
fn xdg_icon_bases() -> Vec<String> {
    let data = xdg_data_dirs();
    let mut bases = Vec::with_capacity(data.len() + 1);
    if let Some(first) = data.first() {
        bases.push(format!("{first}/icons"));
    }
    if let Ok(h) = std::env::var("HOME")
        && !h.is_empty()
    {
        bases.push(format!("{h}/.icons"));
    }
    for d in data.iter().skip(1) {
        bases.push(format!("{d}/icons"));
    }
    bases
}

// The memoized GTK theme name + the resolution cache. A theme switch (config
// reload) resets both.
struct IconMemo {
    theme_name: Option<String>,
    cache: HashMap<String, String>,
}

static ICON_MEMO: std::sync::Mutex<Option<IconMemo>> = std::sync::Mutex::new(None);

fn memo() -> std::sync::MutexGuard<'static, Option<IconMemo>> {
    ICON_MEMO
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

pub fn reset_icon_cache() {
    *memo() = Some(IconMemo {
        theme_name: None,
        cache: HashMap::new(),
    });
}

/// The GTK icon theme from $XDG_CONFIG_HOME/gtk-3.0/settings.ini (once).
fn gtk_icon_theme_name() -> String {
    let mut memo = memo();
    let m = memo.get_or_insert(IconMemo {
        theme_name: None,
        cache: HashMap::new(),
    });
    if let Some(t) = &m.theme_name {
        return t.clone();
    }
    let mut value = String::new();
    let cfg_home = std::env::var("XDG_CONFIG_HOME")
        .ok()
        .filter(|s| !s.is_empty())
        .or_else(|| {
            std::env::var("HOME")
                .ok()
                .filter(|s| !s.is_empty())
                .map(|h| format!("{h}/.config"))
        });
    if let Some(cfg_home) = cfg_home
        && let Ok(content) = std::fs::read_to_string(format!("{cfg_home}/gtk-3.0/settings.ini"))
    {
        for line in content.lines() {
            if let Some(rest) = line.strip_prefix("gtk-icon-theme-name") {
                if let Some(v) = rest.strip_prefix('=') {
                    value = v.trim().to_string();
                }
                break;
            }
        }
    }
    m.theme_name = Some(value.clone());
    value
}

/// Look for <dir>/name.ext directly and one category level down.
fn find_icon_in_dir(dir: &str, name: &str) -> String {
    let exts = [".svg", ".png", ".xpm"];
    for e in exts {
        let p = format!("{dir}/{name}{e}");
        if Path::new(&p).exists() {
            return p;
        }
    }
    if let Ok(mut it) = std::fs::read_dir(dir) {
        while let Some(Ok(entry)) = it.next() {
            if !entry.file_type().is_ok_and(|t| t.is_dir()) {
                continue;
            }
            let cat = entry.path();
            for e in exts {
                let p = format!("{cat}/{name}{e}", cat = cat.display());
                if Path::new(&p).exists() {
                    return p;
                }
            }
        }
    }
    String::new()
}

/// Resolve a freedesktop icon NAME to a file path via themed lookup; "" if
/// unresolved or already a path. Cached per (name, size).
#[allow(clippy::too_many_lines)]
pub fn resolve_icon_name(name: &str, size_px: u32) -> String {
    const CTXS: [&str; 8] = [
        "status",
        "apps",
        "devices",
        "actions",
        "categories",
        "mimetypes",
        "legacy",
        "symbolic",
    ];

    if name.is_empty() || name.contains('/') {
        return String::new();
    }
    let key = format!("{name}\u{1f}{size_px}");
    if let Some(m) = memo().as_ref()
        && let Some(v) = m.cache.get(&key)
    {
        return v.clone();
    }

    let bases = xdg_icon_bases();

    let mut themes: Vec<String> = Vec::new();
    let gt = gtk_icon_theme_name();
    if !gt.is_empty() {
        themes.push(gt.clone());
        for suf in ["-dark", "-light", "-Dark", "-Light"] {
            if let Some(base) = gt.strip_suffix(suf) {
                themes.push(base.to_owned());
            }
        }
    }
    themes.push("hicolor".to_owned());
    themes.push("Adwaita".to_owned());
    themes.push("AdwaitaLegacy".to_owned());

    let mut size_dirs = vec!["scalable".to_owned(), "symbolic".to_owned()];
    for s in [
        size_px, 64, 48, 96, 128, 256, 72, 36, 32, 24, 22, 20, 16, 192, 384, 512, 1024,
    ] {
        size_dirs.push(format!("{s}x{s}"));
    }
    let mut ctx_sizes = vec!["symbolic".to_owned(), "scalable".to_owned()];
    for s in [size_px, 64, 48, 32, 24, 22, 20, 16, 96, 128, 256, 512] {
        ctx_sizes.push(s.to_string());
    }

    let mut found = String::new();
    'themes: for theme in &themes {
        for base in &bases {
            let tdir = format!("{base}/{theme}");
            if !Path::new(&tdir).is_dir() {
                continue;
            }
            for sd in &size_dirs {
                found = find_icon_in_dir(&format!("{tdir}/{sd}"), name);
                if !found.is_empty() {
                    break 'themes;
                }
            }
            for ctx in CTXS {
                if !found.is_empty() {
                    break;
                }
                let cdir = format!("{tdir}/{ctx}");
                if !Path::new(&cdir).is_dir() {
                    continue;
                }
                for sz in &ctx_sizes {
                    for e in [".svg", ".png"] {
                        let p = format!("{cdir}/{sz}/{name}{e}");
                        if Path::new(&p).exists() {
                            found = p;
                            break;
                        }
                    }
                    if !found.is_empty() {
                        break;
                    }
                }
            }
        }
    }
    if found.is_empty() {
        for d in xdg_data_dirs() {
            for e in [".svg", ".png", ".xpm"] {
                let p = format!("{d}/pixmaps/{name}{e}");
                if Path::new(&p).exists() {
                    found = p;
                    break;
                }
            }
            if !found.is_empty() {
                break;
            }
        }
    }
    if let Some(m) = memo().as_mut() {
        m.cache.insert(key, found.clone());
    }
    found
}

// ---------------------------------------------------------------------------
// the desktop-entry index (the Icon= lookup), on a background thread
// ---------------------------------------------------------------------------

static DESKTOP_INDEX: std::sync::Mutex<Option<HashMap<String, String>>> =
    std::sync::Mutex::new(None);
static DESKTOP_DONE: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

fn lower_key(v: &str) -> String {
    v.to_ascii_lowercase()
}

/// Unescape a .desktop string value (\" \\ \n \t sequences).
fn desktop_unescape(v: &str) -> String {
    let mut out = String::with_capacity(v.len());
    let mut chars = v.chars();
    while let Some(c) = chars.next() {
        if c == '\\' {
            match chars.next() {
                Some('n') => out.push('\n'),
                Some('t') => out.push('\t'),
                Some(o) => out.push(o),
                None => out.push('\\'),
            }
        } else {
            out.push(c);
        }
    }
    out
}

fn parse_desktop_entry(contents: &str, path: &Path, index: &mut HashMap<String, String>) {
    let mut icon = String::new();
    let mut wm_class = String::new();
    let mut in_entry = false;
    for line in contents.lines() {
        let line = line.strip_suffix('\r').unwrap_or(line);
        if let Some(rest) = line.strip_prefix('[') {
            if in_entry {
                break;
            }
            in_entry = rest.trim_end_matches(']') == "Desktop Entry";
            continue;
        }
        if !in_entry {
            continue;
        }
        if icon.is_empty()
            && let Some(v) = line.strip_prefix("Icon=")
        {
            icon = desktop_unescape(v);
        } else if wm_class.is_empty()
            && let Some(v) = line.strip_prefix("StartupWMClass=")
        {
            wm_class = desktop_unescape(v);
        }
        if !icon.is_empty() && !wm_class.is_empty() {
            break;
        }
    }
    if icon.is_empty() {
        return;
    }
    let remember = |k: &str, index: &mut HashMap<String, String>| {
        if !k.is_empty() {
            index.entry(lower_key(k)).or_insert_with(|| icon.clone());
        }
    };
    if let Some(stem) = path.file_stem().and_then(|s| s.to_str()) {
        remember(stem, index);
    }
    remember(&wm_class, index);
}

/// Start the background scan of every $XDG_DATA_DIRS/applications *.desktop
/// (bounded). The event loop polls `desktop_done()`; the index map is
/// populated before the flag flips.
pub fn start_desktop_index() {
    DESKTOP_DONE.store(false, std::sync::atomic::Ordering::SeqCst);
    *DESKTOP_INDEX
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner) = None;
    let roots: Vec<String> = xdg_data_dirs()
        .into_iter()
        .map(|d| format!("{d}/applications"))
        .collect();
    std::thread::Builder::new()
        .name("awesome-desktop-index".to_owned())
        .spawn(move || {
            let mut index = HashMap::new();
            let mut budget = 0usize;
            for root in &roots {
                if Path::new(root).is_dir() {
                    walk_desktop(Path::new(root), &mut index, &mut budget, MAX_DESKTOP_FILES);
                }
                if index.len() >= MAX_DESKTOP_FILES {
                    break;
                }
            }
            *DESKTOP_INDEX
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner) = Some(index);
            DESKTOP_DONE.store(true, std::sync::atomic::Ordering::SeqCst);
        })
        .ok();
}

/// One desktop index pass (the .desktop files of the data dirs, depth-first).
fn walk_desktop(dir: &Path, index: &mut HashMap<String, String>, budget: &mut usize, cap: usize) {
    let Ok(mut it) = std::fs::read_dir(dir) else {
        return;
    };
    while let Some(Ok(entry)) = it.next() {
        if *budget >= MAX_DESKTOP_VISITED || index.len() >= cap {
            return;
        }
        *budget += 1;
        let Ok(ft) = entry.file_type() else {
            continue;
        };
        let p = entry.path();
        if ft.is_dir() {
            walk_desktop(&p, index, budget, cap);
        } else if p.extension().and_then(|e| e.to_str()) == Some("desktop")
            && let Ok(meta) = p.metadata()
            && meta.len() <= MAX_DESKTOP_FILE_BYTES
            && let Ok(contents) = std::fs::read_to_string(&p)
        {
            parse_desktop_entry(&contents, &p, index);
        }
        if index.len() >= cap {
            return;
        }
    }
}

pub fn desktop_done() -> bool {
    DESKTOP_DONE.load(std::sync::atomic::Ordering::SeqCst)
}

/// The desktop entry's own Icon=, resolved to a file. Empty while the index
/// has not reached the entry (or for no such entry).
pub fn resolve_desktop_entry_icon(entry: &str, size_px: u32) -> String {
    if !desktop_done() {
        return String::new();
    }
    let guard = DESKTOP_INDEX
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let Some(index) = guard.as_ref() else {
        return String::new();
    };
    let Some(icon) = index.get(&lower_key(entry)) else {
        return String::new();
    };
    nparse::resolve_image(icon, size_px)
}

// ---------------------------------------------------------------------------
// the fallback face dir (iconless cards wear a face)
// ---------------------------------------------------------------------------

static FALLBACK: std::sync::Mutex<Option<Vec<String>>> = std::sync::Mutex::new(None);

pub fn reset_fallback_cache() {
    *FALLBACK
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner) = None;
}

fn scan_fallback(dir: &str) -> Vec<String> {
    let mut files = Vec::new();
    let mut budget = 0usize;
    if Path::new(dir).is_dir() {
        walk_fallback(Path::new(dir), &mut files, &mut budget);
    }
    files
}

/// One fallback-face scan (the image files of the dir, depth-first).
fn walk_fallback(dir: &Path, files: &mut Vec<String>, budget: &mut usize) {
    let Ok(mut it) = std::fs::read_dir(dir) else {
        return;
    };
    while let Some(Ok(entry)) = it.next() {
        if *budget >= MAX_FALLBACK_VISITS {
            return;
        }
        *budget += 1;
        let Ok(ft) = entry.file_type() else {
            continue;
        };
        let p = entry.path();
        if ft.is_dir() {
            walk_fallback(&p, files, budget);
        } else if matches!(
            p.extension()
                .and_then(|e| e.to_str())
                .map(str::to_ascii_lowercase)
                .as_deref(),
            Some(".png" | ".jpg" | ".jpeg" | ".webp" | ".bmp" | ".avif" | ".jxl" | ".svg")
        ) {
            files.push(p.display().to_string());
        }
    }
}

thread_local! {
    // SplitMix64: good enough for a face roll (no rand dependency)
    static RNG: std::cell::Cell<u64> = const { std::cell::Cell::new(0x2545_F491_4F6C_DD1D) };
}

fn roll() -> u64 {
    RNG.with(|r| {
        let z = r.get().wrapping_add(0x9E37_79B9_7F4A_7C15);
        r.set(z);
        let mut z = z ^ (z >> 30);
        z = z.wrapping_mul(0xBF58_476D_1CE4_E5B9);
        let mut z = z ^ (z >> 27);
        z = z.wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    })
}

/// One roll per card; the dir scan is visit-bounded and cached for the config
/// life. "" = nothing to roll from.
pub fn pick_fallback(dir: &str) -> String {
    if dir.is_empty() {
        return String::new();
    }
    let mut fb = FALLBACK
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let files = fb.get_or_insert_with(|| scan_fallback(dir));
    if files.is_empty() {
        return String::new();
    }
    let n = (roll() % files.len() as u64) as usize;
    files[n].clone()
}

// ---------------------------------------------------------------------------
// the async file decode (the fork's worker thread)
// ---------------------------------------------------------------------------

/// A decode in flight (or settled-null), tracked under a keyed source. The
/// key carries a decode-time tint (`\u{1f}tint:r,g,b`) so a differently
/// tinted request decodes separately — the C++ fg-carrying key.
pub struct DecodeJob {
    pub key: String, // the tracking key (path + tint suffix)
    pub token: u32,  // 0 = rejected (not a file / too big)
}

/// A decoded file's derived texture + its pixel size.
pub struct FileTex {
    pub texture: Option<ffi::TextureHandle>,
    pub settled: bool, // false: a decode is still in flight
    pub hero: bool,
}

fn admissible_image_file(source: &str) -> bool {
    match std::fs::metadata(source) {
        Ok(m) => m.is_file() && m.len() <= MAX_IMAGE_FILE_BYTES,
        Err(_) => false,
    }
}

/// The key under which a (path, tint) decode is tracked.
fn decode_key(path: &str, tint: Option<(u8, u8, u8)>) -> String {
    match tint {
        Some((r, g, b)) => format!("{path}\u{1f}tint:{r},{g},{b}"),
        None => path.to_owned(),
    }
}

/// (Re)derive a texture from a file. `tint` applies a symbolic recolor at
/// decode time. Event loop only (the warm pass). `hero_w_px` 0 = icon box.
pub fn file_tex(
    ctx: ffi::Ctx,
    jobs: &mut Vec<DecodeJob>,
    path: &str,
    icon_px: u32,
    hero_w_px: u32,
    hero_h_cap_px: u32,
    tint: Option<(u8, u8, u8)>,
) -> FileTex {
    let key = decode_key(path, tint);
    let svg = if is_svg_icon_path(path) {
        (icon_px.clamp(1, 256)) as i32
    } else {
        0
    };
    let pos = jobs.iter().position(|j| j.key == key);
    if pos.is_none() && jobs.len() < MAX_PENDING_IMAGE_TOKENS && admissible_image_file(path) {
        let token = ffi::image_decode(ctx, path, svg, tint);
        if token != 0 {
            jobs.push(DecodeJob {
                key: key.clone(),
                token,
            });
        }
        // token 0 (no worker slot): leave it unset, retry on the next warm
    }
    let Some(pos) = jobs.iter().position(|j| j.key == key) else {
        // not in flight: rejected (settled null) or no slot (unsettled)
        let rejected = !admissible_image_file(path);
        if rejected {
            jobs.push(DecodeJob { key, token: 0 });
        }
        return FileTex {
            texture: None,
            settled: rejected,
            hero: false,
        };
    };
    let token = jobs[pos].token;
    let Some(status) = ffi::image_token_status(ctx, token) else {
        return FileTex {
            texture: None,
            settled: false,
            hero: false,
        };
    };
    if status == 0 {
        return FileTex {
            texture: None,
            settled: false,
            hero: false,
        };
    }
    if status != 1 {
        // a failed decode stays failed: no disk retry per warm
        ffi::image_token_drop(ctx, token);
        jobs.remove(pos);
        return FileTex {
            texture: None,
            settled: true,
            hero: false,
        };
    }
    let Some((sw, sh)) = ffi::image_token_size(ctx, token) else {
        ffi::image_token_drop(ctx, token);
        jobs.remove(pos);
        return FileTex {
            texture: None,
            settled: true,
            hero: false,
        };
    };
    if sw == 0 || sh == 0 || (sw as f64 * sh as f64) > MAX_DECODED_IMAGE_PIXELS {
        ffi::image_token_drop(ctx, token);
        jobs.remove(pos);
        return FileTex {
            texture: None,
            settled: true,
            hero: false,
        };
    }
    let hero = hero_worthy(sw, sh, hero_w_px);
    let out = if hero {
        let cap = std::cmp::min(
            (hero_w_px as f64 * sh as f64 / sw as f64).round() as u32,
            hero_h_cap_px,
        );
        ffi::image_token_texture(ctx, token, 1, 0, hero_w_px, cap)
    } else {
        ffi::image_token_texture(ctx, token, 0, icon_px, 0, 0)
    };
    let Some((tex, _dw, _dh)) = out else {
        ffi::image_token_drop(ctx, token);
        jobs.remove(pos);
        return FileTex {
            texture: None,
            settled: true,
            hero: false,
        };
    };
    // the texture outlives the token: drop releases only the decoded buffer
    ffi::image_token_drop(ctx, token);
    jobs.remove(pos);
    FileTex {
        texture: Some(tex),
        settled: true,
        hero,
    }
}

/// Whether a decoded size takes the hero layout: 256 px both ways and at
/// least half the hero box wide.
pub fn hero_worthy(sw: u32, sh: u32, hero_w_px: u32) -> bool {
    hero_w_px > 0 && sw >= 256 && sh >= 256 && sw * 2 >= hero_w_px
}

/// Drop every in-flight decode (teardown).
pub fn drop_all_decodes(ctx: ffi::Ctx, jobs: &mut Vec<DecodeJob>) {
    for j in jobs.iter() {
        if j.token != 0 {
            ffi::image_token_drop(ctx, j.token);
        }
    }
    jobs.clear();
}

// ---------------------------------------------------------------------------
// the generated (initials) avatars — an LRU, not a size cap
// ---------------------------------------------------------------------------

// The avatar cache is a thread_local (not a `static Mutex`): the textures it
// holds are not `Sync` (a texture must never be touched off the event loop),
// and the warm pass — its only reader and writer — is single-threaded.
type AvatarCache = (Vec<String>, HashMap<String, Arc<ffi::TextureHandle>>);

thread_local! {
    static AVATARS: std::cell::RefCell<Option<AvatarCache>> =
        const { std::cell::RefCell::new(None) };
}

/// One deterministic initials face per (identity, name, font, size, bg).
pub fn generated_avatar(
    ctx: ffi::Ctx,
    cfg: &crate::notify::NConfig,
    identity: &str,
    name: &str,
    px: u32,
) -> Option<Arc<ffi::TextureHandle>> {
    let dark = (cfg.col_bg.r as f64) + (cfg.col_bg.g as f64) + (cfg.col_bg.b as f64) < 1.5;
    let px = px.clamp(16, 128);
    let key = format!(
        "{}:{identity}\u{1f}{}:{name}\u{1f}{}:{}\u{1f}{px}\u{1f}{dark}",
        identity.len(),
        name.len(),
        cfg.font.len(),
        cfg.font
    );
    AVATARS.with(|cell| {
        let mut av = cell.borrow_mut();
        let (lru, map) = av.get_or_insert_with(|| (Vec::new(), HashMap::new()));
        if let Some(tex) = map.get(&key) {
            // refresh the LRU position (move to the back)
            if let Some(i) = lru.iter().position(|k| k == &key) {
                lru.remove(i);
                lru.push(key.clone());
            }
            return Some(tex.clone());
        }
        if lru.len() >= MAX_GENERATED_AVATARS {
            let old = lru.remove(0);
            map.remove(&old);
        }
        let tint = nparse::avatar_color(identity, dark);
        let label = nparse::initials(if name.is_empty() { identity } else { name });
        let bg = ffi::hl_color_t {
            r: tint.0 as f32,
            g: tint.1 as f32,
            b: tint.2 as f32,
            a: 1.0,
        };
        let fg = if nparse::light_avatar_foreground(tint) {
            ffi::hl_color_t {
                r: 0.98,
                g: 0.99,
                b: 1.0,
                a: 1.0,
            }
        } else {
            ffi::hl_color_t {
                r: 0.06,
                g: 0.08,
                b: 0.10,
                a: 1.0,
            }
        };
        let tex = ffi::avatar_texture(ctx, bg, &label, fg, px, &cfg.font)?;
        let arc = Arc::new(tex);
        lru.push(key.clone());
        map.insert(key, arc.clone());
        Some(arc)
    })
}

pub fn clear_avatars() {
    AVATARS.with(|c| *c.borrow_mut() = None);
}

// ---------------------------------------------------------------------------
// the ensure* recipes (warm pass; the texture rule)
// ---------------------------------------------------------------------------

/// A symbolic icon path (the freedesktop convention).
pub fn is_symbolic_icon_path(path: &str) -> bool {
    is_svg_icon_path(path) && (path.contains("-symbolic") || path.contains("/symbolic/"))
}

fn fg_tint(cfg: &crate::notify::NConfig) -> (u8, u8, u8) {
    (
        ((cfg.col_fg.r as f64) * 255.0) as u8,
        ((cfg.col_fg.g as f64) * 255.0) as u8,
        ((cfg.col_fg.b as f64) * 255.0) as u8,
    )
}

/// (Re)build n.icon (content) and n.ident (identity) when their sources
/// changed. Event loop only.
#[allow(clippy::too_many_lines)]
pub fn ensure_icon_tex(
    ctx: ffi::Ctx,
    cfg: &crate::notify::NConfig,
    jobs: &mut Vec<DecodeJob>,
    n: &mut nparse::Notif,
    icon_px: u32,
    hero_w_px: u32,
    hero_h_cap_px: u32,
) {
    // IDENTITY: the app_icon/desktop-entry, or the rolled fallback face, or
    // the generic mark. An identity is icon-box only (hero_w 0).
    if !n.identity.is_empty() {
        n.fallback_pick.clear();
        let sym = is_symbolic_icon_path(&n.identity);
        let key = if sym {
            format!("{}:\u{1f}{}", n.identity, cfg.fg_hex)
        } else {
            n.identity.clone()
        };
        if n.ident.for_ != key || n.ident.px != icon_px {
            n.ident.tex = None;
            n.ident.for_ = key;
            n.ident.px = icon_px;
            n.ident.settled = false;
        }
        if !n.ident.settled {
            let tint = sym.then(|| fg_tint(cfg));
            let tex = file_tex(ctx, jobs, &n.identity, icon_px, 0, 0, tint);
            if tex.settled {
                n.ident.tex = tex.texture.map(std::sync::Arc::new);
                n.ident.settled = true;
                if n.ident.tex.is_none() {
                    // a known-bad source: the fallback face (or the generic
                    // mark) takes over next warm, not a retry
                    n.identity.clear();
                    n.ident.for_.clear();
                    n.ident.px = 0;
                    n.ident.settled = false;
                }
            }
        }
    } else if n.fallback_pick.is_empty() {
        n.fallback_pick = pick_fallback(&cfg.fallback_icon_dir);
    }
    if n.identity.is_empty() && n.fallback_pick.is_empty() {
        // the generic mark (one neutral face for every iconless card)
        let generic = format!("__notify_generic__:\u{1f}{}", cfg.fg_hex);
        if n.ident.for_ != generic || n.ident.px != icon_px {
            n.ident.tex = None;
            generic.clone_into(&mut n.ident.for_);
            n.ident.px = icon_px;
            n.ident.settled = false;
        }
        if n.ident.for_ == generic && !n.ident.settled {
            n.ident.tex = ffi::generic_mark_texture(
                ctx,
                ffi::hl_color_t {
                    r: cfg.col_frame.r,
                    g: cfg.col_frame.g,
                    b: cfg.col_frame.b,
                    a: cfg.col_frame.a,
                },
                ffi::hl_color_t {
                    r: cfg.col_fg.r,
                    g: cfg.col_fg.g,
                    b: cfg.col_fg.b,
                    a: cfg.col_fg.a,
                },
                icon_px,
            )
            .map(std::sync::Arc::new);
            n.ident.settled = n.ident.tex.is_some();
        }
    }
    if n.identity.is_empty() && !n.fallback_pick.is_empty() {
        if n.ident.for_ != n.fallback_pick || n.ident.px != icon_px {
            n.ident.tex = None;
            n.ident.for_ = n.fallback_pick.clone();
            n.ident.px = icon_px;
            n.ident.settled = false;
        }
        if n.ident.for_ == n.fallback_pick && !n.ident.settled {
            let tex = file_tex(ctx, jobs, &n.fallback_pick, icon_px, 0, 0, None);
            if tex.settled {
                n.ident.tex = tex.texture.map(std::sync::Arc::new);
                n.ident.settled = true;
                if n.ident.tex.is_none() {
                    n.fallback_pick.clear();
                }
            }
        }
    }

    if n.has_pixels {
        if n.pixels.is_empty() {
            return; // uploaded by an earlier warm; the texture carries it now
        }
        let h = pixels_hash(&n.pixels, n.pw, n.ph);
        if n.icon.tex.is_none() || n.pixels_for != h {
            let hero = hero_worthy(n.pw, n.ph, hero_w_px);
            n.hero = hero;
            n.icon.tex = if hero {
                let cap = std::cmp::min(
                    (hero_w_px as f64 * n.ph as f64 / n.pw as f64).round() as u32,
                    hero_h_cap_px,
                );
                cover_argb(ctx, &n.pixels, n.pw, n.ph, hero_w_px, cap)
            } else if n.pw > icon_px || n.ph > icon_px {
                fit_argb(ctx, &n.pixels, n.pw, n.ph, icon_px)
            } else {
                ffi::texture_from_argb(ctx, &n.pixels, n.pw, n.ph, n.pw * 4)
            }
            .map(std::sync::Arc::new);
            n.pixels_for = h;
            n.icon.for_.clear();
        }
        n.pixels.clear();
        n.pixels.shrink_to_fit();
        return;
    }

    if n.image.is_empty() {
        n.icon.tex = None;
        n.icon.for_.clear();
        n.pixels_for = 0;
        n.hero = false;
        n.icon.settled = true;
        return;
    }
    if n.icon.for_ != n.image || n.icon.px != icon_px {
        n.icon.tex = None;
        n.icon.for_ = n.image.clone();
        n.icon.px = icon_px;
        n.pixels_for = 0;
        n.icon.settled = false;
    }
    if !n.icon.settled {
        let tex = file_tex(ctx, jobs, &n.image, icon_px, hero_w_px, hero_h_cap_px, None);
        if tex.settled {
            n.icon.tex = tex.texture.map(std::sync::Arc::new);
            n.icon.settled = true;
            n.hero = tex.hero;
            if n.icon.tex.is_none() {
                n.image.clear();
                n.icon.for_.clear();
                n.icon.px = 0;
                n.icon.settled = false;
            }
        }
    }
}

/// (Re)build an action button's icon (action-icons only).
pub fn ensure_action_icon(
    ctx: ffi::Ctx,
    cfg: &crate::notify::NConfig,
    jobs: &mut Vec<DecodeJob>,
    action_icons: bool,
    a: &mut nparse::Action,
    icon_px: u32,
) {
    if !action_icons {
        a.icon = None;
        a.icon_for.clear();
        a.icon_settled = false;
        a.icon_px = 0;
        return;
    }
    let mut path = a.id.clone();
    if let Some(rest) = path.strip_prefix("file://") {
        path = rest.to_owned();
    }
    if !path.starts_with('/') {
        path = resolve_icon_name(&a.id, icon_px);
    }
    let sym = is_symbolic_icon_path(&path);
    let key = if sym {
        format!("{}:\u{1f}{}", a.id, cfg.fg_hex)
    } else {
        a.id.clone()
    };
    if a.icon_for != key || a.icon_px != icon_px {
        a.icon_for = key;
        a.icon_px = icon_px;
        a.icon = None;
        a.icon_settled = false;
    }
    if a.icon_settled {
        return;
    }
    if path.is_empty() {
        a.icon_settled = true; // a failed resolve stays failed
        return;
    }
    let tint = sym.then(|| fg_tint(cfg));
    let tex = file_tex(ctx, jobs, &path, icon_px, 0, 0, tint);
    if tex.settled {
        a.icon = tex.texture.map(|t| nparse::IconTex {
            tex: Some(std::sync::Arc::new(t)),
            for_: a.icon_for.clone(),
            px: icon_px,
            settled: true,
        });
        a.icon_settled = true;
    }
}

/// (Re)build a body <img> thumbnail.
pub fn ensure_body_image(
    ctx: ffi::Ctx,
    jobs: &mut Vec<DecodeJob>,
    im: &mut nparse::BodyImage,
    max_px: u32,
) {
    if im.src.is_empty() {
        im.tex = None;
        im.built_for.clear();
        im.settled = false;
        im.built_px = 0;
        return;
    }
    if im.built_for != im.src || im.built_px != max_px {
        im.built_for = im.src.clone();
        im.built_px = max_px;
        im.tex = None;
        im.settled = false;
    }
    if im.settled {
        return;
    }
    let tex = file_tex(ctx, jobs, &im.src, max_px, 0, 0, None);
    if tex.settled {
        im.tex = tex.texture.map(std::sync::Arc::new);
        im.settled = true;
    }
}

/// A facepile avatar: the sender's icon, or the generated initials face.
pub fn ensure_avatar_tex(
    ctx: ffi::Ctx,
    cfg: &crate::notify::NConfig,
    jobs: &mut Vec<DecodeJob>,
    p: &mut nparse::Participant,
    px: u32,
) {
    if p.icon.is_empty() {
        let key = format!(
            "__notify_avatar:\u{1f}{}:{}\u{1f}{}:{}\u{1f}{}:{}\u{1f}{px}\u{1f}{}",
            p.key.len(),
            p.key,
            p.name.len(),
            p.name,
            cfg.font.len(),
            cfg.font,
            cfg.bg_hex
        );
        if p.avatar.for_ == key {
            return;
        }
        // the LRU keeps one Arc; this slot holds a second ref for the
        // participant's draw (the texture outlives both)
        p.avatar.tex = generated_avatar(ctx, cfg, &p.key, &p.name, px);
        p.avatar.for_ = key;
        p.avatar.px = px;
        p.avatar.settled = p.avatar.tex.is_some();
        return;
    }
    let key = format!("{}:{px}", p.icon.len());
    if p.avatar.for_ != key || p.avatar.px != px {
        p.avatar.tex = None;
        p.avatar.for_ = key;
        p.avatar.px = px;
        p.avatar.settled = false;
    }
    if p.avatar.settled {
        return;
    }
    let tex = file_tex(ctx, jobs, &p.icon, px, 0, 0, None);
    if tex.settled {
        p.avatar.tex = tex.texture.map(std::sync::Arc::new);
        p.avatar.settled = true;
        if p.avatar.tex.is_none() {
            // a dead icon: the sender falls back to the generated face
            p.icon.clear();
            p.avatar.for_.clear();
            p.avatar.px = 0;
            p.avatar.settled = false;
            ensure_avatar_tex(ctx, cfg, jobs, p, px);
        }
    }
}

// ---------------------------------------------------------------------------
// the image-data (pixel buffer) scaling, CPU-side (the C++ used cairo)
// ---------------------------------------------------------------------------

fn pixels_hash(px: &[u8], pw: u32, ph: u32) -> u64 {
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    for &b in px {
        h = (h ^ b as u64).wrapping_mul(0x1000_0000_01b3);
    }
    for &b in pw.to_le_bytes().iter().chain(ph.to_le_bytes().iter()) {
        h = (h ^ b as u64).wrapping_mul(0x1000_0000_01b3);
    }
    h
}

/// Bilinear-downscale a premultiplied BGRA buffer to within max_px (both
/// axes). Returns a fresh buffer + its size (the caller uploads it).
fn downscale_argb(px: &[u8], w: u32, h: u32, max_px: u32) -> (Vec<u8>, u32, u32) {
    let scale = (max_px as f32 / w as f32).min(max_px as f32 / h as f32);
    let nw = (w as f32 * scale).round().max(1.0) as u32;
    let nh = (h as f32 * scale).round().max(1.0) as u32;
    if nw == w && nh == h {
        return (px.to_vec(), w, h);
    }
    let mut out = vec![0u8; (nw * nh * 4) as usize];
    let ratio = 1.0 / scale;
    for y in 0..nh {
        let sy = (y as f32 * ratio + 0.5).min(h as f32 - 1.0);
        let y0 = sy as u32;
        let fy = sy - y0 as f32;
        for x in 0..nw {
            let sx = (x as f32 * ratio + 0.5).min(w as f32 - 1.0);
            let x0 = sx as u32;
            let fx = sx - x0 as f32;
            for c in 0..4u32 {
                let i = |yy: u32, xx: u32, c: u32| px[((yy * w + xx) * 4 + c) as usize] as f32;
                let top = i(y0, x0, c) * (1.0 - fx) + i(y0, (x0 + 1).min(w - 1), c) * fx;
                let bot = i((y0 + 1).min(h - 1), x0, c) * (1.0 - fx)
                    + i((y0 + 1).min(h - 1), (x0 + 1).min(w - 1), c) * fx;
                out[((y * nw + x) * 4 + c) as usize] =
                    (top * (1.0 - fy) + bot * fy).round().clamp(0.0, 255.0) as u8;
            }
        }
    }
    (out, nw, nh)
}

fn fit_argb(ctx: ffi::Ctx, px: &[u8], w: u32, h: u32, max_px: u32) -> Option<ffi::TextureHandle> {
    let (buf, nw, nh) = downscale_argb(px, w, h, max_px);
    ffi::texture_from_argb(ctx, &buf, nw, nh, nw * 4)
}

/// Cover-crop to exactly W×H (the hero treatment): the overflowing axis is
/// center-cropped, then bilinear-scaled.
fn cover_argb(
    ctx: ffi::Ctx,
    px: &[u8],
    w: u32,
    h: u32,
    dw: u32,
    dh: u32,
) -> Option<ffi::TextureHandle> {
    let s = (dw as f32 / w as f32).max(dh as f32 / h as f32);
    let cw = (dw as f32 / s).round().max(1.0) as u32;
    let ch = (dh as f32 / s).round().max(1.0) as u32;
    let ox = ((w - cw) / 2) as usize;
    let oy = ((h - ch) / 2) as usize;
    let mut crop = vec![0u8; (cw * ch * 4) as usize];
    for y in 0..ch as usize {
        let src =
            &px[((oy + y) * w as usize + ox) * 4..((oy + y) * w as usize + ox + (cw as usize)) * 4];
        crop[y * (cw as usize) * 4..(y + 1) * (cw as usize) * 4].copy_from_slice(src);
    }
    let (buf, nw, nh) = downscale_argb(&crop, cw, ch, dw.max(dh));
    ffi::texture_from_argb(ctx, &buf, nw, nh, nw * 4)
}
