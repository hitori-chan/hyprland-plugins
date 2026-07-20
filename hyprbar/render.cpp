// hyprbar/render.cpp — the bar's skeleton: the strip, the text cache, the
// paint context, the pass element and the widget slots (each widget lives in
// its own unit — see the module map in hyprbar.hpp)

#include "hyprbar.hpp"

namespace NHyprbar {

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
        // key on the RESOLVED font: every caller passes "", and keying the
        // empty string made old-font textures permanent hits across a
        // plugin:hyprbar:font change
        const std::string& F = font.empty() ? cfg.font->value() : font;

        char               meta[48]; // the non-text key parts in one stack write — no to_string churn per call
        const int          METALEN = std::snprintf(meta, sizeof(meta), "|%llx|%d|%d|", (unsigned long long)col.getAsHex(), pt, maxWidth);

        static std::string KEY; // reused; main thread only
        KEY.clear();
        KEY += text;
        KEY.append(meta, METALEN > 0 ? (size_t)METALEN : 0);
        KEY += F;

        if (const auto IT = texCache.find(KEY); IT != texCache.end()) {
            IT->second.gen = texGen;
            return IT->second.tex;
        }

        if (!warming) {
            texStale = true;
            return nullptr;
        }

        auto tex      = g_pHyprRenderer->renderText(text, col, pt, false, F, maxWidth);
        texCache[KEY] = {tex, texGen};
        return tex;
    }

    static bool                      inRenderBar = false; // a render is on the stack: never build textures

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

        // the strip stays visible while locked (clock/battery/tray), but an
        // open tray menu must not float over the lockscreen — close it here,
        // like the fullscreen path below, so it's gone on unlock
        if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked() && Menu::isOpen && Menu::mon.lock() == mon)
            Menu::close();

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

        // A tasklist label can flip without any bar event or strip damage (pin
        // damages only the window; the xdg maximized bit has no event at all):
        // with every variant still cached the next draw is correct but
        // scissored to the window's damage, so the strip on screen keeps the
        // old text. Widgets fold such content into this fingerprint; a warm
        // stamps what the paint it precedes will show, and a draw that lays
        // out anything else may have been clipped — repaint the strip.
        size_t       frameFp = 0;

        const SPaint P{.mon = mon, .hits = &hits, .warm = warm, .scale = SCALE, .mb = MB, .h = H, .pt = PT, .fp = &frameFp};

        P.rect(CBox{MB.x, MB.y, MB.w, H}, color(cfg.colBg));

        // -- the menubar: its own strip right BELOW the bar, the bar stays
        // visible (awesome's menubar is a separate wibox at the workarea top,
        // which sits under the wibar — it never replaced it) --
        Menubar::render(P);

        // ONE walk of the window list for all its consumers: per-workspace
        // urgency + occupancy (taglist) and this workspace's tasks (tasklist,
        // in arrival order).
        static std::vector<std::pair<uint64_t, PHLWINDOW>> tasks; // reused; main thread only
        SFrame                                             F;
        F.ws = WS;
        tasks.clear();
        for (const auto& W : Desktop::windowState()->windows()) {
            if (W->m_isMapped && W->m_workspace) {
                const auto ID = W->m_workspace->m_id;
                if (ID >= 1 && ID <= 9) {
                    F.windows[ID]++;
                    if (W->m_isUrgent)
                        F.urgent[ID] = true;
                }
            }
            if (isTaskOn(W, WS))
                tasks.emplace_back(Tasklist::seqOf(W.get()), W);
        }
        std::sort(tasks.begin(), tasks.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        F.tasks   = &tasks;
        F.focus   = Desktop::focusState() ? Desktop::focusState()->window() : nullptr;
        F.focusWs = F.focus && F.focus->m_workspace ? F.focus->m_workspace->m_id : WORKSPACE_INVALID;

        // one palette fetch per frame: color() memoizes the conversion but
        // still hashes per call, and the widgets make dozens
        F.fg          = color(cfg.colFg);
        F.active      = color(cfg.colActive);
        F.activeBg    = color(cfg.colActiveBg);
        F.urgentFg    = color(cfg.colUrgent);
        F.urgentBg    = color(cfg.colUrgentBg);
        F.squareSel   = color(cfg.colSquareSel);
        F.squareUnsel = color(cfg.colSquareUnsel);
        F.minimized   = color(cfg.colEmpty); // awesome's tasklist_fg_minimize: muted

        // awesome's align layout: the left slot, the tasklist filling the
        // middle, the right slot laid from the edge inwards (awesome's order
        // is [systray][battery][clock][layoutbox], so the layoutbox sits
        // last). Widgets whose fit comes back 0 are hidden this frame.
        IWidget* const LEFT[]  = {&taglistWidget()};
        IWidget* const RIGHT[] = {&trayWidget(), &batteryWidget(), &clockWidget(), &layoutboxWidget()};

        double         x = MB.x;
        for (auto* const W : LEFT) {
            const double WD = W->fit(P, F);
            if (WD > 0)
                W->draw(P, F, CBox{x, MB.y, WD, H});
            x += WD;
        }

        double right = MB.x + MB.w;
        for (size_t i = std::size(RIGHT); i-- > 0;) {
            const double WD = RIGHT[i]->fit(P, F);
            if (WD > 0) {
                right -= WD;
                RIGHT[i]->draw(P, F, CBox{right, MB.y, WD, H});
            }
        }

        // the tasklist splits the whole leftover strip, 8px off the right slot
        tasklistWidget().draw(P, F, CBox{x, MB.y, right - 8 - x, H});

        auto& FP = lastTaskFp[mon->m_id];
        if (warm)
            FP = frameFp;
        else if (FP != frameFp) {
            FP       = frameFp;
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
    }

} // namespace NHyprbar
