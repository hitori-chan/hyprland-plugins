// The inline-reply field (reply.cpp): the armed buffer and its keys.

use super::*;

// ---- the reply field (reply.cpp) ----

pub fn reply_armed(st: &mut NotifyState) -> bool {
    if st.reply_id == 0 {
        return false;
    }
    let live = st.center_on && by_id(st, st.reply_id).is_some();
    if !live {
        st.reply_id = 0;
        st.reply_text.clear();
    }
    live
}

pub fn reply_text(st: &NotifyState) -> &str {
    &st.reply_text
}

pub fn reply_open(ctx: ffi::Ctx, st: &mut NotifyState, id: u32) {
    if st.reply_id == id {
        return;
    }
    st.reply_id = id;
    st.reply_text.clear();
    changed(ctx, st);
}

pub fn reply_close(ctx: ffi::Ctx, st: &mut NotifyState) {
    if st.reply_id == 0 {
        return;
    }
    st.reply_id = 0;
    st.reply_text.clear();
    changed(ctx, st);
}

pub fn reply_exit(st: &mut NotifyState) {
    st.reply_id = 0;
    st.reply_text.clear();
}

/// A key press while the field is armed. False = "not ours" (pass through).
/// Returns the text to send on Return (Some), None otherwise.
#[allow(clippy::too_many_lines)]
pub fn reply_key(
    st: &mut NotifyState,
    sym: &str,
    ctrl: bool,
    alt: bool,
    logo: bool,
    utf8: &str,
) -> Option<String> {
    if st.reply_id == 0 || sym.is_empty() {
        return None;
    }
    if logo || alt {
        return None; // window-management chords are never the field's
    }
    if is_bare_modifier(sym) {
        return None;
    }
    if ctrl {
        match sym {
            "u" | "U" => {
                st.reply_text.clear();
                return None;
            }
            "w" | "W" => {
                while st.reply_text.ends_with(' ') {
                    st.reply_text.pop();
                }
                if let Some(p) = st.reply_text.rfind(' ') {
                    st.reply_text.truncate(p);
                }
                return None;
            }
            _ => return None,
        }
    }
    match sym {
        "Escape" => {
            st.reply_id = 0;
            st.reply_text.clear();
            return None;
        }
        "Return" | "KP_Enter" => {
            let id = st.reply_id;
            let tx = std::mem::take(&mut st.reply_text);
            st.reply_id = 0;
            if tx.is_empty() {
                return None;
            }
            let _ = id;
            return Some(tx);
        }
        "BackSpace" => {
            // one UTF-8 codepoint, not one byte
            if let Some(p) = st.reply_text.char_indices().next_back().map(|(i, _)| i) {
                st.reply_text.truncate(p);
            }
            return None;
        }
        _ => {}
    }
    // anything that types a character types into the field
    if !utf8.is_empty() {
        let c = utf8.chars().next().unwrap();
        if (c as u32) >= 0x20 && c != '\u{7f}' && st.reply_text.len() < 2000 {
            st.reply_text.push(c);
        }
        return None;
    }
    // a key with no character and no meaning: the field swallows it (the
    // user is typing, not driving the desktop)
    None
}

/// A bare modifier keysym (a press of one is not a character).
pub fn is_bare_modifier(sym: &str) -> bool {
    matches!(
        sym,
        "Shift_L"
            | "Shift_R"
            | "Control_L"
            | "Control_R"
            | "Alt_L"
            | "Alt_R"
            | "Alt_Gr"
            | "ISO_Level3_Shift"
            | "Meta_L"
            | "Meta_R"
            | "Super_L"
            | "Super_R"
            | "Hyper_L"
            | "Hyper_R"
    )
}
