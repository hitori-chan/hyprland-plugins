// (the model: model.cpp)

use super::*;

// ---------------------------------------------------------------------------
// the model (model.cpp)
// ---------------------------------------------------------------------------

pub(crate) fn by_id(st: &NotifyState, id: u32) -> Option<usize> {
    st.notifs.iter().position(|n| n.id == id)
}

/// Cards that opt out of residency: they vanish on expiry and never park.
/// This set MUST stay the same everywhere the expiry sweep uses it.
pub fn vanishes(n: &Notif) -> bool {
    n.transient || n.progress >= 0 || in_osd_band(n.id)
}

pub fn group_key_of(n: &Notif) -> String {
    if n.declared_group_key.is_empty() {
        n.app_key.clone()
    } else {
        format!("{}{:}", n.app_key, n.declared_group_key)
    }
}

/// The client sent -1: critical always sticks; ephemerals run the low clock;
/// normal runs timeout_normal. An explicit expire_timeout never lands here.
fn default_timeout(n: &Notif, cfg: &NConfig) -> u64 {
    if n.urgency >= 2 {
        return 0;
    }
    if n.urgency == 0 || n.transient || n.progress >= 0 {
        return cfg.timeout_low.max(0) as u64;
    }
    cfg.timeout_normal.max(0) as u64
}

fn evict_overflow(
    _ctx: ffi::Ctx,
    st: &mut NotifyState,
    cfg: &NConfig,
    replies: &mut Vec<crate::bus::BusReply>,
) {
    let cap = (cfg.max_notifs.max(1)) as usize;
    while st.notifs.len() > cap {
        let mut victim = st.notifs.len() - 1;
        for i in (0..st.notifs.len().saturating_sub(1)).rev() {
            if st.notifs[i].urgency < 2 {
                victim = i;
                break;
            }
        }
        let vid = st.notifs[victim].id;
        st.notifs.remove(victim);
        replies.push(crate::bus::BusReply::Closed {
            id: vid,
            reason: R_UNDEFINED,
        });
    }
}

/// One banner per app: does another card already hold a banner for this app?
fn app_has_banner(st: &NotifyState, self_idx: usize) -> bool {
    let self_key = st.notifs[self_idx].app_key.clone();
    for (i, o) in st.notifs.iter().enumerate() {
        if i == self_idx || o.waiting || !o.banner || vanishes(o) || o.app_key != self_key {
            continue;
        }
        return true;
    }
    false
}

/// The conversation's display body: the latest kept messages, newest on top,
/// group senders prefixed by name.
fn conversation_body(n: &Notif) -> String {
    if n.messages.is_empty() {
        return n.body.clone();
    }
    let mut out = String::new();
    let start =
        nparse::presented_message_start(&n.messages, nparse::MAX_PRESENTED_CONVERSATION_MESSAGES);
    for i in start..n.messages.len() {
        let m = &n.messages[i];
        if m.text.is_empty() {
            continue;
        }
        let mut line = String::new();
        if n.conversation_kind == "group" && !m.sender_name.is_empty() {
            line.push_str(&nparse::one_line(&nparse::sanitize_markup(
                &m.sender_name,
                false,
            )));
            line.push_str(": ");
        }
        line.push_str(&m.text);
        if out.is_empty() {
            out = nparse::cap_utf8(line);
            continue;
        }
        if line.len() + 1 + out.len() > 8192 {
            break;
        }
        let combined = format!("{line}\n{out}");
        out = combined;
    }
    out
}

