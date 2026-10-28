// awesome/notify/model.hpp — the notify module: the freedesktop notification
// daemon, the card model, the surfaces (the banners, the center, the
// inline-reply field), and the input they own.
//
// Per-unit docs at the top of each unit:
//
//   bus.cpp     the org.freedesktop.Notifications connection
//   parse.cpp   the untrusted payload: markup, images, appended bodies
//   model.cpp   the cards: arrival, residency, merging, DND, the expiry
//   policy.cpp  the card menu's persisted switches: banners, sounds, mutes
//   icons.cpp   notification images: content avatars, identity icons,
//               raw image-data
//   text.cpp    the pango rasterizer + the keyed text cache + markup
//               helpers
//   paint.cpp   the shared card recipes and type scale (on the canvas
//               paint context)
//   card.cpp    one card, banner or center
//   popups.cpp  the banner column and the OSD capsules
//   stack.cpp   an app's stack: the deck, the spread stack's header
//   center.cpp  the center: the display list, the expansion budget
//   menu.cpp    the card menu (hold left)
//   surface.cpp the canvas layer: warm/draw, damage, ticks, hover
//   input.cpp   clicks, the hold, wheel paging, keys, pointer ownership
//   reply.cpp   the inline-reply field: its state and its keys
//
// Everything lives in NAwesome::Notify. The only D-Bus in the plugin is
// this fd.o daemon (plus the system module's logind reads): the old private
// bus bridge is gone — the shell's bell reads the model directly.
#pragma once

#include "pixel_model.hpp"

#include "../core/options.hpp"
#include "../core/raster.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>

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
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/canvas.hpp"
#include "../core/config.hpp"
#include "../core/glyph.hpp"
#include "../core/icons.hpp"
#include "../core/queries.hpp"
#include "../core/supervisor.hpp"
#include "../core/version.hpp"

namespace NAwesome::Notify {

    // the OSD scripts pin ids here: replace-in-place, never appended, never
    // grouped, never history; fresh ids and recalls never mint into it
    inline constexpr uint32_t OSD_LO = 9990, OSD_HI = 9999;
    inline constexpr bool     inOsdBand(uint32_t id) {
        return id >= OSD_LO && id <= OSD_HI;
    }

    // ---- the model (model.cpp) ----

    // a non-"default" action: a row of the card menu
    struct SAction {
        std::string id;    // ActionInvoked key
        std::string label; // localized row text
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
        std::string  alt;      // the img's alt text: the body fallback when the load fails
        bool         settled = false; // a pending async decode stays false
        int          builtPx = 0;
    };

    // one message of a chat card's log (the upsert keeps it bounded; see
    // pixel_model.hpp). Every message is a notification of its own on the
    // wire: the Notify that posted it got `nid` back, and the sender may
    // close it alone (Telegram does as a message is read or deleted).
    struct SMessage {
        uint32_t    nid = 0;
        std::string id;
        std::string senderId;
        std::string senderName;
        std::string senderIcon;
        std::string text;
        int64_t     timestampMs = 0;
        bool        historic    = false; // backfill: counted, never unread
    };

    // a distinct sender of a conversation; the avatar texture is warm-built
    // (the texture rule) and owned here so the face pile and the transcript
    // faces draw from one decode
    struct SParticipant {
        std::string  key; // stable: the sender-id, else the name
        std::string  name;
        std::string  iconSource; // the raw sender-icon
        std::string  icon;       // resolved path
        bool         iconRetry = false; // the walk budget deferred iconSource: the warm resolves it again
        SP<ITexture> avatarTex;
        std::string  avatarFor;
        int          avatarPx = 0;
        bool         avatarSettled = false;
    };

    // a content image scaled for its card, off the compositor thread: the
    // icon-box or hero face and a hero's square thumbnail, CPU-side until the
    // card's next warm uploads them (the texture rule)
    struct SScaledImage {
        SPixels main, thumb;
        SPixels badge; // an identity's corner badge, composed at its device size
        bool    hero = false;
    };

