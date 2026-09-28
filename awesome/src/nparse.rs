// The notify port's payload layer: the card data types and the pure
// transforms an arbitrary D-Bus peer's payload goes through before it
// becomes a card (the hostile-input surface). Port of the C++ parse.cpp +
// pixel_model.hpp + text helpers — byte-exact, no behavior drift.
//
// Pure Rust: no FFI, no locks. The model (notify.rs) owns the list.

// ---- bounds (the hostile-sender caps, model.cpp) ----
pub const MAX_ACTIONS: usize = 8;
pub const MAX_BODY_IMAGES: usize = 4;
pub const MAX_CONV_ID_BYTES: usize = 512;
pub const MAX_MESSAGE_ID_BYTES: usize = 512;
pub const MAX_SENDER_ID_BYTES: usize = 512;
pub const MAX_SENDER_NAME_BYTES: usize = 256;
pub const MAX_CONV_KIND_BYTES: usize = 32;
pub const MAX_APP_NAME_BYTES: usize = 256;
pub const MAX_SUMMARY_BYTES: usize = 1024;
pub const MAX_SOURCE_BYTES: usize = 1024; // app icon / image-path / sound-file / desktop-entry
pub const MAX_ACTION_KEY_BYTES: usize = 256;
pub const MAX_REPLY_TEXT_BYTES: usize = 256;
pub const MAX_BODY_RAW_BYTES: usize = 32768;

// the conversation log bounds (pixel_model.hpp)
pub const MAX_CONVERSATION_MESSAGES: usize = 32;
pub const MAX_PRESENTED_CONVERSATION_MESSAGES: usize = 7;
pub const MAX_CONVERSATION_PARTICIPANTS: usize = 16;
pub const MAX_UNREAD_COUNT: u32 = 999;

/// A non-"default" action: a clickable text button on the card.
pub struct Action {
    pub id: String,
    pub label: String,
    pub icon: Option<IconTex>,
    pub icon_for: String,   // staleness: the id the icon was resolved from
    pub icon_settled: bool, // a pending async decode stays false
    pub icon_px: u32,
}

impl Action {
    pub(crate) fn new(id: String, label: String) -> Self {
        Self {
            id,
            label,
            icon: None,
            icon_for: String::new(),
            icon_settled: false,
            icon_px: 0,
        }
    }
}

/// An icon texture slot: the texture (Arc-shared — the avatar LRU holds one
/// ref, the slot another) + what it was built from.
#[derive(Default, Clone)]
pub struct IconTex {
    pub tex: Option<std::sync::Arc<crate::ffi::TextureHandle>>,
    pub for_: String, // the source key it was built from ("" = none)
    pub px: u32,
    pub settled: bool, // false while an async decode is in flight
}

/// A body <img src>: a thumbnail rendered below the text.
#[derive(Default)]
pub struct BodyImage {
    pub src: String, // resolved file path
    pub tex: Option<std::sync::Arc<crate::ffi::TextureHandle>>,
    pub built_for: String, // staleness: the src the tex was built from
    pub alt: String,       // the img's alt text: the body fallback when the load fails
    pub settled: bool,
    pub built_px: u32,
}

/// One structured message of a conversation (the message-id upsert keeps
/// the log bounded).
#[derive(Default, Clone)]
pub struct Message {
    pub id: String,
    pub sender_id: String,
    pub sender_name: String,
    pub sender_icon: String,
    pub text: String,
    pub timestamp_ms: i64,
    pub historic: bool, // backfill: counted, never unread
}

/// A distinct sender of a conversation; the avatar texture is warm-built and
/// owned here so the face pile and the transcript faces draw from one decode.
#[derive(Default)]
pub struct Participant {
    pub key: String, // stable: the sender-id, else the name
    pub name: String,
    pub icon_source: String, // the raw sender-icon
    pub icon: String,        // resolved path
    pub avatar: IconTex,
}