/// The distinct senders of the kept messages, newest first, capped; the
/// textures ride on the participants, not on the messages.
fn rebuild_participants(n: &mut Notif, icon_px: u32) {
    let indices = nparse::latest_distinct_participant_indices(&n.messages);
    let previous = std::mem::take(&mut n.participants);
    n.participants.clear();
    for index in indices {
        let m = n.messages[index].clone();
        let key = nparse::participant_key(&m.sender_id, &m.sender_name);
        let mut p = nparse::Participant {
            key: key.clone(),
            name: m.sender_name.clone(),
            icon_source: m.sender_icon.clone(),
            icon: nparse::resolve_image(&m.sender_icon, icon_px),
            avatar: nparse::IconTex::default(),
        };
        if let Some(old) = previous.iter().find(|item| {
            item.key == p.key && item.name == p.name && item.icon_source == p.icon_source
        }) {
            p.avatar = old.avatar.clone();
        } else if p.icon.is_empty() {
            p.icon = nparse::resolve_image(&m.sender_icon, icon_px);
        }
        n.participants.push(p);
    }
    // resolve the icons for the new participants (warm pass only)
    for p in &mut n.participants {
        if p.icon.is_empty() && !p.icon_source.is_empty() {
            p.icon = nparse::resolve_image(&p.icon_source, icon_px);
        }
    }
}

// ---- the arrival (the big one) ----

