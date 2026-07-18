// hyprbar/render.cpp — the bar itself, the text cache, the paint context, the pass element

#include "hyprbar.hpp"

namespace NHyprbar {

    static const char* KANJI[9] = {"一", "二", "三", "四", "五", "六", "七", "八", "九"};

    // Each entry remembers the warm generation that last wanted it. Evicting
    // whatever the CURRENT layout doesn't name would thrash: sloppy focus
    // re-keys two task labels (colFg <-> colActive) for every window the cursor
    // crosses, so the variant it just left would be rebuilt on the way back —
    // a pango render + upload each time. A grace window keeps those hot and
    // still bounds the map against title churn.
    struct SCachedTex {
        SP<ITexture> tex;
        uint64_t     gen = 0;
    };
    static std::unordered_map<std::string, SCachedTex> texCache;
    static uint64_t                                    texGen         = 0;
    static constexpr uint64_t                          TEX_CACHE_LIFE = 32; // warms an unused texture survives

    // per monitor: a fingerprint of the task labels the strip shows — see
    // the tasklist in renderBar
    static std::unordered_map<uint64_t, size_t> lastTaskFp;

    // Built ONLY by the warm pass — see the texture rule in hyprbar.hpp. A miss
    // during a draw returns null (that one label is missing for one frame)
    // rather than building, which would paint nothing anyway AND swallow every
    // later draw in the element.
    SP<ITexture> textTex(const std::string& text, const CHyprColor& col, int pt, int maxWidth, const std::string& font) {
        char               meta[48]; // the non-text key parts in one stack write — no to_string churn per call
        const int          METALEN = std::snprintf(meta, sizeof(meta), "|%llx|%d|%d|", (unsigned long long)col.getAsHex(), pt, maxWidth);

        static std::string KEY; // reused; main thread only
        KEY.clear();
        KEY += text;
        KEY.append(meta, METALEN > 0 ? (size_t)METALEN : 0);
        KEY += font;

        if (const auto IT = texCache.find(KEY); IT != texCache.end()) {
            IT->second.gen = texGen;
            return IT->second.tex;
        }

        if (!warming) {
            texStale = true;
            return nullptr;
        }

        auto tex      = g_pHyprRenderer->renderText(text, col, pt, false, font.empty() ? cfg.font->value() : font, maxWidth);
        texCache[KEY] = {tex, texGen};
        return tex;
    }

    // awesome's awful.layout: the ordered registry (order matters, like
    // awful.layout.layouts) and each workspace's index — per tag, exactly
    // awesome's model. Future layouts append here and take real effect
    // wherever they get implemented; the bar carries the state and the icon
    // (~/.config/hypr/icons/<name>.png).
    static const std::vector<const char*>          LAYOUTS = {"floating"};
    static std::unordered_map<WORKSPACEID, size_t> wsLayout;
    // keyed by the LAYOUTS literals themselves — pointer identity, no
    // per-frame string
    static std::unordered_map<const char*, SP<ITexture>> layoutTexs;
    static std::unordered_set<const char*>               layoutTexTried;

    static const char*                                   currentLayout(WORKSPACEID ws) {
        const auto IT = wsLayout.find(ws);
        return LAYOUTS[(IT == wsLayout.end() ? 0 : IT->second) % LAYOUTS.size()];
    }

    void layoutInc(int dir) {
        const auto MON = Desktop::focusState() ? Desktop::focusState()->monitor() : nullptr;
        if (!MON || !MON->m_activeWorkspace)
            return;
        const int64_t N   = (int64_t)LAYOUTS.size();
        auto&         IDX = wsLayout[MON->m_activeWorkspace->m_id];
        IDX               = (size_t)(((int64_t)IDX + dir % N + N) % N);
        barChanged();
    }

    // ---- the battery (Android's unified battery, drawn natively) ----
    //
    // A 1:1 transcription of AOSP SystemUI's BatteryLayersDrawable stack
    // (frameworks/base packages/SystemUI battery/unified + res, Apache-2.0),
    // the status bar battery Android 16/17 ships. All constants below are
    // that asset's own, on its 24x14 viewport: body outline stroked 1.5
    // with the cap on the LEFT, frame bg at black 18%, right-anchored fill
    // over [3.5,22.5] trimmed by a CLEAR re-stroke (fill drains toward the
    // cap), percent in google-sans bold — Roboto Bold is AOSP's own
    // fallback — and while charging the space-sharing layout: smaller
    // digits plus the bolt fit into the 6x6 right canvas.
    static SP<ITexture> batteryPill(int percent, bool charging, double hPx, const CHyprColor& fg, const CHyprColor& fill) {
        const double S  = hPx / 14.0; // one viewport unit
        const int    CW = (int)std::ceil(24.0 * S), CH = (int)std::ceil(14.0 * S);

        auto*        SURF = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, CW, CH);
        auto*        CR   = cairo_create(SURF);

