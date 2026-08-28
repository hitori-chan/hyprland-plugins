#pragma once
// hyprnotify — shared declarations between the plugin's translation units.
// The full picture lives at the top of main.cpp; per-module docs at the top
// of each unit:
//
//   bus.cpp     the org.freedesktop.Notifications connection: the object,
//               the vtables, the signals, the name
//   parse.cpp   the untrusted payload: markup, images, bounded fields
//   model.cpp   the cards: arrival, residency, conversations, DND, expiry
//   policy.cpp  the user's own rules: silenced apps, priority chats
//   icons.cpp   notification images: content avatars, identity icons,
//               raw image-data
//   text.cpp    the pango rasterizer + the keyed text cache + markup helpers
//   paint.cpp   the paint context, shared card recipes, type scale, motion
//   popups.cpp  the banner column (the one-card anatomy, springs)
//   row.cpp     one shade row in its two states, and the bundle recipes
//   center.cpp  the shade: grouping, explicit expansion, paging, the panel
//   render.cpp  the render skeleton: warm/draw, damage, ticks, the pass
//               element (surface machinery shared through ui.hpp)
//   input.cpp   clicks, wheel paging, pointer ownership, reply-key delegation
//   reply.cpp   the inline-reply field: its state, its keys, its drawing
//   main.cpp    plugin glue: config, listeners, init/exit
//
// Everything lives in NHyprnotify so no symbol can collide with another
// plugin's at dlopen time.

#include "common/glass.hpp"
#include "markup_attr.hpp"
#include "pixel_model.hpp"

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
#include <hyprland/src/render/AsyncResourceGatherer.hpp>
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
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// plugin-private header: the render types are used unqualified everywhere
using namespace Render;
using namespace Render::GL;

namespace NHyprnotify {

    extern HANDLE PHANDLE;

    // one working number: PLUGIN_INIT and GetServerInformation both return it
    inline constexpr const char* VERSION                = "7.3.0";
    inline constexpr int         DEFAULT_ROUNDING       = 28; // the ROM's 28dp card corner
    inline constexpr float       DEFAULT_ROUNDING_POWER = 2.0f; // superellipse exponent: 2 = circular arc (the AOSP corner), higher = flat shoulders toward a square

    // the OSD scripts pin ids here: replace-in-place, never appended, never
    // grouped, never history; fresh ids and recalls never mint into it
    inline constexpr uint32_t OSD_LO = 9990, OSD_HI = 9999;
    inline constexpr bool     inOsdBand(uint32_t id) {
        return id >= OSD_LO && id <= OSD_HI;
    }

    // ---- config (defined in main.cpp; shared semantic defaults, runtime overrides) ----

