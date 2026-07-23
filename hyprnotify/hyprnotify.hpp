#pragma once
// hyprnotify — shared declarations between the plugin's translation units.
// The full picture lives at the top of main.cpp; per-module docs at the top
// of each unit:
//
//   bus.cpp     the org.freedesktop.Notifications daemon (sdbus-c++), the
//               model, residency, history, append, the state interface
//   icons.cpp   notification images: content avatars, identity icons,
//               raw image-data
//   text.cpp    the pango rasterizer + the keyed text cache + markup helpers
//   paint.cpp   the paint context, shared card recipes, type scale, motion
//   popups.cpp  the banner column (the one-card anatomy, hover-✕, springs)
//   center.cpp  the two-view center: shade/history, rows, 3-state groups
//   render.cpp  the render skeleton: warm/draw, damage, ticks, the pass
//               element (surface machinery shared through ui.hpp)
//   input.cpp   clicks, wheel paging, esc, pointer ownership
//   main.cpp    plugin glue: config, listeners, init/exit
//
// Everything lives in NHyprnotify so no symbol can collide with another
// plugin's at dlopen time.

#include "common/glass.hpp"

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
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
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
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// plugin-private header: the render types are used unqualified everywhere
using namespace Render;
using namespace Render::GL;

extern HANDLE PHANDLE;

namespace NHyprnotify {

    // one working number: PLUGIN_INIT and GetServerInformation both return it
    inline constexpr const char* VERSION = "4.0.3";

    // wide images render card-width ("hero") instead of icon-boxed
    inline constexpr double HERO_ASPECT = 1.5;

    // the OSD scripts pin ids here: replace-in-place, never appended, never
    // grouped, never history; fresh ids and recalls never mint into it
    inline constexpr uint32_t OSD_LO = 9990, OSD_HI = 9999;
    inline constexpr bool     inOsdBand(uint32_t id) {
        return id >= OSD_LO && id <= OSD_HI;
    }

    // ---- config (defined in main.cpp, values arrive from theme.lua) ----

    struct SNotifyConfig {
        SP<Config::Values::CStringValue> font;
        SP<Config::Values::CIntValue>    fontSize;      // body size, logical px; monitor scale applies at raster time
        SP<Config::Values::CIntValue>    width;         // popup card width, logical px (the center panel is width+12)
        SP<Config::Values::CIntValue>    maxHeight;     // popup card height cap
        SP<Config::Values::CIntValue>    maxIcon;       // popup icon column (center rows derive 34/28 from it)
        SP<Config::Values::CIntValue>    margin;        // screen-edge gap AND inter-card gap
        SP<Config::Values::CIntValue>    offsetY;       // popups' and the center's distance from the monitor top
        SP<Config::Values::CIntValue>    timeoutLow;    // ms; urgency defaults when the client sends -1
        SP<Config::Values::CIntValue>    timeoutNormal; // ms; critical never times out
        SP<Config::Values::CIntValue>    rounding;      // card radius; the panel (+6) and rows (-2) derive from it
        SP<Config::Values::CFloatValue>  roundingPower; // superellipse exponent, the compositor's rounding_power
        SP<Config::Values::CIntValue>    maxNotifs;     // model cap; overflow evicts oldest non-critical
        SP<Config::Values::CIntValue>    maxHistory;    // retained cap; 0 disables history
        SP<Config::Values::CIntValue>    ignoreDbusClose; // ignore app-initiated CloseNotification (dunst's knob)
        SP<Config::Values::CColorValue>  colBg;         // glass fill (alpha = the glass)
        SP<Config::Values::CColorValue>  colFg;         // body text
        SP<Config::Values::CColorValue>  colTitle;      // card titles
        SP<Config::Values::CColorValue>  colKicker;     // header/age/secondary text
        SP<Config::Values::CColorValue>  colFrame;      // hairlines
        SP<Config::Values::CColorValue>  colUrgent;     // critical accents
        SP<Config::Values::CColorValue>  colHighlight;  // the accent: progress, actions, selections
        SP<Config::Values::CColorValue>  colLink;       // body hyperlinks
        SP<Config::Values::CStringValue> soundCommand;  // libcanberra player; "" disables sound
    };
    extern SNotifyConfig cfg;

    using NHyprCommon::color; // the memoized config-color fetch (common/glass.hpp)

