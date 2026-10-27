// awesome/shell/strip.cpp — the bar's skeleton: the strip, the text cache,
// the paint context, the canvas layer and the widget slots (each widget
// lives in its own unit — see the module map in shell/shell.hpp)

#include "shell/shell.hpp"

#include "../windows/state.hpp"
#include "core/text.hpp"
#include "core/theme.hpp"

#include <cmath>
#include <limits>

namespace NAwesome::Shell {

    // Each entry remembers the warm generation that last wanted it. Evicting
    // whatever the CURRENT layout doesn't name would thrash: sloppy focus
    // re-keys two task labels (colFg <-> colActive) for every window the cursor
    // crosses, so the variant it just left would be rebuilt on the way back —
    // a pango render + upload each time. A grace window keeps those hot and
    // still bounds the map against title churn.
    static size_t textureBytes(const SP<ITexture>& texture) {
        if (!texture || !std::isfinite(texture->m_size.x) || !std::isfinite(texture->m_size.y) || texture->m_size.x <= 0 || texture->m_size.y <= 0)
            return 0;
        const long double BYTES = (long double)texture->m_size.x * (long double)texture->m_size.y * 4.0L;
        return BYTES >= (long double)std::numeric_limits<size_t>::max() ? std::numeric_limits<size_t>::max() : (size_t)BYTES;
    }

    static NAwesome::CGenCache<SP<ITexture>> texCache{64ull << 20, textureBytes};

    // per monitor: a fingerprint of the task labels the strip shows — see
    // the tasklist in renderBar
    static std::unordered_map<uint64_t, size_t> lastTaskFp;

    void forgetMonitor(uint64_t id) {
        lastTaskFp.erase(id);
        hitboxes.erase(id);
    }

    // Built ONLY by the warm pass — see the texture rule in shell/shell.hpp. A
    // miss during a draw returns null (that one label is missing for one
    // frame) rather than building, which would paint nothing anyway AND
    // swallow every later draw in the element.
    // The shell font, resolved once per warm (the config's string read
    // returns a fresh std::string — a copy per label per frame): every
    // texture build happens inside a warm, so draws between warms key on
    // the same value, and a changed font applies at the next warm.
    static std::string warmFont;
    static const std::string& shellFont() {
        if (warmFont.empty())
            warmFont = cfg().getS("plugin:awesome:shell:font");
        return warmFont;
    }

