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
// own damage). CWarmGate is that state machine; the rasterizers and caches
// stay per plugin (the bar keys a generation cache over the compositor's
// renderText, the cards raster their own wrapped/markup pango layouts).
#pragma once

#include "lifecycle.hpp"

#include <hyprland/src/Compositor.hpp>

#include <functional>

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

} // namespace NHyprCommon
