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
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/state/MonitorState.hpp>

using ITexture  = Render::ITexture;
using Render::GL::g_pHyprOpenGL;

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NAwesome {

    // ---- the warm/draw state machine (port of common/texcache.hpp) ----

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

        CBox toPhys(const CBox& global) const {
            return CBox{global}.translate(Vector2D{-mon->m_position.x, -mon->m_position.y + dy}).scale(scale).round();
        }
        void rect(const CBox& global, const CHyprColor& c, int round = 0, float rp = 2.f) const {
            if (warm)
                return;
            g_pHyprOpenGL->renderRect(toPhys(global), c.modifyA(c.a * alpha), {.round = round, .roundingPower = rp});
        }
        // Semantic container paint. Opaque defaults make the fork skip blur;
        // configured alpha below 1 retains the rounded live-glass path.
        void glass(const CBox& global, const CHyprColor& c, int round, float rp) const {
            if (warm)
                return;
            g_pHyprOpenGL->renderRect(toPhys(global), c.modifyA(c.a * alpha), {.round = round, .roundingPower = rp, .blur = blurOn(), .blurA = alpha});
        }
        void border(const CBox& global, const CHyprColor& c, int round, int sizePx, float rp) const {
            if (warm)
                return;
            g_pHyprOpenGL->renderBorder(toPhys(global), Config::CGradientValueData{c}, {.round = round, .roundingPower = rp, .borderSize = sizePx, .a = alpha});
        }
        void ring(const CBox& global, const CHyprColor& c, int round, float rp, double px = 1.0) const {
            if (warm)
                return;
            // the gradient ctor heap-allocates and OkLab-converts — memoize per color
            static std::unordered_map<uint64_t, Config::CGradientValueData> grads;
            const auto                                                      KEY = c.getAsHex();
            auto                                                            IT  = grads.find(KEY);
            if (IT == grads.end())
                IT = grads.emplace(KEY, Config::CGradientValueData{c}).first;
            g_pHyprOpenGL->renderBorder(toPhys(global), IT->second, {.round = round, .roundingPower = rp, .borderSize = std::max(1, (int)std::lround(px * scale)), .a = alpha});
        }
        void shadow(const CBox& global, int round, float rp, int range) const {
            if (warm)
                return;
            static Config::CGradientValueData GRAD{CHyprColor{Theme::SHADOW}};
            g_pHyprOpenGL->renderRoundedShadow(toPhys(global), round, rp, (int)std::lround(range * scale), GRAD, alpha);
        }
        // native px at a logical position
        void tex(const SP<ITexture>& t, double gx, double gy) const {
            if (warm || !t || t->m_texID == 0)
                return;
            const auto P = toPhys(CBox{gx, gy, 1, 1});
            g_pHyprOpenGL->renderTexture(t, CBox{(double)P.x, (double)P.y, t->m_size.x, t->m_size.y}, {.a = alpha});
        }
        // center the texture inside the cell at native size
        void texIn(const SP<ITexture>& t, const CBox& cell) const {
            if (warm || !t || t->m_texID == 0)
                return;
            const auto B = toPhys(cell);
            CBox       b{B.x + (B.w - t->m_size.x) / 2.0, B.y + (B.h - t->m_size.y) / 2.0, t->m_size.x, t->m_size.y};
            g_pHyprOpenGL->renderTexture(t, b.round(), {.a = alpha});
        }
        // Contain-fit: scale to fill the cell as far as aspect allows,
        // centered. renderTexture stretches to its box, so a non-square icon
        // handed a square cell comes out squashed — fit keeps proportions.
        void texFit(const SP<ITexture>& t, const CBox& cell, int round = 0, float rp = 2.f) const {
            if (warm || !t || t->m_texID == 0)
                return;
            const double TW = t->m_size.x, TH = t->m_size.y;
            if (TW <= 0 || TH <= 0)
                return;
            const auto   B = toPhys(cell);
            const double S = std::min(B.w / TW, B.h / TH);
            const double W = TW * S, H = TH * S;
            CBox         b{B.x + (B.w - W) / 2.0, B.y + (B.h - H) / 2.0, W, H};
            g_pHyprOpenGL->renderTexture(t, b.round(), {.a = alpha, .round = round, .roundingPower = rp});
        }
        // Fill the cell, aspect not preserved (the card's full-bleed images)
        void texStretch(const SP<ITexture>& t, const CBox& cell, int round = 0, float rp = 2.f) const {
            if (warm || !t || t->m_texID == 0)
                return;
            g_pHyprOpenGL->renderTexture(t, toPhys(cell), {.a = alpha, .round = round, .roundingPower = rp});
        }
    };

    // ---- layers ----

    struct ILayer {
        virtual ~ILayer() = default;
        virtual const char* name() const = 0;
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
            m_texturesTickPending = true;
            for (const auto& M : State::monitorState()->monitors())
                if (!only || M == only)
                    for (auto* L : m_layers)
                        L->warm(M);
            m_gate.endWarm();
        }

        // The pass element registers on RENDER_POST_WINDOWS: one pass per
        // monitor renders EVERY layer that claims that monitor.
        void onRenderStage(eRenderStage stage) {
            if (stage != RENDER_POST_WINDOWS)
                return;
            const auto MON = g_pHyprRenderer->m_renderData.pMonitor.lock();
            if (!MON)
                return;
            g_pHyprRenderer->addPassElement(makeUnique<CAwesomePassElement>(MON));
        }

        bool anyLayerVisible(PHLMONITOR mon) const {
            for (auto* L : m_layers)
                if (L->boundingBox(mon))
                    return true;
            return false;
        }

      private:
        friend class CAwesomePassElement;
        std::vector<ILayer*> m_layers;
        CWarmGate            m_gate;
        bool                 m_texturesTickPending = false;

        class CAwesomePassElement : public IPassElement {
          public:
            explicit CAwesomePassElement(PHLMONITOR mon) : m_mon(mon) {}
            virtual ~CAwesomePassElement() = default;

            virtual std::vector<UP<IPassElement>> draw() override {
                auto& C = Canvas::inst();
                C.m_gate.inRender = true;
                if (const auto MON = m_mon.lock()) {
                    SPaint ctx;
                    ctx.mon   = MON;
                    ctx.scale = MON->m_scale;
                    ctx.mb    = MON->logicalBox();
                    for (auto* L : C.m_layers)
                        L->draw(MON, ctx);
                }
                C.m_gate.inRender = false;

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
            virtual bool needsLiveBlur() override {
                // only while a layer actually paints — never claim a live
                // blur of a region that is hidden (blur for nothing)
                const auto MON = m_mon.lock();
                if (!MON || !blurOn())
                    return false;
                for (auto* L : Canvas::inst().m_layers)
                    if (L->needsBlur(MON))
                        return true;
                return false;
            }
            virtual bool needsPrecomputeBlur() override {
                return false;
            }
            virtual std::optional<CBox> boundingBox() override {
                // the union of the layers' boxes: monitor-local LOGICAL px
                const auto MON = m_mon.lock();
                if (!MON)
                    return std::nullopt;
                double minx = 0, miny = 0, maxx = 0, maxy = 0;
                bool   any  = false;
                for (auto* L : Canvas::inst().m_layers)
                    if (const auto B = L->boundingBox(MON)) {
                        minx = std::min(minx, (double)B->x);
                        miny = std::min(miny, (double)B->y);
                        maxx = std::max(maxx, (double)B->x + B->w);
                        maxy = std::max(maxy, (double)B->y + B->h);
                        any  = true;
                    }
                return any ? std::optional<CBox>{CBox{minx, miny, maxx - minx, maxy - miny}} : std::nullopt;
            }
            virtual const char* passName() override {
                return "CAwesomePassElement";
            }
            virtual ePassElementType type() override {
                return EK_CUSTOM;
            }

          private:
            PHLMONITORREF m_mon;
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