/// A Notify payload becomes (or refreshes) a card; returns its id. Runs on
/// the event loop (the bus thread deferred it here). `replies` collects the
/// outbound signals (closed, state) to post back to the bus.
#[allow(clippy::too_many_lines, clippy::too_many_arguments)]
pub fn arrive(
    ctx: ffi::Ctx,
    st: &mut NotifyState,
    app_name: &str,
    replaces_id: u32,
    app_icon: &str,
    summary: &str,
    body: &str,
    sender: &str,
    actions: &[String],
    hints: &crate::bus::HintMap,
    expire_timeout: i32,
    replies: &mut Vec<crate::bus::BusReply>,
) -> u32 {
    let cfg = config(ctx, st);
    let mut id = replaces_id;

    let app = nparse::clip_utf8(app_name.to_owned(), nparse::MAX_APP_NAME_BYTES);
    let app_icon = nparse::clip_utf8(app_icon.to_owned(), nparse::MAX_SOURCE_BYTES);
    let sum = nparse::clip_utf8(summary.to_owned(), nparse::MAX_SUMMARY_BYTES);
    let mut txt = nparse::clip_utf8(body.to_owned(), nparse::MAX_BODY_RAW_BYTES);

    // hint readers (the typed ones: absent = None, over-cap opaque = reject)
    let str_hint = |k: &str| -> String { hints.get_str(k).to_owned() };
    let opt_str = |k: &str, alias: Option<&str>, cap: usize, opaque: bool| -> Option<String> {
        let look = |k: &str| -> Option<String> {
            let v = hints.get_str(k);
            if v.is_empty() {
                return None;
            }
            if opaque {
                (v.len() <= cap).then(|| v.to_owned())
            } else {
                Some(nparse::clip_utf8(v.to_owned(), cap))
            }
        };
        let v = look(k);
        v.or_else(|| alias.and_then(look))
    };

    let desktop = nparse::clip_utf8(str_hint("desktop-entry"), nparse::MAX_SOURCE_BYTES);
    let app_key = if desktop.is_empty() {
        app.clone()
    } else {
        desktop.clone()
    };
    let cat = str_hint("category");
    let category_conversation =
        cat.starts_with("im.") || cat == "im" || cat.starts_with("call.") || cat == "call";

    let conv_id = opt_str(
        "conversation-id",
        Some("x-hyprnotify-conversation-id"),
        nparse::MAX_CONV_ID_BYTES,
        true,
    );
    let conv_title = opt_str(
        "conversation-title",
        Some("x-hyprnotify-conversation-title"),
        512,
        false,
    );
    let conv_kind = opt_str(
        "conversation-kind",
        Some("x-hyprnotify-conversation-kind"),
        nparse::MAX_CONV_KIND_BYTES,
        true,
    );
    let conv_icon = opt_str(
        "conversation-icon",
        Some("x-hyprnotify-conversation-icon"),
        512,
        true,
    );
    let sender_id = opt_str(
        "sender-id",
        Some("x-hyprnotify-sender-id"),
        nparse::MAX_SENDER_ID_BYTES,
        true,
    );
    let sender_name = opt_str(
        "sender-name",
        Some("x-hyprnotify-sender-name"),
        nparse::MAX_SENDER_NAME_BYTES,
        false,
    );
    let sender_icon = opt_str("sender-icon", Some("x-hyprnotify-sender-icon"), 512, true);
    let message_id = opt_str(
        "message-id",
        Some("x-hyprnotify-message-id"),
        nparse::MAX_MESSAGE_ID_BYTES,
        true,
    );
    let declared_group = opt_str(
        "x-hyprnotify-group-key",
        None,
        nparse::MAX_CONV_ID_BYTES,
        true,
    );
    let unread = hints
        .get_u32("unread-count")
        .or_else(|| hints.get_u32("x-hyprnotify-unread-count"));
    let historic = hints
        .get_bool("message-historic")
        .or_else(|| hints.get_bool("x-hyprnotify-message-historic"));
    let message_time = hints
        .get_i64("message-time")
        .or_else(|| hints.get_i64("x-hyprnotify-message-timestamp"));

    let conv_id = conv_id.unwrap_or_default();
    let message_id = message_id.unwrap_or_default();
    let sender_name = sender_name
        .map(|s| nparse::one_line(&nparse::sanitize_markup(&s, false)))
        .unwrap_or_default();
    let historic = historic.unwrap_or(false);

    // THE CONVERSATION MERGE: every message of one chat is ONE card.
    let mut append_onto = String::new();
    let mut canonical_append = false;
    if id == 0 {
        if !conv_id.is_empty() {
            for n in &st.notifs {
                if !in_osd_band(n.id)
                    && !vanishes(n)
                    && nparse::matches_conversation(
                        &app_key,
                        &conv_id,
                        &n.app_key,
                        &n.conversation_id,
                    )
                {
                    id = n.id;
                    break;
                }
            }
        } else {
            let mut append = category_conversation;
            if !append {
                append = hints.get_bool("x-canonical-append").unwrap_or(false);
            }
            canonical_append = append;
            if append {
                let sum1 = nparse::one_line(&nparse::sanitize_markup(&sum, false));
                for n in &st.notifs {
                    if !in_osd_band(n.id)
                        && !vanishes(n)
                        && n.app_key == app_key
                        && n.summary == sum1
                    {
                        id = n.id;
                        n.body.clone_into(&mut append_onto);
                        break;
                    }
                }
            }
        }
    }

    // the reserved band: a fresh chosen id in it requires the private hint
    if id != 0
        && by_id(st, id).is_none()
        && in_osd_band(id)
        && !hints.get_bool("x-hyprnotify-osd").unwrap_or(false)
    {
        id = 0;
    }

    if id == 0 {
        loop {
            id = st.next_id;
            st.next_id = if st.next_id == u32::MAX {
                1
            } else {
                st.next_id + 1
            };
            if by_id(st, id).is_none() && !in_osd_band(id) {
                break;
            }
        }
    }

    let existing = by_id(st, id);
    let now = ffi::steady_ms();
    let idx = if let Some(i) = existing {
        i
    } else {
        let mut n = Notif::new(id);
        n.born = now;
        n.waiting = st.suspended; // DND collects silently (critical punches through below)
        st.notifs.insert(0, n); // newest on top
        evict_overflow(ctx, st, &cfg, replies);
        0
    };
    let n = st.notifs.get_mut(idx).unwrap();

    let same_app = existing.is_some() && n.app_key == app_key;
    let effective_conv_id = if !conv_id.is_empty() {
        conv_id.clone()
    } else if same_app {
        n.conversation_id.clone()
    } else {
        String::new()
    };
    let same_conversation = existing.is_some()
        && !effective_conv_id.is_empty()
        && same_app
        && n.conversation_id == effective_conv_id;
    let conversation = category_conversation
        || canonical_append
        || !effective_conv_id.is_empty()
        || (same_conversation && n.conversation);
    if !same_conversation {
        n.messages.clear();
        n.unread_count = 0;
    }

    n.arrived = now;
    n.banner = true;
    n.absorbed = false;
    app.clone_into(&mut n.app_name);
    n.sender = sender.to_string();
    n.summary = nparse::one_line(&nparse::sanitize_markup(&sum, false));

    let icon_px = (cfg.max_icon.max(8)) as u32;
    n.body_images.clear();
    for p in nparse::extract_images(&mut txt, (64.max(cfg.max_icon * 2)) as u32) {
        if n.body_images.len() >= nparse::MAX_BODY_IMAGES {
            break;
        }
        n.body_images.push(p);
    }
    n.body = nparse::cap_utf8(nparse::sanitize_markup(&txt, true));
    if conversation {
        n.body = nparse::fold_sender_prefix(&n.body);
    }
    if !append_onto.is_empty() {
        n.body = nparse::join_append(&append_onto, &n.body);
    }

    // the structured conversation state
    effective_conv_id.clone_into(&mut n.conversation_id);
    match &conv_title {
        Some(t) => {
            n.conversation_title = if t.is_empty() {
                n.summary.clone()
            } else {
                nparse::one_line(&nparse::sanitize_markup(t, false))
            };
        }
        None if !same_conversation => n.conversation_title = n.summary.clone(),
        None => {}
    }
    match &conv_kind {
        Some(k) => {
            if let Some(kind) = nparse::normalize_conversation_kind(k) {
                n.conversation_kind = kind.to_string();
            } else if !same_conversation {
                n.conversation_kind.clear();
            }
        }
        None if !same_conversation => {
            n.conversation_kind = if effective_conv_id.is_empty() {
                String::new()
            } else {
                "one-to-one".to_owned()
            };
        }
        None => {}
    }
    if n.conversation_kind.is_empty() && !effective_conv_id.is_empty() {
        n.conversation_kind = "one-to-one".to_string();
    }
    if let Some(c) = &conv_icon {
        c.clone_into(&mut n.conversation_icon_source);
        n.conversation_icon = nparse::resolve_image(c, icon_px);
    } else if !same_conversation {
        n.conversation_icon_source.clear();
        n.conversation_icon.clear();
    }
    if let Some(g) = &declared_group {
        g.clone_into(&mut n.declared_group_key);
    } else if !same_app {
        n.declared_group_key.clear();
    }

    if !effective_conv_id.is_empty() {
        let mutation = nparse::upsert_message(
            &mut n.messages,
            &message_id,
            &n.body,
            sender_id.as_deref(),
            Some(sender_name.as_str()),
            sender_icon.as_deref(),
            message_time,
            Some(historic),
        );
        rebuild_participants(n, icon_px);
        n.body = conversation_body(n);
        n.unread_count = nparse::updated_unread_count(n.unread_count, unread, historic, mutation);
    } else {
        n.unread_count = 0;
        n.participants.clear();
    }

    n.urgency = 1;
    n.progress = -1;
    n.image.clear();
    n.identity.clear();
    n.pixels.clear();
    n.has_pixels = false;
    n.pw = 0;
    n.ph = 0;

    // urgency (0..2)
    if let Some(u) = hints
        .get_u8("urgency")
        .or_else(|| hints.get_i32("urgency").map(|v| v.clamp(0, 2) as u8))
    {
        n.urgency = u;
    }
    if n.waiting && n.urgency >= 2 {
        n.waiting = false; // critical bypasses DND
    }
    // value (progress 0..100)
    if let Some(v) = hints
        .get_i32("value")
        .map(|v| v.clamp(0, 100))
        .or_else(|| hints.get_u32("value").map(|v| v.min(100) as i32))
    {
        n.progress = v;
    }

    // the icon anatomy: the CONTENT image owns the icon column; the IDENTITY
    // rides it as a corner badge (or leads alone)
    let pix_cap = (cfg.width as u32 * 2).max(cfg.max_icon as u32 * 3);
    let _ = pix_cap; // applied at unpack (the buffer is capped there)
    if let Some(img) = hints.image_data() {
        let (w, h, stride, has_alpha, bps, ch, data) = (
            img.w,
            img.h,
            img.stride,
            img.has_alpha,
            img.bps,
            img.ch,
            &img.data,
        );
        nparse::unpack_image_data(
            n,
            w as i32,
            h as i32,
            stride as i32,
            has_alpha,
            bps as i32,
            ch as i32,
            data,
        );
        let _ = pix_cap; // shrinkPixels equivalent: the unpack bound holds
    } else {
        let mut cand = String::new();
        for k in ["image-path", "image_path"] {
            if cand.is_empty() {
                cand = str_hint(k);
            }
        }
        if !cand.is_empty() {
            n.image = nparse::clip_utf8(cand, nparse::MAX_SOURCE_BYTES);
        }
    }
    desktop.clone_into(&mut n.desktop_entry);
    n.identity_from_desktop = false;
    n.identity = nparse::resolve_image(&app_icon, icon_px);
    if n.identity.is_empty() && !desktop.is_empty() {
        let entry_icon = nicons::resolve_desktop_entry_icon(&desktop, icon_px);
        if !entry_icon.is_empty() {
            n.identity = entry_icon;
        } else {
            n.identity = nparse::resolve_image(&desktop, icon_px);
            n.identity_from_desktop = true;
        }
    }
    n.app_key = app_key;

    // the inline-reply protocol
    n.can_reply = false;
    n.reply_placeholder = nparse::clip_utf8(
        str_hint("x-kde-reply-placeholder-text"),
        nparse::MAX_REPLY_TEXT_BYTES,
    );
    n.reply_submit_text = nparse::clip_utf8(
        str_hint("x-kde-reply-submit-button-text"),
        nparse::MAX_REPLY_TEXT_BYTES,
    );

    // actions [id0,label0, id1,label1, ...]; "default" is the primary
    n.default_action.clear();
    n.actions.clear();
    let mut i = 0usize;
    while i + 1 < actions.len() {
        let aid = nparse::clip_utf8(actions[i].clone(), nparse::MAX_ACTION_KEY_BYTES);
        let label = nparse::clip_utf8(actions[i + 1].clone(), nparse::MAX_ACTION_KEY_BYTES);
        if aid == "default" {
            n.default_action = aid;
        } else if aid == "inline-reply" {
            n.can_reply = true;
            if n.reply_submit_text.is_empty() {
                n.reply_submit_text = label;
            }
        } else if !label.is_empty() && n.actions.len() < nparse::MAX_ACTIONS {
            n.actions.push(nparse::Action::new(aid, label));
        }
        i += 2;
    }
    if n.default_action.is_empty() && n.actions.len() == 1 {
        n.default_action = n.actions[0].id.clone();
    }

    n.resident = hints.get_bool("resident").unwrap_or(false);
    n.action_icons = hints.get_bool("action-icons").unwrap_or(false);
    n.transient = hints.get_bool("transient").unwrap_or(false);
    n.conversation = conversation;

    n.timeout_ms = match expire_timeout {
        _ if expire_timeout > 0 => expire_timeout as u64,
        0 => 0,
        _ => default_timeout(n, &cfg),
    };
    if n.timeout_ms > 0 && !n.waiting {
        n.deadline = now + n.timeout_ms;
    }

    // one live popup per app
    let soft = !n.waiting && n.urgency < 2 && !vanishes(n);
    let waiting = n.waiting;
    // (n's last use: the rest goes through idx)
    let coalesced = soft && cfg.coalesce && app_has_banner(st, idx);
    if coalesced {
        st.notifs[idx].banner = false;
    }

    if !waiting {
        changed(ctx, st);
    }

    // sound (a shown arrival plays through the player unless suppressed)
    if !waiting {
        let mut suppress = coalesced;
        let mut sound_file = str_hint("sound-file");
        let mut sound_name = str_hint("sound-name");
        if let Some(s) = hints.get_bool("suppress-sound") {
            suppress = s;
        }
        sound_file = nparse::clip_utf8(sound_file, nparse::MAX_SOURCE_BYTES);
        sound_name = nparse::clip_utf8(sound_name, nparse::MAX_SOURCE_BYTES);
        if let Some(rest) = sound_file.strip_prefix("file://") {
            sound_file = rest.to_string();
        }
        if !suppress && !cfg.sound_command.is_empty() {
            if !sound_file.is_empty() {
                spawn_detached(ctx, st, &[&cfg.sound_command, "-f", &sound_file]);
            } else if !sound_name.is_empty() {
                spawn_detached(ctx, st, &[&cfg.sound_command, "-i", &sound_name]);
            }
        }
    }

    rearm_expiry(ctx, st);
    replies.push(crate::bus::BusReply::StateChanged);
    id
}