    struct SNotif {
        uint32_t             id = 0;
        std::string          appName;
        std::string          appKey;  // grouping identity: desktop-entry, else the app name
        std::string          sender;  // the bus name that sent this Notify (the X11 activation lookup)
        bool                 orphaned = false; // its sender left the bus: any sender of the app may take the card over
        std::string          summary; // newlines flattened, whitelisted markup
        std::string          body;    // whitelisted markup (Pango subset); a chat's: one paragraph per message
        // A chat is ONE card: its messages, merged by conversation-id or by
        // app and summary (the legacy contract), keep a log the body is
        // rebuilt from
        std::string          conversationId; // stable chat identity; "" = the legacy merge
        std::string          conversationTitle;
        std::string          conversationKind; // "one-to-one" or "group"
        std::string          declaredGroupKey; // x-notify-group-key: the app's own grouping
        std::vector<SMessage>     messages;     // oldest first, bounded; empty = not a chat
        // renewed by every change to what the card says (an arrival, a
        // message closed), from one model-wide serial: the fitted window's
        // memo key and a click's proof it saw this content
        uint64_t                  rev = 0;
        // the window a chat card last showed, fitted from its newest end at
        // one width and line budget (text.cpp's fittedTranscript)
        struct {
            uint64_t    rev = ~0ull, gen = 0;
            int         pt = 0, w = 0, cap = 0, indent = 0;
            bool        links = false;
            std::string body;
        } fitMemo;
        std::vector<SParticipant> participants; // latest distinct senders, bounded
        SParticipant              chatFace;     // a group chat with no picture of its own: its initials (card.cpp)
        uint32_t             unreadCount = 0;
        uint8_t              urgency  = 1;
        int                  progress = -1; // 0..100 from the "value" hint, -1 = none
        std::string          image;    // CONTENT source (image-path), resolved file path, "" = none
        std::string          identity; // IDENTITY source (app_icon/desktop-entry), resolved path, "" = none
        std::string          desktopEntry;     // the raw desktop-entry hint (the F2 index's key)
        bool                 identityFromDesktop = false; // identity rides the hint: the index may upgrade it
        // Names the theme-walk budget deferred at arrival (core:
        // iconDeferred) are not misses: the warm resolves them again until
        // they land or truly miss. imageRetry: the image-path name;
        // identityRetry: the identity chain is re-run from this app_icon.
        bool                 namesDeferred = false;
        std::string          imageRetry;
        bool                 identityRetry = false;
        std::string          appIconSource; // the raw app_icon, for an identity retry
        std::vector<uint8_t> pixels;   // image-data, premultiplied BGRA (DRM ARGB8888); freed once uploaded
        bool                 hasPixels = false; // the LAST Notify carried image-data (outlives the freed buffer)
        int                  pw = 0, ph = 0;
        uint64_t             pixelsHash = 0; // Raster::hash of `pixels`, set with them
        // the image-data brings its own shape (its corners are clear): a
        // chat's face keeps it — Telegram's forum pictures are rounded
        // squares — where an opaque square is cut to a circle
        bool                 faceShaped = false;
        // an unpack or a scale of this card's image-data on the raster
        // worker: its ticket (one model-wide serial), 0 = none. A replace or
        // a dismissal outdates it; its result lands only on a matching card.
        uint64_t                      pixelsJob = 0;
        std::unique_ptr<SScaledImage> scaled; // the worker's result, waiting for the warm's upload
        std::string          defaultAction; // the "default" action key, "" = none; a body click fires it, never a menu row
        bool                 canReply = false;   // the sender offered an "inline-reply" action
        std::string          replyPlaceholder;   // x-kde-reply-placeholder-text
        std::string          replySubmitText;    // x-kde-reply-submit-button-text, else the action's own label
        std::vector<SAction>    actions;    // non-default actions -> the card menu's rows, in Notify order
        std::vector<SBodyImage> bodyImages; // body <img src> thumbnails
        bool                    resident    = false; // the resident hint: an action keeps the card
        bool                    transient   = false; // the transient hint: bypass history AND residency
        bool                    conversation = false; // fd.o category im.*/call.*: outranks ordinary cards, merges by sender
        bool                    absorbed     = false; // the open shade parked this banner; the close returns it
        std::string             fallbackPick;         // the rolled identity face; survives in-place replaces

        bool                 waiting = false; // arrived while suspended (DND): collected, not shown, timeout held
        bool                 seen    = false; // the open center has listed this revision: no dot on the bell for it
        bool                 cleared = false; // Clear All hid it: off the shade, its close held for the undo window
        bool                 banner  = true;  // the popup is up; expiry drops only this — the card stays resident