    struct SNotifyConfig {
        SP<Config::Values::CStringValue> theme;        // "glass" (the AOSP frost, default) or "ink" (opaque): the v13 material set (ui.hpp's v13())
        SP<Config::Values::CStringValue> font;
        SP<Config::Values::CIntValue>    fontSize;      // body size, logical px; monitor scale applies at raster time
        SP<Config::Values::CIntValue>    width;         // popup card width, logical px (the shade has its own CENTER_W)
        SP<Config::Values::CIntValue>    maxHeight;     // popup card height cap
        SP<Config::Values::CIntValue>    maxIcon;       // popup icon column; shade rows are fixed (ROW_ICON/CHILD_ICON) and only raster at this cap
        SP<Config::Values::CIntValue>    margin;        // inter-card and stack gap
        SP<Config::Values::CIntValue>    offsetY;       // popups' and the center's distance from the monitor top
        SP<Config::Values::CIntValue>    timeoutLow;    // ms; the -1 fallback for ephemerals (low/transient/progress)
        SP<Config::Values::CIntValue>    timeoutNormal; // ms; the -1 fallback for normal urgency, then it retreats to the shade; 0 = sticky (critical always is)
        SP<Config::Values::CIntValue>    coalescePopups; // 1 = at most one live popup per app; same-app extras land resident + silent
        SP<Config::Values::CIntValue>    quietFullscreen; // 1 = opt-in hold while a real fullscreen window owns the monitor; 0 = show banners
        SP<Config::Values::CIntValue>    snoozeSeconds;   // how long a snoozed card stays out of sight before it alerts again
        SP<Config::Values::CIntValue>    rounding;        // outer notification radius; shade/internal radii are explicit Pixel values
        SP<Config::Values::CFloatValue>  roundingPower; // superellipse exponent, the compositor's rounding_power
        SP<Config::Values::CIntValue>    maxNotifs;     // model cap; overflow evicts oldest non-critical
        SP<Config::Values::CIntValue>    ignoreDbusClose; // ignore app-initiated CloseNotification (dunst's knob)
        SP<Config::Values::CColorValue>  colBg;         // glass fill (alpha = the glass)
        SP<Config::Values::CColorValue>  colFg;         // body text
        SP<Config::Values::CColorValue>  colTitle;      // card titles
        SP<Config::Values::CColorValue>  colKicker;     // header/age/secondary text
        SP<Config::Values::CColorValue>  colFrame;      // hairlines
        SP<Config::Values::CColorValue>  colUrgent;     // critical accents
        SP<Config::Values::CColorValue>  colHighlight;  // the accent: progress, actions, selections
        SP<Config::Values::CColorValue>  colLink;       // body hyperlinks
        SP<Config::Values::CColorValue>  colSurface;    // translucent notification/row frost
        SP<Config::Values::CColorValue>  colSurfaceHigh; // raised controls and chips
        SP<Config::Values::CColorValue>  colState;       // hover/selected interaction layer
        SP<Config::Values::CColorValue>  colOnHighlight; // text/icons on a highlight fill
        SP<Config::Values::CColorValue>  colOnUrgent;    // text/icons on an urgent/error fill
        SP<Config::Values::CStringValue> soundCommand;  // libcanberra player; "" disables sound
    };
    extern SNotifyConfig cfg;

    using NHyprCommon::color; // the memoized config-color fetch (common/glass.hpp)

    // Fire-and-forget a child, reaped via pidfd off the event loop (used for
    // hyperlink opening and notification sounds); never blocks render/input.
    void spawnDetached(std::vector<const char*> argv);

    // ---- the model (model.cpp) ----

    // a non-"default" action: a clickable text button on the card
    struct SAction {
        std::string  id;      // ActionInvoked key; also the icon name under action-icons
        std::string  label;   // localized button text
        SP<ITexture> iconTex; // resolved action-icon (warm; action-icons only)
        std::string  iconFor; // staleness: the id the icon was resolved from
        int          iconPx = 0;
        bool         iconSettled = false; // current request decoded or failed
    };

    // a body hyperlink (<a href>): a clickable region opening its URL
    struct SLink {
        std::string href;
        CBox        rel; // logical rect relative to the body texture's top-left (built by warm)
    };

    // a body <img src>: a thumbnail rendered below the text
    struct SBodyImage {
        std::string  src;      // resolved file path
        std::string  alt;      // alt text kept as body fallback when image load fails
        SP<ITexture> tex;      // built by warm
        std::string  builtFor; // staleness: the source + target cap
        int          builtPx = 0;
        bool         settled = false; // current request decoded or failed
    };

    struct SMessage {
        std::string id;
        std::string senderId;
        std::string senderName;
        std::string senderIcon;
        std::string text;
        int64_t     timestampMs = 0;
        bool        historic    = false;
    };

    struct SParticipant {
        std::string  key;
        std::string  name;
        std::string  iconSource;
        std::string  icon;
        SP<ITexture> avatarTex;
        std::string  avatarFor;
        int          avatarPx      = 0;
        bool         avatarSettled = false;
    };