/// Fire-and-forget a child (hyperlink open, notification sound), bounded: a
/// hostile sender can hold a steady spawn rate, so the cap drops spawns.
pub fn spawn_detached(ctx: ffi::Ctx, st: &mut NotifyState, argv: &[&str]) {
    if st.live_children >= 16 {
        return;
    }
    let mut full: Vec<std::ffi::CString> = Vec::with_capacity(argv.len() + 1);
    let mut ok = true;
    for a in argv {
        if let Ok(c) = std::ffi::CString::new(*a) {
            full.push(c);
        } else {
            ok = false;
            break;
        }
    }
    if !ok {
        return;
    }
    let mut ptrs: Vec<*const std::ffi::c_char> = full.iter().map(|c| c.as_ptr()).collect();
    ptrs.push(std::ptr::null());
    st.live_children += 1;
    if let Err(e) = std::process::Command::new(argv[0]).args(&argv[1..]).spawn() {
        // a failed spawn frees its slot immediately
        st.live_children = st.live_children.saturating_sub(1);
        ffi::log_str(
            ctx,
            ffi::LOG_DEBUG,
            &format!("notify: spawn {argv:?} failed: {e}"),
        );
    }
    let _ = ptrs;
}

// ---- lifecycle (expiry, dismissals, DND, the badge) ----