        // battery_unified_frame_path_string (the stroke centerline)
        const auto   body = [&]() {
            cairo_move_to(CR, 2.75 * S, 3 * S);
            cairo_curve_to(CR, 2.75 * S, 1.757 * S, 3.757 * S, 0.75 * S, 5 * S, 0.75 * S);
            cairo_line_to(CR, 20 * S, 0.75 * S);
            cairo_curve_to(CR, 21.795 * S, 0.75 * S, 23.25 * S, 2.205 * S, 23.25 * S, 4 * S);
            cairo_line_to(CR, 23.25 * S, 10 * S);
            cairo_curve_to(CR, 23.25 * S, 11.795 * S, 21.795 * S, 13.25 * S, 20 * S, 13.25 * S);
            cairo_line_to(CR, 5 * S, 13.25 * S);
            cairo_curve_to(CR, 3.757 * S, 13.25 * S, 2.75 * S, 12.243 * S, 2.75 * S, 11 * S);
            cairo_close_path(CR);
        };

        cairo_set_source_rgba(CR, 0, 0, 0, 0.18); // DarkThemeColors.bg
        body();
        cairo_fill(CR);

        cairo_set_source_rgba(CR, fg.r, fg.g, fg.b, fg.a);
        body();
        cairo_set_line_width(CR, 1.5 * S);
        cairo_stroke(CR);

        // the cap, battery_unified_frame's second path
        cairo_move_to(CR, 0, 4 * S);
        cairo_curve_to(CR, 0, 3.448 * S, 0.448 * S, 3 * S, 1 * S, 3 * S);
        cairo_line_to(CR, 1.5 * S, 3 * S);
        cairo_line_to(CR, 1.5 * S, 11 * S);
        cairo_line_to(CR, 1 * S, 11 * S);
        cairo_curve_to(CR, 0.448 * S, 11 * S, 0, 10.552 * S, 0, 10 * S);
        cairo_close_path(CR);
        cairo_fill(CR);

        if (percent > 0) { // BatteryFillDrawable: the group is its saveLayer,
                           // so the CLEAR trim can't punch through the frame
            const double EMPTY = percent >= 100 ? 0.0 : std::floor(19.0 * S * (1.0 - percent / 100.0));
            cairo_push_group(CR);
            cairo_save(CR);
            body();
            cairo_clip(CR);
            cairo_set_source_rgba(CR, fill.r, fill.g, fill.b, fill.a);
            cairo_rectangle(CR, 3.5 * S + EMPTY, 0, 19.0 * S - EMPTY, 14 * S);
            cairo_fill(CR);
            cairo_restore(CR);
            cairo_save(CR);
            cairo_set_operator(CR, CAIRO_OPERATOR_CLEAR);
            body();
            cairo_set_line_width(CR, 1.5 * S);
            cairo_stroke(CR);
            cairo_restore(CR);
            cairo_pop_group_to_source(CR);
            cairo_paint(CR);
        }