        float                timeoutMs = 0; // resolved; 0 = sticky
        Time::steady_tp      deadline;      // meaningful when banner && timeoutMs > 0 and not waiting
        Time::steady_tp      arrived;       // Notify arrival (a replace refreshes it); the age lines
        std::time_t          arrivedWall = 0; // the same moment on the wall clock: the center's times
        uint8_t              osdGlyph = 0;    // a capsule's Glyph::eGlyph (postCard), 0 = a card
        bool                 osdAlarm = false;
        Time::steady_tp      born;          // creation only (a replace keeps it); the arrival spring's key

        // image textures — built ONLY by the warm pass (the texture rule).
        // Text rasters live in text.cpp's keyed cache; only the decoded
        // images cache here (their sources don't re-key per age tick).
        SP<ITexture> iconTex;  // content avatar (or hero)
        SP<ITexture> thumbTex; // a hero's icon-box face: the shade row leads with it, the hero is the popup's
        SP<ITexture> identTex; // identity icon: the lead icon when no content
        SP<ITexture> badgeTex; // the identity as the corner badge on a content face, one raster (paint.cpp)
        std::string  imageFor, identFor;
        bool         heroTex      = false; // iconTex was built for the hero layout
        uint64_t     pixelsFor    = 0;
        bool         imageSettled = false; // false while an async decode is in flight
        bool         identSettled = false;
        int          imageIconPx  = 0, identIconPx = 0;
    };
    extern std::vector<SP<SNotif>> notifs;

    // who a reply typed on this card is for: its conversation, else the
    // app and the summary (the sender's name, for a legacy chat card)
    inline std::string replyTarget(const SNotif& n) {
        return n.conversationId.empty() ? n.appKey + '\x1f' + n.summary : n.conversationId;
    }

    // ---- parse.cpp: the untrusted payload -> values a card can hold ----

    namespace Parse {
        // the spec's image-data: width, height, rowstride, has_alpha,
        // bits_per_sample, channels, RGB(A) bytes
        using ImageData = sdbus::Struct<int32_t, int32_t, int32_t, bool, int32_t, int32_t, std::vector<uint8_t>>;

        // plainText: a summary — a tag that does not format stays literal
        std::string              sanitizeMarkup(const std::string& in, bool allowLinks = false, bool plainText = false);
        std::string              attrValue(const std::string& tag, const std::string& attr); // one quoted attr, case-insensitive name
        std::string              oneLine(std::string s);
        std::string              resolveImage(std::string s, int sizePx); // path, file://, or a themed icon NAME
        std::string              fileUriPath(std::string s); // a path or a file:// URI -> the path, "" if neither
        // one <img src>: the resolved path and its alt — the load-failure
        // fallback line
        struct SImgRef {
            std::string src;
            std::string alt;
        };
        std::vector<SImgRef> extractImages(std::string& body, int sizePx, size_t maxImages); // pulls <img src> out of the body
        // image-data -> premultiplied BGRA within capPx: pure, any thread.
        // Invalid for a malformed or oversized payload. imageDataSize: the
        // payload's own size, {0, 0} when it would not unpack.
        SPixels                  unpackImageData(const ImageData& d, int capPx);
        std::pair<int, int>      imageDataSize(const ImageData& d);
        // the same check off the header alone: the bus reader runs it before
        // it copies a byte
        std::pair<int, int>      imageDataSize(int32_t w, int32_t h, int32_t stride, int32_t bitsPerSample, int32_t channels, size_t bytes);
    }

    // the collapsed row's one-liner: a chat's newest message, its sender
    // inline, else the newest line of the body. Renderer and the topline
    // probe share this so they cannot drift apart.
    std::string lastLine(const std::string& body); // the newest line of any body ends it
    std::string collapsedLine(const SP<SNotif>& n);

    // ---- model.cpp: the cards and their lifetimes ----

    namespace Model {
        // NotificationClosed reasons (the spec's); 4 = undefined, the eviction
        inline constexpr uint32_t R_EXPIRED = 1, R_DISMISSED = 2, R_CLOSED = 3, R_UNDEFINED = 4;