/// One card. The model (notify.rs) decides what it DOES; this is what it IS.
pub struct Notif {
    pub id: u32,
    pub app_name: String,
    pub app_key: String, // grouping identity: desktop-entry, else the app name
    pub sender: String,  // the bus name that sent this Notify
    pub summary: String, // newlines flattened, whitelisted markup
    pub body: String,    // whitelisted markup (Pango subset)
    pub conversation_id: String,
    pub conversation_title: String,
    pub conversation_kind: String, // "one-to-one" or "group"
    pub conversation_icon_source: String,
    pub conversation_icon: String,
    pub declared_group_key: String, // x-hyprnotify-group-key
    pub messages: Vec<Message>,     // oldest first, bounded
    pub participants: Vec<Participant>,
    pub unread_count: u32,
    pub urgency: u8,
    pub progress: i32,    // 0..100 from the "value" hint, -1 = none
    pub image: String,    // CONTENT source (image-path), resolved, "" = none
    pub identity: String, // IDENTITY source (app_icon/desktop-entry), resolved, "" = none
    pub desktop_entry: String,
    pub identity_from_desktop: bool, // identity rides the hint: the index may upgrade it
    pub pixels: Vec<u8>,             // image-data, premultiplied BGRA; freed once uploaded
    pub has_pixels: bool,            // the LAST Notify carried image-data
    pub pw: u32,
    pub ph: u32,
    pub default_action: String, // the "default" action key; a body click fires it
    pub can_reply: bool,
    pub reply_placeholder: String,
    pub reply_submit_text: String,
    pub actions: Vec<Action>,
    pub body_images: Vec<BodyImage>,
    pub action_icons: bool,
    pub resident: bool,
    pub transient: bool,
    pub conversation: bool,
    pub absorbed: bool, // the open shade parked this banner; the close returns it
    pub fallback_pick: String, // the rolled identity face; survives in-place replaces
    pub waiting: bool,  // DND-queued: collected, not shown, timeout held
    pub banner: bool,   // the popup is up; expiry drops only this
    pub timeout_ms: u64, // resolved; 0 = sticky
    pub deadline: u64,  // steady ms; meaningful when banner && timeout && !waiting
    pub arrived: u64,   // steady ms; a replace refreshes it; the age lines
    pub born: u64,      // creation only; the arrival spring's key
    pub icon: IconTex,  // content avatar (or hero)
    pub ident: IconTex, // identity: the corner badge, or the lead when no content
    pub hero: bool,     // icon.tex was built for the hero layout
    pub pixels_for: u64,
}

impl Notif {
    pub fn new(id: u32) -> Self {
        let now = crate::ffi::steady_ms();
        Self {
            id,
            app_name: String::new(),
            app_key: String::new(),
            sender: String::new(),
            summary: String::new(),
            body: String::new(),
            conversation_id: String::new(),
            conversation_title: String::new(),
            conversation_kind: String::new(),
            conversation_icon_source: String::new(),
            conversation_icon: String::new(),
            declared_group_key: String::new(),
            messages: Vec::new(),
            participants: Vec::new(),
            unread_count: 0,
            urgency: 1,
            progress: -1,
            image: String::new(),
            identity: String::new(),
            desktop_entry: String::new(),
            identity_from_desktop: false,
            pixels: Vec::new(),
            has_pixels: false,
            pw: 0,
            ph: 0,
            default_action: String::new(),
            can_reply: false,
            reply_placeholder: String::new(),
            reply_submit_text: String::new(),
            actions: Vec::new(),
            body_images: Vec::new(),
            action_icons: false,
            resident: false,
            transient: false,
            conversation: false,
            absorbed: false,
            fallback_pick: String::new(),
            waiting: false,
            banner: true,
            timeout_ms: 0,
            deadline: 0,
            arrived: now,
            born: now,
            icon: IconTex::default(),
            ident: IconTex::default(),
            hero: false,
            pixels_for: 0,
        }
    }
}

// ---------------------------------------------------------------------------
// the hostile payload -> values a card can hold (parse.cpp)
// ---------------------------------------------------------------------------