    struct SNotif {
        uint32_t             id = 0;
        std::string          appName;
        std::string          appKey;  // grouping identity: desktop-entry, else the app name
        std::string          desktopEntry; // raw freedesktop desktop-entry identity
        std::string          summary; // newlines flattened, whitelisted markup
        std::string          body;    // whitelisted markup (Pango subset)
        std::string               conversationId; // explicit stable chat identity; empty means no merge contract
        std::string               conversationTitle;
        std::string               conversationKind; // one-to-one or group
        std::string               conversationIconSource;
        std::string               conversationIcon;
        std::string               declaredGroupKey;        // app-owned group identity, if supplied
        std::string               section;                 // Pixel-like automatic grouping section
        bool                      sectionExplicit = false; // supplied section survives derived alerting changes
        std::vector<SMessage>     messages;                // oldest first, bounded MessagingStyle adaptation
        std::vector<SParticipant> participants;            // latest distinct senders, bounded and texture-owning
        uint32_t                  unreadCount = 0;
        uint8_t              urgency  = 1;
        int                  progress = -1; // 0..100 from the "value" hint, -1 = none
        std::string          image;    // CONTENT source (image-path), resolved file path, "" = none
        std::string          identity; // IDENTITY source (app_icon/desktop-entry), resolved path, "" = none
        std::string          appIcon;  // raw app_icon; trusted OSD names select stable semantic marks
        std::vector<uint8_t> pixels;   // image-data, premultiplied BGRA (DRM ARGB8888); freed once uploaded
        bool                 hasPixels = false; // the LAST Notify carried image-data (outlives the freed buffer)
        int                  pw = 0, ph = 0;
        std::string          defaultAction; // the "default" action key, "" = none; a body click fires it, never a button
        bool                 canReply = false;   // the sender offered an "inline-reply" action
        std::string          replyActionText;    // the action's localized affordance label, usually "Reply"
        std::string          replyPlaceholder;   // x-kde-reply-placeholder-text
        std::string          replySubmitText;    // x-kde-reply-submit-button-text, else the stable "Send" label
        std::vector<SAction>    actions;    // non-default actions -> buttons, in Notify order
        std::vector<SBodyImage> bodyImages; // body <img src> thumbnails
        bool                    actionIcons = false; // the action-icons hint: button ids are icon names
        bool                    resident    = false; // the resident hint: an action keeps the card
        bool                    transient   = false; // the transient hint: bypass history AND residency
        bool                      conversation        = false; // fd.o category or explicit conversation metadata
        bool                    priority     = false; // the user marked this chat: ranks first, the badge wears the ring
        bool                    identityFromDesktop = false; // desktop-entry Icon= resolved asynchronously

        bool                 waiting = false; // arrived while suspended (DND): collected, not shown, timeout held
        bool                 banner  = true;  // the popup is up; expiry drops only this — the card stays resident
        bool                 snoozed = false; // "remind me": out of sight until snoozeUntil, then it alerts again
        Time::steady_tp      snoozeUntil;
        // The undo window. A snoozed card does not leave the shade at the
        // click: it collapses to a confirmation row until this passes (or the
        // shade closes), which is the only thing that gives the undo something
        // to be clicked ON.
        Time::steady_tp      snoozeConfirmUntil;
        int64_t              snoozeSecs = 0; // the duration in force for the Undo-row label

        float                timeoutMs = 0; // resolved; 0 = sticky
        Time::steady_tp      deadline;      // meaningful when banner && timeoutMs > 0 and not waiting
        Time::steady_tp      arrived;       // Notify arrival (a replace refreshes it); the age lines
        Time::steady_tp      born;          // creation only (a replace keeps it); the arrival spring's key

