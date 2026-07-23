// hyprbar/render.cpp — the bar's skeleton: the compact islands, the text
// cache, the paint context, the pass element and the widget slots (each
// widget lives in its own unit — see the module map in hyprbar.hpp).
//
// The ONE bar state (decided): a 30px transparent band holding 26px glass
// pills — the taglist island left, the task chips filling the middle, the
// status island right (layout chip · tray · bell · wifi · battery · time).
// Identical maximized or not; reserved-top is always the band.

#include "common/lifecycle.hpp"
#include "common/queries.hpp"
#include "common/theme.hpp"

#include "hyprbar.hpp"

#include <hyprland/src/config/ConfigValue.hpp>

namespace NHyprbar {

    SBarHover barHover;

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

    // per monitor: a fingerprint of the labels the strip shows — see
    // the tasklist in renderBar
    static std::unordered_map<uint64_t, size_t> lastTaskFp;

    bool                                        barBlurOn() {
        static auto V = CConfigValue<Config::INTEGER>("decoration:blur:enabled");
        return *V != 0;
    }
    double barBlurRadius() {
        if (!barBlurOn())
            return 0;
        static auto SIZE   = CConfigValue<Config::INTEGER>("decoration:blur:size");
        static auto PASSES = CConfigValue<Config::INTEGER>("decoration:blur:passes");
        return (double)*SIZE * (1 << std::clamp((int)*PASSES, 1, 6));
    }

    // Built ONLY by the warm pass — see the texture rule in hyprbar.hpp. A miss
    // during a draw returns null (that one label is missing for one frame)
    // rather than building, which would paint nothing anyway AND swallow every
    // later draw in the element.
    SP<ITexture> textTex(const std::string& text, const CHyprColor& col, int pt, int maxWidth, const std::string& font, int weight) {
        // key on the RESOLVED font: every caller passes "", and keying the
        // empty string made old-font textures permanent hits across a
        // plugin:hyprbar:font change
        const std::string& F = font.empty() ? cfg.font->value() : font;

        char               meta[56]; // the non-text key parts in one stack write — no to_string churn per call
        const int          METALEN = std::snprintf(meta, sizeof(meta), "|%llx|%d|%d|%d|", (unsigned long long)col.getAsHex(), pt, maxWidth, weight);

        static std::string KEY; // reused; main thread only
        KEY.clear();
        KEY += text;
        KEY.append(meta, METALEN > 0 ? (size_t)METALEN : 0);
        KEY += F;

        if (const auto IT = texCache.find(KEY); IT != texCache.end()) {
            IT->second.gen = texGen;
            return IT->second.tex;
        }

        if (!warmGate.mayBuild())
            return nullptr;

        auto tex      = g_pHyprRenderer->renderText(text, col, pt, false, F, maxWidth, weight);
        texCache[KEY] = {tex, texGen};
        return tex;
    }

    // ---- the paint context (hyprbar.hpp) ----

    CBox SPaint::toPhys(const CBox& global) const {
        return CBox{global}.translate(-mon->m_position).scale(scale).round();
    }

    void SPaint::rect(const CBox& global, const CHyprColor& c, int round) const {
        if (warm)
            return;
        g_pHyprOpenGL->renderRect(toPhys(global), c, {.round = round, .roundingPower = (float)cfg.roundingPower->value()});
    }

    void SPaint::glass(const CBox& global, int round) const {
        if (warm)
            return;
        const float RP = (float)cfg.roundingPower->value();
        static Config::CGradientValueData SHADOW{CHyprColor{NHyprCommon::Theme::SHADOW}};
        g_pHyprOpenGL->renderRoundedShadow(toPhys(global), round, RP, (int)std::lround(10 * scale), SHADOW, 1.f);
        g_pHyprOpenGL->renderRect(toPhys(global), color(cfg.colBg), {.round = round, .roundingPower = RP, .blur = barBlurOn()});
    }