/// The nearest due banner (steady ms); 0 = none. The expiry job is a
/// one-shot armed to it: the handler zeroes the token on fire (a fired
/// one-shot must not be cancelled), and this re-arms from every state
/// change. A pending fire that already runs AT or BEFORE the new nearest
/// deadline is kept — its tick re-arms to whatever is nearest then.
fn rearm_expiry(ctx: ffi::Ctx, st: &mut NotifyState) {
    let now = ffi::steady_ms();
    let mut next: Option<u64> = None;
    for n in &st.notifs {
        if !n.banner || n.timeout_ms == 0 || n.waiting || n.id == st.held_banner {
            continue;
        }
        let ms = n.deadline.saturating_sub(now).max(1);
        if next.is_none_or(|v| ms < v) {
            next = Some(ms);
        }
    }
    let Some(ms) = next else {
        st.expiry_at = 0;
        if st.expiry_token != 0 {
            ffi::job_cancel(ctx, st.expiry_token);
            st.expiry_token = 0;
            st.expiry_armed_ms = 0;
        }
        return;
    };
    st.expiry_at = ms;
    if st.expiry_token != 0 && st.expiry_armed_ms <= ms {
        return;
    }
    if st.expiry_token != 0 {
        ffi::job_cancel(ctx, st.expiry_token);
        st.expiry_token = 0;
    }
    st.expiry_token = crate::probe::arm_timer(
        ctx,
        ms.min(u32::MAX as u64) as u32,
        crate::probe::JOB_N_EXPIRY,
    );
    st.expiry_armed_ms = ms;
}