        // image textures — built ONLY by the warm pass (the texture rule).
        // Text rasters live in render.cpp's keyed cache; only the decoded
        // images cache here (their sources don't re-key per age tick).
        SP<ITexture> iconTex;  // bounded content image; avatar only for conversations
        SP<ITexture> identTex; // one application identity used at top-left and as a conversation badge
        SP<ITexture> conversationTex; // explicit/generated shortcut artwork; participant face piles stay separate
        SP<ITexture> childIconTex;     // 24px sender/source icon for bundle children
        std::string  imageFor, identFor;
        std::string  conversationFor;
        std::string  childIconFor;
        int          imageIconPx  = 0;
        int          identIconPx = 0;
        int          conversationIconPx = 0;
        int          childIconPx = 0;
        bool         imageSettled = false, identSettled = false;
        bool         conversationSettled = false;
        bool         childIconSettled = false;
        uint64_t     pixelsFor = 0;
        int          pixelsIconPx = 0;
    };
    extern std::vector<SP<SNotif>> notifs;

    // ---- parse.cpp: the untrusted payload -> values a card can hold ----

    namespace Parse {
        // The D-Bus transport has already accepted a Notify before this code
        // runs. These limits bound the work and state the compositor retains.
        inline constexpr size_t MAX_APP_NAME_BYTES     = 256;
        inline constexpr size_t MAX_SUMMARY_BYTES      = 2048;
        inline constexpr size_t MAX_BODY_BYTES         = 8192;
        inline constexpr size_t MAX_HINT_TEXT_BYTES    = 1024;
        inline constexpr size_t MAX_SOURCE_BYTES       = 4096;
        inline constexpr size_t MAX_ACTION_ID_BYTES    = 256;
        inline constexpr size_t MAX_ACTION_LABEL_BYTES = 1024;
        inline constexpr size_t MAX_ACTION_PAIRS       = 12;
        inline constexpr size_t MAX_BODY_IMAGES        = 4;
        inline constexpr size_t MAX_MARKUP_TAG_BYTES   = 1024;
        inline constexpr size_t MAX_CONVERSATION_ID_BYTES   = 512;
        inline constexpr size_t MAX_MESSAGE_ID_BYTES        = 512;
        inline constexpr size_t MAX_SENDER_ID_BYTES         = 512;
        inline constexpr size_t MAX_SENDER_NAME_BYTES       = 512;
        inline constexpr size_t MAX_CONVERSATION_KIND_BYTES = 32;

        // the spec's image-data: width, height, rowstride, has_alpha,
        // bits_per_sample, channels, RGB(A) bytes
        using ImageData = sdbus::Struct<int32_t, int32_t, int32_t, bool, int32_t, int32_t, std::vector<uint8_t>>;

        std::string              clipUtf8(std::string_view in, size_t maxBytes);
        std::string              boundedOpaque(std::string_view in, size_t maxBytes);
        std::string              sanitizeMarkup(std::string_view in, bool allowLinks = false);
        std::string              oneLine(std::string s);
        std::string              resolveImage(std::string s, int sizePx); // path, file://, or a themed icon NAME
        std::vector<SBodyImage>  extractImages(std::string& body, int sizePx); // pulls <img src alt> out of the body
        void                     unpackImageData(SNotif& n, const ImageData& d, int capPx); // -> premultiplied BGRA
    }

    void textCacheClear();

    // ---- model.cpp: the cards and their lifetimes ----

    namespace Model {
        // NotificationClosed reasons (the spec's); 4 = undefined, the eviction
        inline constexpr uint32_t R_EXPIRED = 1, R_DISMISSED = 2, R_CLOSED = 3, R_UNDEFINED = 4;

        void       init();
        void       exit();
        SP<SNotif> byId(uint32_t id);
        bool       vanishes(const SP<SNotif>& n); // opts out of residency: expiry takes the whole card

        // a Notify payload becomes (or refreshes) a card; returns its id
        uint32_t arrive(const std::string& appName, uint32_t replacesId, const std::string& appIcon, const std::string& summary, const std::string& body,
                        const std::vector<std::string>& actions, const std::map<std::string, sdbus::Variant>& hints, int32_t expireTimeout);

