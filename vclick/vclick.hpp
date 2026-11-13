// What vclick's units share. The units:
//
//   main.cpp     load and unload: the command, the set_cursor hook, the listeners,
//                the config value
//   command.cpp  the hyprctl command: parse a request, run it, report status
//   lua.cpp      the same verbs as Lua functions
//   pointer.cpp  the app's own pointer: the target under a point, the input in
//                one burst (a jump), a paced scroll, the hover a move holds
//   keyboard.cpp the app's own keyboard: one key, pressed and released
//   glide.cpp    the way there: one motion per tick along the planned path
//   stroke.cpp   a finger's stroke over time: a scroll by pixels
//   path.cpp     the path's math and the user's pace (no compositor)
//   cursor.cpp   the cursor image drawn at the virtual pointer
//   theme.cpp    the cursor theme's shapes, read from XCursor files (no compositor)
#pragma once

#include "path.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/input/Keys.hpp>

#include <linux/input-event-codes.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class CWLSurfaceResource;
class CWLPointerResource;
namespace Event {
    struct SRenderStageEvent;
}

namespace VClick {
    using Path::Vector2D;

    inline constexpr const char* NAME    = "vclick";
    inline constexpr const char* AUTHOR  = "hitori";
    inline constexpr const char* VERSION = "2.3.0"; // lockstep with hyprpm.toml's [vclick]

    constexpr int                MAX_CLICKS = 3;
    constexpr int                MAX_STEPS  = 20;
    constexpr int                STEP_MS    = 50; // a scroll's default pace: a brisk wheel spin
    constexpr int                MAX_MS     = 1000;
    constexpr int                BUSY_MS    = 1000;  // how long a paced step waits out a busy app
    constexpr double             MAX_PX     = 10000; // a finger's scroll
    constexpr int                LIFT_MS    = 200;   // a finger rests before it lifts: a toolkit flings at the speed of the last 150 ms
    constexpr int                GRACE_MS   = 1000;  // a drag-and-drop an app begins this soon after vclick let go is vclick's

    struct SWheel {
        wl_pointer_axis axis;
        int             sign; // +1: down / right (the content moves up / left)
    };

    enum class EVerb : uint8_t {
        CLICK,
        SCROLL,
        MOVE,  // only go there, and stay (with a held button: a drag's way)
        PRESS, // go there and hold a button down
        DRAG,  // press there, glide to `to`, let go
    };

    // one request
    struct SAction {
        EVerb                 verb = EVerb::CLICK;
        std::string           selector;
        Vector2D              pos;
        Vector2D              to;                // where a drag lets go
        uint32_t              button = BTN_LEFT; // a click's, a press's or a drag's
        std::optional<SWheel> wheel;
        bool                  stay  = false;          // the mouse mode: the app keeps the hover after the input
        int                   count = 1;              // clicks, or wheel steps
        int                   ms    = STEP_MS;        // between wheel steps; 0: all at once
        double                px    = 0;              // a finger's scroll, how far; 0: wheel steps
        double                size  = Path::TARGET_W; // the target's size: the glide's Fitts W
        std::optional<bool>   glide;                  // this request's way there; unset: the mode's (mouse glides)
    };

    // one key
    struct SKey {
        std::string         selector;
        uint32_t            code = 0; // evdev
        Input::ModifierMask mods = Input::HL_MODIFIER_NONE;
    };

    // An input's outcome: nothing, or the reply's error line ("error: ...").
    using Result = std::expected<void, std::string>;
    inline std::unexpected<std::string> fail(std::string line) {
        return std::unexpected(std::move(line));
    }
    inline bool busy(const Result& r) {
        return !r && r.error().starts_with("error: busy");
    }

    constexpr const char* verbName(EVerb v) {
        switch (v) {
            case EVerb::CLICK: return "click";
            case EVerb::SCROLL: return "scroll";
            case EVerb::MOVE: return "move";
            case EVerb::PRESS: return "press";
            case EVerb::DRAG: return "drag";
        }
        return "";
    }

    // why no input may go now (the reply's error line), or nothing: the
    // compositor shutting down
    std::optional<std::string> blocked();

    // why the last glide or paced scroll ended early; status reports it
    void noteStop(std::string why);
    void clearStop();

    namespace Command {
        std::string handle(const std::string& request);
        std::string run(const std::vector<std::string_view>& words); // "vclick", the verb, its words
    }

    namespace Lua {
        void init(void* handle); // hl.plugin.vclick.<verb>(...)
    }

    namespace Pointer {
        // the target of a point in a window, checked as the input needs it
        struct STarget {
            PHLWINDOW                           window;
            SP<CWLSurfaceResource>              surf;
            Vector2D                            local;
            std::vector<SP<CWLPointerResource>> pointers;
            SP<CWLSurfaceResource>              user;           // the user's pointer focus
            bool                                shared = false; // the user's pointer is in this app
            bool                                inbox  = true;  // inside the window's box, not only in a popup
        };