/// A banner must not expire out from under the pointer reading it. The
/// hovered card's clock stops (the sweep and the rearm both skip it), and
/// leaving RESTARTS it rather than resuming the sliver that was left — the
/// restart moves the deadline, so the one-shot is re-armed here (as in the
/// C++ original); without it the restarted clock never fires.
pub fn hold_banner(ctx: ffi::Ctx, st: &mut NotifyState, id: u32) {
    if st.held_banner == id {
        return;
    }
    let prev = st.held_banner;
    st.held_banner = id;
    if prev != 0
        && let Some(i) = by_id(st, prev)
    {
        let n = &mut st.notifs[i];
        if n.banner && n.timeout_ms > 0 && !n.waiting {
            n.deadline = ffi::steady_ms() + n.timeout_ms; // leaving RESTARTS it
        }
    }
    rearm_expiry(ctx, st);
}

/// RESIDENCY: a due banner emits EXPIRED and hides only the popup (the card
/// stays resident); transient/progress cards vanish entirely.
pub fn expiry_tick(ctx: ffi::Ctx, st: &mut NotifyState, replies: &mut Vec<crate::bus::BusReply>) {
    // the one-shot that fired is consumed: rearm_expiry arms a fresh one
    st.expiry_token = 0;
    st.expiry_armed_ms = 0;
    let now = ffi::steady_ms();
    let mut changed = false;
    let mut gone = Vec::new();
    for n in &mut st.notifs {
        if !n.banner || n.timeout_ms == 0 || n.waiting || n.id == st.held_banner || n.deadline > now
        {
            continue;
        }
        if vanishes(n) {
            gone.push(n.id);
            continue;
        }
        n.banner = false;
        replies.push(crate::bus::BusReply::Closed {
            id: n.id,
            reason: R_EXPIRED,
        });
        changed = true;
    }
    let gone_any = !gone.is_empty();
    for id in gone {
        st.notifs.retain(|n| n.id != id);
        replies.push(crate::bus::BusReply::Closed {
            id,
            reason: R_EXPIRED,
        });
    }
    if changed || gone_any {
        changed_st(ctx, st, replies);
    }
    rearm_expiry(ctx, st);
}

