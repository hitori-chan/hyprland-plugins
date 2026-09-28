// The banner column (popups.cpp).

use super::*;

// ---------------------------------------------------------------------------
// the banner column (popups.cpp)
// ---------------------------------------------------------------------------

#[allow(clippy::too_many_lines, clippy::cognitive_complexity)]
pub(super) fn popups(p: &mut P, st: &mut NotifyState, cfg: &NConfig) {
    let t = type_scale(cfg, p.scale);
    let mb = p.mb;
    let w = (cfg.width as f64).max(120.0);
    let maxh = (cfg.max_height as f64).max(60.0);
    let gap = (cfg.margin as f64).max(0.0);
    let maxicon = (cfg.max_icon as f64).clamp(16.0, 64.0);
    let round = ((cfg.rounding as f64) * p.scale).round().max(0.0) as u32;
    let rp = cfg.rounding_power;
    let colbody = cfg.col_fg.modify_a(cfg.col_fg.a * 0.92);
    let x = mb.x + mb.w - notify::EDGE - w;
    let mut y = mb.y + cfg.offset_y as f64;
    let font = cfg.font.clone();
    let hovered = &st.hovered;
    let lch = &mut st.last_content_h;
    let lcw = &mut st.last_content_w;

    let notifs = &mut st.notifs;
    let cache = &mut st.text_cache;
    let jobs = &mut st.decode_jobs;
    let cards = &mut st.cards;

    for n in notifs.iter_mut() {
        if n.waiting || !n.banner {
            continue;
        }
        if y + 2.0 * notify::PADY > mb.y + mb.h {
            break;
        }
        let critical = n.urgency >= 2;
        let age = nparse::age_string(ffi::steady_ms(), n.arrived);

        if p.warm {
            nicons::ensure_icon_tex(
                p.ctx,
                cfg,
                jobs,
                n,
                (maxicon * p.scale).round() as u32,
                (w * p.scale).round() as u32,
                (notify::HERO_CAP * p.scale).round() as u32,
            );
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
        let iconw = if leadicon { maxicon } else { 0.0 };

        let textw = w
            - 2.0 * notify::PADX
            - if iconw > 0.0 {
                iconw + notify::ICON_GAP
            } else {
                0.0
            };
        let textwpx = (textw * p.scale).floor().max(1.0) as i32;

        // the header: app • age
        let header_txt = format!("{} • {}", nparse::esc(&n.app_name), age);
        let header = tget(
            p,
            cache,
            &font,
            notify::TextCache::key(
                &header_txt,
                notify::color_hex(cfg.col_kicker),
                t.header as u32,
                textwpx as u32,
                -1,
                0.0,
                true,
                500,
                None,
            ),
            &header_txt,
            cfg.col_kicker,
            t.header,
            textwpx,
            -1,
            0.0,
            500,
            None,
        );
        let title = if n.summary.is_empty() {
            Raster::from(None)
        } else {
            tget(
                p,
                cache,
                &font,
                notify::TextCache::key(
                    &n.summary,
                    notify::color_hex(cfg.col_title),
                    t.title as u32,
                    textwpx as u32,
                    -1,
                    0.0,
                    true,
                    600,
                    None,
                ),
                &n.summary,
                cfg.col_title,
                t.title,
                textwpx,
                -1,
                0.0,
                600,
                None,
            )
        };

        // the action buttons
        if p.warm {
            let enabled = n.action_icons;
            let px = (notify::BTN_ICON * p.scale).round() as u32;
            for a in &mut n.actions {
                nicons::ensure_action_icon(p.ctx, cfg, jobs, enabled, a, px);
            }
        }
        let mut btn_boxes: Vec<RBox> = Vec::new();
        let mut btn_lbls: Vec<Raster> = Vec::new();
        let mut btn_row_y = 0.0;
        {
            let mut bx = 0.0;
            for a in &n.actions {
                let lbl = tget(
                    p,
                    cache,
                    &font,
                    notify::TextCache::key(
                        &a.label,
                        notify::color_hex(cfg.col_highlight),
                        t.action as u32,
                        textwpx as u32,
                        -1,
                        0.0,
                        true,
                        600,
                        None,
                    ),
                    &a.label,
                    cfg.col_highlight,
                    t.action,
                    textwpx,
                    -1,
                    0.0,
                    600,
                    None,
                );
                let lw = lbl.w_px(p.scale);
                let iw = if n.action_icons && a.icon.as_ref().and_then(|i| i.tex.as_ref()).is_some()
                {
                    notify::BTN_ICON + notify::BTN_ICON_GAP
                } else {
                    0.0
                };
                let bw = textw.min(iw + lw + 2.0 * notify::BTN_PADX);
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
        let btn_block = if btn_h > 0.0 {
            notify::BTN_ROW_GAP + btn_h
        } else {
            0.0
        };

        // the body images
        if p.warm {
            for im in &mut n.body_images {
                nicons::ensure_body_image(
                    p.ctx,
                    jobs,
                    im,
                    (notify::BODYIMG_H * p.scale).round() as u32,
                );
            }
        }
        let mut img_boxes: Vec<RBox> = Vec::new();
        let mut img_row_y = 0.0;
        {
            let mut bx = 0.0;
            for im in &n.body_images {
                let Some(tex) = im.tex.as_ref() else {
                    continue;
                };
                let (tw, th) = tex.size();
                let ar = if th > 0 { tw as f64 / th as f64 } else { 1.0 };
                let wd = textw.min(ar * notify::BODYIMG_H);
                if bx > 0.0 && bx + wd > textw + 0.5 {
                    bx = 0.0;
                    img_row_y += notify::BODYIMG_H + notify::IMG_GAP;
                }
                img_boxes.push(RBox::new(bx, img_row_y, wd, notify::BODYIMG_H));
                bx += wd + notify::IMG_GAP;
            }
        }
        let img_h = if img_boxes.is_empty() {
            0.0
        } else {
            img_row_y + notify::BODYIMG_H
        };
        let img_block = if img_h > 0.0 {
            notify::IMG_ROW_GAP + img_h
        } else {
            0.0
        };

        // failed <img> alt lines (the words outlive the missing file)
        let mut alt_lines: Vec<Raster> = Vec::new();
        let mut alt_h = 0.0;
        for im in &n.body_images {
            if im.tex.is_none() && !im.alt.is_empty() {
                let alt = tget(
                    p,
                    cache,
                    &font,
                    notify::TextCache::key(
                        &im.alt,
                        notify::color_hex(cfg.col_kicker),
                        t.body as u32,
                        textwpx as u32,
                        1,
                        0.0,
                        false,
                        400,
                        None,
                    ),
                    &im.alt,
                    cfg.col_kicker,
                    t.body,
                    textwpx,
                    1,
                    0.0,
                    400,
                    None,
                );
                alt_h += alt_res(&alt, p.scale);
                alt_lines.push(alt);
            }
        }
        let alt_block = if alt_h > 0.0 {
            notify::IMG_ROW_GAP + alt_h
        } else {
            0.0
        };

        let hh = header.h_px(p.scale);
        let th = title.h_px(p.scale);
        let avail = maxh
            - 2.0 * notify::PADY
            - if hero { heroh } else { 0.0 }
            - hh
            - if hh > 0.0 { notify::HEAD_GAP } else { 0.0 }
            - th
            - notify::TITLE_GAP
            - if n.progress >= 0 {
                notify::PROGRESS_GAP + notify::PROGRESS_H
            } else {
                0.0
            }
            - btn_block
            - img_block
            - alt_block;
        let linepx = (t.body as f64 * 1.35).round() as i32;
        let bodycap = (linepx * 8)
            .min((avail * p.scale).floor() as i32)
            .max(linepx);
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
                    bodycap,
                    1.1,
                    true,
                    400,
                    Some(notify::color_hex(cfg.col_link)),
                ),
                &n.body,
                colbody,
                t.body,
                textwpx,
                bodycap,
                1.1,
                400,
                Some(cfg.col_link),
            )
        };

        let bh = body.h_px(p.scale);
        let mut tht = hh
            + if hh > 0.0 { notify::HEAD_GAP } else { 0.0 }
            + th
            + if th > 0.0 && bh > 0.0 {
                notify::TITLE_GAP
            } else {
                0.0
            }
            + bh
            + img_block;
        if n.progress >= 0 {
            tht += if tht > 0.0 { notify::PROGRESS_GAP } else { 0.0 } + notify::PROGRESS_H;
        }
        tht += btn_block;

        let ch = if hero {
            heroh + notify::PADY + tht.min(maxh - notify::HERO_TEXT_MIN) + notify::PADY
        } else {
            maxh.min(iconw.max(tht) + 2.0 * notify::PADY)
        };

        // the arrival spring: fade + an 8px drop, keyed on born
        let mut cp = *p;
        let at = if animations_on(p.ctx) && n.banner {
            anim_t(n.born, ffi::steady_ms(), notify::MOTION_SPATIAL_MS)
        } else {
            1.0
        };
        if at < 1.0 {
            cp.alpha = p.alpha * ease_out_cubic(at) as f32;
            cp.dy = p.dy - (1.0 - ease_out_back(at)) * 8.0;
        }

        let card_box = RBox::new(x, y, w, ch);
        cp.shadow(card_box, round, rp, 16.0);
        cp.glass(card_box, cfg.col_bg, round, rp);
        if critical {
            cp.ring(card_box, cfg.col_urgent, round, rp, 1.0);
        }

        if hero {
            if n.icon.tex.is_some() {
                cp.texfit(
                    &Raster::from(Some(&lead_arc(&n.icon))),
                    RBox::new(x, y, w, heroh),
                    round,
                    rp,
                );
            }
        } else if leadicon {
            paint_icon_column(
                &cp,
                n,
                RBox::new(x + notify::PADX, y + notify::PADY, iconw, iconw),
                true,
                rp,
            );
        }

        let tx = x
            + notify::PADX
            + if iconw > 0.0 {
                iconw + notify::ICON_GAP
            } else {
                0.0
            };
        let mut ty = if hero {
            y + heroh + notify::PADY
        } else {
            y + notify::PADY
        };
        cp.tex(&header, tx, ty);
        ty += hh + if hh > 0.0 { notify::HEAD_GAP } else { 0.0 };
        cp.tex(&title, tx, ty);
        ty += th
            + if th > 0.0 && bh > 0.0 {
                notify::TITLE_GAP
            } else {
                0.0
            };
        cp.tex(&body, tx, ty);
        let mut card_links: Vec<(RBox, String)> = Vec::new();
        for (href, r) in &body.links {
            card_links.push((
                RBox::new(
                    tx + r.0 / p.scale,
                    ty + r.1 / p.scale,
                    r.2 / p.scale,
                    r.3 / p.scale,
                ),
                href.clone(),
            ));
        }
        ty += bh;
        if !img_boxes.is_empty() {
            ty += notify::IMG_ROW_GAP;
            let mut bi = 0usize;
            for im in &n.body_images {
                if im.tex.is_some() && bi < img_boxes.len() {
                    let b = img_boxes[bi];
                    bi += 1;
                    cp.texfit(
                        &Raster::from(Some(&im_arc(im))),
                        RBox::new(tx + b.x, ty + b.y, b.w, b.h),
                        round,
                        rp,
                    );
                }
            }
            ty += img_h;
        }
        if !alt_lines.is_empty() {
            ty += notify::IMG_ROW_GAP;
            for a in &alt_lines {
                // the reserved line: the texture's height, floored at 14
                let res = alt_res(a, p.scale);
                if a.tex.is_some() {
                    cp.tex(a, tx, ty + (res - a.h_px(p.scale)) / 2.0);
                }
                ty += res;
            }
        }
        if n.progress >= 0 {
            ty += if tht > 0.0 { notify::PROGRESS_GAP } else { 0.0 };
            paint_progress(&cp, cfg, tx, ty, textw, n.progress, critical);
            ty += notify::PROGRESS_H;
        }

        let mut card_btns: Vec<(RBox, String)> = Vec::new();
        if !btn_boxes.is_empty() {
            ty += notify::BTN_ROW_GAP;
            let bx0 = tx - notify::BTN_PADX;
            for (k, a) in n.actions.iter().enumerate() {
                let b = btn_boxes[k];
                let rect = RBox::new(bx0 + b.x, ty + b.y, b.w, b.h);
                let bhov = hovered.kind == CardKind::Popup
                    && hovered.id == n.id
                    && hovered.btn == k as i32;
                if bhov {
                    cp.rect(
                        rect,
                        ACCENT_DIM,
                        (notify::BTN_H / 2.0 * p.scale).round() as u32,
                        2.0,
                    );
                }
                let mut cx = rect.x + notify::BTN_PADX;
                if n.action_icons && a.icon.as_ref().and_then(|i| i.tex.as_ref()).is_some() {
                    if let Some(icon) = &a.icon {
                        cp.texfit(
                            &Raster::from(Some(&lead_arc(icon))),
                            RBox::new(
                                cx,
                                rect.y + (rect.h - notify::BTN_ICON) / 2.0,
                                notify::BTN_ICON,
                                notify::BTN_ICON,
                            ),
                            0,
                            2.0,
                        );
                    }
                    cx += notify::BTN_ICON + notify::BTN_ICON_GAP;
                }
                cp.tex(
                    &btn_lbls[k],
                    cx,
                    rect.y + (rect.h - btn_lbls[k].h_px(p.scale)) / 2.0,
                );
                card_btns.push((rect, a.id.clone()));
            }
        }

        // the hover-✕ (revealed while the pointer is on the card)
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
        let cardhov = hovered.kind == CardKind::Popup && hovered.id == n.id;
        let mut card = Card {
            kind: CardKind::Popup,
            rect: card_box,
            id: n.id,
            ..Card::default()
        };
        card.buttons = card_btns;
        card.links = card_links;
        if cardhov {
            let xb = RBox::new(
                x + w - notify::XCIRC - 8.0,
                y + 8.0,
                notify::XCIRC,
                notify::XCIRC,
            );
            let xhov = hovered.part == 2;
            cp.rect(
                xb,
                if xhov { cfg.col_urgent } else { FILL2 },
                (notify::XCIRC / 2.0 * p.scale).round() as u32,
                2.0,
            );
            let g = if xhov { &xghot } else { &xg };
            cp.tex(
                g,
                xb.x + (xb.w - g.w_px(p.scale)) / 2.0,
                xb.y + (xb.h - g.h_px(p.scale)) / 2.0,
            );
            card.close = xb;
        }

        cards.push(card);
        y += ch + gap;
    }

    *lch = (y - gap - (mb.y + cfg.offset_y as f64)).max(0.0);
    *lcw = w;
}

/// A BodyImage's raster (a copy, so the image's Arc stays shared).
fn im_arc(im: &nparse::BodyImage) -> notify::CachedText {
    notify::CachedText {
        tex: im.tex.clone(),
        links: Vec::new(),
        w: im.tex.as_ref().map_or(0, |t| t.size().0),
        h: im.tex.as_ref().map_or(0, |t| t.size().1),
    }
}
