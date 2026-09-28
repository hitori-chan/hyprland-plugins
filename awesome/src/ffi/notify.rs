// The notify port surface: markup text, keyboard, cursor, icons,
// activation, and the fork-side async image decode.
#![allow(unsafe_code)]

use super::*;

// ---------------------------------------------------------------------------
// notify port — markup text, keyboard, cursor, icons, activation
// ---------------------------------------------------------------------------

/// A link hit-rect (physical px, texture-relative, inclusive-ish x0/y0..x1/y1).
#[derive(Clone, Copy)]
pub struct LinkRect {
    pub x0: f32,
    pub y0: f32,
    pub x1: f32,
    pub y1: f32,
}

/// A rendered markup line: the texture (one ref) + its pixel size + the link
/// rects/hrefs of its `<a href>` spans.
/// overflowed the cap (the texture is still valid, some spans lost their
/// rect).
pub struct MarkupText {
    pub tex: TextureHandle,
    pub w: u32,
    pub h: u32,
    pub links: Vec<LinkRect>,
    pub hrefs: Vec<String>,
}

/// Rasterize markup text (the body/summary renderer). `max_w` 0 = no cap;
/// `max_h` > 0 caps the pixel height (tail ellipsis), < 0 caps the line count,
/// 0 caps nothing. `weight` 400 regular / 500 medium / 600 bold. A link span
/// paints in `link_col` when `link_col.is_some()`.
#[allow(clippy::too_many_arguments)]
pub fn markup_text(
    ctx: Ctx,
    text: &str,
    col: hl_color_t,
    pt: u32,
    font: &str,
    max_w: u32,
    max_h: i32,
    line_sp: f32,
    weight: i32,
    link_col: Option<hl_color_t>,
    links_cap: u32,
) -> Option<MarkupText> {
    let t = format!("{text}\0");
    let f = format!("{font}\0");
    let mut out: *mut hl_texture = std::ptr::null_mut();
    let mut w: u32 = 0;
    let mut h: u32 = 0;
    let mut links: Vec<hl_link_rect_t> = vec![
        hl_link_rect_t {
            x0: 0.0,
            y0: 0.0,
            x1: 0.0,
            y1: 0.0,
        };
        links_cap as usize
    ];
    let mut hrefs: Vec<hl_str_t> = vec![
        hl_str_t {
            d: std::ptr::null(),
            l: 0,
        };
        links_cap as usize
    ];
    let lc = match link_col {
        Some(c) => c,
        None => hl_color_t {
            r: 0.0,
            g: 0.0,
            b: 0.0,
            a: 0.0,
        },
    };
    let lc_ref: *const hl_color_t = &raw const lc;
    let rc = unsafe {
        hl_markup_text(
            ctx,
            t.as_ptr().cast::<c_char>(),
            col,
            pt,
            f.as_ptr().cast::<c_char>(),
            max_w,
            max_h,
            line_sp,
            weight,
            if link_col.is_some() {
                lc_ref
            } else {
                std::ptr::null()
            },
            links_cap,
            &raw mut out,
            &raw mut w,
            &raw mut h,
            links.as_mut_ptr(),
            hrefs.as_mut_ptr(),
        )
    };
    if rc != HL_E_OK && rc != HL_E_FULL {
        return None;
    }
    let tex = unsafe { TextureHandle::from_raw(out) }?;
    Some(MarkupText {
        tex,
        w,
        h,
        links: links
            .iter()
            .map(|r| LinkRect {
                x0: r.x0,
                y0: r.y0,
                x1: r.x1,
                y1: r.y1,
            })
            .collect(),
        hrefs: hrefs.iter().map(str_from).collect(),
    })
}

/// The seat keyboard's effective state for `keycode`: the keysym name, the
/// ctrl/alt/logo modifiers in effect, and the UTF-8 char (if any).
pub fn keyboard_key(
    ctx: Ctx,
    keycode: u32,
    utf8_cap: u32,
) -> Option<(String, bool, bool, bool, String)> {
    let mut sym: hl_str_t = hl_str_t {
        d: std::ptr::null(),
        l: 0,
    };
    let mut ctrl: u32 = 0;
    let mut alt: u32 = 0;
    let mut logo: u32 = 0;
    let mut buf = vec![0i8; utf8_cap as usize];
    let rc = unsafe {
        hl_keyboard_key(
            ctx,
            keycode,
            &raw mut sym,
            &raw mut ctrl,
            &raw mut alt,
            &raw mut logo,
            buf.as_mut_ptr(),
            utf8_cap,
        )
    };
    if rc != HL_E_OK {
        return None;
    }
    let s = str_from(&sym);
    let end = buf.iter().position(|&b| b == 0).unwrap_or(buf.len());
    let u = unsafe {
        String::from_utf8_lossy(std::slice::from_raw_parts(buf.as_ptr().cast::<u8>(), end))
            .into_owned()
    };
    Some((s, ctrl != 0, alt != 0, logo != 0, u))
}

/// The cursor shape override for the SPECIAL_ACTION group ("pointer"/"grab";
/// `None` = the shape is unset, i.e. the pointer reverts).
pub fn cursor_override(ctx: Ctx, shape: Option<&str>, on: bool) {
    let s = shape.unwrap_or("").to_string();
    let z = format!("{s}\0");
    unsafe { hl_cursor_override(ctx, z.as_ptr().cast::<c_char>(), u32::from(on)) };
}

/// The backend PID of `w` (0 when the window is not X11 — the gate uses this
/// to distinguish fake-sni from the nested instance).
pub fn window_pid(ctx: Ctx, w: &WindowHandle) -> u32 {
    unsafe { hl_window_pid(ctx, w.as_raw()) }
}