        PHLWINDOW                           windowOf(std::string_view selector);
        Vector2D                            windowPos(const PHLWINDOW& w);
        Vector2D                            windowSize(const PHLWINDOW& w);
        SP<CWLSurfaceResource>              mainSurface(const PHLWINDOW& w);
        wl_client*                          clientOf(const PHLWINDOW& w); // null: no window or surface
        SP<CWLSurfaceResource>              surfaceAt(const PHLWINDOW& w, const Vector2D& pos, Vector2D& local);
        std::vector<SP<CWLPointerResource>> pointersOf(wl_client* client);
        // the same, looked up again only when the app changes or one went
        // away: an app may hold thousands of resources, a tick runs 125
        // times a second
        class CPointers {
          public:
            std::vector<SP<CWLPointerResource>>        of(wl_client* client);
            const std::vector<WP<CWLPointerResource>>& last() const {
                return m_cached;
            }

          private:
            std::vector<WP<CWLPointerResource>> m_cached;
            wl_client*                          m_client = nullptr;
        };
        std::expected<STarget, std::string> resolve(std::string_view selector, const Vector2D& pos);
        bool                                userIn(wl_client* client); // the user's pointer focus is this app's
        uint32_t                            nowMs();

        // the input at once: a jump, and the end of a glide
        Result dispatch(const SAction& c);
        // the app's pointer there, and it stays (a move's, a press's, a
        // stroke's start); refused when the user's pointer is in the app
        std::expected<STarget, std::string> place(const SAction& c);

        // the hover a move keeps: keep it, end it (unless it is in keep's app)
        void hover(const SP<CWLSurfaceResource>& surf);
        void unhover(wl_client* keep = nullptr);

        // The button vclick holds down (press ... release), one at a time:
        // the app's implicit grab, so every motion goes to the surface the
        // press began in. A leave releases it (the compositor sends the
        // release), and the seat leaves every app's pointer whenever the
        // user's pointer changes surface: that ends it.
        extern bool                dragGuard; // main.cpp's initiateDrag hook is in: press may hold
        std::optional<uint32_t>    heldButton();
        PHLWINDOW                  heldWindow();
        Result                     release();                                    // let go where it is
        Result                     heldMotion(const Vector2D& pos);              // window coords, to the press's surface
        SP<CWLSurfaceResource>     heldAt(const Vector2D& pos, Vector2D& local); // the press's surface, and pos in its coords
        std::optional<std::string> holdLost();                                   // why a hold ended by itself (noted), and forgets it
        void                       seatMoved();                                  // the user's pointer changed surface: the seat let a held button go
        void                       dropHold(const std::string& why);             // the app began a drag-and-drop: let go, say why
        bool                       holdsIn(wl_client* client);                   // a held button there, or one let go there under a second ago
        std::string                buttonName(uint32_t button);

        // where the virtual pointer last was in each window
        void                       rememberPos(const PHLWINDOW& w, const Vector2D& pos);
        std::optional<Vector2D>    lastPos(const PHLWINDOW& w);

        std::optional<std::string> scrollStatus(); // "scrolling d/t" while a paced scroll runs
        void                       reset();
    }

    namespace Keyboard {
        // <key> [mods]: an xkb key name or code:<n>, mods "ctrl+shift"
        std::expected<SKey, std::string> parse(std::string_view key, std::string_view mods);
        Result                           send(const SKey& k);
    }

    namespace Glide {
        // nullopt: this request goes as a jump (the user's pointer is in the
        // app, or the point is in a popup outside the window's box)
        std::optional<Result> start(const SAction& c);
        bool                  active();
        void                  stop();                   // leaves the app's pointer, if it can
        void                  abandon();                // ends where it is: no leave
        Result                travel(const SAction& c); // a drag's way, its button down: no overshoot, a rest, the release
        bool                  dragging();               // a held button moves

        const Path::SProfile& profile();
        std::string           loadProfile(const std::string& path); // empty: the default path
    }

    // A finger's stroke on a touchpad, over time: a scroll by pixels.
    namespace Stroke {
        Result                     scroll(const SAction& c); // c.px down the wheel's axis and sign
        std::optional<std::string> status();                 // "scrolling d/t px" while one runs
        void                       reset();                  // the finger lifts
    }

    namespace Cursor {
        void        remember(wl_client* client, const SP<CWLSurfaceResource>& surf, const Vector2D& hotspot, std::string shape);
        void        render(const Event::SRenderStageEvent& ev);
        void        prepare(const PHLWINDOW& w);                   // the arrow, the window's app's shape; not inside a render
        void        show(const PHLWINDOW& w, const Vector2D& pos); // while a glide runs
        void        moveTo(const Vector2D& pos);
        void        rest(bool stay);                                // the input ended: stay there (mouse), or linger and fade (tap)
        void        place(const PHLWINDOW& w, const Vector2D& pos); // a jump in the mouse mode: there, and stay
        void        hide();
        void        userMoved(); // the user's pointer changed surface: into the app it takes the cursor; out, the next input starts where it left
        std::string where();     // "address:0x... x y", or "nowhere"
        std::string describe(std::string_view selector);
        void        reset();
        extern bool appImages; // the set_cursor hook is in
    }
}