pub fn close_one(
    ctx: ffi::Ctx,
    st: &mut NotifyState,
    id: u32,
    reason: u32,
    replies: &mut Vec<crate::bus::BusReply>,
) -> bool {
    if by_id(st, id).is_none() {
        return false;
    }
    st.notifs.retain(|n| n.id != id);
    replies.push(crate::bus::BusReply::Closed { id, reason });
    changed_st(ctx, st, replies);
    rearm_expiry(ctx, st);
    true
}

pub fn dismiss_all_live(
    ctx: ffi::Ctx,
    st: &mut NotifyState,
    replies: &mut Vec<crate::bus::BusReply>,
) {
    for n in &st.notifs {
        if !n.waiting {
            replies.push(crate::bus::BusReply::Closed {
                id: n.id,
                reason: R_DISMISSED,
            });
        }
    }
    st.notifs.retain(|n| n.waiting);
    changed_st(ctx, st, replies);
    rearm_expiry(ctx, st);
}

pub fn dismiss_app(
    ctx: ffi::Ctx,
    st: &mut NotifyState,
    key: &str,
    replies: &mut Vec<crate::bus::BusReply>,
) {
    let (app, group) = match key.find('\u{1f}') {
        Some(p) => (key[..p].to_owned(), key[p + 1..].to_owned()),
        None => (key.to_owned(), String::new()),
    };
    for n in &st.notifs {
        if !n.waiting && n.app_key == app && (group.is_empty() || n.declared_group_key == group) {
            replies.push(crate::bus::BusReply::Closed {
                id: n.id,
                reason: R_DISMISSED,
            });
        }
    }
    st.notifs.retain(|n| {
        n.waiting || n.app_key != app || (!group.is_empty() && n.declared_group_key != group)
    });
    changed_st(ctx, st, replies);
    rearm_expiry(ctx, st);
}

