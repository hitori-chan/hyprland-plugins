// The shade (center.cpp + row.cpp): the display list, the expansion
// budget, the digest, the bundle, the panel.

use super::row::{ROW_CHILD, ROW_SINGLE, measure_row, render_row};
use super::*;

// ---------------------------------------------------------------------------
// the shade (center.cpp + row.cpp)
// ---------------------------------------------------------------------------

/// Android's ranking: urgent, conversations, normal, silent.
fn tier(n: &nparse::Notif) -> u8 {
    if n.urgency >= 2 {
        0
    } else if n.urgency == 0 {
        4
    } else if n.conversation {
        2
    } else {
        3
    }
}
fn bundleable(n: &nparse::Notif) -> bool {
    !n.conversation && !n.app_key.is_empty()
}

/// The display list: one ranked list, apps bundled at four (conversations
/// never bundle).
fn build_display(st: &mut NotifyState) {
    let src: Vec<(u8, usize)> = st
        .notifs
        .iter()
        .enumerate()
        .filter(|(_, n)| !n.waiting && !notify::in_osd_band(n.id))
        .map(|(i, n)| (tier(n), i))
        .collect();
    let mut src = src;
    // notifs is newest-first; the index order keeps that inside each tier
    src.sort_by_key(|&(t, i)| (t, i));

    // how many bundleable cards each (app, group) holds (one linear scan)
    let mut counts: Vec<(String, u32)> = Vec::new();
    for &(_, i) in &src {
        let n = &st.notifs[i];
        if bundleable(n) {
            let k = notify::group_key_of(n);
            match counts.iter_mut().find(|c| c.0 == k) {
                Some(c) => c.1 += 1,
                None => counts.push((k, 1)),
            }
        }
    }
    let count_of = |k: &str| -> u32 { counts.iter().find(|c| c.0 == k).map_or(0, |c| c.1) };

    let mut placed_first: std::collections::HashMap<String, usize> =
        std::collections::HashMap::new();
    st.disp.clear();
    for &(_, i) in &src {
        let n = &st.notifs[i];
        if bundleable(n) {
            let k = notify::group_key_of(n);
            if count_of(&k) >= notify::AUTOGROUP_AT {
                if let Some(&first) = placed_first.get(&k) {
                    st.disp[first].items.push(i);
                    continue;
                }
                placed_first.insert(k, st.disp.len());
            }
        }
        st.disp.push(notify::Disp {
            items: vec![i],
            key: notify::group_key_of(n),
        });
    }
}

/// The expansion budget: walk the page opening what fits (the top row always
/// opens; a user override wins in both directions).
#[allow(clippy::too_many_lines)]
fn run_budget(p: &mut P, cfg: &NConfig, st: &mut NotifyState, content_w: f64, body_cap: f64) {
    let mut used = 0.0;
    for i in st.skip..st.disp.len() {
        let d = st.disp[i].clone();
        let lead = if i == st.skip { 0.0 } else { notify::STACK_GAP };
        let top = i == st.skip;
        let notifs = &mut st.notifs;
        let cache = &mut st.text_cache;
        let jobs = &mut st.decode_jobs;
        let hovered = &st.hovered;
        let reply_id = st.reply_id;
        let reply_text = st.reply_text.clone();
        if d.items.len() < 2 {
            let idx = d.items[0];
            let ch = {
                let n = &mut notifs[idx];
                measure_row(
                    p,
                    cfg,
                    n,
                    cache,
                    jobs,
                    hovered,
                    reply_id,
                    &reply_text,
                    content_w,
                    false,
                    false,
                )
            };
            let id = notifs[idx].id;
            let force_open = st.opened_rows.contains(&id);
            let force_fold = st.folded_rows.contains(&id);
            let mut open = false;
            let mut more = true;
            let mut h = ch;
            if !force_fold && (force_open || top || used + lead + ch < body_cap) {
                let oh = {
                    let n = &mut notifs[idx];
                    measure_row(
                        p,
                        cfg,
                        n,
                        cache,
                        jobs,
                        hovered,
                        reply_id,
                        &reply_text,
                        content_w,
                        true,
                        false,
                    )
                };
                more = oh > ch + 0.5;
                open = more && (force_open || top || used + lead + oh <= body_cap);
                if open {
                    h = oh;
                }
            }
            if i < st.item_h.len() {
                st.item_h[i] = h;
                st.item_open[i] = open;
                st.item_more[i] = more;
            }
            *st.row_state.entry(id).or_insert(false) = open;
        } else {
            let dh = digest_h(type_scale(cfg, p.scale), d.items.len() as u32, p.scale);
            let force_open = st.opened_groups.contains(&d.key);
            let force_fold = st.folded_groups.contains(&d.key);
            let mut open = false;
            let mut h = dh;
            if !force_fold && (force_open || top || used + lead + dh < body_cap) {
                let mut oh = group_head_h();
                let mut ch_vec = Vec::with_capacity(d.items.len());
                for &idx in &d.items {
                    let c = {
                        let n = &mut notifs[idx];
                        measure_row(
                            p,
                            cfg,
                            n,
                            cache,
                            jobs,
                            hovered,
                            reply_id,
                            &reply_text,
                            content_w,
                            true,
                            true,
                        )
                    };
                    ch_vec.push(c);
                    oh += notify::CHILD_GAP + c;
                }
                if force_open || used + lead + oh <= body_cap {
                    open = true;
                    h = oh;
                    if i < st.child_h.len() {
                        st.child_h[i] = ch_vec;
                    }
                }
            }
            if i < st.item_h.len() {
                st.item_h[i] = h;
                st.item_open[i] = open;
                st.item_more[i] = true;
            }
            *st.group_state.entry(d.key.clone()).or_insert(false) = open;
        }
        used += lead
            + if i < st.item_h.len() {
                st.item_h[i]
            } else {
                0.0
            };
    }
}

