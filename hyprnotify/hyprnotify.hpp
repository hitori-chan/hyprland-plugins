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
#include <hyprland/src/state/MonitorState.hpp>
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

#include <linux/input-event-codes.h>
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
        SP<Config::Values::CColorValue>  colBg;
        SP<Config::Values::CColorValue>  colFg;
        SP<Config::Values::CColorValue>  colFrame;     // card frame
        SP<Config::Values::CColorValue>  colUrgent;    // critical frame + critical progress
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
        int                  progress = -1; // 0..100 from the "value" hint, -1 = none
        std::string          image;         // resolved file path, "" = none
        std::vector<uint8_t> pixels;        // image-data, premultiplied BGRA (DRM ARGB8888)
        int                  pw = 0, ph = 0;
        std::string          defaultAction; // action key a left click invokes, "" = just dismiss

        float                timeoutMs = 0; // resolved; 0 = sticky
        Time::steady_tp      deadline;      // meaningful when timeoutMs > 0

        // render cache — built ONLY by the warm pass (the texture rule). The
        // *For keys say what the texture was built from so a replace only
        // rebuilds what actually changed (volume OSD keeps its title + icon).
        SP<ITexture>         titleTex, bodyTex, iconTex;
        std::string          titleFor, bodyFor, imageFor;
        std::vector<uint8_t> pixelsFor;
        int                  builtPt = 0, builtTextW = 0;
        uint64_t             builtFg = 0;
    };
    extern std::vector<SP<SNotif>> notifs;

    // ---- bus.cpp ----

    namespace Bus {
        // NotificationClosed reasons (the spec's)
        inline constexpr uint32_t R_EXPIRED = 1, R_DISMISSED = 2, R_CLOSED = 3;

        void                      pollSoon(); // pull the next DBus poll tick close after a send
        void                      init();
        void                      exit();
        void                      invokeAction(uint32_t id, const std::string& key);
        void                      closeOne(uint32_t id, uint32_t reason);
        void                      closeAll(uint32_t reason);
        void                      rearmExpiry();
    }

    // ---- icons.cpp ----

    // (Re)build n.iconTex when its source changed; maxPx caps the raster.
    void ensureIconTex(SNotif& n, int maxPx);

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
    void renderExit();

    // card boxes of the last layout, global logical — input hit-tests these
    struct SCard {
        CBox     box;
        uint32_t id = 0;
    };
    extern std::vector<SCard> cards;
    extern PHLMONITORREF      cardsMon; // the monitor the cards were laid out on

    // ---- input.cpp ----

    void onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info);
    void onMouseMove(const Vector2D& pos, Event::SCallbackInfo& info);
    void releasePointer();
    void inputExit();

} // namespace NHyprnotify
