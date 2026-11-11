// What vclick's units share. The units:
//
//   main.cpp     load and unload: the command, the set_cursor hook, the listeners
//   command.cpp  the hyprctl command: parse a request, run it, report status
//   pointer.cpp  the app's own pointer: the target under a point, the input in
//                one burst (a jump), a paced scroll, the hover a move holds
//   keyboard.cpp the app's own keyboard: one key, pressed and released
//   glide.cpp    the way there: one motion per tick along the planned path
//   path.cpp     the path's math and the user's pace (no compositor)
//   cursor.cpp   the cursor image drawn at the virtual pointer
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
    inline constexpr const char* VERSION = "2.1.0"; // lockstep with hyprpm.toml's [vclick]

    constexpr int MAX_CLICKS = 3;
    constexpr int MAX_STEPS  = 20;
    constexpr int STEP_MS    = 50;   // a scroll's default pace: a brisk wheel spin
    constexpr int MAX_MS     = 1000;
    constexpr int BUSY_MS    = 1000; // how long a paced step waits out a busy app

    struct SWheel {
        wl_pointer_axis axis;
        int             sign; // +1: down / right (the content moves up / left)
    };

    // one request
    struct SAction {
        std::string           selector;
        Vector2D              pos;
        uint32_t              button = BTN_LEFT; // a click, unless wheel or move
        std::optional<SWheel> wheel;
        bool                  move  = false;   // only go there, and stay
        bool                  stay  = false;   // the mouse mode: the app keeps the hover after the input
        int                   count = 1;       // clicks, or wheel steps
        int                   ms    = STEP_MS; // between wheel steps; 0: all at once
        std::optional<bool>   glide;           // this request's way there; unset: the mode's (mouse glides)
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

    // why no input may go now (the reply's error line), or nothing
    std::optional<std::string> blocked();

    // why the last glide or paced scroll ended early; status reports it
    void noteStop(std::string why);
    void clearStop();

    namespace Command {
        std::string handle(const std::string& request);
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
        std::expected<STarget, std::string> resolve(std::string_view selector, const Vector2D& pos);
        bool                                userIn(wl_client* client); // the user's pointer focus is this app's
        uint32_t                            nowMs();

        // the input at once: a jump, and the end of a glide
        Result                  dispatch(const SAction& c);

        // the hover a move holds: hold, end (unless it is in keep's app)
        void                    hold(const SP<CWLSurfaceResource>& surf);
        void                    release(wl_client* keep = nullptr);

        // where the virtual pointer last was in each window
        void                    rememberPos(const PHLWINDOW& w, const Vector2D& pos);
        std::optional<Vector2D> lastPos(const PHLWINDOW& w);

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
        std::optional<Result>      start(const SAction& c);
        bool                       active();
        void                       stop(); // leaves the app's pointer, if it can

        const Path::SProfile&      profile();
        std::string                loadProfile(const std::string& path); // empty: the default path
    }

    namespace Cursor {
        void        remember(wl_client* client, const SP<CWLSurfaceResource>& surf, const Vector2D& hotspot, std::string shape);
        void        render(const Event::SRenderStageEvent& ev);
        void        prepare();                                      // build the arrow; not inside a render
        void        show(const PHLWINDOW& w, const Vector2D& pos);  // while a glide runs
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