fn digest_h(t: T, count: u32, scale: f64) -> f64 {
    notify::ROW_PADT
        + notify::ROW_ICON.max(t.title as f64 / scale + 2.0)
        + count.min(2) as f64 * (t.body as f64 / scale * 1.35 + 3.0)
        + notify::ROW_PADB
}
fn group_head_h() -> f64 {
    notify::ROW_PADT + notify::CHILD_ICON + notify::ROW_PADB
}

/// The folded bundle: the app's identity, a count pill, and the two newest
/// cards previewed a line each.
#[allow(clippy::too_many_lines, clippy::too_many_arguments)]
fn paint_digest(
    p: &mut P,
    cfg: &NConfig,
    notifs: &mut [nparse::Notif],
    cache: &mut notify::TextCache,
    jobs: &mut Vec<nicons::DecodeJob>,
    hovered: &notify::Hover,
    d: &notify::Disp,
    box_: RBox,
    cards: &mut Vec<Card>,
) {
    let t = type_scale(cfg, p.scale);
    let subhex = nparse::hex6(cfg.col_kicker.r, cfg.col_kicker.g, cfg.col_kicker.b);
    let rp = cfg.rounding_power;
    let font = cfg.font.clone();

    let hov = hovered.kind == CardKind::Digest && hovered.group == d.key;
    p.rect(
        box_,
        if hov { ACCENT_DIM } else { FILL },
        r_row(cfg, p.scale),
        rp,
    );

    if p.warm {
        let n = &mut notifs[d.items[0]];
        nicons::ensure_icon_tex(
            p.ctx,
            cfg,
            jobs,
            n,
            (cfg.max_icon as f64 * p.scale).round() as u32,
            (box_.w * p.scale).round() as u32,
            (notify::HERO_CAP * p.scale).round() as u32,
        );
    }
    let idt = if notifs[d.items[0]].ident.tex.is_some() {
        lead_arc(&notifs[d.items[0]].ident)
    } else {
        lead_arc(&notifs[d.items[0]].icon)
    };
    p.texfit(
        &Raster::from(Some(&idt)),
        RBox::new(
            box_.x + notify::ROW_PADX,
            box_.y + notify::ROW_PADT,
            notify::ROW_ICON,
            notify::ROW_ICON,
        ),
        (notify::ROW_ICON * 10.0 / 44.0 * p.scale).round() as u32,
        rp,
    );

    let tx = box_.x + notify::ROW_PADX + notify::ROW_ICON + notify::ROW_ICON_GAP;
    let count_s = d.items.len().to_string();
    let pill = tget(
        p,
        cache,
        &font,
        notify::TextCache::key(
            &count_s,
            notify::color_hex(cfg.col_fg),
            t.small as u32,
            64,
            -1,
            0.0,
            false,
            600,
            None,
        ),
        &count_s,
        cfg.col_fg,
        t.small,
        64,
        -1,
        0.0,
        600,
        None,
    );
    let pchv = chevron(
        p,
        cache,
        0,
        cfg.col_fg,
        (t.small as f64 * 2.0).round() as i32,
    );
    let pill_w = pill.w_px(p.scale) + 3.0 + pchv.w_px(p.scale) + 14.0;
    let pb = RBox::new(
        box_.x + box_.w - notify::ROW_PADX - pill_w,
        box_.y + notify::ROW_PADT + (notify::ROW_ICON - notify::PILL_H) / 2.0,
        pill_w,
        notify::PILL_H,
    );
    p.rect(
        pb,
        if hov { ACCENT_DIM } else { FILL2 },
        (notify::PILL_H / 2.0 * p.scale).round() as u32,
        2.0,
    );
    let pl_w = pill.w_px(p.scale);
    let pl_h = pill.h_px(p.scale);
    let ch_w = pchv.w_px(p.scale);
    let ch_h = pchv.h_px(p.scale);
    let x0 = pb.x + (pb.w - (pl_w + 3.0 + ch_w)) / 2.0;
    p.tex(&pill, x0, pb.y + (pb.h - pl_h) / 2.0);
    p.tex(&pchv, x0 + pl_w + 3.0, pb.y + (pb.h - ch_h) / 2.0);

    let sumline_txt = format!(
        "{} <span foreground=\"{}\">• {} • {}</span>",
        nparse::esc(&notifs[d.items[0]].app_name),
        subhex,
        d.items.len(),
        nparse::age_string(ffi::steady_ms(), notifs[d.items[0]].arrived)
    );
    let sumw = ((pb.x - 8.0 - tx) * p.scale).floor().max(1.0) as i32;
    let sumline = tget(
        p,
        cache,
        &font,
        notify::TextCache::key(
            &sumline_txt,
            notify::color_hex(cfg.col_title),
            t.title as u32,
            sumw as u32,
            -1,
            0.0,
            true,
            600,
            None,
        ),
        &sumline_txt,
        cfg.col_title,
        t.title,
        sumw,
        -1,
        0.0,
        600,
        None,
    );
    p.tex(
        &sumline,
        tx,
        box_.y + notify::ROW_PADT + (notify::ROW_ICON - sumline.h_px(p.scale)) / 2.0,
    );

    // <=2 preview lines, indented into the text column
    let mut py = box_.y + notify::ROW_PADT + notify::ROW_ICON.max(t.title as f64 / p.scale + 2.0);
    let prev = d.items.len().min(2);
    for i in 0..prev {
        if p.warm {
            let n = &mut notifs[d.items[i]];
            nicons::ensure_icon_tex(
                p.ctx,
                cfg,
                jobs,
                n,
                (cfg.max_icon as f64 * p.scale).round() as u32,
                (box_.w * p.scale).round() as u32,
                (notify::HERO_CAP * p.scale).round() as u32,
            );
        }
        let n = &notifs[d.items[i]];
        let lh = t.body as f64 / p.scale * 1.35;
        py += 3.0;
        let mut px = tx;
        // each child's OWN face (the bundle children share one app icon)
        let pv = if n.icon.tex.is_some() && !n.hero {
            lead_arc(&n.icon)
        } else {
            lead_arc(&n.ident)
        };
        let pv_r = Raster::from(Some(&pv));
        if pv_r.tex.is_some() {
            p.texfit(
                &pv_r,
                RBox::new(
                    px,
                    py + (lh - notify::PREV_ICON) / 2.0,
                    notify::PREV_ICON,
                    notify::PREV_ICON,
                ),
                (notify::PREV_ICON / 2.0 * p.scale).round() as u32,
                2.0,
            );
            px += notify::PREV_ICON + 6.0;
        }
        let ln_txt = format!(
            "<b>{}</b>  <span foreground=\"{}\">{}</span>",
            nparse::esc(&n.summary),
            subhex,
            nparse::last_line(&n.body)
        );
        let lnw = ((box_.x + box_.w - notify::ROW_PADX - px) * p.scale)
            .floor()
            .max(1.0) as i32;
        let ln = tget(
            p,
            cache,
            &font,
            notify::TextCache::key(
                &ln_txt,
                notify::color_hex(cfg.col_fg),
                t.body as u32,
                lnw as u32,
                -1,
                0.0,
                true,
                400,
                None,
            ),
            &ln_txt,
            cfg.col_fg,
            t.body,
            lnw,
            -1,
            0.0,
            400,
            None,
        );
        p.tex(&ln, px, py + (lh - ln.h_px(p.scale)) / 2.0);
        py += lh;
    }

    cards.push(Card {
        kind: CardKind::Digest,
        rect: box_,
        group: d.key.clone(),
        ..Card::default()
    });
}