        void       init();
        void       exit();
        SP<SNotif> byId(uint32_t id);
        // The id the card answers for on the wire: a chat's newest message
        // (Reply quotes it, Open and Mark as read act on it, as macOS's
        // stack acts as its top notification), else the card's own.
        uint32_t   wireId(const SNotif& n);
        // CloseNotification: one message of a chat goes (the card with its
        // last), any other id takes its card. False: no such id.
        bool       closeWire(uint32_t id, uint32_t reason);
        // a unique bus name left the bus: its cards lose their owner (the
        // app's next instance may continue them) and their reply field
        void       senderGone(const std::string& name);
        bool       vanishes(const SP<SNotif>& n); // opts out of residency: expiry takes the whole card
        // never a shade row, never on the badge: the OSD band, and a
        // non-critical transient card (the spec's "bypass persistence")
        bool       offShade(const SP<SNotif>& n);

        // a Notify payload becomes (or refreshes) a card; returns the id the
        // sender gets: the card's, or a chat message's own
        // `osd`: an in-process post (postCard) — only those may pin a fresh
        // id in the OSD band; no wire hint grants it
        // `image`: the payload's image-data, read apart from the hints (the
        // bus reader takes it out of the message in one block)
        uint32_t arrive(const std::string& appName, uint32_t replacesId, const std::string& appIcon, const std::string& summary, const std::string& body,
                        const std::string& sender, const std::vector<std::string>& actions, const std::map<std::string, sdbus::Variant>& hints, int32_t expireTimeout,
                        std::optional<Parse::ImageData> image = std::nullopt, bool osd = false);

        // In-process card posting (the system module's feedback: audio,
        // pad; the shell's battery alerts). The bus face is for
        // FOREIGN daemons; ours is a function call — no bus, no proxy, no
        // hint-table ceremony. The id pins the replace-in-place slot (the
        // OSD band 9990-9999 keeps its old semantics: replace, no history,
        // no grouping).
        struct SPostCard {
            uint32_t id = 0; // stable replaces-id
            const char* icon = nullptr;   // icon-theme name or path; "" = none
            const char* summary = nullptr;
            const char* body = nullptr;
            int32_t expireTimeout = 0; // 0 = the model's policy
            int value = -1;            // the value hint, 0..100; -1 = none
            uint8_t urgency = 1;       // 0 low / 1 normal / 2 critical (sticky)
            bool osd = false;          // an OSD-band post: may pin a fresh band id
            // a non-NONE glyph draws the post as a capsule, top center (the
            // system's feedback); alarm: its words in the urgent colour
            Glyph::eGlyph glyph = Glyph::eGlyph::NONE;
            bool          alarm = false;
        };
        void postCard(const SPostCard& c);

        bool       closeOne(uint32_t id, uint32_t reason); // the card goes, every id it answers for closed
        void       dismissAllLive(bool offShadeToo = false); // every shade row goes now (a script's sweep); the DND queue stays
        // Clear All's undo window (UNDO_WINDOW): the click hides every shade
        // row and holds its NotificationClosed; Undo brings them back; the
        // window's end, the center's close or a sweep closes them for good.
        // An arrival into a hidden card shows it again, as any arrival.
        inline constexpr auto UNDO_WINDOW = std::chrono::seconds(5);
        void       clearLive();
        void       undoClear();
        void       commitClear();
        bool       clearPending(); // a hidden card waits on the window
        void       dismissCards(const std::vector<SP<SNotif>>& cards); // a stack's right-click or Delete: the cards it drew
        void       dismissAppAll(const std::string& appKey); // the card menu's "Close All": every shade card of the app, its chats too
        void       lowerBanners(const std::string& appKey);  // a menu switch silenced the app (or a chat), "" quiet began: the soft banners go to the shade now
        std::string groupKeyOf(const SP<SNotif>& n);       // the stack's identity: the app key, sub-keyed by the declared group
        bool       stackable(const SP<SNotif>& n);        // may join its app's stack: a card with an app
        void       absorbPopped();                // opening the shade parks the popped stack
        void       repopAbsorbed(bool repop = true);      // closing it returns the parked stack to banners (or parks it for good)
        void       rearmExpiry();
        void       holdBanner(uint32_t id); // the hovered popup's countdown pauses; 0 releases (and restarts it)
        // DND, remembered across sessions: 0 off, -1 until turned off, else
        // its end (unix seconds; one already past turns it off). The resume
        // renders the queue on fresh timeouts.
        void                          setSuspend(int64_t until);
        void                          toggleSuspend(); // off, or on until turned off
        bool                          suspendedNow();
        int64_t                       suspendEnd();  // while on: -1, or its end
        std::pair<uint32_t, uint32_t> badgeCounts(); // {bannered, resident} — the bell's two numbers
        uint32_t                      heldCount();   // the DND queue — the bell's dot while DND holds
        // The bell's dot: a card the center lists that it has not shown since
        // the card arrived or changed. Opening and closing the center marks
        // what it lists (never the DND queue, which it does not list).
        bool                          unseen();
        void                          markSeen();