/// Mint an xdg-activation token (the click-through portal). `None` when the
/// activation protocol is unavailable.
pub fn activation_token(ctx: Ctx) -> Option<String> {
    let mut s: hl_str_t = hl_str_t {
        d: std::ptr::null(),
        l: 0,
    };
    (unsafe { hl_activation_token(ctx, &raw mut s) } == HL_E_OK).then(|| str_from(&s))
}

// ---- async image decode (the fork's worker thread) ----

/// Queue a file decode. Returns a token (0 = no free slot, retry later).
/// `svg_px` 0 = raster only / native size; a hint for SVGs. `tint` sets a
/// symbolic-recolor (r,g,b in 0..=255) when Some.
pub fn image_decode(ctx: Ctx, path: &str, svg_px: i32, tint: Option<(u8, u8, u8)>) -> u32 {
    let z = format!("{path}\0");
    match tint {
        Some((r, g, b)) => unsafe {
            hl_image_decode(ctx, z.as_ptr().cast::<c_char>(), svg_px, 1, r, g, b)
        },
        None => unsafe { hl_image_decode(ctx, z.as_ptr().cast::<c_char>(), svg_px, 0, 0, 0, 0) },
    }
}

/// A decode token's state: 0 pending, 1 ready, 2 failed, None unknown.
pub fn image_token_status(ctx: Ctx, token: u32) -> Option<i32> {
    let s = unsafe { hl_image_token_status(ctx, token) };
    (s != -1).then_some(s)
}

/// A ready decode's pixel size.
pub fn image_token_size(ctx: Ctx, token: u32) -> Option<(u32, u32)> {
    let mut w: u32 = 0;
    let mut h: u32 = 0;
    (unsafe { hl_image_token_size(ctx, token, &raw mut w, &raw mut h) } == HL_E_OK)
        .then_some((w, h))
}

/// Derive a texture from a ready token (event-loop context only — the warm
/// pass). `mode` 0 = fit within max_px, 1 = cover-crop to w×h. Returns the
/// texture + its derived pixel size.
pub fn image_token_texture(
    ctx: Ctx,
    token: u32,
    mode: u32,
    max_px: u32,
    w: u32,
    h: u32,
) -> Option<(TextureHandle, u32, u32)> {
    let mut out: *mut hl_texture = std::ptr::null_mut();
    let mut dw: u32 = 0;
    let mut dh: u32 = 0;
    if unsafe {
        hl_image_token_texture(
            ctx,
            token,
            mode,
            max_px,
            w,
            h,
            &raw mut out,
            &raw mut dw,
            &raw mut dh,
        )
    } != HL_E_OK
    {
        return None;
    }
    unsafe { TextureHandle::from_raw(out) }.map(|t| (t, dw, dh))
}

/// Release a decode token (its decoded buffer; the derived textures live on).
pub fn image_token_drop(ctx: Ctx, token: u32) {
    unsafe { hl_image_token_drop(ctx, token) };
}

/// An initials avatar (the faceless-sender face): `bg` square, `text` in
/// `fg`, centered bold.
pub fn avatar_texture(
    ctx: Ctx,
    bg: hl_color_t,
    text: &str,
    fg: hl_color_t,
    px: u32,
    font: &str,
) -> Option<TextureHandle> {
    let t = format!("{text}\0");
    let f = format!("{font}\0");
    let mut out: *mut hl_texture = std::ptr::null_mut();
    let rc = unsafe {
        hl_avatar_texture(
            ctx,
            bg,
            t.as_ptr().cast::<c_char>(),
            fg,
            px,
            f.as_ptr().cast::<c_char>(),
            &raw mut out,
        )
    };
    if rc != HL_E_OK {
        return None;
    }
    unsafe { TextureHandle::from_raw(out) }
}

/// A Material chevron (the fold indicator): `dir` 0 = down, 1 = up.
pub fn chevron_texture(ctx: Ctx, dir: u32, col: hl_color_t, px: u32) -> Option<TextureHandle> {
    let mut out: *mut hl_texture = std::ptr::null_mut();
    let rc = unsafe { hl_chevron_texture(ctx, dir, col, px, &raw mut out) };
    if rc != HL_E_OK {
        return None;
    }
    unsafe { TextureHandle::from_raw(out) }
}

/// The alpha-carrying twin of `texture_from_rgba`: premultiplied BGRA
/// (the fd.o image-data layout).
pub fn texture_from_argb(
    ctx: Ctx,
    data: &[u8],
    w: u32,
    h: u32,
    stride: u32,
) -> Option<TextureHandle> {
    let mut out: *mut hl_texture = std::ptr::null_mut();
    let rc = unsafe { hl_texture_from_argb(ctx, data.as_ptr(), w, h, stride, &raw mut out) };
    if rc != HL_E_OK {
        return None;
    }
    unsafe { TextureHandle::from_raw(out) }
}

/// The iconless-card generic mark (a rounded plate + a 2×2 glyph grid).
pub fn generic_mark_texture(
    ctx: Ctx,
    plate: hl_color_t,
    ink: hl_color_t,
    px: u32,
) -> Option<TextureHandle> {
    let mut out: *mut hl_texture = std::ptr::null_mut();
    let rc = unsafe { hl_generic_mark_texture(ctx, plate, ink, px, &raw mut out) };
    if rc != HL_E_OK {
        return None;
    }
    unsafe { TextureHandle::from_raw(out) }
}