        bool                          closeOne(uint32_t id, uint32_t reason);
        void                          dismissAllLive();                      // "Clear all": every visible card goes; the DND queue stays
        void                          dismissGroup(const std::string& groupKey); // dismiss one displayed declared/automatic group

        // the in-memory history (v13): dismissed cards and Clear-all sweeps
        // stash a bounded entry; the footer's history pill reveals the last
        // few. Transient and OSD-band cards never stash.
        struct SHistoryEntry {
            std::string iconSource; // the identity source; the 20px texture resolves at render time
            std::string app;
            std::string title;
        };
        void                          pushHistory(const SP<SNotif>& n); // stashes unless the card opts out
        void                          clearHistory();
        const std::vector<SHistoryEntry>& history();
        void                          absorbPopped();                        // opening the shade parks the popped stack (no re-pop on close)
        void                          rearmExpiry();
        void                          snooze(uint32_t id);                        // out of sight, then back with a fresh banner
        void                          snoozeWith(uint32_t id, int64_t seconds); // the hold menu's chosen duration
        void                          snoozeUndo(uint32_t id);  // inside the undo window: as if it never happened
        void                          snoozeEndConfirm();       // the shade closed; every confirmation row goes
        bool                          snoozeConfirming(const SP<SNotif>& n); // still showing its undo row
        std::string                   snoozeLabel(const SP<SNotif>& n);      // "15 min", "2 hours"
        uint32_t                      snoozedCount();
        void                          holdBanner(uint32_t id); // the hovered popup's countdown pauses; 0 releases (and restarts it)
        void                          toggleSuspend();         // DND; resume renders the queue, fresh timeouts
        bool                          suspendedNow();
        std::pair<uint32_t, uint32_t> badgeCounts(); // {bannered, resident} — the bell's two numbers
        std::string                   stateString(); // "center:N live:N dnd:N" — raw model counts, the debug line
        std::string                   badgeString(); // "banners:N resident:N" — the popup/shade split the bell reads (state's `live` can't see it)
    }

    // ---- policy.cpp: the user's rules, persisted ----

    namespace Policy {
        enum class eAlertingMode : uint8_t {
            PRIORITY,
            DEFAULT,
            SILENT,
        };

        void        init();
        void        exit();
        bool        silenced(const std::string& appKey);                            // no banner, no sound, ranked quiet
        int64_t     silenceRemaining(const std::string& appKey);                    // seconds until silence expires; 0 = forever; -1 = not silenced
        bool          priority(const std::string& appKey, const std::string& conversationId); // this chat outranks everything but critical
        eAlertingMode mode(const std::string& appKey, const std::string& conversationId, bool conversation);
        bool          setMode(const std::string& appKey, const std::string& conversationId, bool conversation, eAlertingMode mode);
        bool          setSilenceUntil(const std::string& appKey, int64_t seconds); // timed silence: 0 = forever, >0 = duration
        void          refreshExpired(); // event-loop/warm only; keeps draw-side reads pure
        void        unsilenceAll(); // the footer chip: one click out of every standing rule
        size_t      silencedCount(); // rules in force — the footer never lets one hide
        std::string stateString();   // the debug line, and what the gate reads
    }

    // ---- bus.cpp: the connection ----

    namespace Bus {
        void pollSoon(); // pull the next DBus poll tick close after a send
        void init();
        void exit();
        void invokeAction(uint32_t id, const std::string& key);
        void sendReply(uint32_t id, const std::string& text); // NotificationReplied, then close unless resident
        void emitClosed(uint32_t id, uint32_t reason);        // the model's outbound half of a card's death
        void emitStateSoon(); // coalesced org.hitori.hyprnotify State signal (the bar's bell: shade counts)
    }

    // ---- icons.cpp ----

