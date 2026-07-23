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

#include <random>

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
        g_pHyprOpenGL->renderRect(toPhys(global), color(cfg.colBg), {.round = round, .roundingPower = RP, .blur = blurOn()});
    }

    // the strip material: a square frosted band — flat color, live blur, the
    // soft under-shadow for depth. No hairlines, no gradients (decided).
    void SPaint::band(const CBox& global, const CHyprColor& c) const {
        if (warm)
            return;
        static Config::CGradientValueData SHADOW{CHyprColor{NHyprCommon::Theme::SHADOW}};
        g_pHyprOpenGL->renderRoundedShadow(toPhys(global), 0, (float)cfg.roundingPower->value(), (int)std::lround(10 * scale), SHADOW, 1.f);
        g_pHyprOpenGL->renderRect(toPhys(global), c, {.round = 0, .blur = blurOn()});
    }

    // ---- the strip's grain (one tile per monitor, band-row sized) ----

    struct SGrainTex {
        SP<ITexture> tex;
        int          w = 0, h = 0;
    };
    static std::unordered_map<uint64_t, SGrainTex> grainTex;

    // Built by the warm pass (the texture rule). Fixed seed: the tile is a
    // material constant, not per-frame noise — static grain, no shimmer.
    static void ensureGrain(PHLMONITOR mon) {
        const int W = std::max(1, (int)std::lround(mon->logicalBox().w * mon->m_scale));
        const int H = std::max(1, (int)std::lround(barHeight() * mon->m_scale));
        auto&     G = grainTex[mon->m_id];
        if (G.tex && G.w == W && G.h == H)
            return;
        if (!warmGate.mayBuild())
            return;
        auto* SURF = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
        if (cairo_surface_status(SURF) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(SURF);
            return;
        }
        auto*        D      = cairo_image_surface_get_data(SURF);
        const int    STRIDE = cairo_image_surface_get_stride(SURF);
        std::minstd_rand rng{0x9e0f1218u};
        for (int y = 0; y < H; y++) {
            auto* row = D + (size_t)y * STRIDE;
            for (int x = 0; x < W; x++) {
                // premultiplied white at alpha 0..9/255 — ~1.5% mean grain
                const uint8_t A = (uint8_t)(rng() % 10);
                row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = row[x * 4 + 3] = A;
            }
        }
        cairo_surface_mark_dirty(SURF);
        G.tex = g_pHyprRenderer->createTexture(SURF);
        G.w   = W;
        G.h   = H;
        cairo_surface_destroy(SURF);
    }

    void stripGrain(const SPaint& P, const CBox& row) {
        if (P.warm)
            return;
        const auto IT = grainTex.find(P.mon->m_id);
        if (IT == grainTex.end() || !IT->second.tex || IT->second.tex->m_texID == 0)
            return; // first frame after a resize: the band is grainless once
        P.tex(IT->second.tex, P.toPhys(row));
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

        // -- the band: 26px pills on a transparent 30px band (islands), or
        // one full-bleed frosted strip whose cells run the full height and
        // reach y=0 and both corners (strip — the letterbox/Fitts mode) --
        const bool   STRIP = stripMode();
        const double IH    = STRIP ? H : H - 4;
        const double IY    = STRIP ? MB.y : MB.y + 2;
        const int    RPILL = STRIP ? 0 : (int)std::lround(IH / 2 * SCALE);

        if (STRIP) {
            if (warm)
                ensureGrain(mon);
            const CBox BAND{MB.x, MB.y, MB.w, H};
            auto       BC = color(cfg.colBg); // col_bg's RGB at the strip's own alpha
            BC.a          = std::clamp((float)cfg.barAlpha->value(), 0.f, 1.f);
            P.band(BAND, BC);
            stripGrain(P, BAND);
        }

        // LEFT: the taglist — flush to the corner in strip (the top-left
        // throw lands on 一), a glass island otherwise
        const double TAGW = taglistWidget().fit(P, F);
        double       leftEnd = MB.x + (STRIP ? 0 : 6);
        if (TAGW > 0) {
            const CBox ISL{leftEnd, IY, TAGW, IH};
            if (!STRIP)
                P.glass(ISL, RPILL);
            taglistWidget().draw(P, F, ISL);
            leftEnd = ISL.x + ISL.w;
        }

        // RIGHT: the status cluster — layout chip · tray · bell · battery ·
        // time, gap 7, no separators (wifi lives in the tray: nm-applet's
        // own SNI icon carries the strength — a second wedge said it twice)
        IWidget* const STATUS[] = {&kbdWidget(), &trayWidget(), &bellWidget(), &batteryWidget(), &clockWidget()};
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
        double rightStart = MB.x + MB.w - (STRIP ? 0 : 6);
        if (shown > 0) {
            // strip: cells ride the band itself — text keeps an 8px edge
            // clearance, the band swallows the corner pixels regardless
            const double PADL = STRIP ? 0 : SPAD, PADR = STRIP ? 8 : SPAD;
            const double ISLW = total + (shown - 1) * SGAP + PADL + PADR;
            const CBox   ISL{MB.x + MB.w - (STRIP ? 0 : 6) - ISLW, IY, ISLW, IH};
            if (!STRIP)
                P.glass(ISL, RPILL);
            double x = ISL.x + PADL;
            for (size_t i = 0; i < std::size(STATUS); i++) {
                if (sw[i] <= 0)
                    continue;
                STATUS[i]->draw(P, F, CBox{x, IY, sw[i], IH});
                x += sw[i] + SGAP;
            }
            rightStart = ISL.x;
        }

        // MIDDLE: the task chips fill what's left
        const double MIDGAP = STRIP ? 8 : 6;
        tasklistWidget().draw(P, F, CBox{leftEnd + MIDGAP, IY, rightStart - MIDGAP - (leftEnd + MIDGAP), IH});

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
            return blurOn(); // the islands' glass samples what's beneath
        }
        virtual bool needsPrecomputeBlur() override {
            return false;
        }
        virtual std::optional<CBox> boundingBox() override {
            const auto MON = m_mon.lock();
            if (!MON)
                return std::nullopt;
            const double PAD = blurRadius() + 12; // island/band shadows reach below the band
            double       h   = barHeight() + PAD;
            if (Menubar::isOpen && Menubar::mon.lock() == MON)
                h = barHeight() * 2 + (stripMode() ? 0 : 4) + PAD; // the open menubar below the bar (docked in strip)
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
        // real fullscreen hides the band (renderBar early-outs): adding the
        // element anyway claimed a live-blur region every frame for nothing.
        // The open menubar is the one surface that floats above fullscreen.
        const auto WS = MON->m_activeWorkspace;
        if (WS && Fullscreen::controller()->getFullscreenModes(WS).internal == Fullscreen::FSMODE_FULLSCREEN && !(Menubar::isOpen && Menubar::mon.lock() == MON) &&
            !(Menu::isOpen && Menu::mon.lock() == MON))
            return;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CBarPassElement>(MON));
    }

    void renderExit() {
        // the gate's rewarm hop is already down: PLUGIN_EXIT's resetAll runs
        // before this, and its barChanged would touch the caches cleared below
        texCache.clear();
        lastTaskFp.clear();
        grainTex.clear();
        barHover = {};
    }

} // namespace NHyprbar