        // The shell's bell repaints when the badge numbers can have moved:
        // the shell registers the hook at init; it fires from notifChanged,
        // the single funnel of every model change, on the event loop.
        inline std::function<void()>& badgeChangedHook() {
            static std::function<void()> H;
            return H;
        }
        std::string                   stateString(); // "center:N live:N dnd:N" — raw model counts, the debug line
        std::string                   badgeString(); // "banners:N resident:N" — the popup/shade split the bell reads
        std::string                   toplineString(); // the newest card's collapsed one-liner — the gate's text probe
    }

    // ---- policy.cpp: the card menu's persisted switches ----
    //
    // Per app: banners off (an arrival lands silently in the shade, the bell
    // still counts it) and sounds off. Per chat: a mute until a time — no
    // banner, no sound. Critical punches through all three, as through DND.
    namespace Policy {
        enum eFlag : int {
            BANNERS_OFF = 1,
            SOUNDS_OFF  = 2,
        };
        // the names a row is listed by: the app's own, the chat's title
        std::string appLabel(const SNotif& n);
        std::string chatLabel(const SNotif& n);
        int         appFlags(const std::string& appKey);
        // a row's label is set from a non-empty `label`, else kept
        void        setAppFlag(const std::string& appKey, int flag, bool on, std::string_view label = {});
        std::string chatKey(const SNotif& n);            // "" = not a chat: only a conversation mutes
        int64_t     mutedUntil(const std::string& chat); // 0 = not muted
        // until <= now unmutes
        void        mute(const std::string& chat, int64_t until, std::string_view label = {});
        bool        silenced(const SNotif& n);   // the app's banners switched off, or its chat muted
        bool        bannersOff(const SNotif& n); // silenced, or quiet automatically
        bool        soundsOff(const SNotif& n);
        void        prune(); // forget the mutes that ended

        // what the bell's Silenced Apps lists: the apps with a switch off,
        // then the muted chats, each kind newest first (MAX_SILENCED rows)
        struct SSilenced {
            std::string key, label;
            int         flags = 0; // an app's
            int64_t     until = 0; // a chat's mute end (0: an app)
        };
        inline constexpr size_t MAX_SILENCED = 64;
        std::vector<SSilenced>  silenced();
        void                    restore(const SSilenced& s); // the app's switches back on, or the chat unmuted
    }

    // ---- quiet.cpp: quiet automatically ----
    //
    // Two remembered switches (core/options.hpp), off until the bell's menu
    // turns them on: banners and sounds hold while the focused window is
    // fullscreen, or while a display (or a region of one) is shared.
    namespace Quiet {
        bool on(eOption o);
        void set(eOption o, bool enable); // QUIET_FULLSCREEN or QUIET_SHARING
        enum class eWhy : uint8_t {
            NONE,
            FULLSCREEN, // the focused window is fullscreen
            SHARING,    // a display or a region of one is shared (named over a fullscreen)
        };
        eWhy why();                       // what holds the arrivals now (the center's status line)
        bool now();                       // an arrival holds now (critical aside)
        bool hushed();                    // a share is up: a critical banner shows without its words
        void refresh();                   // a share, a fullscreen or the focus moved (deferred out of its emission)
        void exit();
    }

    // ---- bus.cpp: the connection ----

    namespace Bus {
        void init();
        void exit();
        // raise: the sender's own window comes up (an action that keeps the
        // user here, a resident card's, raises nothing; its token still goes)
        void invokeAction(uint32_t id, const std::string& key, const std::string& sender, bool raise);
        // NotificationReplied to the sender, then close unless resident;
        // nothing unless the card is still `target` (replyTarget). false =
        // not sent (the card changed, or the outbound queue refused it)
        bool sendReply(uint32_t id, const std::string& text, const std::string& target);
        void emitClosed(uint32_t id, uint32_t reason);        // the model's outbound half of a card's death
    }

    // ---- icons.cpp ----

