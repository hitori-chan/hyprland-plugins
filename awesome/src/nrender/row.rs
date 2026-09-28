// One shade row (row.cpp renderRow), both faces: the shared card recipes
// live in the parent module.

use super::*;

#[derive(Clone, Copy)]
pub(super) struct RowStyle {
    icon_px: f64,
    with_badge: bool,
    header_has_app: bool,
    has_chevron: bool,
    can_reply: bool,
}
pub(super) const ROW_SINGLE: RowStyle = RowStyle {
    icon_px: notify::ROW_ICON,
    with_badge: true,
    header_has_app: true,
    has_chevron: true,
    can_reply: true,
};
pub(super) const ROW_CHILD: RowStyle = RowStyle {
    icon_px: notify::CHILD_ICON,
    with_badge: false,
    header_has_app: false,
    has_chevron: false,
    can_reply: false,
};

/// One shade row (row.cpp renderRow), both faces. Returns its height and the
// card's hit boxes (the caller pushes the card).
#[allow(
    clippy::too_many_lines,
    clippy::too_many_arguments,
    clippy::cognitive_complexity
)]
pub(super) fn render_row(
    p: &mut P,
    cfg: &NConfig,
    n: &mut nparse::Notif,
    cache: &mut notify::TextCache,
    jobs: &mut Vec<nicons::DecodeJob>,
    hovered: &notify::Hover,
    reply_id: u32,
    reply_text: &str,
    box_: RBox,
    open: bool,
    more: bool,
    style: RowStyle,
    child: bool,
) -> (f64, Card) {
    let t = type_scale(cfg, p.scale);
    let colbody = cfg.col_fg.modify_a(cfg.col_fg.a * 0.92);
    let age = nparse::age_string(ffi::steady_ms(), n.arrived);
    let subhex = nparse::hex6(cfg.col_kicker.r, cfg.col_kicker.g, cfg.col_kicker.b);
    let rp = cfg.rounding_power;
    let facepile_d = 20.0;
    let facepile_gap = 4.0;
    let font = cfg.font.clone();

    if p.warm {
        nicons::ensure_icon_tex(
            p.ctx,
            cfg,
            jobs,
            n,
            (style.icon_px.max(cfg.max_icon as f64) * p.scale).round() as u32,
            (box_.w * p.scale).round() as u32,
            (notify::HERO_CAP * p.scale).round() as u32,
        );
        if n.conversation_kind == "group" {
            for pa in &mut n.participants {
                nicons::ensure_avatar_tex(
                    p.ctx,
                    cfg,
                    jobs,
                    pa,
                    (facepile_d * p.scale).round() as u32,
                );
            }
        }
    }

    let hero = n.icon.tex.is_some() && n.hero;
    let heroh = if hero {
        n.icon
            .tex
            .as_ref()
            .map_or(0.0, |t2| t2.size().1 as f64 / p.scale)
    } else {
        0.0
    };
    let leadicon = !hero && has_lead_icon(n);
    let iconw = if leadicon { style.icon_px } else { 0.0 };
    let tx = box_.x
        + notify::ROW_PADX
        + if iconw > 0.0 {
            iconw + notify::ROW_ICON_GAP
        } else {
            0.0
        };
    let chevron_on = style.has_chevron && (open || more);
    let rtrim = if chevron_on { notify::CHEV + 8.0 } else { 0.0 };
    let textw = box_.x + box_.w - notify::ROW_PADX - rtrim - tx;
    let textwpx = (textw * p.scale).floor().max(1.0) as i32;

    let ty = if hero {
        box_.y + heroh + notify::PADY
    } else {
        box_.y + notify::ROW_PADT
    };
    let mut card = Card {
        id: n.id,
        kind: if child {
            CardKind::Child
        } else {
            CardKind::Row
        },
        ..Card::default()
    };
    let mut th = 0.0_f64;

    if !open {
        // collapsed: bold "title • age" + the newest body line (+progress)
        let line_txt = format!(
            "{} <span foreground=\"{}\">• {}</span>",
            nparse::esc(&n.summary),
            subhex,
            age
        );
        let line = tget(
            p,
            cache,
            &font,
            notify::TextCache::key(
                &line_txt,
                notify::color_hex(cfg.col_title),
                t.title as u32,
                textwpx as u32,
                -1,
                0.0,
                true,
                600,
                None,
            ),
            &line_txt,
            cfg.col_title,
            t.title,
            textwpx,
            -1,
            0.0,
            600,
            None,
        );
        let b1s = nparse::collapsed_line(n);
        let b1 = if b1s.is_empty() {
            Raster::from(None)
        } else {
            tget(
                p,
                cache,
                &font,
                notify::TextCache::key(
                    &b1s,
                    notify::color_hex(colbody),
                    t.body as u32,
                    textwpx as u32,
                    -1,
                    0.0,
                    true,
                    400,
                    None,
                ),
                &b1s,
                colbody,
                t.body,
                textwpx,
                -1,
                0.0,
                400,
                None,
            )
        };
        // += on the zero base: the expanded face assigns absolutely
        th += line.h_px(p.scale)
            + if b1.tex.is_some() {
                2.0 + b1.h_px(p.scale)
            } else {
                0.0
            }
            + if n.progress >= 0 {
                notify::PROGRESS_GAP + notify::PROGRESS_H
            } else {
                0.0
            };
        p.tex(&line, tx, ty);
        let mut yy = ty + line.h_px(p.scale) + 2.0;
        p.tex(&b1, tx, yy);
        yy += b1.h_px(p.scale);
        if n.progress >= 0 {
            yy += notify::PROGRESS_GAP;
            paint_progress(p, cfg, tx, yy, textw, n.progress, n.urgency >= 2);
        }
    } else {
        // expanded: age/header, title, body, progress, the card's actions
        let facepile = n.conversation_kind == "group" && !n.participants.is_empty();
        let pile_n = if facepile {
            n.participants.len().min(3)
        } else {
            0
        };
        let unread_num = if n.conversation && n.unread_count > 0 {
            n.unread_count.to_string()
        } else {
            String::new()
        };
        let unread = if unread_num.is_empty() {
            Raster::from(None)
        } else {
            tget(
                p,
                cache,
                &font,
                notify::TextCache::key(
                    &unread_num,
                    notify::color_hex(ON_ACCENT),
                    t.small as u32,
                    64,
                    -1,
                    0.0,
                    false,
                    600,
                    None,
                ),
                &unread_num,
                ON_ACCENT,
                t.small,
                64,
                -1,
                0.0,
                600,
                None,
            )
        };
        let pill_w2 = if unread.tex.is_some() {
            unread.w_px(p.scale) + 12.0
        } else {
            0.0
        };
        let facew = if pile_n > 0 {
            pile_n as f64 * facepile_d + (pile_n - 1) as f64 * facepile_gap
        } else {
            0.0
        };
        let extraw = (if facew > 0.0 { facew + 6.0 } else { 0.0 })
            + (if pill_w2 > 0.0 {
                pill_w2 + if facew > 0.0 { 6.0 } else { 0.0 }
            } else {
                0.0
            });
        let kickwpx = ((textw - extraw) * p.scale).floor().max(1.0) as i32;

        let kick_txt = if style.header_has_app {
            format!("{} • {}", nparse::esc(&n.app_name), age)
        } else {
            age.clone()
        };
        let kick = tget(
            p,
            cache,
            &font,
            notify::TextCache::key(
                &kick_txt,
                notify::color_hex(cfg.col_kicker),
                t.header as u32,
                kickwpx as u32,
                -1,
                0.0,
                true,
                500,
                None,
            ),
            &kick_txt,
            cfg.col_kicker,
            t.header,
            kickwpx,
            -1,
            0.0,
            500,
            None,
        );
        let title_src = if n.conversation_title.is_empty() {
            n.summary.clone()
        } else {
            n.conversation_title.clone()
        };
        let title = if title_src.is_empty() {
            Raster::from(None)
        } else {
            tget(
                p,
                cache,
                &font,
                notify::TextCache::key(
                    &title_src,
                    notify::color_hex(cfg.col_title),
                    t.title as u32,
                    textwpx as u32,
                    -1,
                    0.0,
                    true,
                    600,
                    None,
                ),
                &title_src,
                cfg.col_title,
                t.title,
                textwpx,
                -1,
                0.0,
                600,
                None,
            )
        };
        let capl = (t.body as f64 * 1.35 * if n.conversation { 7.0 } else { 4.0 }).round() as i32;
        let body = if n.body.is_empty() {
            Raster::from(None)
        } else {
            tget(
                p,
                cache,
                &font,
                notify::TextCache::key(
                    &n.body,
                    notify::color_hex(colbody),
                    t.body as u32,
                    textwpx as u32,
                    capl,
                    1.1,
                    true,
                    400,
                    Some(notify::color_hex(cfg.col_link)),
                ),
                &n.body,
                colbody,
                t.body,
                textwpx,
                capl,
                1.1,
                400,
                Some(cfg.col_link),
            )
        };

        // the reply chip, then the card's own actions (the primary fires the
        // row body, so it gets no button)
        let armed = style.can_reply && reply_id != 0 && reply_id == n.id;
        let mut btn_src: Vec<(String, String)> = Vec::new();
        if style.can_reply && n.can_reply && !armed {
            btn_src.push((
                "inline-reply".to_owned(),
                if n.reply_submit_text.is_empty() {
                    "Reply".to_owned()
                } else {
                    n.reply_submit_text.clone()
                },
            ));
        }
        for a in &n.actions {
            btn_src.push((a.id.clone(), a.label.clone()));
        }

        let mut btn_boxes: Vec<RBox> = Vec::new();
        let mut btn_lbls: Vec<Raster> = Vec::new();
        let mut btn_row_y = 0.0;
        {
            let mut bx = 0.0;
            for (_bid, blbl) in &btn_src {
                let lbl = tget(
                    p,
                    cache,
                    &font,
                    notify::TextCache::key(
                        blbl,
                        notify::color_hex(cfg.col_highlight),
                        t.action as u32,
                        textwpx as u32,
                        -1,
                        0.0,
                        true,
                        600,
                        None,
                    ),
                    blbl,
                    cfg.col_highlight,
                    t.action,
                    textwpx,
                    -1,
                    0.0,
                    600,
                    None,
                );
                let bw = textw.min(lbl.w_px(p.scale) + 2.0 * notify::BTN_PADX);
                if bx > 0.0 && bx + bw > textw + 0.5 {
                    bx = 0.0;
                    btn_row_y += notify::BTN_H + notify::BTN_GAP;
                }
                btn_boxes.push(RBox::new(bx, btn_row_y, bw, notify::BTN_H));
                btn_lbls.push(lbl);
                bx += bw + notify::BTN_GAP;
            }
        }
        let btn_h = if btn_boxes.is_empty() {
            0.0
        } else {
            btn_row_y + notify::BTN_H
        };

        let kh = kick.h_px(p.scale);
        let th2 = title.h_px(p.scale);
        let bh = body.h_px(p.scale);
        th = kh
            + if kh > 0.0 { notify::HEAD_GAP } else { 0.0 }
            + th2
            + if th2 > 0.0 && bh > 0.0 {
                notify::TITLE_GAP
            } else {
                0.0
            }
            + bh
            + if n.progress >= 0 {
                notify::PROGRESS_GAP + notify::PROGRESS_H
            } else {
                0.0
            }
            + if btn_h > 0.0 {
                notify::BTN_ROW_GAP + btn_h
            } else {
                0.0
            }
            + if armed {
                notify::BTN_ROW_GAP + notify::BTN_H
            } else {
                0.0
            };

        p.tex(&kick, tx, ty);
        // the header's right side: the unread pill, then the facepile
        let mut right = box_.x + box_.w - notify::ROW_PADX - rtrim;
        if pill_w2 > 0.0 && unread.tex.is_some() {
            let pb2 = RBox::new(
                right - pill_w2,
                ty + (kh.max(notify::PILL_H) - notify::PILL_H) / 2.0,
                pill_w2,
                notify::PILL_H,
            );
            p.rect(
                pb2,
                ACCENT_DIM,
                (notify::PILL_H / 2.0 * p.scale).round() as u32,
                2.0,
            );
            p.tex(
                &unread,
                pb2.x + (pb2.w - unread.w_px(p.scale)) / 2.0,
                pb2.y + (pb2.h - unread.h_px(p.scale)) / 2.0,
            );
            right -= pill_w2 + 6.0;
        }
        for i in 0..pile_n {
            let pa = &n.participants[i];
            p.texfit(
                &Raster::from(Some(&lead_arc(&pa.avatar))),
                RBox::new(
                    right - facepile_d,
                    ty + (kh.max(facepile_d) - facepile_d) / 2.0,
                    facepile_d,
                    facepile_d,
                ),
                (facepile_d / 2.0 * p.scale).round() as u32,
                rp,
            );
            right -= facepile_d + facepile_gap;
        }
        let mut yy = ty + kh + if kh > 0.0 { notify::HEAD_GAP } else { 0.0 };
        p.tex(&title, tx, yy);
        yy += th2
            + if th2 > 0.0 && bh > 0.0 {
                notify::TITLE_GAP
            } else {
                0.0
            };
        p.tex(&body, tx, yy);
        for (href, r) in &body.links {
            card.links.push((
                RBox::new(
                    tx + r.0 / p.scale,
                    yy + r.1 / p.scale,
                    r.2 / p.scale,
                    r.3 / p.scale,
                ),
                href.clone(),
            ));
        }
        yy += bh;
        if n.progress >= 0 {
            yy += notify::PROGRESS_GAP;
            paint_progress(p, cfg, tx, yy, textw, n.progress, n.urgency >= 2);
            yy += notify::PROGRESS_H;
        }
        if btn_h > 0.0 {
            yy += notify::BTN_ROW_GAP;
            let bx0 = tx - notify::BTN_PADX;
            for (k, b) in btn_boxes.iter().enumerate() {
                let rect = RBox::new(bx0 + b.x, yy + b.y, b.w, b.h);
                let bhov = hovered.id == n.id && hovered.btn == k as i32;
                if bhov {
                    p.rect(
                        rect,
                        ACCENT_DIM,
                        (notify::BTN_H / 2.0 * p.scale).round() as u32,
                        2.0,
                    );
                }
                p.tex(
                    &btn_lbls[k],
                    rect.x + notify::BTN_PADX,
                    rect.y + (rect.h - btn_lbls[k].h_px(p.scale)) / 2.0,
                );
                card.buttons.push((rect, btn_src[k].0.clone()));
            }
        }

        // the armed inline-reply field
        if armed {
            yy += notify::BTN_ROW_GAP;
            let txt = reply_text.to_owned();
            let slbl_txt = if n.reply_submit_text.is_empty() {
                "Send".to_owned()
            } else {
                n.reply_submit_text.clone()
            };
            let slbl = tget(
                p,
                cache,
                &font,
                notify::TextCache::key(
                    &slbl_txt,
                    notify::color_hex(ON_ACCENT),
                    t.action as u32,
                    textwpx as u32,
                    -1,
                    0.0,
                    false,
                    600,
                    None,
                ),
                &slbl_txt,
                ON_ACCENT,
                t.action,
                textwpx,
                -1,
                0.0,
                600,
                None,
            );
            let sendw = (textw / 2.0).min(slbl.w_px(p.scale) + 2.0 * notify::BTN_PADX);
            let fb = RBox::new(
                tx,
                yy,
                40.0_f64.max(textw - sendw - notify::BTN_GAP),
                notify::BTN_H,
            );
            let sb = RBox::new(tx + textw - sendw, yy, sendw, notify::BTN_H);
            let rb = (notify::BTN_H / 2.0 * p.scale).round() as u32;

            let ent = if txt.is_empty() {
                Raster::from(None)
            } else {
                tget(
                    p,
                    cache,
                    &font,
                    notify::TextCache::key(
                        &txt,
                        notify::color_hex(cfg.col_fg),
                        t.action as u32,
                        ((fb.w - 2.0 * notify::BTN_PADX) * p.scale).max(1.0) as u32,
                        -1,
                        0.0,
                        false,
                        400,
                        None,
                    ),
                    &txt,
                    cfg.col_fg,
                    t.action,
                    ((fb.w - 2.0 * notify::BTN_PADX) * p.scale).max(1.0) as i32,
                    -1,
                    0.0,
                    400,
                    None,
                )
            };
            let ph_txt = if n.reply_placeholder.is_empty() {
                "Type a reply…".to_owned()
            } else {
                n.reply_placeholder.clone()
            };
            let ph = if txt.is_empty() {
                tget(
                    p,
                    cache,
                    &font,
                    notify::TextCache::key(
                        &ph_txt,
                        notify::color_hex(cfg.col_kicker),
                        t.action as u32,
                        ((fb.w - 2.0 * notify::BTN_PADX) * p.scale).max(1.0) as u32,
                        -1,
                        0.0,
                        false,
                        400,
                        None,
                    ),
                    &ph_txt,
                    cfg.col_kicker,
                    t.action,
                    ((fb.w - 2.0 * notify::BTN_PADX) * p.scale).max(1.0) as i32,
                    -1,
                    0.0,
                    400,
                    None,
                )
            } else {
                Raster::from(None)
            };
            p.rect(fb, FILL2, rb, 2.0);
            p.ring(fb, cfg.col_highlight, rb, rp, 1.0);
            let show = if ent.tex.is_some() { &ent } else { &ph };
            let mut cx = fb.x + notify::BTN_PADX;
            p.tex(show, cx, fb.y + (fb.h - show.h_px(p.scale)) / 2.0);
            if ent.tex.is_some() {
                cx += ent.w_px(p.scale);
            }
            // the caret sits at the end: editing is append + backspace
            p.rect(
                RBox::new(cx + 1.0, fb.y + 5.0, 1.5, fb.h - 10.0)
                    .shift_x((cx + 1.0).min(fb.x + fb.w - 3.0)),
                cfg.col_highlight,
                0,
                2.0,
            );
            let sb_hov = hovered.id == n.id && hovered.part == 4;
            p.rect(
                sb,
                if txt.is_empty() {
                    FILL2
                } else if sb_hov {
                    cfg.col_highlight
                } else {
                    ACCENT_DIM
                },
                rb,
                2.0,
            );
            p.tex(
                &slbl,
                sb.x + (sb.w - slbl.w_px(p.scale)) / 2.0,
                sb.y + (sb.h - slbl.h_px(p.scale)) / 2.0,
            );
            card.reply_field = fb;
            card.reply_send = sb;
        }
    }

    let rowh = (if hero {
        heroh + notify::PADY
    } else {
        notify::ROW_PADT
    }) + th.max(iconw)
        + notify::ROW_PADB;

    let hero_raster = if hero && n.icon.tex.is_some() {
        Some(Raster::from(Some(&lead_arc(&n.icon))))
    } else {
        None
    };
    if let Some(hr) = &hero_raster {
        p.texfit(
            hr,
            RBox::new(box_.x, box_.y, box_.w, heroh),
            r_row(cfg, p.scale),
            rp,
        );
    }
    if leadicon {
        // collapsed rows center the icon; expanded top-pin it
        let iy = if open {
            ty
        } else {
            box_.y + (rowh - iconw) / 2.0
        };
        paint_icon_column(
            p,
            n,
            RBox::new(box_.x + notify::ROW_PADX, iy, iconw, iconw),
            style.with_badge,
            rp,
        );
    }
    if chevron_on {
        let cy = if open {
            ty
        } else if hero {
            ty + (th.max(iconw) - notify::CHEV) / 2.0
        } else {
            box_.y + (rowh - notify::CHEV) / 2.0
        };
        let cb = RBox::new(
            box_.x + box_.w - notify::ROW_PADX - notify::CHEV,
            cy,
            notify::CHEV,
            notify::CHEV,
        );
        let g = chevron(p, cache, u32::from(open), cfg.col_fg, notify::CHEV as i32);
        let chov = hovered.id == n.id && hovered.part == 1 && hovered.btn < 0;
        p.rect(
            cb,
            if chov { ACCENT_DIM } else { FILL2 },
            (notify::CHEV / 2.0 * p.scale).round() as u32,
            2.0,
        );
        p.tex(
            &g,
            cb.x + (cb.w - g.w_px(p.scale)) / 2.0,
            cb.y + (cb.h - g.h_px(p.scale)) / 2.0,
        );
        card.chevron = cb;
    }

    card.rect = RBox::new(box_.x, box_.y, box_.w, rowh);
    (rowh, card)
}

/// The ruler: the same code with a paint context that draws nothing.
#[allow(clippy::too_many_arguments)]
pub(super) fn measure_row(
    p: &mut P,
    cfg: &NConfig,
    n: &mut nparse::Notif,
    cache: &mut notify::TextCache,
    jobs: &mut Vec<nicons::DecodeJob>,
    hovered: &notify::Hover,
    reply_id: u32,
    reply_text: &str,
    w: f64,
    open: bool,
    child: bool,
) -> f64 {
    render_row(
        p,
        cfg,
        n,
        cache,
        jobs,
        hovered,
        reply_id,
        reply_text,
        RBox::new(0.0, 0.0, w, 0.0),
        open,
        true,
        if child { ROW_CHILD } else { ROW_SINGLE },
        child,
    )
    .0
}