fn is_ascii_alpha(b: u8) -> bool {
    b.is_ascii_alphabetic()
}

/// Whitelist the Pango markup subset; drop disallowed tags; escape stray
/// '<'/'&'. `allow_links` adds <a>. <img> is extracted BEFORE this runs.
/// Byte-exact port of the C++ sanitizer (it iterates bytes).
pub fn sanitize_markup(input: &str, allow_links: bool) -> String {
    let in_b = input.as_bytes();
    let mut out = String::with_capacity(input.len() + 16);
    let mut i = 0usize;
    while i < in_b.len() {
        let ch = in_b[i];
        if ch == b'<' {
            let mut j = i + 1;
            if j < in_b.len() && in_b[j] == b'/' {
                j += 1;
            }
            let ns = j;
            while j < in_b.len() && is_ascii_alpha(in_b[j]) {
                j += 1;
            }
            if j > ns
                && let Some(end_rel) = in_b[j..].iter().position(|&b| b == b'>')
            {
                let end = j + end_rel;
                let name = std::str::from_utf8(&in_b[ns..j]).unwrap_or("");
                let name = name.to_ascii_lowercase();
                if name == "br" {
                    out.push('\n');
                } else if allowed_tag(&name, allow_links) {
                    out.push_str(&input[i..=end]); // live tag, verbatim
                }
                // else: disallowed tag, dropped
                i = end + 1;
                continue;
            }
            out.push_str("&lt;"); // a bare '<' that forms no tag
            i += 1;
            continue;
        }
        if ch == b'&' {
            if let Some(end_rel) = in_b[i..].iter().position(|&b| b == b';') {
                let end = i + end_rel;
                if end - i <= 10 {
                    let e = &input[i + 1..end];
                    if e == "amp"
                        || e == "lt"
                        || e == "gt"
                        || e == "quot"
                        || e == "apos"
                        || (e.len() > 1 && e.starts_with('#'))
                    {
                        out.push_str(&input[i..=end]); // a real entity
                        i = end + 1;
                        continue;
                    }
                }
            }
            out.push_str("&amp;"); // a bare '&'
            i += 1;
            continue;
        }
        if ch != b'\r' {
            out.push(ch as char);
        }
        i += 1;
    }
    out
}

fn allowed_tag(name: &str, allow_links: bool) -> bool {
    matches!(name, "b" | "i" | "u" | "span" | "br") || (allow_links && name == "a")
}

/// One quoted attribute out of one tag, case-insensitive name; the name must
/// stand alone (tag start or whitespace before) and be followed by '='.
/// Byte-exact port.
pub fn attr_value(tag: &str, attr: &str) -> String {
    let lower = tag.to_ascii_lowercase();
    let lb = lower.as_bytes();
    let tb = tag.as_bytes();
    let ab = attr.as_bytes();
    let mut in_quote: u8 = 0;
    let mut i = 0usize;
    while i < lb.len() {
        let ch = lb[i];
        if in_quote != 0 {
            if ch == in_quote {
                in_quote = 0;
            }
            i += 1;
            continue;
        }
        if ch == b'"' || ch == b'\'' {
            in_quote = ch;
            i += 1;
            continue;
        }
        if lb.get(i..i + ab.len()) == Some(ab) {
            let prev_ok = i == 0 || matches!(lb[i - 1], b' ' | b'\t');
            if prev_ok {
                let mut p = i + ab.len();
                while p < lb.len() && matches!(lb[p], b' ' | b'\t') {
                    p += 1;
                }
                if p < lb.len() && lb[p] == b'=' {
                    p += 1;
                    while p < lb.len() && matches!(lb[p], b' ' | b'\t') {
                        p += 1;
                    }
                    if p < lb.len() && (lb[p] == b'"' || lb[p] == b'\'') {
                        let q = tb[p];
                        if let Some(end_rel) = tb[p + 1..].iter().position(|&b| b == q) {
                            return String::from_utf8_lossy(&tb[p + 1..p + 1 + end_rel])
                                .into_owned();
                        }
                        return String::new();
                    }
                }
            }
        }
        i += 1;
    }
    String::new()
}

