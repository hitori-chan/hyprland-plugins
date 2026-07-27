// common/texcache.hpp — the warm/draw state machine shared by the
// compositor-drawn surfaces (hyprbar's strip, hyprnotify's cards).
//
// The texture rule (crash class 4): a texture created inside a frame cannot
// be painted by that same frame — wherever in the frame it was created —
// and the miss silently swallows everything drawn after it in the element.
// So textures are built ONLY by a warm pass running from the event loop,
// one frame ahead; the draw pass paints cache hits and never builds. A draw
// that finds a texture missing flags the gate, and the pass element backs
// out to the event loop to warm and repaint (every shown state needs its
// own damage). CWarmGate is that state machine, and CGenCache below is what
// both plugins key their rasters into. The RASTERIZERS stay per plugin: the
// bar hands short labels to the compositor's renderText, the cards build
// their own wrapped/markup pango layouts.
#pragma once

#include "lifecycle.hpp"

#include <hyprland/src/Compositor.hpp>

#include <functional>
#include <string>
#include <unordered_map>

namespace NHyprCommon {

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
        // A TOKEN IS NOT RE-ENTRY. It only says "building is allowed here", and
        // a warm inside one must still run: hyprbar's tray menu damages — and
        // so warms — from inside the token its dbusmenu reply holds, and while
        // that counted as re-entry the warm silently no-opped. The layout never
        // ran, so the menu was damaged at the box it had BEFORE its rows
        // arrived (measured: 180x32 for a panel that draws 460x490), and the
        // rows painted only inside that sliver until an unrelated damage swept
        // them. Hence the separate pass flag, and the saved permission below.
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
        // Never construct one inside a render. Warming from inside a token IS
        // fine (beginWarm above) — that is the usual shape, since the damage
        // that follows the resolve has to lay the surface out first.
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
    // over simply misses to a new one. What does need bounding is the MAP —
    // title churn would grow it without limit — and the bound is a grace
    // generation rather than a size cap: evict only what no recent warm asked
    // for, so no single warm is ever left rebuilding everything at once.
    //
    // LIFE is how many warms an untouched entry survives. It differs by
    // surface, which is the only reason it is a parameter: the bar warms on
    // every clock tick and wants a longer grace than the shade, which warms
    // only when its model changes.
    template <typename TValue, uint64_t LIFE = 32>
    class CGenCache {
      public:
        // the entry if present, TOUCHED so this generation's sweep spares it
        TValue* find(const std::string& key) {
            const auto IT = m_map.find(key);
            if (IT == m_map.end())
                return nullptr;
            IT->second.gen = m_gen;
            return &IT->second.val;
        }

        TValue* insert(const std::string& key, TValue&& val) {
            auto& E = m_map[key];
            E.val   = std::move(val);
            E.gen   = m_gen;
            return &E.val;
        }

        void tick() {
            m_gen++;
        }

        // Call ONLY from a warm that enumerated every surface: a scoped warm
        // (one monitor, one menu) never asks for the textures it left alone,
        // so ageing on one would evict them for having been skipped.
        void sweep() {
            if (m_gen > LIFE)
                std::erase_if(m_map, [this](const auto& E) { return E.second.gen + LIFE < m_gen; });
        }

        void clear() {
            m_map.clear();
        }

      private:
        struct SEntry {
            TValue   val;
            uint64_t gen = 0;
        };
        std::unordered_map<std::string, SEntry> m_map;
        uint64_t                                m_gen = 0;
    };

} // namespace NHyprCommon
