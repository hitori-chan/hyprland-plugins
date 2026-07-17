// hyprbar/render.cpp — the bar itself, the text cache, the paint context, the pass element

#include "hyprbar.hpp"

namespace NHyprbar {

    static const char*                         KANJI[9] = {"一", "二", "三", "四", "五", "六", "七", "八", "九"};

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
    static uint64_t                          texGen        = 0;
    static constexpr uint64_t                TEX_CACHE_LIFE = 32; // warms an unused texture survives

    // Built ONLY by the warm pass — see the texture rule in hyprbar.hpp. A miss
    // during a draw returns null (that one label is missing for one frame)
    // rather than building, which would paint nothing anyway AND swallow every
    // later draw in the element.
    SP<ITexture> textTex(const std::string& text, const CHyprColor& col, int pt, int maxWidth, const std::string& font) {
        static std::string KEY; // reused; main thread only
        KEY.clear();
        KEY += text;
        KEY += '|';
        KEY += std::to_string(col.getAsHex());
        KEY += '|';
        KEY += std::to_string(pt);
        KEY += '|';
        KEY += std::to_string(maxWidth);
        KEY += '|';
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

    static SP<ITexture>              layoutTex;
    static bool                      layoutTexTried = false;
    static bool                      inRenderBar    = false; // a render is on the stack: never build textures

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

    void SPaint::rect(const CBox& global, const CHyprColor& c) const {
        if (warm)
            return;
        g_pHyprOpenGL->renderRect(toPhys(global), c, {});
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
        bool                                              urgentWS[10]  = {};
        int                                               wsWindows[10] = {};
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

        { // layoutbox: the old theme's floating icon (the only layout — a pure
            // indicator, no click action, exactly like the old bar effectively was)
            if (!layoutTexTried) {
                layoutTexTried = true;
                if (const char* HOME = std::getenv("HOME"))
                    layoutTex = loadPng(std::string{HOME} + "/.config/hypr/icons/floating.png");
            }
            if (layoutTex && layoutTex->m_texID != 0) {
                // 3px inset, the bar's icon rhythm
                const CBox   CELL{right - H, MB.y, H, H};
                const double S = std::round((H - 6) * SCALE);
                const auto   P = toPhys(CELL);
                CBox         b{P.x + (P.w - S) / 2.0, P.y + (P.h - S) / 2.0, S, S};
                drawTex(layoutTex, b.round());
                right -= H;
            }
        }

        { // clock — 6px each side, the bar's text pad
            const auto   TEX = textTex(clockText, COLFG, PT);
            const double W   = TEX ? TEX->m_size.x / SCALE + 12 : 0;
            drawTexIn(TEX, CBox{right - W, MB.y, W, H});
            right -= W;
        }

        if (!batteryText.empty()) {                                           // Material glyph + percent, the old widget's look:
            const int    GPT = std::max(6, (int)std::round(PT * 15.0 / 9.0)); // font_icon 15 vs font 9,
            const auto   GT  = textTex(batteryGlyphText, COLFG, GPT, 0, cfg.fontIcon->value());
            const auto   VT  = textTex(batteryText, COLFG, PT);
            const double GW  = GT ? GT->m_size.x / SCALE : 0;
            const double VW  = VT ? VT->m_size.x / SCALE : 0;
            const double W   = 6 + GW + 2 + VW + 6; // breathing room off the tray
            drawTexIn(GT, CBox{right - W + 6, MB.y, GW, H});
            drawTexIn(VT, CBox{right - W + 6 + GW + 2, MB.y, VW, H});
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
            } else {
                std::string L = IT->iconName.empty() ? "?" : IT->iconName.substr(0, 1);
                L[0]          = std::toupper((unsigned char)L[0]);
                drawTexIn(textTex(L, color(cfg.colMuted), PT), CELL);
            }

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
                    } else {
                        std::string L = W->m_class.empty() ? "?" : W->m_class.substr(0, 1);
                        L[0]          = std::toupper((unsigned char)L[0]);
                        drawTexIn(textTex(L, COLACTIVE, PT), CBox{tx, MB.y, ICON, H});
                    }
                    tx += ICON + 4;

                    const auto TEX = textTex(taskLabel(W), fg, PT, (int)std::round((ITEMW - (tx - x) - 4) * SCALE));
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
        texCache.clear();
        layoutTex.reset();
        layoutTexTried = false;
    }

} // namespace NHyprbar
