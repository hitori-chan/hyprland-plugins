// awesome/core/canvas.hpp — the compositor-drawing platform: the warm/draw
// gate (crash class 4), the keyed raster cache, the shared paint context,
// the layer table, and the one pass element that renders every layer.
//
// The texture rule, unchanged: a texture created inside a frame cannot be
// painted by that same frame — wherever in the frame it was created — and
// the miss silently swallows everything drawn after it in the element. So
// textures are built ONLY by a warm pass running from the event loop, one
// frame ahead; the draw pass paints cache hits and never builds. A draw
// that finds a texture missing flags the gate, and the pass element backs
// out to the event loop to warm and repaint.
//
// Layers are the modules' drawing units: the shell's strip (and its menu
// and menubar), the notify popups, the notify shade, the windows snap
// indicator. Each warms, draws, damages, and reports its blur need and
// bounding box; the pass unions the boxes and blurs, and the gate keeps
// the whole thing one-frame apart.
#pragma once

#include "hop.hpp"
#include "theme.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Context.hpp>
#include <hyprland/src/render/WindowRenderPresentation.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/state/MonitorState.hpp>

using ITexture  = Render::ITexture;
using Render::GL::g_pHyprOpenGL;

#include <algorithm>
#include <chrono>
#include <format>
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NAwesome {

#ifdef AWESOME_GATE
    // the gate's draw profile: time spent inside the paint calls, so a
    // `drawstats` read can split the draw into layout and paint
    inline uint64_t& gatePaintNs() {
        static uint64_t N = 0;
        return N;
    }
    struct SGatePaintScope {
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~SGatePaintScope() {
            gatePaintNs() += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        }
    };
#define AW_PAINT_SCOPE NAwesome::SGatePaintScope awPaintScope_
    // named buckets for one-off attribution: AW_PROFILE("name") in a scope
    inline std::unordered_map<std::string_view, std::pair<uint64_t, uint64_t>>& gateProfile() {
        static std::unordered_map<std::string_view, std::pair<uint64_t, uint64_t>> M;
        return M;
    }
    struct SGateScope {
        std::string_view                      name;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~SGateScope() {
            auto& E = gateProfile()[name];
            E.first++;
            E.second += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        }
    };
#define AW_PROFILE_CAT2(a, b) a##b
#define AW_PROFILE_CAT(a, b) AW_PROFILE_CAT2(a, b)
#define AW_PROFILE(n) NAwesome::SGateScope AW_PROFILE_CAT(awProf_, __LINE__){n}
#else
#define AW_PAINT_SCOPE
#define AW_PROFILE(n)
#endif

    // ---- the warm/draw state machine ----

    class CWarmGate {
      public:
        bool warming  = false; // building a texture is PERMITTED (a warm pass, or a token)
        bool texStale = false; // a draw ran ahead of the screen -> warm + repaint
        bool inRender = false; // a draw is on the stack: never build, never warm
        bool inPass   = false; // a warm PASS is on the stack — the re-entry guard

        // a draw-side texture miss: no build (that would paint nothing anyway
        // AND swallow every later draw in the element), remember to rewarm
        bool mayBuild() {
            if (warming)
                return true;
            texStale = true;
            return false;
        }

        // The warm bracket; begin refuses re-entry and mid-render calls so
        // callers never have to check.
        //
        // A TOKEN IS NOT RE-ENTRY. It only says "building is allowed here",
        // and a warm inside one must still run: the shell's tray menu
        // damages — and so warms — from inside the token its dbusmenu reply
        // holds, and while that counted as re-entry the warm silently
        // no-opped. Hence the separate pass flag, and the saved permission.
        bool beginWarm() {
            if (inPass || inRender || !g_pCompositor)
                return false;
            inPass          = true;
            m_tokenPermit   = warming;
            warming         = true;
            return true;
        }
        void endWarm() {
            inPass   = false;
            warming  = m_tokenPermit; // hand the grant back to a token still in scope
            texStale = false;
        }

        // pass-element tail: back out to the event loop to build what the
        // draw found missing, then repaint — we are inside the render when
        // we notice, so it must be deferred
        void rewarmIfStale(std::function<void()> warmRepaint) {
            if (texStale)
                m_rewarm.arm(std::move(warmRepaint));
        }

        // Some textures resolve OUTSIDE the warm pass, from the event loop
        // (a dbusmenu icon-name arriving in a DBus reply, a row built in a
        // deferred click). warming gates creation, but the real safety
        // condition is "not inside a render" — which those contexts never
        // are. The token grants the permission around such a resolve; the
        // caller damages after, so the new texture gets its own frame.
        // Never construct one inside a render. Warming from inside a token
        // IS fine (beginWarm above) — that is the usual shape, since the
        // damage that follows the resolve has to lay the surface out first.
        struct SToken {
            CWarmGate& g;
            bool       prev;
            explicit SToken(CWarmGate& gate) : g(gate), prev(gate.warming) {
                g.warming = true;
            }
            ~SToken() {
                g.warming = prev;
            }
        };

      private:
        bool m_tokenPermit = false; // warming as it stood when this pass began
        CHop m_rewarm;
    };

    // The keyed raster cache. Staleness needs no bookkeeping at all: content
    // plus style IS the key, so a retitled window or an age bucket ticking
    // over simply misses to a new one. What needs bounding is the MAP —
    // title churn would grow it without limit — and the bound is a grace
    // generation rather than a size cap: evict only what no recent warm
    // asked for, so no single warm is ever left rebuilding everything at
    // once. LIFE (how many warms an untouched entry survives) differs per
    // surface: the bar warms on every clock tick and wants a longer grace
    // than the shade, which warms only when its model changes.
    template <typename TValue, uint64_t LIFE = 32, size_t MAX_BYTES = 64ull << 20>
    class CGenCache {
      public:
        using Measure = std::function<size_t(const TValue&)>;

        CGenCache() : CGenCache(MAX_BYTES, {}) {}
        CGenCache(size_t maxBytes, Measure measure) : m_maxBytes(maxBytes), m_measure(std::move(measure)) {
            if (!m_measure)
                m_measure = [](const TValue&) { return sizeof(TValue); };
        }

        // the entry if present, TOUCHED so this generation's sweep spares it
        TValue* find(const std::string& key) {
            const auto IT = m_map.find(key);
            if (IT == m_map.end())
                return nullptr;
            IT->second.gen = m_gen;
            return &IT->second.val;
        }

        TValue* insert(const std::string& key, TValue&& val) {
            const size_t BYTES = entryBytes(key, val);
            if (BYTES > m_maxBytes)
                return nullptr; // never retain one hostile raster above the cap

            if (const auto OLD = m_map.find(key); OLD != m_map.end()) {
                m_bytes -= OLD->second.bytes;
                OLD->second.val   = std::move(val);
                OLD->second.gen   = m_gen;
                OLD->second.bytes = BYTES;
                m_bytes += BYTES;
                return &OLD->second.val;
            }

            while (m_bytes + BYTES > m_maxBytes && !m_map.empty()) {
                const auto VICTIM = std::min_element(m_map.begin(), m_map.end(), [](const auto& A, const auto& B) { return A.second.gen < B.second.gen; });
                // Never evict an entry this generation touched or inserted:
                // a caller may still hold the pointer insert() returned for
                // it, and erasing the node would dangle it mid-warm.
                if (VICTIM->second.gen >= m_gen)
                    return nullptr;
                m_bytes -= VICTIM->second.bytes;
                m_map.erase(VICTIM);
            }
            auto [IT, INSERTED] = m_map.emplace(key, SEntry{.val = std::move(val), .gen = m_gen, .bytes = BYTES});
            (void)INSERTED;
            m_bytes += BYTES;
            return &IT->second.val;
        }

        void tick() {
            m_gen++;
        }

        // Call ONLY from a warm that enumerated every surface: a scoped warm
        // (one monitor, one menu) never asks for the textures it left alone,
        // so ageing on one would evict them for having been skipped.
        void sweep() {
            if (m_gen <= LIFE)
                return;
            for (auto IT = m_map.begin(); IT != m_map.end();) {
                if (IT->second.gen + LIFE < m_gen) {
                    m_bytes -= IT->second.bytes;
                    IT = m_map.erase(IT);
                } else
                    ++IT;
            }
        }

        void clear() {
            m_map.clear();
            m_bytes = 0;
        }

      private:
        size_t entryBytes(const std::string& key, const TValue& val) const {
            const size_t VALUE = m_measure(val);
            if (VALUE > std::numeric_limits<size_t>::max() - key.size())
                return std::numeric_limits<size_t>::max();
            return key.size() + VALUE;
        }

        struct SEntry {
            TValue   val;
            uint64_t gen = 0;
            size_t   bytes = 0;
        };
        std::unordered_map<std::string, SEntry> m_map;
        uint64_t                                m_gen = 0;
        size_t                                  m_bytes = 0;
        size_t                                  m_maxBytes;
        Measure                                  m_measure;
    };

    // ---- the paint context (the union of the old bar's and cards') ----

    struct SPaint {
        PHLMONITOR              mon   = nullptr;
        std::vector<CBox>*      hits  = nullptr; // per-monitor hit cells, global px
        bool                    warm  = false;
        double                  scale = 1.0;
        float                   alpha = 1.f; // motion: the arriving surface fades in
        double                  dy    = 0;   // motion: slide offset, painting only — hit boxes stay final
        CBox                    mb{};  // the monitor's logical box
        double                  h     = 0; // the shell strip height (bar layers)
        int                     pt    = 12; // the base type role, physical
        size_t*                 fp    = nullptr; // layout fingerprint sink (shell)
        // the frame's render session, borrowed for the pass element's draw;
        // null outside a draw (a warm or a measure paints nothing)
        Render::CRenderContext* rctx  = nullptr;

        bool paints() const {
            return !warm && rctx;
        }

        CBox toPhys(const CBox& global) const {
            return CBox{global}.translate(Vector2D{-mon->m_position.x, -mon->m_position.y + dy}).scale(scale).round();
        }
        void rect(const CBox& global, const CHyprColor& c, int round = 0, float rp = 2.f) const {
            if (!paints())
                return;
            AW_PAINT_SCOPE;
            g_pHyprOpenGL->renderRect(*rctx, toPhys(global), c.modifyA(c.a * alpha), {.round = round, .roundingPower = rp});
        }
        // Semantic container paint. Opaque defaults make the fork skip blur;
        // configured alpha below 1 retains the rounded live-glass path.
        void glass(const CBox& global, const CHyprColor& c, int round, float rp) const {
            if (!paints())
                return;
            AW_PAINT_SCOPE;
            g_pHyprOpenGL->renderRect(*rctx, toPhys(global), c.modifyA(c.a * alpha), {.round = round, .roundingPower = rp, .blur = blurOn(), .blurA = alpha});
        }
        void border(const CBox& global, const CHyprColor& c, int round, int sizePx, float rp) const {
            if (!paints())
                return;
            AW_PAINT_SCOPE;
            g_pHyprOpenGL->renderBorder(*rctx, toPhys(global), Config::CGradientValueData{c}, {.round = round, .roundingPower = rp, .borderSize = sizePx, .a = alpha});
        }
        void ring(const CBox& global, const CHyprColor& c, int round, float rp, double px = 1.0) const {
            if (!paints())
                return;
            AW_PAINT_SCOPE;
            // the gradient ctor heap-allocates and OkLab-converts — memoize per color
            static std::unordered_map<uint64_t, Config::CGradientValueData> grads;
            const auto                                                      KEY = c.getAsHex();
            auto                                                            IT  = grads.find(KEY);
            if (IT == grads.end())
                IT = grads.emplace(KEY, Config::CGradientValueData{c}).first;
            g_pHyprOpenGL->renderBorder(*rctx, toPhys(global), IT->second, {.round = round, .roundingPower = rp, .borderSize = std::max(1, (int)std::lround(px * scale)), .a = alpha});
        }
        void shadow(const CBox& global, int round, float rp, int range) const {
            if (!paints())
                return;
            AW_PAINT_SCOPE;
            static Config::CGradientValueData GRAD{CHyprColor{Theme::SHADOW}};
            g_pHyprOpenGL->renderRoundedShadow(*rctx, toPhys(global), round, rp, (int)std::lround(range * scale), GRAD, alpha, Render::SWindowRenderPresentation{});
        }
        // native px at a logical position
        void tex(const SP<ITexture>& t, double gx, double gy) const {
            if (!paints() || !t || t->m_texID == 0)
                return;
            AW_PAINT_SCOPE;
            const auto P = toPhys(CBox{gx, gy, 1, 1});
            g_pHyprOpenGL->renderTexture(*rctx, t, CBox{(double)P.x, (double)P.y, t->m_size.x, t->m_size.y}, {.a = alpha});
        }
        // center the texture inside the cell at native size
        void texIn(const SP<ITexture>& t, const CBox& cell) const {
            if (!paints() || !t || t->m_texID == 0)
                return;
            AW_PAINT_SCOPE;
            const auto B = toPhys(cell);
            CBox       b{B.x + (B.w - t->m_size.x) / 2.0, B.y + (B.h - t->m_size.y) / 2.0, t->m_size.x, t->m_size.y};
            g_pHyprOpenGL->renderTexture(*rctx, t, b.round(), {.a = alpha});
        }
        // Contain-fit: scale to fill the cell as far as aspect allows,
        // centered. renderTexture stretches to its box, so a non-square icon
        // handed a square cell comes out squashed — fit keeps proportions.
        void texFit(const SP<ITexture>& t, const CBox& cell, int round = 0, float rp = 2.f) const {
            if (!paints() || !t || t->m_texID == 0)
                return;
            AW_PAINT_SCOPE;
            const double TW = t->m_size.x, TH = t->m_size.y;
            if (TW <= 0 || TH <= 0)
                return;
            const auto   B = toPhys(cell);
            const double S = std::min(B.w / TW, B.h / TH);
            const double W = TW * S, H = TH * S;
            CBox         b{B.x + (B.w - W) / 2.0, B.y + (B.h - H) / 2.0, W, H};
            g_pHyprOpenGL->renderTexture(*rctx, t, b.round(), {.a = alpha, .round = round, .roundingPower = rp});
        }
        // Fill the cell, aspect not preserved (the card's full-bleed images)
        void texStretch(const SP<ITexture>& t, const CBox& cell, int round = 0, float rp = 2.f) const {
            if (!paints() || !t || t->m_texID == 0)
                return;
            AW_PAINT_SCOPE;
            g_pHyprOpenGL->renderTexture(*rctx, t, toPhys(cell), {.a = alpha, .round = round, .roundingPower = rp});
        }
    };

    // ---- layers ----

    struct ILayer {
        virtual ~ILayer() = default;
        virtual const char* name() const = 0;
        // A canvas warm pass is about to start (true) / has finished
        // (false) with `scoped` = one monitor only. Layers with a
        // generation-cache tick the generation on a FULL walk and sweep its
        // grace window after it; a scoped walk does neither — the textures it
        // left unenumerated were not unwanted, just out of scope, and ageing
        // on a scoped warm would evict them (every later scoped warm
        // rebuilding them: the strip's task-label thrash). Default: none.
        virtual void warmBegin(bool scoped) {}
        virtual void warmEnd(bool scoped) {}
        // Build every texture this monitor's NEXT frame will paint.
        virtual void        warm(PHLMONITOR mon) = 0;
        // Paint. Must never build.
        virtual void        draw(PHLMONITOR mon, SPaint& ctx) = 0;
        // Damage this layer's own region(s).
        virtual void        damage() {}
        // Monitor-local LOGICAL px the layer may paint (the pass scales by
        // m_scale itself). nullopt = nothing this monitor.
        virtual std::optional<CBox> boundingBox(PHLMONITOR mon) const {
            return std::nullopt;
        }
        virtual bool needsBlur(PHLMONITOR mon) const {
            return false;
        }
        // Must paint over a fullscreen client on this monitor right now: a
        // solitary/direct-scanout fullscreen client skips the scene render
        // (no RENDER_POST_WINDOWS), so the canvas blocks solitary while any
        // layer says so (monitor.blockSolitary). False by default — a
        // layer that claims this needlessly costs fullscreen video its
        // scanout.
        virtual bool overFullscreen(PHLMONITOR mon) const {
            return false;
        }
    };

    // ---- the canvas: layer table, warm, the one pass ----

    class Canvas {
      public:
        static Canvas& inst() {
            static Canvas C;
            return C;
        }
        CWarmGate& gate() {
            return m_gate;
        }

        void addLayer(ILayer* L) {
            m_layers.push_back(L);
        }

        // From the event loop: build every texture the NEXT frame will
        // paint, on every monitor (or one). Must run outside the render
        // cycle — a texture built during a frame cannot be painted by that
        // same frame, wherever in it it was built. No-ops inside a render so
        // callers never have to check.
        void warmAll(PHLMONITOR only = nullptr) {
            if (!m_gate.beginWarm())
                return;
            const bool SCOPED = only != nullptr;
            for (auto* L : m_layers)
                L->warmBegin(SCOPED);
            for (const auto& M : State::monitorState()->monitors())
                if (!only || M == only)
                    for (auto* L : m_layers)
                        L->warm(M);
            for (auto* L : m_layers)
                L->warmEnd(SCOPED);
            m_gate.endWarm();
        }

        // The pass elements register on RENDER_POST_WINDOWS. The context is
        // borrowed for this emission only — each element gets its own
        // context back at draw time.
        //
        // One element PER LAYER, each with its own box: the renderer drops an
        // element whose box misses the frame's damage, so a card redrawn over
        // a busy window no longer re-runs the bar's whole layout (one union
        // element did: ~190 us of a ~300 us draw, every frame). A layer with
        // nothing on this monitor adds no element at all.
        void onRenderStage(const Event::SRenderStageEvent& ev) {
            if (ev.stage != RENDER_POST_WINDOWS || !ev.monitor || !ev.context)
                return;
            for (auto* L : m_layers)
                if (L->boundingBox(ev.monitor))
                    Render::IHyprRenderer::addPassElement(ev.context->get(), makeUnique<CAwesomePassElement>(ev.monitor, L));
        }

        void onBlockSolitary(PHLMONITOR mon, bool& block) const {
            if (block || !mon)
                return;
            for (auto* L : m_layers)
                if (L->overFullscreen(mon)) {
                    block = true;
                    return;
                }
        }

        bool anyLayerVisible(PHLMONITOR mon) const {
            for (auto* L : m_layers)
                if (L->boundingBox(mon))
                    return true;
            return false;
        }

#ifdef AWESOME_GATE
        // the gate's draw profile (`hyprctl awesome drawstats`): time inside
        // the canvas pass's draw — layout and the paint calls it issues —
        // since the last read
        std::string drawStats() {
            const auto OUT = std::format("draws:{} ns:{} avg_us:{:.1f} paint_avg_us:{:.1f}", m_draws, m_drawNs, m_draws ? m_drawNs / 1000.0 / m_draws : 0.0,
                                         m_draws ? gatePaintNs() / 1000.0 / m_draws : 0.0);
            std::string buckets;
            for (const auto& [N, E] : gateProfile())
                buckets += std::format(" {}:{}x/{:.1f}us", N, E.first, m_draws ? E.second / 1000.0 / m_draws : 0.0);
            gateProfile().clear();
            m_draws = m_drawNs = 0;
            gatePaintNs()      = 0;
            return OUT + buckets;
        }
#endif

      private:
        friend class CAwesomePassElement;
        std::vector<ILayer*> m_layers;
        CWarmGate            m_gate;
#ifdef AWESOME_GATE
        uint64_t m_draws = 0, m_drawNs = 0;
#endif

        class CAwesomePassElement : public IPassElement {
          public:
            CAwesomePassElement(PHLMONITOR mon, ILayer* layer) : m_mon(mon), m_layer(layer) {}
            virtual ~CAwesomePassElement() = default;

            virtual std::vector<UP<IPassElement>> draw(Render::CRenderContext& rctx) override {
                auto& C = Canvas::inst();
#ifdef AWESOME_GATE
                const auto T0 = std::chrono::steady_clock::now();
#endif
                C.m_gate.inRender = true;
                if (const auto MON = m_mon.lock()) {
                    SPaint ctx;
                    ctx.mon   = MON;
                    ctx.scale = MON->m_scale;
                    ctx.mb    = MON->logicalBox();
                    ctx.rctx  = &rctx;
                    m_layer->draw(MON, ctx);
                }
                C.m_gate.inRender = false;
#ifdef AWESOME_GATE
                C.m_drawNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - T0).count();
                C.m_draws++;
#endif

                // Something changed without warming first (a texture the warm
                // never enumerated). One glyph is missing for one frame;
                // warm it and repaint. Never build here — that is the bug
                // this all guards.
                C.m_gate.rewarmIfStale([]() {
                    Canvas::inst().warmAll();
                    for (auto* L : Canvas::inst().m_layers)
                        L->damage();
                });
                return {};
            }
            virtual bool needsLiveBlur(Render::CRenderContext&) override {
                // only while the layer actually paints translucent glass —
                // never claim a live blur of a region that is hidden
                const auto MON = m_mon.lock();
                return MON && blurOn() && m_layer->needsBlur(MON);
            }
            virtual bool needsPrecomputeBlur(Render::CRenderContext&) override {
                return false;
            }
            virtual std::optional<CBox> boundingBox(Render::CRenderContext&) override {
                // monitor-local LOGICAL px
                const auto MON = m_mon.lock();
                return MON ? m_layer->boundingBox(MON) : std::nullopt;
            }
            virtual const char* passName() override {
                return "CAwesomePassElement";
            }
            virtual ePassElementType type() override {
                return EK_CUSTOM;
            }

          private:
            PHLMONITORREF m_mon;
            ILayer*       m_layer; // a static module layer: outlives every frame's pass
        };
    };

    // Damage a global box on every renderer; the margin covers hairlines
    // riding outside boxes, the glass's blur radius, and shadow range.
    inline void damageBox(const CBox& global) {
        if (!g_pHyprRenderer)
            return;
        g_pHyprRenderer->damageBox(global);
    }
    inline void damageMonitor(PHLMONITOR mon) {
        if (g_pHyprRenderer && mon)
            g_pHyprRenderer->damageMonitor(mon);
    }

} // namespace NAwesome