        const auto TXT = std::to_string(std::clamp(percent, 0, 100));
        cairo_select_font_face(CR, "Roboto", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_source_rgba(CR, fg.r, fg.g, fg.b, fg.a);
        cairo_text_extents_t te;
        if (!charging) {
            // BatteryPercentTextOnlyDrawable: 18x10 canvas at (4,2), size 10,
            // nudged up 1.5; centered on the advance, like Paint.measureText
            cairo_set_font_size(CR, 10.0 * S);
            cairo_text_extents(CR, TXT.c_str(), &te);
            cairo_move_to(CR, 4 * S + (18 * S - te.x_advance) / 2.0, 10.5 * S);
            cairo_show_text(CR, TXT.c_str());
        } else {
            // BatterySpaceSharingPercentTextDrawable: 12x10 canvas at (4,2);
            // 3 digits -> size 6 nudge 1, fewer -> size 9 nudge 1.25
            const bool   THREE = TXT.size() == 3;
            const double SIZE = (THREE ? 6.0 : 9.0) * S, NUDGE = (THREE ? 1.0 : 1.25) * S;
            cairo_set_font_size(CR, SIZE);
            cairo_text_extents(CR, TXT.c_str(), &te);
            cairo_move_to(CR, 4 * S + (12 * S - te.x_advance) / 2.0, 2 * S + (10 * S + SIZE) / 2.0 - NUDGE);
            cairo_show_text(CR, TXT.c_str());

            // battery_unified_attr_charging (16x20 viewport, 8x10 intrinsic)
            // fit-center left-aligned into the 6x6 right canvas at (16,4):
            // 4.8x6 -> a flat 0.3 scale
            static const double P[][2] = {{4, 20}, {5, 13}, {0, 13}, {9, 0}, {11, 0}, {10, 8}, {16, 8}, {6, 20}};
            const double        BS = 0.3 * S, BX = 16.0 * S, BY = 4.0 * S;
            cairo_move_to(CR, BX + P[0][0] * BS, BY + P[0][1] * BS);
            for (int i = 1; i < 8; i++)
                cairo_line_to(CR, BX + P[i][0] * BS, BY + P[i][1] * BS);
            cairo_close_path(CR);
            cairo_fill(CR);
        }

        cairo_surface_flush(SURF);
        auto tex = g_pHyprRenderer->createTexture(SURF);
        cairo_destroy(CR);
        cairo_surface_destroy(SURF);
        return tex;
    }

    // per physical height, so mixed-scale monitors don't evict each other
    struct SPill {
        SP<ITexture> tex;
        uint64_t     key = 0;
    };
    static std::unordered_map<int, SPill> pillCache;

    static bool                           inRenderBar = false; // a render is on the stack: never build textures

    static UP<SEventLoopDoLaterLock> pendingRewarm;

    // Back out to the event loop to build what a draw found missing, then
    // repaint. Deferred because we are inside the render when we notice.
    static void scheduleWarmRepaint() {
        if (!g_pEventLoopManager)
            return;
        pendingRewarm = g_pEventLoopManager->doLaterLock([]() { barChanged(); });
    }

    // ---- the paint context (hyprbar.hpp) ----

    CBox SPaint::toPhys(const CBox& global) const {
        return CBox{global}.translate(-mon->m_position).scale(scale).round();
    }

    void SPaint::rect(const CBox& global, const CHyprColor& c, int round) const {
        if (warm)
            return;
        g_pHyprOpenGL->renderRect(toPhys(global), c, {.round = round});
    }

    void SPaint::border(const CBox& global, const CHyprColor& c, int round, int sizePx) const {
        if (warm)
            return;
        g_pHyprOpenGL->renderBorder(toPhys(global), Config::CGradientValueData{c}, {.round = round, .borderSize = sizePx});
    }

    void SPaint::tex(const SP<ITexture>& t, const CBox& physBox) const {
        if (warm || !t || t->m_texID == 0)
            return;
        g_pHyprOpenGL->renderTexture(t, physBox, {});
    }

    void SPaint::texIn(const SP<ITexture>& t, const CBox& cell) const {
        if (warm || !t || t->m_texID == 0)
            return;
        const auto B = toPhys(cell);
        CBox       b{B.x + (B.w - t->m_size.x) / 2.0, B.y + (B.h - t->m_size.y) / 2.0, t->m_size.x, t->m_size.y};
        g_pHyprOpenGL->renderTexture(t, b.round(), {});
    }

    // ---- rendering ----

