// awesome/notify/model.hpp — the notify module: the freedesktop notification
// daemon, the card model, the surfaces (popups, the shade, the
// inline-reply field), and the input they own.
//
// The full picture lives at the top of notify.cpp; per-module docs at the
// top of each unit:
//
//   bus.cpp     the org.freedesktop.Notifications connection
//   parse.cpp   the untrusted payload: markup, images, appended bodies
//   model.cpp   the cards: arrival, residency, merging, DND, the expiry
//   icons.cpp   notification images: content avatars, identity icons,
//               raw image-data
//   text.cpp    the pango rasterizer + the keyed text cache + markup
//               helpers
//   paint.hpp   the shared card recipes and type scale (on the canvas
//               paint context)
//   popups.cpp  the banner column (the one-card anatomy, hover-✕, springs)
//   row.cpp     one shade row in its two states, and the bundle recipes
//   center.cpp  the shade: the display list, the expansion budget, the panel
//   surface.cpp the canvas layer: warm/draw, damage, ticks
//   input.cpp   clicks, wheel paging, esc, pointer ownership
//   reply.cpp   the inline-reply field: its state, its keys, its drawing
//
// Everything lives in NAwesome::Notify. The only D-Bus in the plugin is
// this fd.o daemon (plus the system module's logind reads): the old
// the old private bus bridge is gone — the shell's bell reads the model
// directly.
#pragma once

