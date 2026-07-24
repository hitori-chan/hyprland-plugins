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
    inline constexpr const char* VERSION = "5.1.0";

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
        SP<Config::Values::CIntValue>    timeoutLow;    // ms; the -1 fallback for ephemerals (low/transient/progress)
        SP<Config::Values::CIntValue>    timeoutNormal; // ms; the -1 fallback otherwise; 0 (default) = sticky
        SP<Config::Values::CIntValue>    rounding;  // corner radius, logical px
        SP<Config::Values::CIntValue>    maxNotifs;  // model cap; overflow evicts oldest non-critical
        SP<Config::Values::CIntValue>    maxHistory; // retained-for-recall cap; 0 disables history
        SP<Config::Values::CStringValue> fallbackIconDir; // iconless cards draw a random image from here
        SP<Config::Values::CColorValue>  colBg;
        SP<Config::Values::CColorValue>  colFg;        // body text
        SP<Config::Values::CColorValue>  colTitle;     // summary line
        SP<Config::Values::CColorValue>  colKicker;    // app-name kicker + hovered frame
        SP<Config::Values::CColorValue>  colFrame;     // card frame
        SP<Config::Values::CColorValue>  colUrgent;    // critical frame/kicker + critical progress
        SP<Config::Values::CColorValue>  colHighlight; // progress bar
        SP<Config::Values::CColorValue>  colLink;      // body hyperlinks
        SP<Config::Values::CStringValue> soundCommand; // libcanberra player; "" disables sound
    };
    extern SNotifyConfig cfg;

    CHyprColor           color(const SP<Config::Values::CColorValue>& v);

    // Fire-and-forget a child, reaped via pidfd off the event loop (used for
    // hyperlink opening and notification sounds); never blocks render/input.
    void spawnDetached(std::vector<const char*> argv);

    // ---- the model (bus.cpp) ----

    // a non-"default" action: a clickable button on the card
    struct SAction {
        std::string  id;    // ActionInvoked key; also the icon name under action-icons
        std::string  label; // localized button text
        SP<ITexture> tex;      // rendered label (warm)
        SP<ITexture> iconTex;  // resolved action-icon (warm; action-icons only)
        std::string  builtFor; // staleness: label the tex was built from
        std::string  iconFor;  // staleness: the id the icon was resolved from
    };

    // a body hyperlink (<a href>): a clickable region opening its URL
    struct SLink {
        std::string href;
        CBox        rel; // logical rect relative to the body texture's top-left (built by warm)
    };

    // a body <img src>: a thumbnail rendered below the text
    struct SBodyImage {
        std::string  src;      // resolved file path
        SP<ITexture> tex;      // built by warm
        std::string  builtFor; // staleness: the src the tex was built from
    };

    struct SNotif {
        uint32_t             id = 0;
        std::string          appName;
        std::string          summary; // newlines flattened, whitelisted markup
        std::string          body;    // whitelisted markup (Pango subset)
        uint8_t              urgency  = 1;
        int                  progress = -1;     // 0..100 from the "value" hint, -1 = none
        std::string          image;             // resolved file path, "" = none
        std::vector<uint8_t> pixels;            // image-data, premultiplied BGRA (DRM ARGB8888); freed once uploaded
        bool                 hasPixels = false; // the LAST Notify carried image-data (outlives the freed buffer)
        int                  pw = 0, ph = 0;
        std::string          defaultAction; // action key a body click invokes, "" = just dismiss
        std::vector<SAction>    actions;          // non-default actions -> buttons, in Notify order
        std::vector<SLink>      links;            // body <a href> hit regions (relative; built by warm)
        std::vector<SBodyImage> bodyImages;       // body <img src> thumbnails
        bool                    actionIcons = false; // the action-icons hint: button ids are icon names
        bool                    resident    = false; // the resident hint: an action keeps the card
        bool                    transient   = false; // the transient hint: bypass history on close

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
        void                      recall();        // re-display the most recently closed retained notification
        size_t                    historySize();   // retained (recallable) notifications
    }

    // ---- icons.cpp ----

    // (Re)build n.iconTex when its source changed. iconPx caps the icon-box
    // raster; sources wider than HERO_ASPECT (and at least half the hero box)
    // raster to heroWPx instead, cover-cropped to heroHCapPx, and set heroTex.
    void ensureIconTex(SNotif& n, int iconPx, int heroWPx, int heroHCapPx);

    // (Re)build an action button's icon when action-icons is set and its id (an
    // icon name or a path) changed; clears it when the hint is off.
    void ensureActionIcon(SNotif& n, SAction& a, int iconPx);

    // (Re)build a body <img> thumbnail when its src changed. maxPx caps the
    // decoded raster.
    void ensureBodyImage(SBodyImage& im, int maxPx);

    // Forget the fallback_icon_dir listing (a config reload rescans).
    void resetFallbackCache();

    // Resolve a freedesktop icon NAME (app_icon / image-path / desktop-entry /
    // an action key) to a file path via themed lookup; "" if unresolved or if
    // the string is already a path. Cached per name. A config reload clears it.
    std::string resolveIconName(const std::string& name, int sizePx);
    void        resetIconThemeCache();

    // Downscale n.pixels in place when it exceeds maxPx (unpack-time cap).
    void shrinkPixels(SNotif& n, int maxPx);

    // ---- the texture rule (see hyprbar) ----

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
        struct SBtn {
            CBox        box;
            std::string id;
        };
        std::vector<SBtn> buttons; // action-button hit rects (global logical)
        struct SLinkHit {
            CBox        box;
            std::string href;
        };
        std::vector<SLinkHit> links; // body-hyperlink hit rects (global logical)
    };
    extern std::vector<SCard> cards;
    extern PHLMONITORREF      cardsMon; // the monitor the cards were laid out on

    // hover affordance: the frame under the pointer warms to the kicker color,
    // or a specific action button highlights. id 0 = none; btn -1 = the frame,
    // >=0 = that button index. A change damages the cards involved (no textures
    // move).
    void setHovered(uint32_t id, int btn = -1);

    // ---- input.cpp ----

    void onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info);
    void onMouseMove(const Vector2D& pos, Event::SCallbackInfo& info);
    void releasePointer();
    void refreshPointerOwnership(); // the hovered card vanished under a still pointer
    void inputExit();

} // namespace NHyprnotify