    // Fire-and-forget a child, reaped via pidfd off the event loop (used for
    // hyperlink opening and notification sounds); never blocks render/input.
    void spawnDetached(std::vector<const char*> argv);

    // ---- the model (bus.cpp) ----

    // a non-"default" action: a clickable text button on the card
    struct SAction {
        std::string  id;      // ActionInvoked key; also the icon name under action-icons
        std::string  label;   // localized button text
        SP<ITexture> iconTex; // resolved action-icon (warm; action-icons only)
        std::string  iconFor; // staleness: the id the icon was resolved from
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
        std::string          appKey;  // grouping identity: desktop-entry, else the app name
        std::string          summary; // newlines flattened, whitelisted markup
        std::string          body;    // whitelisted markup (Pango subset)
        uint8_t              urgency  = 1;
        int                  progress = -1; // 0..100 from the "value" hint, -1 = none
        std::string          image;    // CONTENT source (image-path), resolved file path, "" = none
        std::string          identity; // IDENTITY source (app_icon/desktop-entry), resolved path, "" = none
        std::vector<uint8_t> pixels;   // image-data, premultiplied BGRA (DRM ARGB8888); freed once uploaded
        bool                 hasPixels = false; // the LAST Notify carried image-data (outlives the freed buffer)
        int                  pw = 0, ph = 0;
        std::string          defaultAction; // action key a body click invokes, "" = just dismiss
        std::vector<SAction>    actions;    // non-default actions -> buttons, in Notify order
        std::vector<SBodyImage> bodyImages; // body <img src> thumbnails
        bool                    actionIcons = false; // the action-icons hint: button ids are icon names
        bool                    resident    = false; // the resident hint: an action keeps the card
        bool                    transient   = false; // the transient hint: bypass history AND residency

        bool                 waiting = false; // arrived while suspended (DND): collected, not shown, timeout held
        bool                 banner  = true;  // the popup is up; expiry drops only this — the card stays resident

        float                timeoutMs = 0; // resolved; 0 = sticky
        Time::steady_tp      deadline;      // meaningful when banner && timeoutMs > 0 and not waiting
        Time::steady_tp      arrived;       // Notify arrival (a replace refreshes it); the age lines
        Time::steady_tp      born;          // creation only (a replace keeps it); the arrival spring's key
        uint64_t             hseq = 0;      // history identity while retired; 0 = live

        // image textures — built ONLY by the warm pass (the texture rule).
        // Text rasters live in render.cpp's keyed cache; only the decoded
        // images cache here (their sources don't re-key per age tick).
        SP<ITexture> iconTex;  // content avatar (or hero)
        SP<ITexture> identTex; // identity icon: the corner badge, or the lead icon when no content
        std::string  imageFor, identFor;
        bool         heroTex   = false; // iconTex was built for the hero layout
        uint64_t     pixelsFor = 0;
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
        // best-effort invoke on a retired card: ActionInvoked with the
        // ORIGINAL id (apps still tracking it react), the entry is consumed
        void                      invokeHistoryAction(uint64_t hseq, const std::string& key);
        void                      closeOne(uint32_t id, uint32_t reason);
        void                      dismissAllLive(); // sweep/Clear all: every visible card -> history; the DND queue stays
        void                      dismissApp(const std::string& appKey); // a shade group's right-click
        void                      rearmExpiry();
        void                      toggleSuspend(); // DND; resume renders the queue, fresh timeouts
        bool                      suspendedNow();
        void                      recall();                // re-display the most recently retired card
        bool                      recallAt(uint64_t hseq); // re-display one specific retained card
        void                      eraseHistory(uint64_t hseq);
        void                      eraseHistoryApp(const std::string& appKey); // a history group's delete
        void                      clearHistory();
        const std::vector<SP<SNotif>>& historyView(); // retained cards, oldest first
        size_t                    historySize();
        std::string               stateString(); // "center:N live:N hist:N dnd:N"
        void                      emitStateSoon(); // coalesced org.hitori.hyprnotify State signal (the bar's bell)
    }

    // ---- icons.cpp ----

    // (Re)build n.iconTex (content) and n.identTex (identity) when their
    // sources changed. iconPx caps the icon-box raster; content sources wider
    // than HERO_ASPECT (and at least half the hero box) raster to heroWPx
    // instead, cover-cropped to heroHCapPx, and set heroTex.
    void ensureIconTex(SNotif& n, int iconPx, int heroWPx, int heroHCapPx);