/// Newlines to spaces (the one-line summary).
pub fn one_line(s: &str) -> String {
    s.replace('\n', " ")
}

/// A path (file:// or absolute) is taken verbatim; anything else is a
/// freedesktop icon NAME resolved against the theme. "" = nothing usable.
pub fn resolve_image(mut s: &str, size_px: u32) -> String {
    if s.is_empty() {
        return String::new();
    }
    if let Some(rest) = s.strip_prefix("file://") {
        s = rest;
    }
    if s.starts_with('/') {
        return s.to_owned();
    }
    crate::nicons::resolve_icon_name(s, size_px)
}

const MAX_SRC_BYTES: usize = 1024;

fn clip_attr(mut s: String) -> String {
    const CAP: usize = 512;
    if s.len() <= CAP {
        return s;
    }
    s.truncate(CAP);
    while !s.is_empty() && (s.as_bytes()[s.len() - 1] & 0xc0) == 0x80 {
        s.pop();
    }
    if !s.is_empty() && s.as_bytes()[s.len() - 1] >= 0xc2 {
        s.pop();
    }
    s
}

/// Pull every `<img src>` out of the body (dropping the tags), resolving each
/// src (path or themed icon name). http(s)/data: srcs are skipped.
pub fn extract_images(body: &mut String, size_px: u32) -> Vec<BodyImage> {
    let mut out = Vec::new();
    let mut i = 0usize;
    while i < body.len() {
        let b = body.as_bytes();
        if b[i] != b'<' {
            i += 1;
            continue;
        }
        let mut j = i + 1;
        while j < b.len() && is_ascii_alpha(b[j]) {
            j += 1;
        }
        let name = std::str::from_utf8(&b[i + 1..j])
            .unwrap_or("")
            .to_ascii_lowercase();
        if name != "img" {
            i += 1;
            continue;
        }
        let Some(end_rel) = body[i..].find('>') else {
            break;
        };
        let end = i + end_rel;
        let tag = &body[i..=end];
        let src = attr_value(tag, "src");
        let alt = one_line(&sanitize_markup(&clip_attr(attr_value(tag, "alt")), false));
        if !src.is_empty()
            && src.len() <= MAX_SRC_BYTES
            && !src.starts_with("http")
            && !src.starts_with("data:")
            && !resolve_image(&src, size_px).is_empty()
        {
            out.push(BodyImage {
                src: resolve_image(&src, size_px),
                alt,
                ..BodyImage::default()
            });
        }
        body.replace_range(i..=end, ""); // drop the tag from the text
        // i now points at the char after the removal
    }
    out
}

/// "<b>Alice</b>\nmessage" -> "<b>Alice</b>: message" (the sender's leading
/// bold line folds to one line per message).
pub fn fold_sender_prefix(body: &str) -> String {
    if !body.starts_with("<b>") {
        return body.to_owned();
    }
    let Some(close) = body.find("</b>") else {
        return body.to_owned();
    };
    let Some(nl) = body[close..].find('\n').map(|r| close + r) else {
        return body.to_owned(); // no message line after the sender
    };
    let mut out = String::with_capacity(body.len());
    out.push_str(&body[..close + 4]); // through the '>' of "</b>"
    out.push_str(": ");
    out.push_str(&body[nl + 1..]);
    out
}

/// Newest-front join: the card's visible lines are the LATEST messages; the
/// 8192-byte cap drops the oldest (bottom) lines.
pub fn join_append(old_body: &str, add: &str) -> String {
    const CAP: usize = 8192;
    let joined = if old_body.is_empty() {
        add.to_owned()
    } else {
        format!("{add}\n{old_body}")
    };
    let mut joined = joined;

    while joined.len() > CAP {
        if let Some(nl) = joined.rfind('\n') {
            joined.replace_range(nl.., ""); // the separator plus the oldest line
        } else {
            joined.truncate(CAP);
            break;
        }
    }
    joined
}

