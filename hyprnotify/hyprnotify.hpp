#pragma once
// hyprnotify — shared declarations between the plugin's translation units.
// The full picture lives at the top of main.cpp; per-module docs at the top
// of each unit:
//
//   bus.cpp     the org.freedesktop.Notifications daemon (sdbus-c++), the
//               notification model and its timeouts
//   icons.cpp   notification images: files via hyprgraphics, raw image-data
//   render.cpp  the cards, their textures, the pass element, damage
//   input.cpp   clicks and pointer ownership over the cards
//   main.cpp    plugin glue: config, listeners, init/exit
//
// Everything lives in NHyprnotify so no symbol can collide with another
// plugin's at dlopen time.

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>

#include <linux/input-event-codes.h>
#include <poll.h>
#include <wayland-server-core.h>
#include <sdbus-c++/sdbus-c++.h>
#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <hyprgraphics/image/Image.hpp>
#include <drm_fourcc.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// plugin-private header: the render types are used unqualified everywhere
using namespace Render;
using namespace Render::GL;

extern HANDLE PHANDLE;

namespace NHyprnotify {

    // one working number: PLUGIN_INIT and GetServerInformation both return it
    inline constexpr const char* VERSION = "2.0.3";

    // wide images render card-width ("hero") instead of icon-boxed
    inline constexpr double HERO_ASPECT = 1.5;

    // ---- config (defined in main.cpp, values arrive from theme.lua) ----

    struct SNotifyConfig {
        SP<Config::Values::CStringValue> font;
        SP<Config::Values::CIntValue>    fontSize;      // logical px; monitor scale applies at raster time
        SP<Config::Values::CIntValue>    width;         // card width, logical px
        SP<Config::Values::CIntValue>    maxHeight;     // card height cap
        SP<Config::Values::CIntValue>    maxIcon;       // image box cap (the old naughty icon_size)
        SP<Config::Values::CIntValue>    margin;        // screen-edge gap AND inter-card gap
        SP<Config::Values::CIntValue>    offsetY;       // first card's distance from the monitor top (below the bar)
        SP<Config::Values::CIntValue>    timeoutLow;    // ms; urgency defaults when the client sends -1
        SP<Config::Values::CIntValue>    timeoutNormal; // ms; critical never times out
        SP<Config::Values::CIntValue>    rounding;  // corner radius, logical px
        SP<Config::Values::CIntValue>    maxNotifs; // model cap; overflow evicts oldest non-critical
        SP<Config::Values::CStringValue> fallbackIconDir; // iconless cards draw a random image from here
        SP<Config::Values::CColorValue>  colBg;
        SP<Config::Values::CColorValue>  colFg;        // body text
        SP<Config::Values::CColorValue>  colTitle;     // summary line
        SP<Config::Values::CColorValue>  colKicker;    // app-name kicker + hovered frame
        SP<Config::Values::CColorValue>  colFrame;     // card frame
        SP<Config::Values::CColorValue>  colUrgent;    // critical frame/kicker + critical progress
        SP<Config::Values::CColorValue>  colHighlight; // progress bar
    };
    extern SNotifyConfig cfg;

    CHyprColor           color(const SP<Config::Values::CColorValue>& v);

    // ---- the model (bus.cpp) ----

    struct SNotif {
        uint32_t             id = 0;
        std::string          appName;
        std::string          summary; // newlines flattened
        std::string          body;    // markup stripped (the server never advertises body-markup)
        uint8_t              urgency  = 1;
        int                  progress = -1;     // 0..100 from the "value" hint, -1 = none
        std::string          image;             // resolved file path, "" = none
        std::vector<uint8_t> pixels;            // image-data, premultiplied BGRA (DRM ARGB8888); freed once uploaded
        bool                 hasPixels = false; // the LAST Notify carried image-data (outlives the freed buffer)
        int                  pw = 0, ph = 0;
        std::string          defaultAction; // action key a left click invokes, "" = just dismiss

        std::string          fallbackPick;    // the card's rolled fallback image; survives in-place replaces
        bool                 waiting = false; // arrived while suspended (DND): collected, not shown, timeout held