/// The expanded bundle: a header that owns the app's identity, its count and
/// the ✕ that dismisses the lot, then every child as a full row.
#[allow(
    clippy::too_many_lines,
    clippy::too_many_arguments,
    clippy::cognitive_complexity
)]
fn paint_group(
    p: &mut P,
    cfg: &NConfig,
    notifs: &mut [nparse::Notif],
    cache: &mut notify::TextCache,
    jobs: &mut Vec<nicons::DecodeJob>,
    hovered: &notify::Hover,
    reply_id: u32,
    reply_text: &str,
    d: &notify::Disp,
    box_: RBox,
    child_h: &[f64],
    cards: &mut Vec<Card>,
) {
    let t = type_scale(cfg, p.scale);
    let subhex = nparse::hex6(cfg.col_kicker.r, cfg.col_kicker.g, cfg.col_kicker.b);
    let rp = cfg.rounding_power;
    let font = cfg.font.clone();

    let hhov = hovered.kind == CardKind::GHead && hovered.group == d.key;
    let headrh = group_head_h();
    p.rect(
        RBox::new(box_.x, box_.y, box_.w, headrh),
        if hhov { ACCENT_DIM } else { FILL },
        r_row(cfg, p.scale),
        rp,
    );

    if p.warm {
        let n = &mut notifs[d.items[0]];
        nicons::ensure_icon_tex(
            p.ctx,
            cfg,
            jobs,
            n,
            (cfg.max_icon as f64 * p.scale).round() as u32,
            (box_.w * p.scale).round() as u32,
            (notify::HERO_CAP * p.scale).round() as u32,
        );
    }
    let idt = if notifs[d.items[0]].ident.tex.is_some() {
        lead_arc(&notifs[d.items[0]].ident)
    } else {
        lead_arc(&notifs[d.items[0]].icon)
    };
    p.texfit(
        &Raster::from(Some(&idt)),
        RBox::new(
            box_.x + notify::ROW_PADX,
            box_.y + notify::ROW_PADT,
            notify::CHILD_ICON,
            notify::CHILD_ICON,
        ),
        (notify::CHILD_ICON * 10.0 / 44.0 * p.scale).round() as u32,
        rp,
    );

    // the static ✕ (dismiss the whole app's bundle)
    let xb = RBox::new(
        box_.x + box_.w - notify::ROW_PADX - notify::XCIRC,
        box_.y + notify::ROW_PADT + (notify::CHILD_ICON - notify::XCIRC) / 2.0,
        notify::XCIRC,
        notify::XCIRC,
    );
    let xhov = hhov && hovered.part == 2;
    // BOTH colours, every pass: hover flips without a rewarm
    let xg = tget(
        p,
        cache,
        &font,
        notify::TextCache::key(
            "✕",
            notify::color_hex(cfg.col_fg),
            t.small as u32,
            64,
            -1,
            0.0,
            false,
            600,
            None,
        ),
        "✕",
        cfg.col_fg,
        t.small,
        64,
        -1,
        0.0,
        600,
        None,
    );
    let xghot = tget(
        p,
        cache,
        &font,
        notify::TextCache::key(
            "✕",
            notify::color_hex(ON_ACCENT),
            t.small as u32,
            64,
            -1,
            0.0,
            false,
            600,
            None,
        ),
        "✕",
        ON_ACCENT,
        t.small,
        64,
        -1,
        0.0,
        600,
        None,
    );
    p.rect(
        xb,
        if xhov { cfg.col_urgent } else { FILL2 },
        (notify::XCIRC / 2.0 * p.scale).round() as u32,
        2.0,
    );
    let g = if xhov { &xghot } else { &xg };
    p.tex(
        g,
        xb.x + (xb.w - g.w_px(p.scale)) / 2.0,
        xb.y + (xb.h - g.h_px(p.scale)) / 2.0,
    );

    let count_s = d.items.len().to_string();
    let pill = tget(
        p,
        cache,
        &font,
        notify::TextCache::key(
            &count_s,
            notify::color_hex(cfg.col_fg),
            t.small as u32,
            64,
            -1,
            0.0,
            false,
            600,
            None,
        ),
        &count_s,
        cfg.col_fg,
        t.small,
        64,
        -1,
        0.0,
        600,
        None,
    );
    let pchv = chevron(
        p,
        cache,
        1,
        cfg.col_fg,
        (t.small as f64 * 2.0).round() as i32,
    );
    let pill_w = pill.w_px(p.scale) + 3.0 + pchv.w_px(p.scale) + 14.0;
    let pb = RBox::new(
        xb.x - 6.0 - pill_w,
        box_.y + notify::ROW_PADT + (notify::CHILD_ICON - notify::PILL_H) / 2.0,
        pill_w,
        notify::PILL_H,
    );
    p.rect(
        pb,
        FILL2,
        (notify::PILL_H / 2.0 * p.scale).round() as u32,
        2.0,
    );
    let pl_w = pill.w_px(p.scale);
    let pl_h = pill.h_px(p.scale);
    let ch_w = pchv.w_px(p.scale);
    let ch_h = pchv.h_px(p.scale);
    let x0 = pb.x + (pb.w - (pl_w + 3.0 + ch_w)) / 2.0;
    p.tex(&pill, x0, pb.y + (pb.h - pl_h) / 2.0);
    p.tex(&pchv, x0 + pl_w + 3.0, pb.y + (pb.h - ch_h) / 2.0);

    let tx = box_.x + notify::ROW_PADX + notify::CHILD_ICON + notify::ROW_ICON_GAP;
    let headline_txt = format!(
        "{} <span foreground=\"{}\">• {} • {}</span>",
        nparse::esc(&notifs[d.items[0]].app_name),
        subhex,
        d.items.len(),
        nparse::age_string(ffi::steady_ms(), notifs[d.items[0]].arrived)
    );
    let hlw = ((pb.x - 8.0 - tx) * p.scale).floor().max(1.0) as i32;
    let headline = tget(
        p,
        cache,
        &font,
        notify::TextCache::key(
            &headline_txt,
            notify::color_hex(cfg.col_title),
            t.title as u32,
            hlw as u32,
            -1,
            0.0,
            true,
            600,
            None,
        ),
        &headline_txt,
        cfg.col_title,
        t.title,
        hlw,
        -1,
        0.0,
        600,
        None,
    );
    p.tex(
        &headline,
        tx,
        box_.y + notify::ROW_PADT + (notify::CHILD_ICON - headline.h_px(p.scale)) / 2.0,
    );

    cards.push(Card {
        kind: CardKind::GHead,
        rect: RBox::new(box_.x, box_.y, box_.w, headrh),
        group: d.key.clone(),
        close: xb,
        ..Card::default()
    });

    // the children, each fully readable (no third fold state)
    let mut cy = box_.y + headrh;
    for (k, &idx) in d.items.iter().enumerate() {
        cy += notify::CHILD_GAP;
        let ch2 = if k < child_h.len() { child_h[k] } else { 0.0 };
        let chov = hovered.kind == CardKind::Child
            && hovered.id == notifs[idx].id
            && hovered.btn < 0
            && hovered.part == 0;
        p.rect(
            RBox::new(box_.x, cy, box_.w, ch2),
            if chov { ACCENT_DIM } else { FILL },
            r_joint(p.scale),
            rp,
        );
        let ncard = {
            let n = &mut notifs[idx];
            render_row(
                p,
                cfg,
                n,
                cache,
                jobs,
                hovered,
                reply_id,
                reply_text,
                RBox::new(box_.x, cy, box_.w, 0.0),
                true,
                false,
                ROW_CHILD,
                true,
            )
            .1
        };
        let mut ncard = ncard;
        d.key.clone_into(&mut ncard.group);
        cards.push(ncard);
        cy += ch2;
    }
}