    // (Re)build n.iconTex (content) and n.identTex (identity) when their
    // sources changed. iconPx caps every decoded/uploaded card image.
    void iconsInit();
    void iconsExit();
    void        resetDesktopIconCache();     // re-resolve app identity and restart the bounded desktop-entry index
    void        resetGeneratedAvatarCache(); // re-resolve conversation sources and clear theme/font-sensitive textures
    SP<ITexture> historyIcon(const std::string& source, int iconPx); // the 20px history-row mark; null = none yet
    void        historyIconsClear();
    std::string resolveDesktopEntryIcon(const std::string& entry, int sizePx);
    void        ensureIconTex(SNotif& n, int iconPx);
    void        ensureConversationIcons(SNotif& n, int iconPx);
    void        ensureChildIcon(SNotif& n, int iconPx);

    // (Re)build an action button's icon when action-icons is set and its id (an
    // icon name or a path) changed; clears it when the hint is off.
    void ensureActionIcon(SNotif& n, SAction& a, int iconPx);

    // (Re)build a body <img> thumbnail when its src changed. maxPx caps the
    // decoded raster.
    void ensureBodyImage(SBodyImage& im, int maxPx);

    // Mark ready asynchronous sources for this warm, then release the ones
    // consumed or superseded by it. True means a queue slot opened for a
    // deferred visible request and a follow-up warm is needed.
    void iconsWarmBegin();
    bool iconsWarmEnd();

    // ---- render.cpp ----

    void warmNotifs();   // build every texture the next frame will paint; no-ops inside a render
    void damageNotifs(); // damage the previous layout and the fresh one

    // The model changed: rebuild textures and damage, deferred to the event
    // loop; bursts (an OSD volume sweep) coalesce into one warm.
    void notifChanged();

    // the shade: ONE list of live cards, newest first, no lifecycle sections
    bool centerVisible();
    void setCenter(bool on);  // event-loop only (input/hyprctl defer through main.cpp's queue)
    void centerPage(int dir); // wheel: >0 towards older rows
    void                  centerToggleGroup(const std::string& groupKey);
    void centerToggleRow(uint32_t id);
    void     centerToggleManage(uint32_t id); // long-press: one row at a time wears its manage panel
    void                  centerToggleManageGroup(const std::string& groupKey);
    Policy::eAlertingMode centerManageMode();
    void                  centerChooseManageMode(Policy::eAlertingMode mode);
    bool                  centerManageBundle(); // the target is a bundle (no snooze section)
    bool                  centerManageConversation(); // the target carries a conversation (hold header line)
    void                  centerCommitManage(uint32_t id, const std::string& appKey = {});
    // v13: the hold menu's snooze section, staged like the importance mode —
    // Done commits whichever of the two is open
    bool          centerManageSnoozeOpen();
    void          centerToggleManageSnooze();
    int64_t       centerManageSnoozeSecs();
    void          centerChooseManageSnooze(int64_t seconds);
    // v13: kid-level expansion (the time + name + action row under one kid)
    bool          centerKidExpanded(const std::string& childKey);
    void          centerToggleChild(const std::string& childKey);
    // v13: the footer history panel
    bool          centerHistOpen();
    void          centerToggleHistory();
    void          centerHistoryClear();

    void centerExit();

    void onRenderStage(eRenderStage stage);
    // render.preChecks: ask the target monitor to keep a visible card
    // composited over a solitary fullscreen window (else scanout/solitary
    // render skips the notify pass)
    void onRenderPreChecks(PHLMONITOR mon);
    void renderInit(); // the age/motion tick timers
    void renderExit();