    void SPaint::border(const CBox& global, const CHyprColor& c, int round, int sizePx) const {
        if (warm)
            return;
        g_pHyprOpenGL->renderBorder(toPhys(global), Config::CGradientValueData{c}, {.round = round, .roundingPower = (float)cfg.roundingPower->value(), .borderSize = sizePx});
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
    // DRAW paints and must never build (the texture rule — see hyprbar.hpp).
    static void renderBar(PHLMONITOR mon, bool warm) {
        if (!mon)
            return;

        auto& hits = hitboxes[mon->m_id];
        hits.clear(); // capacity retained: no per-frame allocations

        // the islands stay visible while locked (clock/battery/tray), but an
        // open tray menu must not float over the lockscreen — close it here,
        // like the fullscreen path below, so it's gone on unlock
        if (NHyprCommon::sessionLocked() && Menu::isOpen && Menu::mon.lock() == mon)
            Menu::close();

        const auto WS = mon->m_activeWorkspace;
        if (WS && Fullscreen::controller()->getFullscreenModes(WS).internal == Fullscreen::FSMODE_FULLSCREEN && !(Menubar::isOpen && Menubar::mon.lock() == mon)) {
            if (Menu::isOpen && Menu::mon.lock() == mon)
                Menu::close();
            return; // real fullscreen owns the whole output — except the open
                    // menubar, which floats above it
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

        // -- the menubar: its own floating pill below the bar --
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
                    if (W->m_isUrgent && !Taglist::seen(W.get())) // viewing cleared it
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
        F.fg        = color(cfg.colFg);
        F.active    = color(cfg.colActive);
        F.activeBg  = color(cfg.colActiveBg);
        F.urgentFg  = color(cfg.colUrgent);
        F.urgentBg  = color(cfg.colUrgentBg);
        F.minimized = color(cfg.colEmpty);

        // -- the compact islands: 26px pills on the 30px band --
        const double IH    = H - 4;
        const double IY    = MB.y + 2;
        const int    RPILL = (int)std::lround(IH / 2 * SCALE);

        // LEFT: the taglist island
        const double TAGW = taglistWidget().fit(P, F);
        double       leftEnd = MB.x + 6;
        if (TAGW > 0) {
            const CBox ISL{MB.x + 6, IY, TAGW, IH};
            P.glass(ISL, RPILL);
            taglistWidget().draw(P, F, ISL);
            leftEnd = ISL.x + ISL.w;
        }

        // RIGHT: the status island — layout chip · tray · bell · wifi ·
        // battery · time, gap 7, no separators
        IWidget* const STATUS[] = {&kbdWidget(), &trayWidget(), &bellWidget(), &wifiWidget(), &batteryWidget(), &clockWidget()};
        constexpr double SGAP = 7, SPAD = 10;
        double           sw[std::size(STATUS)];
        double           total = 0;
        int              shown = 0;
        for (size_t i = 0; i < std::size(STATUS); i++) {
            sw[i] = STATUS[i]->fit(P, F);
            if (sw[i] > 0) {
                total += sw[i];
                shown++;
            }
        }
        double rightStart = MB.x + MB.w - 6;
        if (shown > 0) {
            const double ISLW = total + (shown - 1) * SGAP + 2 * SPAD;
            const CBox   ISL{MB.x + MB.w - 6 - ISLW, IY, ISLW, IH};
            P.glass(ISL, RPILL);
            double x = ISL.x + SPAD;
            for (size_t i = 0; i < std::size(STATUS); i++) {
                if (sw[i] <= 0)
                    continue;
                STATUS[i]->draw(P, F, CBox{x, IY, sw[i], IH});
                x += sw[i] + SGAP;
            }
            rightStart = ISL.x;
        }

        // MIDDLE: the task chips fill what's left
        tasklistWidget().draw(P, F, CBox{leftEnd + 6, IY, rightStart - 6 - (leftEnd + 6), IH});

        auto& FP = lastTaskFp[mon->m_id];
        if (warm)
            FP = frameFp;
        else if (FP != frameFp) {
            FP                = frameFp;
            warmGate.texStale = true;
        }

        tasks.clear(); // don't keep strong window refs across frames

        // -- the open tray menu, panel by panel --
        Menu::render(P);
    }

    // ---- pass element ----

    class CBarPassElement : public IPassElement {
      public:
        CBarPassElement(PHLMONITOR mon) : m_mon(mon) {}
        virtual ~CBarPassElement() = default;

        virtual std::vector<UP<IPassElement>> draw() override {
            warmGate.inRender = true;
            renderBar(m_mon.lock(), false);
            warmGate.inRender = false;

            // Something changed without warming first (a texture the warm never
            // enumerated). One label is missing for one frame; build it and
            // repaint. Never build here — that is the bug this all guards.
            warmGate.rewarmIfStale([]() { barChanged(); });

            return {};
        }
        virtual bool needsLiveBlur() override {
            return barBlurOn(); // the islands' glass samples what's beneath
        }
        virtual bool needsPrecomputeBlur() override {
            return false;
        }
        virtual std::optional<CBox> boundingBox() override {
            const auto MON = m_mon.lock();
            if (!MON)
                return std::nullopt;
            const double PAD = barBlurRadius() + 12; // island shadows reach below the band
            double       h   = barHeight() + PAD;
            if (Menubar::isOpen && Menubar::mon.lock() == MON)
                h = barHeight() + 4 + barHeight() + PAD; // the floating strip below the bar
            if (Menu::isOpen && Menu::mon.lock() == MON)
                h = MON->logicalBox().h; // cascades anchor anywhere below the bar — cover it all
            // monitor-local LOGICAL px — the pass scales by m_scale itself
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
        if (!warmGate.beginWarm())
            return;

        if (!only)
            texGen++;

        for (const auto& M : State::monitorState()->monitors())
            if (!only || M == only)
                renderBar(M, true);

        // Bound the cache against title churn, but only drop what no warm has
        // wanted for a while — see SCachedTex. A scoped warm never enumerates
        // every monitor's textures, so it must not age or evict.
        if (!only && texGen > TEX_CACHE_LIFE)
            std::erase_if(texCache, [](const auto& E) { return E.second.gen + TEX_CACHE_LIFE < texGen; });

        warmGate.endWarm();
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
        // the gate's rewarm hop is already down: PLUGIN_EXIT's resetAll runs
        // before this, and its barChanged would touch the caches cleared below
        texCache.clear();
        lastTaskFp.clear();
        barHover = {};
    }

} // namespace NHyprbar