// ---------------------------------------------------------------------------
// the panel (center.cpp renderCenter)
// ---------------------------------------------------------------------------

#[allow(clippy::too_many_lines, clippy::cognitive_complexity)]
pub(super) fn center(p: &mut P, st: &mut NotifyState, cfg: &NConfig) {
    // the open spring: fade + a 6px rise
    let now = ffi::steady_ms();
    if st.animating && now.saturating_sub(st.opened_at) >= notify::MOTION_SPATIAL_MS as u64 {
        st.animating = false;
    }
    if st.animating {
        let at = anim_t(st.opened_at, now, notify::MOTION_SPATIAL_MS);
        p.alpha *= ease_out_cubic(at) as f32;
        p.dy -= (1.0 - ease_out_back(at)) * 6.0;
    }

    let mb = p.mb;
    let rpanel = r_panel(cfg, p.scale);
    let rrow = r_row(cfg, p.scale);
    let rp = cfg.rounding_power;

    let x = mb.x + mb.w - notify::EDGE - notify::CENTER_W;
    let y0 = mb.y + cfg.offset_y as f64;

    let content_x = x + notify::BODY_PADX;
    let content_w = notify::CENTER_W - 2.0 * notify::BODY_PADX;

    let bar_h = notify::BAR_PADT + notify::BAR_BTN + notify::BAR_PADB;
    // the shade runs to what the monitor leaves below offset_y (a margin of
    // air) — the expansion budget spends exactly this
    let avail_h = mb.h - cfg.offset_y as f64 - cfg.margin as f64;
    let body_cap = notify::ROW_ICON.max(avail_h - bar_h - notify::BODY_PADT - notify::BODY_PADB);

    // the display list, every height AND every fold verdict are decided once
    // per warm and reused by the draws between warms
    if p.warm {
        build_display(st);
        st.skip = if st.disp.is_empty() {
            0
        } else {
            st.skip.min(st.disp.len() - 1)
        };
        // a dismissal shrinks the list under the selection: keeping the
        // INDEX lands it on the card that took the dismissed one's place
        if st.sel >= st.disp.len() as i32 {
            st.sel = if st.disp.is_empty() {
                -1
            } else {
                st.disp.len() as i32 - 1
            };
        }
        st.item_h = vec![0.0; st.disp.len()];
        st.item_open = vec![false; st.disp.len()];
        st.item_more = vec![false; st.disp.len()];
        st.child_h = vec![Vec::new(); st.disp.len()];
        st.row_state.clear();
        st.group_state.clear();
        run_budget(p, cfg, st, content_w, body_cap);
    }
    let disp = st.disp.clone();
    st.items = disp.len();
    st.skip = if disp.is_empty() {
        0
    } else {
        st.skip.min(disp.len() - 1)
    };

    // place the items that fit; STACK_GAP joins the cards into one column
    let mut placed: Vec<(usize, f64)> = Vec::new();
    let mut used_h = 0.0;
    for i in st.skip..disp.len().min(st.item_h.len()) {
        let lead = if placed.is_empty() {
            0.0
        } else {
            notify::STACK_GAP
        };
        if !placed.is_empty() && used_h + lead + st.item_h[i] > body_cap {
            break;
        }
        used_h += lead + st.item_h[i];
        placed.push((i, st.item_h[i]));
    }
    st.last_vis = if placed.is_empty() {
        st.skip
    } else {
        placed.last().map_or(st.skip, |pp| pp.0)
    };

    let empty = disp.is_empty();
    let emptyh = 46.0;
    let bodyh = if empty { emptyh } else { used_h };
    let panelh = notify::BODY_PADT + bodyh + notify::BODY_PADB + bar_h;
    let panel = RBox::new(x, y0, notify::CENTER_W, panelh);

    p.shadow(panel, rpanel, rp, 22.0);
    p.glass(panel, cfg.col_bg, rpanel, rp);
    st.cards.push(Card {
        kind: CardKind::Panel,
        rect: panel,
        ..Card::default()
    });

    let mut y = y0 + notify::BODY_PADT;

    let t = type_scale(cfg, p.scale);
    let font = cfg.font.clone();
    let hovered = &st.hovered;
    let reply_id = st.reply_id;
    let reply_text = st.reply_text.clone();
    let notifs = &mut st.notifs;
    let cache = &mut st.text_cache;
    let jobs = &mut st.decode_jobs;
    let cards = &mut st.cards;

    if empty {
        let ekw = (notify::CENTER_W * p.scale).floor() as i32;
        let e = tget(
            p,
            cache,
            &font,
            notify::TextCache::key(
                "You're all caught up!",
                notify::color_hex(cfg.col_kicker),
                t.body as u32,
                ekw as u32,
                -1,
                0.0,
                false,
                500,
                None,
            ),
            "You're all caught up!",
            cfg.col_kicker,
            t.body,
            ekw,
            -1,
            0.0,
            500,
            None,
        );
        p.tex(
            &e,
            x + (notify::CENTER_W - e.w_px(p.scale)) / 2.0,
            y + (emptyh - e.h_px(p.scale)) / 2.0,
        );
        y += emptyh;
    }

    let mut first = true;
    for &(idx, ih) in &placed {
        let d = &disp[idx];
        let open = idx < st.item_open.len() && st.item_open[idx];
        let more = idx < st.item_more.len() && st.item_more[idx];
        if !first {
            y += notify::STACK_GAP;
        }
        first = false;

        let slot = RBox::new(content_x, y, content_w, ih);
        if d.items.len() < 2 {
            // the single's fill, then the row
            let id = notifs[d.items[0]].id;
            let hov = hovered.kind == CardKind::Row
                && hovered.id == id
                && hovered.btn < 0
                && hovered.part == 0;
            p.rect(slot, if hov { ACCENT_DIM } else { FILL }, rrow, rp);
            let card = {
                let n = &mut notifs[d.items[0]];
                render_row(
                    p,
                    cfg,
                    n,
                    cache,
                    jobs,
                    hovered,
                    reply_id,
                    &reply_text,
                    RBox::new(slot.x, slot.y, slot.w, 0.0),
                    open,
                    more,
                    ROW_SINGLE,
                    false,
                )
                .1
            };
            cards.push(card);
        } else if !open {
            paint_digest(p, cfg, notifs, cache, jobs, hovered, d, slot, cards);
        } else {
            let child_h = if idx < st.child_h.len() {
                st.child_h[idx].clone()
            } else {
                Vec::new()
            };
            paint_group(
                p,
                cfg,
                notifs,
                cache,
                jobs,
                hovered,
                reply_id,
                &reply_text,
                d,
                slot,
                &child_h,
                cards,
            );
        }

        // the keyboard selection: a hairline in the accent, over the whole
        // item (an open bundle's header and children together)
        if (idx as i32) == st.sel {
            p.ring(slot, cfg.col_highlight, rrow, rp, 1.0);
        }
        y += ih;
    }

    // ---- the footer: ⊖ DND · a global "Clear all" ----
    let bary = y0 + panelh - notify::BAR_PADB - notify::BAR_BTN;
    let mut bx = x + notify::BAR_PADX;

    {
        // ⊖ do-not-disturb
        let b = RBox::new(bx, bary, notify::BAR_BTN, notify::BAR_BTN);
        let lit = st.suspended;
        let gcol = if lit { ON_ACCENT } else { cfg.col_fg };
        let g = tget(
            p,
            cache,
            &font,
            notify::TextCache::key(
                "⊖",
                notify::color_hex(gcol),
                t.bar as u32,
                64,
                -1,
                0.0,
                false,
                600,
                None,
            ),
            "⊖",
            gcol,
            t.bar,
            64,
            -1,
            0.0,
            600,
            None,
        );
        let hov = hovered.kind == CardKind::BtnDnd;
        p.rect(
            b,
            if lit {
                cfg.col_highlight
            } else if hov {
                ACCENT_DIM
            } else {
                FILL2
            },
            (notify::BAR_BTN / 2.0 * p.scale).round() as u32,
            2.0,
        );
        p.tex(
            &g,
            b.x + (b.w - g.w_px(p.scale)) / 2.0,
            b.y + (b.h - g.h_px(p.scale)) / 2.0,
        );
        cards.push(Card {
            kind: CardKind::BtnDnd,
            rect: b,
            ..Card::default()
        });
        bx += notify::BAR_BTN + notify::BAR_GAP;
    }

    {
        // "Clear all" — the global sweep; greys when the shade is empty
        let cw = x + notify::CENTER_W - notify::BAR_PADX - bx;
        let b = RBox::new(bx, bary, cw, notify::BAR_BTN);
        let target = notifs
            .iter()
            .any(|n| !n.waiting && !notify::in_osd_band(n.id));
        let lcol = if target {
            cfg.col_fg
        } else {
            cfg.col_kicker.modify_a(0.35)
        };
        let ekw = (cw * p.scale).floor() as i32;
        let l = tget(
            p,
            cache,
            &font,
            notify::TextCache::key(
                "Clear all",
                notify::color_hex(lcol),
                t.bar as u32,
                ekw as u32,
                -1,
                0.0,
                false,
                600,
                None,
            ),
            "Clear all",
            lcol,
            t.bar,
            ekw,
            -1,
            0.0,
            600,
            None,
        );
        let hov = hovered.kind == CardKind::BtnClear;
        let fillc = if hov && target {
            ACCENT_DIM
        } else {
            NColors {
                r: 1.0,
                g: 1.0,
                b: 1.0,
                a: (if target { 0.09 } else { 0.035 }) as f32,
            }
        };
        p.rect(
            b,
            fillc,
            (notify::BAR_BTN / 2.0 * p.scale).round() as u32,
            2.0,
        );
        p.tex(
            &l,
            b.x + (b.w - l.w_px(p.scale)) / 2.0,
            b.y + (b.h - l.h_px(p.scale)) / 2.0,
        );
        cards.push(Card {
            kind: CardKind::BtnClear,
            rect: b,
            ..Card::default()
        });
    }

    // Paging cues: a wheel-scroll is invisible otherwise
    if !empty {
        let mut below = 0usize;
        let start = if placed.is_empty() {
            0
        } else {
            placed.last().map_or(0, |pp| pp.0) + 1
        };
        for d in disp.iter().skip(start) {
            below += d.items.len();
        }
        if st.skip > 0 {
            let u = chevron(
                p,
                cache,
                1,
                cfg.col_kicker,
                (t.small as f64 * 2.0).round() as i32,
            );
            p.tex(&u, x + (notify::CENTER_W - u.w_px(p.scale)) / 2.0, y0 + 2.0);
        }
        if below > 0 {
            let num_s = below.to_string();
            let num = tget(
                p,
                cache,
                &font,
                notify::TextCache::key(
                    &num_s,
                    notify::color_hex(cfg.col_kicker),
                    t.small as u32,
                    128,
                    -1,
                    0.0,
                    false,
                    500,
                    None,
                ),
                &num_s,
                cfg.col_kicker,
                t.small,
                128,
                -1,
                0.0,
                500,
                None,
            );
            let dch = chevron(
                p,
                cache,
                0,
                cfg.col_kicker,
                (t.small as f64 * 2.0).round() as i32,
            );
            let nw = num.w_px(p.scale);
            let nh = num.h_px(p.scale);
            let cw2 = dch.w_px(p.scale);
            let chh = dch.h_px(p.scale);
            let ch = nh.max(chh);
            let tot = cw2 + 3.0 + nw;
            let cx = x + (notify::CENTER_W - tot) / 2.0;
            let cy = bary - ch - 3.0;
            p.rect(
                RBox::new(cx - 8.0, cy - 2.0, tot + 16.0, ch + 4.0),
                FILL2,
                ((ch / 2.0 + 2.0) * p.scale).round() as u32,
                2.0,
            );
            p.tex(&dch, cx, cy + (ch - chh) / 2.0);
            p.tex(&num, cx + cw2 + 3.0, cy + (ch - nh) / 2.0);
        }
    }

    st.last_content_h = panelh;
    st.last_content_w = notify::CENTER_W;
}