    // The compositor's renderText, under the plugin's text options (core/
    // text.hpp): the same extents, ellipsizing and premultiplied ARGB32,
    // over the soft shadow every text over glass wears (textPad).
    static SP<ITexture> rasterText(const std::string& text, const CHyprColor& col, int pt, int maxWidth, const std::string& family, int weight) {
        PangoContext*         ctx    = newTextContext();
        PangoLayout*          layout = pango_layout_new(ctx);
        PangoFontDescription* fd     = pango_font_description_new();
        g_object_unref(ctx);
        pango_font_description_set_family(fd, family.c_str());
        pango_font_description_set_absolute_size(fd, pt * PANGO_SCALE);
        pango_font_description_set_weight(fd, (PangoWeight)weight);
        pango_layout_set_font_description(layout, fd);
        pango_layout_set_text(layout, text.c_str(), -1);
        if (maxWidth > 0) {
            pango_layout_set_width(layout, maxWidth * PANGO_SCALE);
            pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        }
        PangoRectangle ink = {}, log = {};
        pango_layout_get_pixel_extents(layout, &ink, &log);
        const int  W = std::max(1, std::max(log.width, ink.x + ink.width)), H = std::max(1, std::max(log.height, ink.y + ink.height));
        const auto SH = textShadow(eShadow::SOFT, pt);

        auto*      SURF = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W + 2 * SH.pad(), H + 2 * SH.pad());
        auto*      CR   = cairo_create(SURF);
        showShadowedLayout(CR, layout, W, H, SH, col.r, col.g, col.b, col.a);
        pango_font_description_free(fd);
        g_object_unref(layout);
        cairo_surface_flush(SURF);
        auto tex = g_pHyprRenderer->createTexture(SURF);
        cairo_destroy(CR);
        cairo_surface_destroy(SURF);
        return tex;
    }

    SP<ITexture> textTex(const std::string& rawText, const CHyprColor& col, int pt, int maxWidth, const std::string& font, int weight) {
        // One line, always: every label here is a client's string (a title,
        // a tray or menu label) and the bar and its menus have one line per
        // cell. Pango makes a paragraph of each newline — ~4000 of them in a
        // 4 KiB title rasterized a texture tens of thousands of px tall —
        // so control characters become spaces before anything else.
        static std::string CLEAN; // reused; main thread only
        const bool         DIRTY = std::ranges::any_of(rawText, [](char c) { return (unsigned char)c < 0x20 || c == 0x7f; });
        if (DIRTY) {
            CLEAN = rawText;
            for (auto& c : CLEAN)
                if ((unsigned char)c < 0x20 || c == 0x7f)
                    c = ' ';
        }
        const std::string& text = DIRTY ? CLEAN : rawText;

        // key on the RESOLVED font: every caller passes "", and keying the
        // empty string made old-font textures permanent hits across a
        // font change
        const std::string& F = font.empty() ? shellFont() : font;

        char               meta[64]; // the non-text key parts in one stack write — no to_string churn per call
        const int          METALEN = std::snprintf(meta, sizeof(meta), "|%llx|%d|%d|%d|", (unsigned long long)col.getAsHex(), pt, maxWidth, weight);

        static std::string KEY; // reused; main thread only
        KEY.clear();
        KEY += text;
        KEY.append(meta, METALEN > 0 ? (size_t)METALEN : 0);
        KEY += F;

        if (const auto* HIT = texCache.find(KEY))
            return *HIT;

        if (!warmGate().mayBuild())
            return nullptr;

        if (const auto* ENTRY = texCache.insert(KEY, rasterText(text, col, pt, maxWidth, F, weight)))
            return *ENTRY;
        return nullptr;
    }

    SPaint paintFor(PHLMONITOR mon, Render::CRenderContext* rctx) {
        SPaint P;
        P.mon   = mon;
        P.warm  = rctx == nullptr;
        P.scale = mon->m_scale;
        P.mb    = mon->logicalBox();
        P.rctx  = rctx;
        P.h     = barHeight();
        P.pt    = std::max(6, (int)std::round((double)cfg().getI("plugin:awesome:shell:font_size") * P.scale));
        return P;
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
    // the bar yields the whole output to a real fullscreen window (except an
    // open menubar — awesome's ontop wibox): renderBar early-returns, so the
    // layer must not claim a live blur of a strip it never paints
    static bool barHidden(PHLMONITOR mon) {
        if (!mon)
            return true;
        const auto WS = mon->m_activeWorkspace;
        return WS && Fullscreen::controller()->getFullscreenModes(WS).internal == Fullscreen::FSMODE_FULLSCREEN && !(Menubar::isOpen && Menubar::mon.lock() == mon);
    }

    static void renderBar(PHLMONITOR mon, Render::CRenderContext* rctx) {
        const bool warm = rctx == nullptr;
        if (!mon)
            return;

        auto& hits = hitboxes[mon->m_id];
        hits.clear(); // capacity retained: no per-frame allocations

        // the strip stays visible while locked (clock/battery/tray), but an
        // open tray menu must not float over the lockscreen — close it here,
        // like the fullscreen path below, so it's gone on unlock
        if (NAwesome::sessionLocked() && Menu::isOpen && Menu::mon.lock() == mon)
            Menu::close();

        const auto WS = mon->m_activeWorkspace;
        if (barHidden(mon)) {
            if (Menu::isOpen && Menu::mon.lock() == mon)
                Menu::close();
            return; // real fullscreen owns the whole output, like awesome —
                    // except the open menubar: awesome's is an ontop wibox
        }


        // A tasklist label can flip without any bar event or strip damage (pin
        // damages only the window; the xdg maximized bit has no event at all):
        // with every variant still cached the next draw is correct but
        // scissored to the window's damage, so the strip on screen keeps the
        // old text. Widgets fold such content into this fingerprint; a warm
        // stamps what the paint it precedes will show, and a draw that lays
        // out anything else may have been clipped — repaint the strip.
        size_t frameFp = 0;

        auto P = paintFor(mon, rctx);
        P.hits = &hits;
        P.fp   = &frameFp;
        const auto   MB = P.mb;
        const double H  = P.h;

        P.glass(CBox{MB.x, MB.y, MB.w, H}, color(cfg().getColor("plugin:awesome:theme:col_glass")), 0, 2.f);

        // ONE walk of the window list for all its consumers: per-workspace
        // urgency + occupancy (taglist) and this workspace's tasks (tasklist,
        // in arrival order). A tag another monitor owns is not this screen's
        // (awesome's tags are per screen; its clicks and wheel skip it here),
        // so its windows neither fill nor flag this taglist.
        static std::vector<std::pair<uint64_t, PHLWINDOW>> tasks; // reused; main thread only
        SFrame                                             F;
        F.ws = WS;
        tasks.clear();
        for (const auto& W : Desktop::windowState()->windows()) {
            if (NAwesome::isClient(W) && W->m_workspace && W->m_workspace->m_monitor.lock() == mon) {
                const auto NUM = W->m_workspace->numberedID();
                if (NUM && *NUM >= 1 && *NUM <= 9) {
                    F.windows[*NUM]++;
                    if (W->m_hints & Desktop::View::WINDOW_HINT_URGENT)
                        F.urgent[*NUM] = true;
                }
            }
            if (isTaskOn(W, WS))
                tasks.emplace_back(NAwesome::winOrder().seqOf(W.get()), W);
        }
        std::sort(tasks.begin(), tasks.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        F.tasks   = &tasks;
        const auto GLOBALFOCUS = Desktop::focusState() ? Desktop::focusState()->window() : nullptr;
        // Focus is global, but the bar is rendered once per output. A window
        // focused on another output must not mark this monitor's tag/task.
        F.focus   = GLOBALFOCUS && GLOBALFOCUS->m_monitor.lock() == mon ? GLOBALFOCUS : nullptr;

        // one palette fetch per frame: color() memoizes the conversion but
        // still hashes per call, and the widgets make dozens
        F.fg          = color(cfg().getColor("plugin:awesome:shell:col_fg"));
        F.active      = color(cfg().getColor("plugin:awesome:shell:col_active"));
        F.urgentFg    = color(cfg().getColor("plugin:awesome:shell:col_urgent"));
        F.empty       = color(cfg().getColor("plugin:awesome:shell:col_empty"));
        F.activeHover = lifted(F.active);
        F.urgentHover = lifted(F.urgentFg);

        // awesome's align layout: the left slot, the tasklist filling the
        // middle, the right slot laid from the edge inwards ([tray][bell]
        // [control center][battery][clock]). Widgets whose fit comes back 0
        // are hidden this frame. The strip's 8 px inset and 14 between the slots
        // and the tasks; the right slot keeps macOS's menu-extra rhythm,
        // measured off its 2x menu bar: 20 between the widgets (the tray's
        // own icons as far apart, shell:tray_spacing) and 16 off the edge.
        constexpr double INSET = 8, SLOT_GAP = 14, RIGHT_GAP = 20, RIGHT_INSET = 16;
        IWidget* const   LEFT[]  = {&taglistWidget()};
        IWidget* const   RIGHT[] = {&trayWidget(), &bellWidget(), &controlsWidget(), &batteryWidget(), &clockWidget()};

        // Fitts: a pointer thrown into a screen corner stops there, so the
        // corners are the bar's easiest targets — the slot at each end owns
        // its edge, never a dead inset. Every hit spans the bar's height.
        const size_t LEFT_HITS = P.hits->size();
        double       x         = MB.x + INSET;
        for (auto* const W : LEFT) {
            const double WD = std::min(W->fit(P, F), std::max(0.0, MB.x + MB.w - x));
            if (WD > 0)
                W->draw(P, F, CBox{x, MB.y, WD, H});
            x += WD;
        }
        if (P.hits->size() > LEFT_HITS) {
            auto& FIRST = (*P.hits)[LEFT_HITS].box;
            FIRST.w += FIRST.x - MB.x;
            FIRST.x = MB.x;
            auto& LAST = P.hits->back().box;
            LAST.w     = std::max(LAST.w, x + SLOT_GAP / 2 - LAST.x);
        }

        const size_t RIGHT_HITS = P.hits->size();
        double       right      = MB.x + MB.w - RIGHT_INSET;
        bool         first      = true;
        for (size_t i = std::size(RIGHT); i-- > 0;) {
            // Keep the right slot inside the space left by the taglist. If a
            // narrow output cannot fit every widget, leftmost right-side
            // widgets yield first and no cell can overlap the tasklist.
            const double GAP = first ? 0 : RIGHT_GAP;
            const double WD  = std::min(RIGHT[i]->fit(P, F), std::max(0.0, right - GAP - x - SLOT_GAP));
            if (WD > 0) {
                right -= GAP + WD;
                const CBox   CELL{right, MB.y, WD, H};
                const size_t BEFORE = P.hits->size();
                RIGHT[i]->draw(P, F, CELL);
                // a widget with nothing to click (the battery) still owns
                // its cell: without one its neighbours' cells met across it,
                // and its clicks went to them
                if (P.hits->size() == BEFORE)
                    P.hits->push_back(SHit{.box = CELL, .widget = RIGHT[i]});
                first = false;
            }
        }
        // the right slot's cells meet halfway across each gap, the leftmost
        // reaches half a gap out, the rightmost (the clock) the screen's edge
        if (P.hits->size() > RIGHT_HITS) {
            const auto BEGIN = P.hits->begin() + (std::ptrdiff_t)RIGHT_HITS;
            std::sort(BEGIN, P.hits->end(), [](const SHit& a, const SHit& b) { return a.box.x < b.box.x; });
            for (auto it = BEGIN; it + 1 != P.hits->end(); ++it) {
                auto&        A   = it->box;
                auto&        B   = (it + 1)->box;
                const double MID = std::round((A.x + A.w + B.x) / 2);
                if (MID > A.x + A.w && MID < B.x) {
                    A.w = MID - A.x;
                    B.w += B.x - MID;
                    B.x = MID;
                }
            }
            auto& L = BEGIN->box;
            L.w += RIGHT_GAP / 2;
            L.x -= RIGHT_GAP / 2;
            auto& R = P.hits->back().box;
            R.w     = MB.x + MB.w - R.x;
        }

        // the tasklist splits the whole leftover strip, 14 px off each slot;
        // its outer cells reach across those gaps to meet the slots' cells
        const size_t TASK_HITS = P.hits->size();
        if (const double TASKW = std::max(0.0, right - x - 2 * SLOT_GAP); TASKW > 0)
            tasklistWidget().draw(P, F, CBox{x + SLOT_GAP, MB.y, TASKW, H});
        if (P.hits->size() > TASK_HITS) {
            const double LEFT_EDGE  = RIGHT_HITS > LEFT_HITS ? (*P.hits)[RIGHT_HITS - 1].box.x + (*P.hits)[RIGHT_HITS - 1].box.w : MB.x;
            const double RIGHT_EDGE = TASK_HITS > RIGHT_HITS ? (*P.hits)[RIGHT_HITS].box.x : MB.x + MB.w; // sorted: the slot's leftmost
            auto&        FIRST      = (*P.hits)[TASK_HITS].box;
            FIRST.w += FIRST.x - LEFT_EDGE;
            FIRST.x    = LEFT_EDGE;
            auto& LAST = P.hits->back().box;
            LAST.w     = RIGHT_EDGE - LAST.x;
        }

        auto& FP = lastTaskFp[mon->m_id];
        if (warm)
            FP = frameFp;
        else if (FP != frameFp) {
            FP             = frameFp;
            warmGate().texStale = true;
        }

        tasks.clear(); // don't keep strong window refs across frames
    }

    // The shell's popups on their own layer, above the cards (eLayerZ): an
    // open menu or the menubar prompt owns every press, so it must also be
    // what's seen there. Their hit geometry is their own (Menu::levels, the
    // menubar strip), laid out by this same pass, warm and draw alike.
    static bool popupsOn(PHLMONITOR mon) {
        return mon && ((Menu::isOpen && Menu::mon.lock() == mon) || (Menubar::isOpen && Menubar::mon.lock() == mon));
    }

    static void renderPopups(PHLMONITOR mon, Render::CRenderContext* rctx) {
        // the bar's pass (a layer below) closes a menu its bar no longer
        // shows (session lock, real fullscreen) before this one runs
        if (!popupsOn(mon))
            return;
        const auto P = paintFor(mon, rctx);

        // -- the menubar: its own strip right BELOW the bar, the bar stays
        // visible (awesome's menubar is a separate wibox at the workarea top,
        // which sits under the wibar — it never replaced it) --
        Menubar::render(P);

        // -- the open menu, panel by panel: the client list is fixed at 250
        // wide (the old rc's client_list width); dbusmenu levels size
        // themselves, and submenus cascade out beside their parent — like
        // the GTK menus these were under X11 --
        Menu::render(P);
    }

    // ---- the canvas layer ----

    class CStripLayer : public NAwesome::ILayer {
      public:
        const char* name() const override {
            return "shell-strip";
        }

        // the cache's grace bookkeeping rides the canvas's full-walk bracket
        // (a scoped warm leaves the other monitors' textures unenumerated and
        // must not age them)
        void warmBegin(bool scoped) override {
            warmFont = cfg().getS("plugin:awesome:shell:font");
            if (!scoped)
                texCache.tick();
        }
        void warmEnd(bool scoped) override {
            if (!scoped)
                texCache.sweep();
        }

        void warm(PHLMONITOR mon) override {
            renderBar(mon, nullptr);
        }
        void draw(PHLMONITOR mon, NAwesome::SPaint& P) override {
            if (P.rctx)
                renderBar(mon, P.rctx);
        }
        void damage() override {
            damageBars();
        }
        // monitor-local LOGICAL px — the canvas pass scales by m_scale itself
        std::optional<CBox> boundingBox(PHLMONITOR mon) const override {
            if (!mon)
                return std::nullopt;
            return CBox{0, 0, mon->logicalBox().w, barHeight()};
        }
        // the open menubar is awesome's ontop wibox: it paints over a
        // fullscreen client (barHidden() keeps the rest of the strip off)
        bool overFullscreen(PHLMONITOR mon) const override {
            return Menubar::isOpen && Menubar::mon.lock() == mon;
        }
        bool needsBlur(PHLMONITOR mon) const override {
            // only while the band actually paints — never claim a live blur of
            // a strip that is hidden under a fullscreen window (blur for nothing)
            // and only while its glass is translucent: an opaque col_glass
            // draws without blur, so claiming live blur would make
            // the renderer blur and disable occlusion under the strip for a
            // result nobody sees
            return NAwesome::blurOn() && !barHidden(mon) && color(cfg().getColor("plugin:awesome:theme:col_glass")).a < 1.0;
        }

        void exit() {
            // the gate's rewarm hop is already down: supervisor().stop() runs
            // before module teardown, and its barChanged would touch the
            // caches cleared below
            texCache.clear();
            lastTaskFp.clear();
            warmFont.clear();
        }
    };

    static CStripLayer& stripLayer() {
        static CStripLayer L;
        return L;
    }

    class CPopupLayer : public NAwesome::ILayer {
      public:
        const char* name() const override {
            return "shell-popups";
        }
        // the strip layer brackets the shared texture cache's generation
        void warm(PHLMONITOR mon) override {
            renderPopups(mon, nullptr);
        }
        void draw(PHLMONITOR mon, NAwesome::SPaint& P) override {
            if (P.rctx)
                renderPopups(mon, P.rctx);
        }
        void damage() override {
            if (!g_pHyprRenderer)
                return;
            for (const auto& M : State::monitorState()->monitors())
                if (const auto B = boundingBox(M))
                    g_pHyprRenderer->damageBox(CBox{*B}.translate(M->logicalBox().pos()));
        }
        // The prompt band and the menu's panels as last laid out (a level
        // change warms before it damages), their rim and drop included. The
        // whole monitor made every damage on it re-run the menu's layout
        // and live-blur the screen behind it while a menu was open.
        std::optional<CBox> boundingBox(PHLMONITOR mon) const override {
            if (!popupsOn(mon))
                return std::nullopt;
            const auto MB = mon->logicalBox();
            double     x0 = MB.w, y0 = MB.h, x1 = 0, y1 = 0;
            const auto ADD = [&](const CBox& B) {
                x0 = std::min(x0, B.x);
                y0 = std::min(y0, B.y);
                x1 = std::max(x1, B.x + B.w);
                y1 = std::max(y1, B.y + B.h);
            };
            if (Menubar::isOpen && Menubar::mon.lock() == mon)
                ADD(CBox{0, barHeight(), MB.w, barHeight()});
            if (Menu::isOpen && Menu::mon.lock() == mon) {
                const double MARGIN = Menu::reach(mon);
                for (const auto& L : Menu::levels) {
                    if (L.box.w <= 0 || L.box.h <= 0)
                        return CBox{0, 0, MB.w, MB.h}; // not laid out yet: an undersized box clips
                    ADD(CBox{L.box}.translate(Vector2D{-MB.x, -MB.y}).expand(MARGIN));
                }
            }
            x0 = std::max(x0, 0.0);
            y0 = std::max(y0, 0.0);
            x1 = std::min(x1, MB.w);
            y1 = std::min(y1, MB.h);
            if (x1 <= x0 || y1 <= y0)
                return CBox{0, 0, MB.w, MB.h};
            return CBox{x0, y0, x1 - x0, y1 - y0};
        }
        // the open menubar is awesome's ontop wibox: it paints over a
        // fullscreen client
        bool overFullscreen(PHLMONITOR mon) const override {
            return Menubar::isOpen && Menubar::mon.lock() == mon;
        }
        bool needsBlur(PHLMONITOR mon) const override {
            return NAwesome::blurOn() && popupsOn(mon) && color(cfg().getColor("plugin:awesome:theme:col_glass")).a < 1.0;
        }
    };

    static CPopupLayer& popupLayer() {
        static CPopupLayer L;
        return L;
    }

    // A title is the one bar input a client drives at will: a terminal or a
    // player can retitle every frame or faster, and each change warmed every
    // layer on every monitor once per event-loop turn — a kHz title kept the
    // compositor thread busy. Titles warm the strip alone, at most once per
    // TITLE_GAP: the first at once, a burst's last one gap after.
    static constexpr auto                 TITLE_GAP = std::chrono::milliseconds(16);
    static SP<CEventLoopTimer>            titleTimer;
    static bool                           titleArmed = false;
    static std::chrono::steady_clock::time_point lastTitleWarm;

    static void                           titleWarm() {
        titleArmed    = false;
        lastTitleWarm = std::chrono::steady_clock::now();
        NAwesome::Canvas::inst().warmAll(nullptr, &stripLayer());
        damageBars();
    }

    void titleChanged() {
        if (!titleTimer) {
            barChanged();
            return;
        }
        if (titleArmed)
            return;
        titleArmed       = true;
        const auto SINCE = std::chrono::steady_clock::now() - lastTitleWarm;
        titleTimer->updateTimeout(SINCE >= TITLE_GAP ? std::chrono::milliseconds(0) : std::chrono::ceil<std::chrono::milliseconds>(TITLE_GAP - SINCE));
    }

    void stripInit() {
        NAwesome::Canvas::inst().addLayer(&stripLayer(), NAwesome::eLayerZ::BAR);
        NAwesome::Canvas::inst().addLayer(&popupLayer(), NAwesome::eLayerZ::POPUPS);
        if (g_pEventLoopManager) {
            titleTimer = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { titleWarm(); }, nullptr);
            g_pEventLoopManager->addTimer(titleTimer);
        }
    }
    void stripExit() {
        if (titleTimer && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(titleTimer);
        titleTimer.reset();
        titleArmed = false;
        stripLayer().exit();
    }

    void warmBars(PHLMONITOR only) {
        // the canvas owns the bracket (and warms every layer, the cards and
        // the snap zone with it — the old plugins each warmed alone)
        NAwesome::Canvas::inst().warmAll(only);
    }

    // ---- util ----

    double barHeight() {
        return std::max((double)cfg().getI("plugin:awesome:shell:height"), 8.0);
    }

    void damageBars() {
        if (!g_pHyprRenderer)
            return;
        const double H = Menubar::isOpen ? barHeight() * 2 : barHeight();
        for (const auto& M : State::monitorState()->monitors()) {
            const auto MB = M->logicalBox();
            g_pHyprRenderer->damageBox(CBox{MB.x, MB.y, MB.w, H});
        }
    }

    // Coalesced: one warm + damage per event-loop turn, however many bar
    // events (title, focus, urgency, tray icons) arrived in it — each used to
    // warm every layer on every monitor synchronously. A frame that renders
    // first finds the stale texture and rewarms itself (the canvas gate).
    static NAwesome::CHop pendingBarChange;
    void barChanged() {
        pendingBarChange.arm([]() {
            warmBars();
            damageBars();
        });
    }

    // The launcher's own changes (a keystroke, a selection, a scan landing):
    // the popup layer on the prompt's monitor and the prompt band only — a
    // full bar change warmed every layer on every monitor and damaged every
    // bar, per keystroke.
    static NAwesome::CHop pendingPromptChange;
    void promptChanged() {
        pendingPromptChange.arm([]() {
            const auto M = Menubar::mon.lock();
            if (!M || !Menubar::isOpen) {
                warmBars();
                damageBars();
                return;
            }
            NAwesome::Canvas::inst().warmAll(M, &popupLayer());
            if (g_pHyprRenderer) {
                const auto MB = M->logicalBox();
                g_pHyprRenderer->damageBox(CBox{MB.x, MB.y + barHeight(), MB.w, barHeight()});
            }
        });
    }

    // A menu hover moved: the popup layer on the menu's monitor, once per
    // event-loop turn — damageMenu warmed every layer on the monitor per
    // row the pointer crossed. A hover moves no panel: the boxes and rows
    // the hit-tests read stay as they are.
    static NAwesome::CHop pendingMenuHover;
    void menuHoverChanged() {
        pendingMenuHover.arm([]() {
            const auto M = Menu::mon.lock();
            if (!M || !Menu::isOpen)
                return;
            NAwesome::Canvas::inst().warmAll(M, &popupLayer());
            Menu::damagePanels(); // the ring and a panel the warm resized too
        });
    }

    bool isTaskOn(const PHLWINDOW& w, const PHLWORKSPACE& ws) {
        // minimized windows are hidden but still belong to their tag, like
        // awesome — keep their row; other hidden windows (swallowed) stay out.
        return NAwesome::isClient(w) && ws && w->m_workspace && w->m_workspace->id() == ws->id() && (!w->isHidden() || NAwesome::Windows::Tasklist::isMinimized(w));
    }

    std::string lower(std::string s) {
        for (auto& c : s)
            c = std::tolower((unsigned char)c);
        return s;
    }

} // namespace NAwesome::Shell