    // (Re)build n.iconTex (content) and n.identTex (identity) when their
    // sources changed. iconPx caps the icon-box raster; screenshot-sized
    // content (256 px both ways, at least half the hero box) raster to
    // heroWPx instead, cover-cropped to heroHCapPx, and set heroTex (with
    // thumbTex, the whole image square-cropped to iconPx).
    void resetFallbackCache(); // forget the fallback_icon_dir listing (a config reload rescans)
    void ensureIconTex(SNotif& n, int iconPx, int heroWPx, int heroHCapPx);
    void ensureAvatarTex(SParticipant& p, int px);
    std::string resolveDesktopEntryIcon(const std::string& entry, int sizePx);
    // The warm's retry of a card's deferred names (SNotif::namesDeferred);
    // iconNamesDeferred re-warms the cards one walk window later.
    void retryDeferredNames(SNotif& n);
    void iconNamesDeferred();
    void        iconsInit();
    void        iconsExit();

    // (Re)build a body <img> thumbnail when its src changed. maxPx caps the
    // decoded raster.
    void ensureBodyImage(SBodyImage& im, int maxPx);

    // Downscale n.pixels in place when it exceeds maxPx (the retained-pixel
    // budget's cap).
    void shrinkPixels(SNotif& n, int maxPx);

    // Past this many source pixels an image's unpack or scale runs on the
    // raster worker (core/raster.hpp), not in the dispatch or warm that
    // asked: tens of ms for a screenshot, under a realtime budget.
    constexpr int64_t ASYNC_IMAGE_PIXELS = 512 * 512;
    // a fresh ticket for a worker job on a card's image (SNotif::pixelsJob)
    // or a decode's scale: one serial, never 0
    uint64_t imageJobTicket();

    // ---- surface.cpp ----

    void warmNotifs();   // build every texture the next frame will paint; no-ops inside a render
    void damageNotifs(); // damage the previous layout and the fresh one

    // The model changed: rebuild textures and damage, deferred to the event
    // loop; bursts (an OSD volume sweep) coalesce into one warm.
    void notifChanged();

    // the shade: ONE list of live cards, newest first, no lifecycle sections
    bool centerVisible();
    // event-loop only (input/hyprctl defer through the module's queue).
    // repop: an explicit close (outside click, esc, toggle) returns the
    // absorbed stack to banners; an action that closes on its way out (the
    // sender is coming up over them) does not.
    void setCenter(bool on, bool repop = false);
    void centerPage(int dir); // wheel: >0 towards older rows
    void centerPageScreen(int dir); // the paging chips: a screenful
    void centerToggleGroup(const std::string& appKey);
    void centerDismissStack(const std::string& key);
    void centerToggleRow(uint32_t id);
    void centerSelectMove(int dir);                         // ↑/↓: move the keyboard selection, paging to keep it on screen
    bool centerHasRows();                                   // the shade lists something a selection can land on
    void centerDeselect();
    bool centerSelection(uint32_t& id, std::string& group); // the selected item; group non-empty = a stack. false = none

    // the click door (Lua, the bell, `hyprctl awesome center`): deferred and
    // accumulating
    // the shade opens on `on` when given (the bell clicked there), else
    // where the cards are
    void queueCenterToggle(PHLMONITOR on = nullptr);
    // the control center (controls.cpp): the same column, the bar's switch
    // glyph its door; one panel open at a time
    bool controlsVisible();
    void setControls(bool on); // event-loop only
    void queueControlsToggle(PHLMONITOR on = nullptr);
    // move the cards (and the shade) to this monitor, damaging both spots
    void placeCardsOn(PHLMONITOR mon);
    void centerExit();

    void surfaceInit(); // the age/motion tick timers
    void surfaceExit();