    // (Re)build an action button's icon when action-icons is set and its id (an
    // icon name or a path) changed; clears it when the hint is off.
    void ensureActionIcon(SNotif& n, SAction& a, int iconPx);

    // (Re)build a body <img> thumbnail when its src changed. maxPx caps the
    // decoded raster.
    void ensureBodyImage(SBodyImage& im, int maxPx);

    // Downscale n.pixels in place when it exceeds maxPx (unpack-time cap).
    void shrinkPixels(SNotif& n, int maxPx);

    // ---- render.cpp ----

    void warmNotifs();   // build every texture the next frame will paint; no-ops inside a render
    void damageNotifs(); // damage the previous layout and the fresh one

    // The model changed: rebuild textures and damage, deferred to the event
    // loop; bursts (an OSD volume sweep) coalesce into one warm.
    void notifChanged();

    // the two-view center (shade | history)
    bool centerVisible();
    void setCenter(bool on);      // event-loop only (input/hyprctl defer through main.cpp's queue)
    void centerFlipView();        // the bottom bar's ⏱
    bool centerInHistory();
    void centerPage(int dir);     // wheel: >0 towards older rows
    void centerToggleGroup(const std::string& appKey, bool hist);
    void centerToggleRow(uint32_t id, uint64_t hseq); // a chevron: live or history/child

    void onRenderStage(eRenderStage stage);
    // render.preChecks: keep a visible card compositing over a solitary
    // fullscreen window (else scanout/solitary-render skips the notify pass)
    void onRenderPreChecks(PHLMONITOR mon);
    void renderInit(); // the age/motion tick timers
    void renderExit();

    // hit rects of the last layout, global logical — input hit-tests these
    struct SCard {
        enum eKind : uint8_t {
            POPUP = 0,
            ROW,       // a center row: live (id) or history (hseq)
            DIGEST,    // a folded group card (group = app key)
            GHEAD,     // an expanded group's header row
            CHILD,     // a group child row: live (id) or history (hseq)
            BTN_HIST,  // bottom bar ⏱
            BTN_CLEAR, // bottom bar "Clear all"/"Clear history"
            BTN_DND,   // bottom bar ⊖
            PANEL,     // the center panel body: swallows clicks, owns the wheel
        };
        eKind       kind = POPUP;
        CBox        box;
        uint32_t    id   = 0; // live identity
        uint64_t    hseq = 0; // history identity
        std::string group;    // DIGEST/GHEAD/CHILD: the app key
        bool        hist = false; // the row lives in the history view
        CBox        chevron;      // ROW/CHILD: the 24Ø circle; w = 0 -> none
        CBox        close;        // POPUP hover-✕ / GHEAD ✕; w = 0 -> none
        struct SBtn {
            CBox        box;
            std::string id;
        };
        std::vector<SBtn> buttons; // action-button hit rects (global logical)
        struct SLinkHit {
            CBox        box;
            std::string href;
        };
        std::vector<SLinkHit> links; // body-hyperlink hit rects (POPUP only)
    };
    extern std::vector<SCard> cards;
    extern PHLMONITORREF      cardsMon; // the monitor the layout ran on

    // hover affordance: rows/buttons warm under the pointer. `btn` -1 = the
    // surface itself, >= 0 = that action button; `part` distinguishes the
    // chevron/✕ corners. A change damages only the boxes involved.
    struct SHover {
        uint32_t    id   = 0;
        uint64_t    hseq = 0;
        std::string group;
        SCard::eKind kind = SCard::POPUP;
        int         btn  = -1;
        uint8_t     part = 0; // 0 body, 1 chevron, 2 close
        bool        operator==(const SHover&) const = default;
    };
    void setHovered(const SHover& h);

    // ---- input.cpp ----

    void onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info);
    void onMouseMove(const Vector2D& pos, Event::SCallbackInfo& info);
    void onMouseAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info);
    void onKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info); // esc peels the center
    void releasePointer();
    void refreshPointerOwnership(); // the hovered card vanished under a still pointer
    void inputExit();

    // main.cpp: the deferred center toggle every entry point funnels through
    // (bell click over the bus, hyprctl, Lua, F12's user bind)
    void queueCenterToggle();

} // namespace NHyprnotify