#include "pixel_model.hpp"

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

    // a non-"default" action: a clickable text button on the card
    struct SAction {
        std::string  id;      // ActionInvoked key; also the icon name under action-icons
        std::string  label;   // localized button text
        SP<ITexture> iconTex; // resolved action-icon (warm; action-icons only)
        std::string  iconFor; // staleness: the id the icon was resolved from
        bool         iconSettled = false; // a pending async decode stays false
        int          iconPx = 0;
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

    // one structured message of a conversation (the message-id upsert keeps
    // the log bounded; see pixel_model.hpp)
    struct SMessage {
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
        SP<ITexture> avatarTex;
        std::string  avatarFor;
        int          avatarPx = 0;
        bool         avatarSettled = false;
    };

    struct SNotif {
        uint32_t             id = 0;
        std::string          appName;
        std::string          appKey;  // grouping identity: desktop-entry, else the app name
        std::string          sender;  // the bus name that sent this Notify (the X11 activation lookup)
        std::string          summary; // newlines flattened, whitelisted markup
        std::string          body;    // whitelisted markup (Pango subset)
        // structured conversation metadata: a sender with a stable
        // conversation-id grows
        // ONE card per chat, its message log kept and its body rebuilt from
        // the latest of it
        std::string          conversationId; // stable chat identity; "" = no merge contract
        std::string          conversationTitle;
        std::string          conversationKind; // "one-to-one" or "group"
        std::string          conversationIconSource; // the raw conversation-icon
        std::string          conversationIcon;       // resolved path
        std::string          declaredGroupKey; // x-notify-group-key: the app's own grouping
        std::vector<SMessage>     messages;     // oldest first, bounded
        // bumped by every arrival that touches the card (messages,
        // conversation kind): conversationBody's memo key — the banner
        // rebuilt the transcript body, sanitizing every message, per frame
        uint64_t                  rev = 0;
        struct {
            uint64_t    rev   = ~0ull;
            size_t      limit = 0;
            std::string body;
        } convMemo;
        std::vector<SParticipant> participants; // latest distinct senders, bounded
        uint32_t             unreadCount = 0;
        uint8_t              urgency  = 1;
        int                  progress = -1; // 0..100 from the "value" hint, -1 = none
        std::string          image;    // CONTENT source (image-path), resolved file path, "" = none
        std::string          identity; // IDENTITY source (app_icon/desktop-entry), resolved path, "" = none
        std::string          desktopEntry;     // the raw desktop-entry hint (the F2 index's key)
        bool                 identityFromDesktop = false; // identity rides the hint: the index may upgrade it
        std::vector<uint8_t> pixels;   // image-data, premultiplied BGRA (DRM ARGB8888); freed once uploaded
        bool                 hasPixels = false; // the LAST Notify carried image-data (outlives the freed buffer)
        int                  pw = 0, ph = 0;
        std::string          defaultAction; // the "default" action key, "" = none; a body click fires it, never a button
        bool                 canReply = false;   // the sender offered an "inline-reply" action
        std::string          replyPlaceholder;   // x-kde-reply-placeholder-text
        std::string          replySubmitText;    // x-kde-reply-submit-button-text, else the action's own label
        std::vector<SAction>    actions;    // non-default actions -> buttons, in Notify order
        std::vector<SBodyImage> bodyImages; // body <img src> thumbnails
        bool                    actionIcons = false; // the action-icons hint: button ids are icon names
        bool                    resident    = false; // the resident hint: an action keeps the card
        bool                    transient   = false; // the transient hint: bypass history AND residency
        bool                    conversation = false; // fd.o category im.*/call.*: outranks ordinary cards, never bundles, merges by sender
        bool                    absorbed     = false; // the open shade parked this banner; the close returns it
        std::string             fallbackPick;         // the rolled identity face; survives in-place replaces

        bool                 waiting = false; // arrived while suspended (DND): collected, not shown, timeout held
        bool                 banner  = true;  // the popup is up; expiry drops only this — the card stays resident

        float                timeoutMs = 0; // resolved; 0 = sticky
        Time::steady_tp      deadline;      // meaningful when banner && timeoutMs > 0 and not waiting
        Time::steady_tp      arrived;       // Notify arrival (a replace refreshes it); the age lines
        Time::steady_tp      born;          // creation only (a replace keeps it); the arrival spring's key

        // image textures — built ONLY by the warm pass (the texture rule).
        // Text rasters live in text.cpp's keyed cache; only the decoded
        // images cache here (their sources don't re-key per age tick).
        SP<ITexture> iconTex;  // content avatar (or hero)
        SP<ITexture> identTex; // identity icon: the corner badge, or the lead icon when no content
        std::string  imageFor, identFor;
        bool         heroTex      = false; // iconTex was built for the hero layout
        uint64_t     pixelsFor    = 0;
        bool         imageSettled = false; // false while an async decode is in flight
        bool         identSettled = false;
        int          imageIconPx  = 0, identIconPx = 0;
    };
    extern std::vector<SP<SNotif>> notifs;

    // ---- parse.cpp: the untrusted payload -> values a card can hold ----

    namespace Parse {
        // the spec's image-data: width, height, rowstride, has_alpha,
        // bits_per_sample, channels, RGB(A) bytes
        using ImageData = sdbus::Struct<int32_t, int32_t, int32_t, bool, int32_t, int32_t, std::vector<uint8_t>>;

        std::string              sanitizeMarkup(const std::string& in, bool allowLinks = false);
        std::string              attrValue(const std::string& tag, const std::string& attr); // one quoted attr, case-insensitive name
        std::string              oneLine(std::string s);
        std::string              resolveImage(std::string s, int sizePx); // path, file://, or a themed icon NAME
        // one <img src>: the resolved path and its alt — the load-failure
        // fallback line
        struct SImgRef {
            std::string src;
            std::string alt;
        };
        std::vector<SImgRef> extractImages(std::string& body, int sizePx); // pulls <img src> out of the body
        void                     unpackImageData(SNotif& n, const ImageData& d, int capPx); // -> premultiplied BGRA
        std::string              joinAppend(const std::string& oldBody, const std::string& add);
        std::string              foldSenderPrefix(std::string body); // "<b>S</b>\nmsg" -> "<b>S</b>: msg"
    }

    // the collapsed row's one-liner: every card body is chronological
    // (transcript, legacy join and ordinary alike), so the newest message
    // ENDS it. Renderer and the topline probe share this so they cannot
    // drift apart.
    std::string lastLine(const std::string& body); // the newest line of any body ends it
    std::string collapsedLine(const SP<SNotif>& n);

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
                        const std::string& sender,
                        const std::vector<std::string>& actions, const std::map<std::string, sdbus::Variant>& hints, int32_t expireTimeout);

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
            bool osd = false;          // the x-notify-osd blip mark
        };
        void postCard(const SPostCard& c);

        bool       closeOne(uint32_t id, uint32_t reason);
        void       dismissAllLive();                      // "Clear all": every visible card goes; the DND queue stays
        void       dismissApp(const std::string& key);    // a bundle's right-click; the key may be (app, group)
        std::string groupKeyOf(const SP<SNotif>& n);       // the bundle's identity: the app key, sub-keyed by the declared group
        void       absorbPopped();                        // opening the shade parks the popped stack
        void       repopAbsorbed();                       // closing it returns the parked stack to banners
        // the transcript of the latest `limit` kept messages, chronological
        // (oldest leads, newest ends): the banner previews five, the shade
        // renders all seven through the card's stored body
        std::string conversationBody(const SP<SNotif>& n, size_t limit);
        void       rearmExpiry();
        void       holdBanner(uint32_t id); // the hovered popup's countdown pauses; 0 releases (and restarts it)
        void       toggleSuspend();         // DND; resume renders the queue, fresh timeouts
        bool       suspendedNow();
        std::pair<uint32_t, uint32_t> badgeCounts(); // {bannered, resident} — the bell's two numbers

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

    // ---- bus.cpp: the connection ----

    namespace Bus {
        void init();
        void exit();
        void invokeAction(uint32_t id, const std::string& key, const std::string& sender);
        void sendReply(uint32_t id, const std::string& text); // NotificationReplied, then close unless resident
        void emitClosed(uint32_t id, uint32_t reason);        // the model's outbound half of a card's death
    }

    // ---- icons.cpp ----

    // (Re)build n.iconTex (content) and n.identTex (identity) when their
    // sources changed. iconPx caps the icon-box raster; screenshot-sized
    // content (256 px both ways, at least half the hero box) raster to
    // heroWPx instead, cover-cropped to heroHCapPx, and set heroTex.
    void resetFallbackCache(); // forget the fallback_icon_dir listing (a config reload rescans)
    void ensureIconTex(SNotif& n, int iconPx, int heroWPx, int heroHCapPx);
    void ensureAvatarTex(SParticipant& p, int px);
    std::string resolveDesktopEntryIcon(const std::string& entry, int sizePx);
    void        iconsInit();
    void        iconsExit();

    // (Re)build an action button's icon when action-icons is set and its id (an
    // icon name or a path) changed; clears it when the hint is off.
    void ensureActionIcon(SNotif& n, SAction& a, int iconPx);

    // (Re)build a body <img> thumbnail when its src changed. maxPx caps the
    // decoded raster.
    void ensureBodyImage(SBodyImage& im, int maxPx);

    // Downscale n.pixels in place when it exceeds maxPx (unpack-time cap).
    void shrinkPixels(SNotif& n, int maxPx);

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
    void centerToggleRow(uint32_t id);
    void centerSelectMove(int dir);                         // ↑/↓: move the keyboard selection, paging to keep it on screen
    bool centerSelection(uint32_t& id, std::string& group); // the selected item; group non-empty = a bundle. false = none

    // the click door (Lua, the bell, `hyprctl awesome center`): deferred and
    // accumulating
    // the shade opens on `on` when given (the bell clicked there), else
    // where the cards are
    void queueCenterToggle(PHLMONITOR on = nullptr);
    // move the cards (and the shade) to this monitor, damaging both spots
    void placeCardsOn(PHLMONITOR mon);
    void centerExit();

    void surfaceInit(); // the age/motion tick timers
    void surfaceExit();

    // hit rects of the last layout, global logical — input hit-tests these
    struct SCard {
        enum eKind : uint8_t {
            POPUP = 0,
            ROW,       // a shade row
            DIGEST,    // a folded app bundle (group = app key)
            GHEAD,     // an expanded bundle's header row
            CHILD,     // a bundle child row
            BTN_CLEAR, // footer "Clear all": the global sweep
            BTN_DND,   // footer ⊖ (do-not-disturb)
            PANEL,     // the shade panel body: swallows clicks, owns the wheel
            PAGE_UP,   // the shade's paging chips (drawn over the edge rows)
            PAGE_DOWN,
        };
        eKind       kind = POPUP;
        CBox        box;
        uint32_t    id   = 0; // live identity
        std::string group;    // DIGEST/GHEAD/CHILD: the app key
        CBox        chevron;      // ROW: the 24Ø fold indicator; w = 0 -> none
        CBox        close;        // POPUP hover-✕ / GHEAD ✕; w = 0 -> none
        CBox        replyField;   // ROW: the armed inline-reply box (swallows, never acts)
        CBox        replySend;    // ROW: its send pill
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
    // chevron/✕ corners. A change damages only the boxes involved.
    struct SHover {
        uint32_t     id = 0;
        std::string  group;
        SCard::eKind kind = SCard::POPUP;
        int          btn  = -1;
        // 0 body, 1 chevron, 2 close, 3 reply field, 4 send
        uint8_t      part = 0;
        bool         operator==(const SHover&) const = default;
    };
    void setHovered(const SHover& h);

    // ---- reply.cpp: the inline-reply field ----
    //
    // A card whose sender offered "inline-reply" grows a text field in its
    // open shade row. While one is armed the shade owns EVERY key (the same
    // grab the shell's menubar prompt takes) — there is no keyboard focus to
    // hand it, so it takes the keys and gives back what it does not use.

    bool               replyArmedOn(uint32_t id); // this card's field is the armed one
    bool               replyArmed();
    void               replyOpen(uint32_t id);
    void               replyClose();
    const std::string& replyText();
    // a key while armed. Returns false when the key is not ours to eat.
    bool               replyKey(xkb_state* state, uint32_t keycode);
    void               replyExit();

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
    void inputExit();

} // namespace NAwesome::Notify