/// The plain Pango escape (headers and labels are escaped, never sanitized:
/// the app name is trusted-but-escaped, tags would just show as text).
pub fn esc(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars() {
        match c {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            _ => out.push(c),
        }
    }
    out
}

/// A colour as a Pango span hex (`#rrggbb`, no alpha — spans can't carry it).
pub fn hex6(r: f32, g: f32, b: f32) -> String {
    format!(
        "#{:02x}{:02x}{:02x}",
        (r * 255.0).round().clamp(0.0, 255.0) as u32,
        (g * 255.0).round().clamp(0.0, 255.0) as u32,
        (b * 255.0).round().clamp(0.0, 255.0) as u32
    )
}

/// Cut a string to at most `cap` bytes, codepoint-safe (a cut that splits a
/// UTF-8 sequence drops the partial tail).
pub fn clip_utf8(mut s: String, cap: usize) -> String {
    if s.len() <= cap {
        return s;
    }
    s.truncate(cap);
    while !s.is_empty() && (s.as_bytes()[s.len() - 1] & 0xc0) == 0x80 {
        s.pop();
    }
    if !s.is_empty() && s.as_bytes()[s.len() - 1] >= 0xc2 {
        s.pop(); // a lead byte whose followers the cut took
    }
    s
}

pub fn cap_utf8(s: String) -> String {
    clip_utf8(s, 8192)
}

/// The collapsed row's one-liner for an ORDINARY body: the last non-empty
/// line (the newest message ends a chronological body).
pub fn last_line(body: &str) -> &str {
    let end = body
        .char_indices()
        .rfind(|(_, c)| *c != '\n')
        .map_or(0, |(i, _)| i + 1);
    if end == 0 {
        return "";
    }
    let nl = body[..end].rfind('\n').map_or(0, |i| i + 1);
    &body[nl..end]
}

/// The conversation twin: a conversation card's body is built NEWEST-FRONT,
/// so its newest message LEADS.
pub fn first_line(body: &str) -> &str {
    let start = body
        .char_indices()
        .find(|(_, c)| *c != '\n')
        .map_or(body.len(), |(i, _)| i);
    if start == body.len() {
        return "";
    }
    let end = body[start..].find('\n').map_or(body.len(), |r| start + r);
    &body[start..end]
}

/// The collapsed row's one-liner, whichever end holds the newest message.
pub fn collapsed_line(n: &Notif) -> String {
    if n.body.is_empty() {
        return String::new();
    }
    let line = if n.conversation {
        first_line(&n.body)
    } else {
        last_line(&n.body)
    };
    line.to_owned()
}

/// Bucketed age: "now", "5m", "2h", "3d" (steady-ms `born`/`arrived`).
pub fn age_string(now_ms: u64, since_ms: u64) -> String {
    let s = now_ms.saturating_sub(since_ms) / 1000;
    if s < 60 {
        "now".to_owned()
    } else if s < 3600 {
        format!("{}m", s / 60)
    } else if s < 86400 {
        format!("{}h", s / 3600)
    } else {
        format!("{}d", s / 86400)
    }
}