    // hit rects of the last layout, global logical — input hit-tests these
    struct SCard {
        enum eKind : uint8_t {
            POPUP = 0,
            ROW,       // a center card (group = its stack's key inside an expanded stack)
            DECK,      // a collapsed stack: its top card and the deck (group = the app key)
            SHEAD,     // an expanded stack's header: Show Less and its close button
            BTN_CLEAR, // the center header's "Clear All": the global sweep
            PANEL,     // a panel's column (the center, the control center): swallows clicks, owns the wheel
            PAGE_UP,   // the center's paging chips (drawn over the edge cards)
            PAGE_DOWN,
            CAPSULE,   // an OSD capsule: damage and bounds only, never hit
            MENU,      // the card menu's panel: swallows, never acts
            MENU_ITEM, // one of its rows (id = the row's index)
            CTRL_SWITCH, // a control center button (id = controls.cpp's tile)
            CTRL_SLIDER, // a control center level's module (id = Controls::eLevel)
            SILENT,      // the center's silent fold: a click spreads it (group = its key)
        };
        eKind       kind = POPUP;
        CBox        box;
        uint32_t    id   = 0; // live identity
        uint64_t    rev  = 0; // the card's revision this layout drew: a click acts on what it saw
        std::string group;    // DECK/SHEAD, a stacked ROW: the app key
        CBox        less;         // SHEAD: Show Less; w = 0 -> none
        CBox        replyField;   // POPUP/ROW: the armed inline-reply box (swallows, never acts)
        CBox        replySend;    // POPUP/ROW: its send circle
        CBox        track;        // CTRL_SLIDER: the track a press drags along
        bool        folded = false; // ROW: the budget folded it and the open form shows more (a click unfolds)
        struct SLinkHit {
            CBox        box;
            std::string href;
        };
        std::vector<SLinkHit> links; // body-hyperlink hit rects (popups and open shade rows)
    };
    extern std::vector<SCard> cards;
    extern PHLMONITORREF      cardsMon; // the monitor the layout ran on
    SCard                     recycledCard(); // an empty card whose vectors keep last frame's capacity

    // hover affordance: rows/buttons warm under the pointer. `btn` -1 = the
    // surface itself, >= 0 = that action button; `part` distinguishes the
    // chevron/✕ corners. A change damages only the boxes involved.
    struct SHover {
        uint32_t     id = 0;
        std::string  group;
        SCard::eKind kind = SCard::POPUP;
        // 0 body, 1 Show Less, 3 reply field, 4 send
        uint8_t      part = 0;
        bool         operator==(const SHover&) const = default;
    };
    void setHovered(const SHover& h);
    // The one banner whose clock stops (Model::holdBanner): the card whose
    // menu is open, else the one a reply is being typed into, else the one
    // under the pointer. Every change to those three calls it.
    void refreshBannerHold();

    // ---- reply.cpp: the inline-reply field ----
    //
    // A card whose sender offered "inline-reply" opens a text field under its
    // words when Reply is picked from its menu (or Tab in the center), in
    // the banner or the center card alike. While one is armed the notify
    // layer owns EVERY key (the same grab the shell's menubar prompt takes) —
    // there is no keyboard focus to hand it, so it takes the keys and gives
    // back what it does not use.

    bool               replyArmedOn(uint32_t id); // this card's field is the armed one
    uint32_t           replyId();                 // the card the held arm is on; 0 = none
    // an arm is held, live or not: replyArmed() is the liveness test, and a
    // field that died since disarms there
    bool               replyHeld();
    bool               replyArmed();
    void               replyOpen(uint32_t id);
    // a send that did not go out (Bus::sendReply false): the sentence goes
    // back into the card's field, if the card still takes it
    void               replySendRefused(uint32_t id, const std::string& text, const std::string& target);
    void               replyClose();
    const std::string& replyText();
    const std::string& replyTargetKey(); // the armed card's replyTarget at arm time
    // a key press while armed (evdev code, the keyboard that typed it).
    // Returns false when the key is not ours to eat.
    bool               replyKey(const SP<IKeyboard>& kb, uint32_t keycode, bool repeating = false);
    void               replyKeyEvent(const IKeyboard::SKeyEvent& e); // every key event: ends a held key's repeat
    void               replyExit(); // disarm: a lock, the panel gone
    void               replyInit(); // the repeat timer
    void               replyTeardown();

    // ---- input.cpp ----

    void onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info);
    void onMouseMove(const Vector2D& pos, Event::SCallbackInfo& info);
    void onMouseAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info);
    void onKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info); // esc peels the center; ↑↓ space enter delete drive it
    // the pipeline's blocked branch (session lock or a native capture
    // session): reset every swallow mask, held counter, armed queue — a
    // lock goes further (crash class 7: the field releases the keyboard)
    void onInputBlocked();
    void releasePointer();
    void refreshPointerOwnership(); // the hovered card vanished under a still pointer
    void inputInit(); // the hold timer
    void inputExit();
    void controlsInit(); // controls.cpp: the control center's block (its hook, its hold timer)
    void controlsExit();

} // namespace NAwesome::Notify