        float                timeoutMs = 0; // resolved; 0 = sticky
        Time::steady_tp      deadline;      // meaningful when timeoutMs > 0 and not waiting

        // render cache — built ONLY by the warm pass (the texture rule). The
        // *For keys say what the texture was built from so a replace only
        // rebuilds what actually changed (volume OSD keeps its title + icon);
        // pixels are hashed, not copied, and freed once uploaded.
        SP<ITexture> kickerTex, titleTex, bodyTex, iconTex;
        std::string  kickerFor, titleFor, bodyFor, imageFor;
        bool         heroTex = false; // iconTex was built for the hero layout
        uint64_t     pixelsFor = 0;
        int          builtPt = 0, builtTextW = 0;
        uint64_t     builtFg = 0;
        bool         builtCrit = false; // criticality recolors the kicker
    };
    extern std::vector<SP<SNotif>> notifs;

    // ---- bus.cpp ----

    namespace Bus {
        // NotificationClosed reasons (the spec's); 4 = undefined, the eviction
        inline constexpr uint32_t R_EXPIRED = 1, R_DISMISSED = 2, R_CLOSED = 3, R_UNDEFINED = 4;

        void                      pollSoon(); // pull the next DBus poll tick close after a send
        void                      init();
        void                      exit();
        void                      invokeAction(uint32_t id, const std::string& key);
        void                      closeOne(uint32_t id, uint32_t reason);
        void                      closeAll(uint32_t reason); // visible cards only: a sweep never kills the DND queue
        void                      rearmExpiry();
        void                      toggleSuspend(); // naughty.suspend: resume renders the queue, fresh timeouts
    }

    // ---- icons.cpp ----

    // (Re)build n.iconTex when its source changed. iconPx caps the icon-box
    // raster; sources wider than HERO_ASPECT (and at least half the hero box)
    // raster to heroWPx instead, cover-cropped to heroHCapPx, and set heroTex.
    void ensureIconTex(SNotif& n, int iconPx, int heroWPx, int heroHCapPx);

    // Forget the fallback_icon_dir listing (a config reload rescans).
    void resetFallbackCache();

    // Downscale n.pixels in place when it exceeds maxPx (unpack-time cap).
    void shrinkPixels(SNotif& n, int maxPx);

    // ---- the texture rule (see hyprbar) ----
    //
    // A texture cannot be painted by the frame that created it, and creating
    // one mid-draw silently swallows every later draw in the same element.
    // Textures are built ONLY by the warm pass, from the event loop:
    extern bool warming;  // warmNotifs() is building; the ONLY time a texture may be created
    extern bool texStale; // a draw ran ahead of the screen -> warm + repaint

    // ---- render.cpp ----

    void warmNotifs();   // build every texture the next frame will paint; no-ops inside a render
    void damageNotifs(); // damage the previous card column and the freshly-laid-out one

    // The model changed: rebuild textures and damage, deferred to the event
    // loop; bursts (an OSD volume sweep) coalesce into one warm.
    void notifChanged();

    void onRenderStage(eRenderStage stage);
    // render.preChecks: keep a visible card compositing over a solitary
    // fullscreen window (else scanout/solitary-render skips the notify pass)
    void onRenderPreChecks(PHLMONITOR mon);
    void renderExit();

    // card boxes of the last layout, global logical — input hit-tests these
    struct SCard {
        CBox     box;
        uint32_t id = 0;
    };
    extern std::vector<SCard> cards;
    extern PHLMONITORREF      cardsMon; // the monitor the cards were laid out on

    // hover affordance: the frame under the pointer warms to the kicker color.
    // 0 = none; a change damages the cards involved (no textures move).
    void setHovered(uint32_t id);

    // ---- input.cpp ----

    void onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info);
    void onMouseMove(const Vector2D& pos, Event::SCallbackInfo& info);
    void releasePointer();
    void refreshPointerOwnership(); // the hovered card vanished under a still pointer
    void inputExit();

} // namespace NHyprnotify