/// The spec's image-data: width, height, rowstride, has_alpha,
/// bits_per_sample, channels, RGB(A) bytes -> premultiplied BGRA.
#[allow(clippy::too_many_arguments)]
pub fn unpack_image_data(
    n: &mut Notif,
    w: i32,
    h: i32,
    stride: i32,
    _has_alpha: bool,
    bps: i32,
    channels: i32,
    data: &[u8],
) {
    if w <= 0
        || h <= 0
        || (w as u64) * (h as u64) > (16 << 20)
        || bps != 8
        || (channels != 3 && channels != 4)
        || (stride as i64) < (w as i64) * (channels as i64)
        || data.len() < (stride as usize) * (h - 1).max(0) as usize + w as usize * channels as usize
    {
        return;
    }
    let (w, h, ch) = (w as usize, h as usize, channels as usize);
    let mut px = vec![0u8; w * h * 4];
    for y in 0..h {
        let row = &data[y * stride as usize..];
        let out = &mut px[y * w * 4..(y + 1) * w * 4];
        for x in 0..w {
            let r = row[x * ch];
            let g = row[x * ch + 1];
            let b = row[x * ch + 2];
            let a = if ch == 4 { row[x * ch + 3] } else { 255 };
            out[x * 4] = (b as u32 * a as u32 / 255) as u8;
            out[x * 4 + 1] = (g as u32 * a as u32 / 255) as u8;
            out[x * 4 + 2] = (r as u32 * a as u32 / 255) as u8;
            out[x * 4 + 3] = a;
        }
    }
    n.pixels = px;
    n.pw = w as u32;
    n.ph = h as u32;
    n.has_pixels = true;
}

// ---------------------------------------------------------------------------
// the pixel model (pixel_model.hpp)
// ---------------------------------------------------------------------------

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mutation {
    None,
    Inserted,
    Replaced,
}

pub fn normalize_conversation_kind(kind: &str) -> Option<&'static str> {
    match kind {
        "one-to-one" => Some("one-to-one"),
        "group" => Some("group"),
        _ => None,
    }
}

pub fn matches_conversation(
    app_key: &str,
    conversation_id: &str,
    candidate_app_key: &str,
    candidate_conversation_id: &str,
) -> bool {
    !conversation_id.is_empty()
        && app_key == candidate_app_key
        && conversation_id == candidate_conversation_id
}

/// Upsert one message by id; sort the log oldest-first; bound it (historic
/// entries drop first). Returns the mutation.
#[allow(clippy::too_many_arguments)]
pub fn upsert_message(
    messages: &mut Vec<Message>,
    message_id: &str,
    text: &str,
    sender_id: Option<&str>,
    sender_name: Option<&str>,
    sender_icon: Option<&str>,
    timestamp_ms: Option<i64>,
    historic: Option<bool>,
) -> Mutation {
    if text.is_empty() && message_id.is_empty() {
        return Mutation::None;
    }
    let it = if message_id.is_empty() {
        None
    } else {
        messages.iter().position(|m| m.id == message_id)
    };
    let mut m = it.map(|i| messages[i].clone()).unwrap_or_default();
    m.id = message_id.to_string();
    m.text = text.to_string();
    if let Some(v) = sender_id {
        m.sender_id = v.to_string();
    }
    if let Some(v) = sender_name {
        m.sender_name = v.to_string();
    }
    if let Some(v) = sender_icon {
        m.sender_icon = v.to_string();
    }
    if let Some(v) = timestamp_ms {
        m.timestamp_ms = v;
    }
    if let Some(v) = historic {
        m.historic = v;
    }
    let mutation = if it.is_some() {
        if let Some(i) = it {
            messages[i] = m;
        }
        Mutation::Replaced
    } else {
        messages.push(m);
        Mutation::Inserted
    };
    messages.sort_by_key(|a| {
        if a.timestamp_ms > 0 {
            a.timestamp_ms
        } else {
            i64::MAX
        }
    });
    while messages.len() > MAX_CONVERSATION_MESSAGES {
        let historic = messages.iter().position(|e| e.historic);
        if let Some(i) = historic {
            messages.remove(i);
        } else {
            messages.remove(0);
        }
    }
    mutation
}

/// The first index of the `limit` newest non-empty messages (the presented
/// window's start, walking the log oldest-first).
pub fn presented_message_start(messages: &[Message], limit: usize) -> usize {
    if limit == 0 {
        return messages.len();
    }
    let mut visible = 0usize;
    for i in (0..messages.len()).rev() {
        if !messages[i].text.is_empty() {
            visible += 1;
            if visible == limit {
                return i;
            }
        }
    }
    0
}