    // One layout, two modes. WARM builds every texture and paints nothing;
    // DRAW paints and must never build.
    //
    // A texture returned by renderText()/createTexture() cannot be painted by
    // the frame that created it — wherever in that frame it was created — and
    // the miss silently swallows everything drawn after it too. Building them
    // lazily mid-draw therefore blanked the strip from the first miss onward
    // for exactly one frame. Since itemw is part of the texture cache key, one
    // window closing re-keys every task label at once: the whole tasklist
    // vanished on every open/close ("the bar blinks when I close a window").
    // So warmBars() runs this same layout from the EVENT LOOP, a frame ahead;
    // by the time draw() runs, everything is a cache hit.
    static void renderBar(PHLMONITOR mon, bool warm) {
        if (!mon)
            return;

        auto& hits = hitboxes[mon->m_id];
        hits.clear(); // capacity retained: no per-frame allocations

        const auto WS = mon->m_activeWorkspace;
        if (WS && Fullscreen::controller()->getFullscreenModes(WS).internal == Fullscreen::FSMODE_FULLSCREEN && !(Menubar::isOpen && Menubar::mon.lock() == mon)) {
            if (Menu::isOpen && Menu::mon.lock() == mon)
                Menu::close();
            return; // real fullscreen owns the whole output, like awesome —
                    // except the open menubar: awesome's is an ontop wibox
        }

        const double SCALE = mon->m_scale;
        const auto   MB    = mon->logicalBox();
        const double H     = barHeight();
        const int    PT    = std::max(6, (int)std::round((double)cfg.fontSize->value() * SCALE));

        const SPaint P{.mon = mon, .hits = &hits, .warm = warm, .scale = SCALE, .mb = MB, .h = H, .pt = PT};

        const auto   toPhys    = [&](const CBox& b) { return P.toPhys(b); };
        const auto   drawRect  = [&](const CBox& b, const CHyprColor& c) { P.rect(b, c); };
        const auto   drawTex   = [&](const SP<ITexture>& t, const CBox& b) { P.tex(t, b); };
        const auto   drawTexIn = [&](const SP<ITexture>& t, const CBox& b) { P.texIn(t, b); };

        drawRect(CBox{MB.x, MB.y, MB.w, H}, color(cfg.colBg));

        // -- the menubar: its own strip right BELOW the bar, the bar stays
        // visible (awesome's menubar is a separate wibox at the workarea top,
        // which sits under the wibar — it never replaced it) --
        Menubar::render(P);

        // ONE walk of the window list for all its consumers: per-workspace
        // urgency + occupancy (taglist) and this workspace's tasks (tasklist,
        // in arrival order).
        static std::vector<std::pair<uint64_t, PHLWINDOW>> tasks; // reused; main thread only
        bool                                               urgentWS[10]  = {};
        int                                                wsWindows[10] = {};
        tasks.clear();
        for (const auto& W : Desktop::windowState()->windows()) {
            if (W->m_isMapped && W->m_workspace) {
                const auto ID = W->m_workspace->m_id;
                if (ID >= 1 && ID <= 9) {
                    wsWindows[ID]++;
                    if (W->m_isUrgent)
                        urgentWS[ID] = true;
                }
            }
            if (isTaskOn(W, WS)) {
                const auto [SEQ, NEW] = winSeq.try_emplace(W.get(), winSeqNext);
                if (NEW)
                    winSeqNext++;
                tasks.emplace_back(SEQ->second, W);
            }
        }
        std::sort(tasks.begin(), tasks.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

        const auto FOCUS   = Desktop::focusState() ? Desktop::focusState()->window() : nullptr;
        const auto FOCUSWS = FOCUS && FOCUS->m_workspace ? FOCUS->m_workspace->m_id : WORKSPACE_INVALID;

        // one palette fetch per frame: color() memoizes the conversion but
        // still hashes per call
        const CHyprColor COLFG = color(cfg.colFg), COLACTIVE = color(cfg.colActive), COLACTIVEBG = color(cfg.colActiveBg), COLURGENT = color(cfg.colUrgent),
                         COLURGENTBG = color(cfg.colUrgentBg), COLSQSEL = color(cfg.colSquareSel), COLSQUNSEL = color(cfg.colSquareUnsel);

        // -- taglist (awesome's exact state matrix: the viewed tag gets the
        // focus colors, an urgent one the urgent colors, and everything else —
        // occupied or empty — the plain text color; occupancy shows as the
        // little corner square instead, filled when the tag holds the focused
        // window, hollow otherwise) --
        double     x  = MB.x;
        const auto SQ = std::round(H * 4.0 / 19.0); // the 4px square of a 19px wibar, scaled
        for (int i = 1; i <= 9; i++) {
            const int  WINDOWS = wsWindows[i];
            const bool ACTIVE  = WS && WS->m_id == i;

            CHyprColor fg    = COLFG;
            CHyprColor bg    = {};
            bool       hasBg = false;
            if (ACTIVE) {
                bg    = COLACTIVEBG;
                hasBg = true;
                fg    = COLACTIVE;
            } else if (urgentWS[i]) {
                bg    = COLURGENTBG;
                hasBg = true;
                fg    = COLURGENT;
            }

            // awesome's tag button width (text + 12), label centered
            const auto   TEX = textTex(KANJI[i - 1], fg, PT);
            const double TW  = TEX ? TEX->m_size.x / SCALE : H;
            const CBox   CELL{x, MB.y, TW + 12, H};

            if (hasBg)
                drawRect(CELL, bg);

            if (WINDOWS > 0) {
                if (FOCUSWS == i)
                    drawRect(CBox{x, MB.y, SQ, SQ}, COLSQSEL);
                else { // hollow
                    drawRect(CBox{x, MB.y, SQ, 1}, COLSQUNSEL);
                    drawRect(CBox{x, MB.y + SQ - 1, SQ, 1}, COLSQUNSEL);
                    drawRect(CBox{x, MB.y, 1, SQ}, COLSQUNSEL);
                    drawRect(CBox{x + SQ - 1, MB.y, 1, SQ}, COLSQUNSEL);
                }
            }

            drawTexIn(TEX, CELL);

            SHit h;
            h.box  = CELL;
            h.kind = SHit::TAG;
            h.tag  = i;
            hits.push_back(h);
            x += CELL.w;
        }

        // -- right side, laid out from the edge inwards; awesome's order is
        // [systray][battery][clock][layoutbox], so the layoutbox sits last --
        double right = MB.x + MB.w;

        { // layoutbox: the active workspace's layout icon; click/wheel cycles
            // the registry — with its single entry it is still the static
            // floating indicator it always was
            const char* NAME = currentLayout(WS ? WS->m_id : WORKSPACE_INVALID);
            auto&       TEX  = layoutTexs[NAME];
            if (!TEX && !layoutTexTried.contains(NAME)) {
                if (!warming)
                    texStale = true; // an icon is a texture too: warm builds it
                else {
                    layoutTexTried.insert(NAME);
                    if (const char* HOME = std::getenv("HOME"))
                        TEX = loadPng(std::string{HOME} + "/.config/hypr/icons/" + NAME + ".png");
                }
            }
            const CBox CELL{right - H, MB.y, H, H};
            if (TEX && TEX->m_texID != 0) {
                // 3px inset, the bar's icon rhythm
                const double S = std::round((H - 6) * SCALE);
                const auto   P = toPhys(CELL);
                CBox         b{P.x + (P.w - S) / 2.0, P.y + (P.h - S) / 2.0, S, S};
                drawTex(TEX, b.round());
            }
            SHit h;
            h.box  = CELL;
            h.kind = SHit::LAYOUT;
            hits.push_back(h);
            right -= H;
        }

        { // clock — 6px each side, the bar's text pad
            const auto   TEX = textTex(clockText, COLFG, PT);
            const double W   = TEX ? TEX->m_size.x / SCALE + 12 : 0;
            drawTexIn(TEX, CBox{right - W, MB.y, W, H});
            right -= W;
        }

        if (batteryPercent >= 0) { // Android's unified battery: percent inside, the fill colored by state
            const int        PH   = (int)std::round((H - 6) * SCALE); // the bar's 3px-inset icon rhythm
            // AOSP's ladder: charging -> active green, <= 20 discharging ->
            // error red, else the dark-theme fill (GM Gray 700)
            const CHyprColor FILL = batteryCharging          ? color(cfg.colCharging) :
                batteryPercent <= 20                         ? color(cfg.colLow) :
                                                               CHyprColor{0xff5f6368};
            const uint64_t   KEY  = ((uint64_t)batteryPercent << 40) ^ (batteryCharging ? 1ull << 47 : 0) ^ (COLFG.getAsHex() * 0x9E3779B97F4A7C15ULL) ^ FILL.getAsHex();
            auto&            PILL = pillCache[PH];
            if (warm) {
                if (!PILL.tex || PILL.key != KEY) {
                    PILL.tex = batteryPill(batteryPercent, batteryCharging, PH, COLFG, FILL);
                    PILL.key = KEY;
                }
            } else if (!PILL.tex || PILL.key != KEY)
                texStale = true; // level moved under a scissored repaint: warm + repaint

            const double PW = PILL.tex ? PILL.tex->m_size.x / SCALE : (H - 6) * 24.0 / 14.0;
            const double W  = 6 + PW + 6; // breathing room off the tray
            drawTexIn(PILL.tex, CBox{right - W + 6, MB.y, PW, H});
            right -= W;
        }

        // -- tray icons, spaced like awesome's systray_icon_spacing --
        bool firstTray = true;
        for (const auto& IT : Tray::items) {
            if (IT->status == "Passive")
                continue; // SNI: Passive means don't show the item
            if (!firstTray)
                right -= (double)cfg.traySpacing->value();
            firstTray = false;
            // The pixmap is a texture too, so the rule applies: rebuild it on
            // the warm only. A dirty item reaching a draw keeps its old icon
            // for this frame and asks for a repaint.
            if (IT->dirty) {
                if (!warming)
                    texStale = true;
                else {
                    IT->dirty = false;
                    IT->tex.reset();
                    if (!IT->pixels.empty())
                        IT->tex = g_pHyprRenderer->createTexture(DRM_FORMAT_ARGB8888, IT->pixels.data(), IT->pw * 4, Vector2D{(double)IT->pw, (double)IT->ph});
                    if ((!IT->tex || IT->tex->m_texID == 0) && !IT->iconName.empty())
                        IT->tex = trayIcon(IT->iconName, IT->themePath);
                }
            }

            const CBox CELL{right - H, MB.y, H, H};
            if (IT->tex && IT->tex->m_texID != 0) {
                // 3px inset: SNI pixmaps lack the internal padding XEmbed
                // icons carried, full-bleed reads as cramped
                const double S = std::round((H - 6) * SCALE);
                const auto   P = toPhys(CELL);
                CBox         b{P.x + (P.w - S) / 2.0, P.y + (P.h - S) / 2.0, S, S};
                drawTex(IT->tex, b.round());
            } else
                drawTexIn(textTex(letterOf(IT->iconName), color(cfg.colMuted), PT), CELL);

            SHit h;
            h.box     = CELL;
            h.kind    = SHit::TRAY;
            h.tray    = IT;
            h.anchorX = CELL.x + H / 2.0;
            h.mon     = mon;
            hits.push_back(h);
            right -= H;
        }

        // -- tasklist: the active workspace's windows, in arrival order
        // (collected in the single window walk above) --
        size_t taskFp = 0;
        {
            const double avail = right - 8 - x;
            if (!tasks.empty() && avail >= 40) {
                // awesome tasklist behavior: the windows split the WHOLE free
                // strip between taglist and tray — one window = one huge item
                const double ITEMW = avail / (double)tasks.size();

                for (const auto& [SEQ, W] : tasks) {
                    const CBox CELL{x, MB.y, ITEMW, H};
                    // the old theme: the focused task is cyan TEXT on the plain
                    // bar (tasklist_bg_focus = bg_normal, no box); urgent gets
                    // the urgent bg — and focus wins over urgent, like awesome
                    CHyprColor fg = COLFG;
                    if (W == FOCUS)
                        fg = COLACTIVE;
                    else if (W->m_isUrgent) {
                        drawRect(CELL, COLURGENTBG);
                        fg = COLURGENT;
                    }

                    // [4][icon][4][title] — awesome's item margins, icon on
                    // the bar's 3px-inset rhythm
                    const double ICON = H - 6;
                    double       tx   = x + 4;
                    if (const auto ITEX = appIcon(W->m_class); ITEX && ITEX->m_texID != 0) {
                        const auto P = toPhys(CBox{tx, MB.y + 3, ICON, ICON});
                        drawTex(ITEX, P);
                    } else
                        drawTexIn(textTex(letterOf(W->m_class), COLACTIVE, PT), CBox{tx, MB.y, ICON, H});
                    tx += ICON + 4;

                    static std::string LBL; // reused; main thread only
                    taskLabel(W, LBL);
                    taskFp         = taskFp * 1099511628211ULL + std::hash<std::string>{}(LBL);
                    const auto TEX = textTex(LBL, fg, PT, (int)std::round((ITEMW - (tx - x) - 4) * SCALE));
                    if (TEX && TEX->m_texID != 0) {
                        const auto P = toPhys(CBox{tx, MB.y, 1, H});
                        CBox       b{P.x, P.y + (P.h - TEX->m_size.y) / 2.0, TEX->m_size.x, TEX->m_size.y};
                        drawTex(TEX, b.round());
                    }

                    SHit h;
                    h.box    = CELL;
                    h.kind   = SHit::TASK;
                    h.window = W;
                    hits.push_back(h);
                    x += ITEMW;
                }
            }
        }
        // A label can flip without any bar event or strip damage (pin damages
        // only the window; the xdg maximized bit has no event at all): with
        // every variant still cached the next draw is correct but scissored
        // to the window's damage, so the strip on screen keeps the old text.
        // A warm stamps what the paint it precedes will show; a draw that
        // lays out anything else may have been clipped — repaint the strip.
        auto& FP = lastTaskFp[mon->m_id];
        if (warm)
            FP = taskFp;
        else if (FP != taskFp) {
            FP       = taskFp;
            texStale = true;
        }

        tasks.clear(); // don't keep strong window refs across frames

        // -- the open menu, panel by panel: the client list is fixed at 250
        // wide (the old rc's client_list width); dbusmenu levels size
        // themselves, and submenus cascade out beside their parent — like
        // the GTK menus these were under X11 --
        Menu::render(P);
    }

    // ---- pass element ----

    class CBarPassElement : public IPassElement {
      public:
        CBarPassElement(PHLMONITOR mon) : m_mon(mon) {}
        virtual ~CBarPassElement() = default;

        virtual std::vector<UP<IPassElement>> draw() override {
            inRenderBar = true;
            renderBar(m_mon.lock(), false);
            inRenderBar = false;

            // Something changed without warming first (a texture the warm never
            // enumerated). One label is missing for one frame; build it and
            // repaint. Never build here — that is the bug this all guards.
            if (texStale)
                scheduleWarmRepaint();

            return {};
        }
        virtual bool needsLiveBlur() override {
            return false;
        }
        virtual bool needsPrecomputeBlur() override {
            return false;
        }
        virtual std::optional<CBox> boundingBox() override {
            const auto MON = m_mon.lock();
            if (!MON)
                return std::nullopt;
            double h = barHeight();
            if (Menubar::isOpen && Menubar::mon.lock() == MON)
                h += barHeight(); // the prompt strip below the bar
            if (Menu::isOpen && Menu::mon.lock() == MON)
                h = MON->logicalBox().h; // cascades anchor anywhere below the bar — cover it all, an undersized box clips
            // monitor-local LOGICAL px — the pass scales by m_scale itself
            // (stock elements divide their physical boxes back down; see
            // CRenderPass::simplify)
            return CBox{0, 0, MON->logicalBox().w, h};
        }
        virtual const char* passName() override {
            return "CBarPassElement";
        }
        virtual ePassElementType type() override {
            return EK_CUSTOM;
        }

      private:
        PHLMONITORREF m_mon;
    };

    // Build every texture the NEXT frame will paint. Must run outside the
    // render cycle (see renderBar): a texture built during a frame cannot be
    // painted by that same frame, wherever in it it was built. Steady state is
    // all cache hits — a layout walk and nothing else.
    //
    // No-ops inside a render so callers never have to check: Menu::close()
    // damages, and renderBar itself can close the menu when a window goes
    // fullscreen.
    void warmBars(PHLMONITOR only) {
        if (warming || inRenderBar || !g_pCompositor)
            return;

        warming = true;
        if (!only)
            texGen++;

        for (const auto& M : State::monitorState()->monitors())
            if (!only || M == only)
                renderBar(M, true);

        // Bound the cache against title churn, but only drop what no warm has
        // wanted for a while — see SCachedTex. (The old size-capped full flush
        // is survivable now that a draw can never build, but it still meant one
        // warm rebuilding everything at once.) A scoped warm never enumerates
        // every monitor's textures, so it must not age or evict.
        if (!only && texGen > TEX_CACHE_LIFE)
            std::erase_if(texCache, [](const auto& E) { return E.second.gen + TEX_CACHE_LIFE < texGen; });

        warming  = false;
        texStale = false;
    }

    void onRenderStage(eRenderStage stage) {
        if (stage != RENDER_POST_WINDOWS)
            return;
        const auto MON = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!MON)
            return;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CBarPassElement>(MON));
    }

    void renderExit() {
        pendingRewarm.reset(); // its barChanged would touch the caches cleared below
        texCache.clear();
        lastTaskFp.clear();
        layoutTexs.clear();
        layoutTexTried.clear();
        wsLayout.clear();
        pillCache.clear();
    }

} // namespace NHyprbar