/// Opening the center absorbs the popped stack (the banners stand down into
/// parked shade rows); closing it (repop) returns them.
pub fn absorb_popped(ctx: ffi::Ctx, st: &mut NotifyState, replies: &mut Vec<crate::bus::BusReply>) {
    let mut changed = false;
    for n in &mut st.notifs {
        if !n.banner || n.waiting || vanishes(n) {
            continue;
        }
        n.banner = false;
        n.absorbed = true;
        changed = true;
    }
    if changed {
        changed_st(ctx, st, replies);
    }
}

pub fn repop_absorbed(
    ctx: ffi::Ctx,
    st: &mut NotifyState,
    replies: &mut Vec<crate::bus::BusReply>,
) {
    // two-pass: the re-banner decision looks at the siblings, so it is
    // computed (immutably) before any card mutates
    let cfg = config(ctx, st);
    let now = ffi::steady_ms();
    let mut rebanner: Vec<usize> = Vec::new();
    for (i, n) in st.notifs.iter().enumerate() {
        if n.absorbed && !n.waiting && !vanishes(n) {
            rebanner.push(i);
        }
    }
    if rebanner.is_empty() {
        return;
    }
    for n in &mut st.notifs {
        if n.absorbed {
            n.absorbed = false;
            n.banner = false;
        }
    }
    for i in rebanner {
        let suppress = coalesce_suppress(&*st, i, &cfg);
        let n = &mut st.notifs[i];
        n.banner = !suppress;
        if n.banner && n.timeout_ms > 0 {
            n.deadline = now + n.timeout_ms;
        }
    }
    changed_st(ctx, st, replies);
}

pub fn toggle_suspend(
    ctx: ffi::Ctx,
    st: &mut NotifyState,
    replies: &mut Vec<crate::bus::BusReply>,
) {
    st.suspended = !st.suspended;
    if st.suspended {
        changed_st(ctx, st, replies);
        return;
    }
    // two-pass: the resume decision reads the siblings (the suppression
    // test), so it is computed before any card mutates
    let cfg = config(ctx, st);
    let now = ffi::steady_ms();
    let mut resume: Vec<usize> = Vec::new();
    for (i, n) in st.notifs.iter().enumerate() {
        if n.waiting {
            resume.push(i);
        }
    }
    for i in resume {
        let suppress = coalesce_suppress(&*st, i, &cfg);
        let n = &mut st.notifs[i];
        n.waiting = false;
        n.banner = !suppress;
        if n.banner && n.timeout_ms > 0 {
            n.deadline = now + n.timeout_ms;
        }
    }
    changed_st(ctx, st, replies);
    rearm_expiry(ctx, st);
}

/// The badge's truth is the shade: bannered popups + resident cards.
pub fn badge_counts(st: &NotifyState) -> (u32, u32) {
    let (mut live, mut kept) = (0u32, 0u32);
    for n in &st.notifs {
        if n.waiting || in_osd_band(n.id) {
            continue;
        }
        if n.banner {
            live += 1;
        } else {
            kept += 1;
        }
    }
    (live, kept)
}

pub fn state_string(st: &NotifyState) -> String {
    format!(
        "center:{} live:{} dnd:{}",
        u8::from(st.center_on),
        st.notifs.len(),
        u8::from(st.suspended)
    )
}

pub fn badge_string(st: &NotifyState) -> String {
    let (live, kept) = badge_counts(st);
    format!("banners:{live} resident:{kept}")
}

pub fn topline_string(st: &NotifyState) -> String {
    for n in &st.notifs {
        if !n.waiting && !in_osd_band(n.id) {
            return nparse::collapsed_line(n);
        }
    }
    String::new()
}

/// A card's one-per-app coalesce check at resume time (repop, DND release).
fn coalesce_suppress(st: &NotifyState, idx: usize, cfg: &NConfig) -> bool {
    if !cfg.coalesce {
        return false;
    }
    let n = &st.notifs[idx];
    n.urgency < 2 && !vanishes(n) && app_has_banner(st, idx)
}