pub fn updated_unread_count(
    current: u32,
    explicit: Option<u32>,
    historic: bool,
    mutation: Mutation,
) -> u32 {
    if let Some(e) = explicit {
        return e.min(MAX_UNREAD_COUNT);
    }
    if !historic && mutation == Mutation::Inserted {
        return current.saturating_add(1).min(MAX_UNREAD_COUNT);
    }
    current
}

pub fn participant_key(sender_id: &str, sender_name: &str) -> String {
    let (prefix, value) = if sender_id.is_empty() {
        ("name:", sender_name)
    } else {
        ("id:", sender_id)
    };
    format!("{prefix}{}:{value}", value.len())
}

fn first_codepoint(text: &str, at: usize) -> &str {
    if at >= text.len() {
        return "";
    }
    let c = text[at..].chars().next().map_or(1, char::len_utf8);
    &text[at..at + c.min(text.len() - at)]
}

/// Up to two leading letters (one per word, capped at the last word).
pub fn initials(name: &str) -> String {
    let mut words: Vec<String> = Vec::new();
    let mut boundary = true;
    let mut i = 0usize;
    while i < name.len() {
        let b = name.as_bytes()[i];
        if b >= 0x80 {
            if boundary {
                words.push(first_codepoint(name, i).to_owned());
            }
            let cp = first_codepoint(name, i);
            i += cp.len().max(1);
            boundary = false;
            continue;
        }
        if b.is_ascii_alphanumeric() {
            if boundary {
                words.push((b.to_ascii_uppercase() as char).to_string());
            }
            boundary = false;
        } else {
            boundary = true;
        }
        i += 1;
    }
    if words.is_empty() {
        return "?".to_owned();
    }
    if words.len() == 1 {
        return words[0].clone();
    }
    format!("{}{}", words[0], words[words.len() - 1])
}

/// FNV-1a 32-bit over the identity bytes.
pub fn avatar_hash(identity: &str) -> u32 {
    let mut h: u32 = 2_166_136_261;
    for &c in identity.as_bytes() {
        h = h.wrapping_mul(16_777_619) ^ c as u32;
    }
    h
}

/// HSL-to-RGB avatar tint (the faceless-sender face), dark-theme aware.
pub fn avatar_color(identity: &str, dark_theme: bool) -> (f64, f64, f64) {
    let h = avatar_hash(identity) as f64 % 360.0 / 60.0;
    let s: f64 = if dark_theme { 0.46 } else { 0.52 };
    let l: f64 = if dark_theme { 0.38 } else { 0.70 };
    let c = (1.0 - (2.0 * l - 1.0).abs()) * s;
    let x = c * (1.0 - (h % 2.0 - 1.0).abs());
    let m = l - c / 2.0;
    let (r, g, b) = if h < 1.0 {
        (c, x, 0.0)
    } else if h < 2.0 {
        (x, c, 0.0)
    } else if h < 3.0 {
        (0.0, c, x)
    } else if h < 4.0 {
        (0.0, x, c)
    } else if h < 5.0 {
        (x, 0.0, c)
    } else {
        (c, 0.0, x)
    };
    (r + m, g + m, b + m)
}

pub fn light_avatar_foreground(color: (f64, f64, f64)) -> bool {
    0.2126 * color.0 + 0.7152 * color.1 + 0.0722 * color.2 < 0.50
}

/// The distinct senders of the kept messages, newest first, capped at 16.
pub fn latest_distinct_participant_indices(messages: &[Message]) -> Vec<usize> {
    let mut out = Vec::new();
    let mut seen: std::collections::HashSet<String> = std::collections::HashSet::new();
    for i in (0..messages.len()).rev() {
        if out.len() >= MAX_CONVERSATION_PARTICIPANTS {
            break;
        }
        let key = participant_key(&messages[i].sender_id, &messages[i].sender_name);
        if key == "name:0:" || !seen.insert(key) {
            continue;
        }
        out.push(i);
    }
    out
}