    // hit rects of the last layout, global logical — input hit-tests these
    struct SCard {
        enum eKind : uint8_t {
            POPUP = 0,
            ROW,       // a v13 card body (plain, conversation, or bundle lead)
            DIGEST,    // a folded app bundle (group = app key) — its lead card
            GHEAD,     // an expanded bundle's header row
            CHILD,     // a kid row: a conversation message/sender or a bundle child
            SNOOZE,    // a snoozed card's undo row, in the slot the card held
            MANAGE,    // the hold menu, in the card's own slot (long-press)
            BTN_RULES, // footer muted-count control: the silences in force and the way out
            BTN_CLEAR, // footer "Clear all": the global sweep
            BTN_DND,   // footer DND control
            BTN_HISTORY, // footer history pill: toggles the history panel
            HIST_BOX,   // the history panel itself (swallows clicks)
            HIST_CLEAR, // the history panel's "Clear" button
            PANEL,     // the shade panel body: swallows clicks, owns the wheel
        };
        eKind       kind = POPUP;
        CBox        box;
        uint32_t    id = 0;             // live identity
        std::string group;              // DIGEST/GHEAD/CHILD: the app key
        std::string childKey;           // CHILD: its expansion key (card id + sender/app identity)
        bool        expandable = false; // ROW: open form reveals hidden compact content
        CBox        replyField;         // ROW: the armed inline-reply box (swallows, never acts)
        CBox        replySend;          // ROW: its send pill
        CBox        expandButton;       // Pixel expand/count pill hit target
        bool        expansionButton = false;
        // every small control the surface carries — the undo row's action and a
        // manage panel's entries — as one rect per part code, so another verb
        // costs an entry here and not a member
        struct SManage {
            CBox    box;
            uint8_t part;
        };
        std::vector<SManage> manage;
        struct SBtn {
            CBox        box;
            std::string id;
        };
        std::vector<SBtn> buttons; // action-button hit rects (global logical)
        struct SLinkHit {
            CBox        box;
            std::string href;
        };
        std::vector<SLinkHit> links; // body-hyperlink hit rects (popups and open shade rows)
    };
    extern std::vector<SCard> cards;
    extern PHLMONITORREF      cardsMon; // the monitor the layout ran on

    // hover affordance: rows/buttons warm under the pointer. `btn` -1 = the
    // surface itself, >= 0 = that action button; `part` distinguishes the
    // reply, management, and undo controls. A change damages only the boxes involved.
    struct SHover {
        uint32_t     id = 0;
        std::string  group;
        std::string  childKey; // CHILD: distinguishes siblings that share one card id
        SCard::eKind kind = SCard::POPUP;
        int          btn  = -1;
        // part codes are per-kind (SCard::manage boxes and the reply boxes
        // carry their own):
        //  ROW/DIGEST/GHEAD  1 expand chip, 2 icon column
        //  CHILD             1 kid chevron
        //  MANAGE            0..2 importance, 3 snooze head, 4..7 snooze
        //                    options (15/30/60/120), 8 Done, 9 Dismiss
        //  SNOOZE            8 undo
        uint8_t      part = 0;
        bool         operator==(const SHover&) const = default;
    };
    void setHovered(const SHover& h);

    // ---- reply.cpp: the inline-reply field ----
    //
    // A card whose sender offered "inline-reply" grows a text field in its
    // open shade row. While one is armed the shade owns EVERY key (the same
    // grab hyprbar's menubar prompt takes) — there is no keyboard focus to
    // hand it, so it takes the keys and gives back what it does not use.

    bool               replyArmedOn(uint32_t id); // this card's field is the armed one
    bool               replyArmed();
    void               replyOpen(uint32_t id);
    void               replyClose();
    const std::string& replyText();
    size_t                    replyCursor();
    std::pair<size_t, size_t> replySelection();
    // a key while armed. Returns false when the key is not ours to eat.
    bool               replyKey(xkb_state* state, uint32_t keycode);
    void               replyExit();

    // ---- input.cpp ----

    void onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info);
    void onMouseMove(const Vector2D& pos, Event::SCallbackInfo& info);
    void onMouseAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info);
    void onKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info); // only an armed inline-reply field owns keys
    void releasePointer();
    void refreshPointerOwnership(); // the hovered card vanished under a still pointer
    void inputCancelLongPress();
    void inputExit();

    // main.cpp: the deferred center toggle used by the bar bus and hyprctl
    void queueCenterToggle();

} // namespace NHyprnotify
