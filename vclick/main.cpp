// vclick: click or scroll inside a window without the user's pointer.
//
// The input goes to the target client's own wl_pointer objects (enter,
// motion, press and release or wheel steps, leave), never through the
// seat: the user's
// pointer focus, cursor position and image, held buttons and keyboard
// focus stay the user's. The one shared resource is the client's pointer
// itself: when the user's pointer is in the same client (one Firefox
// process owns all its windows), it steps out of the user's surface for
// the input and back in under the user's cursor (docs/design.md).
// A multi-step scroll is paced: one step per interval on the compositor's
// event loop, each step checked and sent on its own.
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/ViewHitTester.hpp>
#include <hyprland/src/desktop/state/ViewQuery.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/protocols/core/DataDevice.hpp>
#include <hyprland/src/protocols/core/Seat.hpp>

#include <linux/input-event-codes.h>
#include <wayland-server-core.h>

#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <expected>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace NVClick {
    inline constexpr const char* NAME    = "vclick";
    inline constexpr const char* AUTHOR  = "hitori";
    inline constexpr const char* VERSION = "1.1.0"; // lockstep with hyprpm.toml's [vclick]

    constexpr int              MAX_CLICKS = 3;
    constexpr int              MAX_STEPS  = 20;
    constexpr int              STEP_MS    = 50;   // the default pace: a brisk wheel spin
    constexpr int              MAX_MS     = 1000;
    constexpr int              BUSY_MS    = 1000; // how long a paced step waits out a busy app
    constexpr std::string_view USAGE      = "usage: vclick <window> <x> <y> [button [clicks 1-3] | scroll-up|scroll-down|scroll-left|scroll-right [steps 1-20 [ms 0-1000]]]\n"
                                            "       vclick status\n"
                                            "  button: left (default), right, middle, side, extra, or a code 272-279\n"
                                            "  ms: between scroll steps (default 50); 0 sends them all at once";

    // One wheel detent as a real mouse wheel sends it (libinput: 15 degrees,
    // value120 120), never scaled by input:scroll_factor: a script's step is
    // the same on every setup.
    constexpr double WHEEL_STEP     = 15.0;
    constexpr int    WHEEL_STEP_120 = 120;

    struct SWheel {
        wl_pointer_axis axis;
        int             sign; // +1: down / right (the content moves up / left)
    };

    struct SAction {
        std::string           selector;
        Vector2D              pos;
        uint32_t              button = BTN_LEFT; // a click, unless wheel
        std::optional<SWheel> wheel;
        int                   count = 1;       // clicks, or wheel steps
        int                   ms    = STEP_MS; // between wheel steps; 0: all at once
    };

    static std::vector<std::string_view> words(std::string_view s) {
        std::vector<std::string_view> out;
        size_t                        i = 0;
        while (i < s.size()) {
            while (i < s.size() && std::isspace(sc<unsigned char>(s[i])))
                ++i;
            const size_t START = i;
            while (i < s.size() && !std::isspace(sc<unsigned char>(s[i])))
                ++i;
            if (i > START)
                out.emplace_back(s.substr(START, i - START));
        }
        return out;
    }

    template <typename T>
    static std::optional<T> number(std::string_view s) {
        T          v{};
        const auto END        = s.data() + s.size();
        const auto [ptr, err] = std::from_chars(s.data(), END, v);
        if (err != std::errc{} || ptr != END)
            return std::nullopt;
        return v;
    }

    static std::optional<uint32_t> buttonCode(std::string_view s) {
        if (s == "left")
            return BTN_LEFT;
        if (s == "right")
            return BTN_RIGHT;
        if (s == "middle")
            return BTN_MIDDLE;
        if (s == "side")
            return BTN_SIDE;
        if (s == "extra")
            return BTN_EXTRA;
        const auto CODE = number<uint32_t>(s);
        if (CODE && *CODE >= BTN_LEFT && *CODE <= BTN_TASK) // the BTN_MOUSE block
            return CODE;
        return std::nullopt;
    }

    static std::optional<SWheel> wheelOf(std::string_view s) {
        if (s == "scroll-down")
            return SWheel{WL_POINTER_AXIS_VERTICAL_SCROLL, +1};
        if (s == "scroll-up")
            return SWheel{WL_POINTER_AXIS_VERTICAL_SCROLL, -1};
        if (s == "scroll-right")
            return SWheel{WL_POINTER_AXIS_HORIZONTAL_SCROLL, +1};
        if (s == "scroll-left")
            return SWheel{WL_POINTER_AXIS_HORIZONTAL_SCROLL, -1};
        return std::nullopt;
    }

    // W: the request's words, "vclick <window> <x> <y> [button|scroll-*] [count] [ms]"
    static std::expected<SAction, std::string> parse(const std::vector<std::string_view>& W) {
        if (W.size() < 4 || W.size() > 7 || W[0] != NAME)
            return std::unexpected(std::string{USAGE});

        SAction    c{.selector = std::string{W[1]}};
        const auto X = number<double>(W[2]);
        const auto Y = number<double>(W[3]);
        if (!X || !Y || !std::isfinite(*X) || !std::isfinite(*Y))
            return std::unexpected(std::format("error: bad coordinates '{} {}'\n{}", W[2], W[3], USAGE));
        c.pos = {*X, *Y};

        if (W.size() > 4) {
            c.wheel = wheelOf(W[4]);
            if (!c.wheel) {
                const auto B = buttonCode(W[4]);
                if (!B)
                    return std::unexpected(std::format("error: unknown button '{}'\n{}", W[4], USAGE));
                c.button = *B;
            }
        }
        if (W.size() > 5) {
            const int  MAX = c.wheel ? MAX_STEPS : MAX_CLICKS;
            const auto N   = number<int>(W[5]);
            if (!N || *N < 1 || *N > MAX)
                return std::unexpected(std::format("error: {} must be 1-{}\n{}", c.wheel ? "steps" : "count", MAX, USAGE));
            c.count = *N;
        }
        if (W.size() > 6) {
            if (!c.wheel)
                return std::unexpected(std::string{USAGE});
            const auto MS = number<int>(W[6]);
            if (!MS || *MS < 0 || *MS > MAX_MS)
                return std::unexpected(std::format("error: ms must be 0-{}\n{}", MAX_MS, USAGE));
            c.ms = *MS;
        }
        return c;
    }

    // Every wl_pointer the client bound, across all its wl_seat bindings (a
    // toolkit and the app may each bind one; the seat sends real input to
    // all of them). The seat protocol's own list is private; the client's
    // resource list is public libwayland.
    static std::vector<SP<CWLPointerResource>> pointersOf(wl_client* client) {
        std::vector<SP<CWLPointerResource>> out;
        wl_client_for_each_resource(
            client,
            [](wl_resource* r, void* data) -> wl_iterator_result {
                if (std::string_view{wl_resource_get_class(r)} == "wl_pointer" && wl_resource_get_user_data(r)) {
                    if (auto p = CWLPointerResource::fromResource(r); p && p->good())
                        sc<std::vector<SP<CWLPointerResource>>*>(data)->emplace_back(std::move(p));
                }
                return WL_ITERATOR_CONTINUE;
            },
            &out);
        return out;
    }

    static std::string act(const SAction& c) {
        if (g_pCompositor->m_isShuttingDown)
            return "error: the compositor is shutting down";
        if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked())
            return "error: the session is locked";

        const auto W = Desktop::viewState()->query().selector(c.selector).runWindow();
        if (!W)
            return "error: window not found";
        if (!Desktop::View::validMapped(W) || W->isHidden())
            return "error: the window is not mapped";
        // the hit test RASSERTs on an X11 window: that aborts the compositor
        if (W->backend().isX11())
            return "error: XWayland windows are not supported";

        // x, y: logical px from the window box's top-left, the origin of a
        // window capture (grim -T)
        const auto SIZE   = W->size(Desktop::View::IGeometric::GEOMETRIC_GOAL);
        const auto GLOBAL = W->position(Desktop::View::IGeometric::GEOMETRIC_GOAL) + c.pos;
        const bool INBOX  = c.pos.x >= 0 && c.pos.y >= 0 && c.pos.x < SIZE.x && c.pos.y < SIZE.y;
        if (!INBOX && !W->hasPopupAt(GLOBAL))
            return std::format("error: {:g},{:g} is outside the window ({:g}x{:g})", c.pos.x, c.pos.y, SIZE.x, SIZE.y);

        // the compositor's own hit test: popups, then the subsurface tree;
        // a point no input region takes falls back to the main surface, as
        // for the real pointer
        Vector2D local;
        auto     surf = Desktop::viewState()->hitTest().windowSurfaceAt(GLOBAL, W, local);
        if (!surf) {
            surf  = W->wlSurface() ? W->wlSurface()->resource() : nullptr;
            local = c.pos + W->contentOffset();
        }
        if (!surf)
            return "error: the window has no surface";

        wl_client* const CLIENT   = surf->client();
        const auto       POINTERS = pointersOf(CLIENT);
        if (POINTERS.empty())
            return "error: the app has no pointer (it bound no wl_pointer)";

        // The user's pointer. Another client never hears of this input. The
        // same client has one pointer: it leaves the user's surface and
        // re-enters it under the cursor — never under a press (a leave
        // releases the client's held buttons), a constraint or a drag.
        const auto USER   = g_pSeatManager->m_state.pointerFocus.lock();
        const bool SHARED = USER && USER->client() == CLIENT;
        if (SHARED && (g_pInputManager->hasHeldButtons() || g_pInputManager->isConstrained() || (PROTO::data && PROTO::data->dndActive())))
            return "error: busy: the user's pointer is pressed, constrained or dragging in this app; retry later";
        // the cursor over the surface's global box, as the input manager
        // places a held focus; the seat's last motion is stale after an
        // enter without one (a workspace switch, a window closing under it)
        auto USERLOCAL = g_pSeatManager->lastPointerLocal();
        if (SHARED) {
            const auto HL  = Desktop::View::CWLSurface::fromResource(USER);
            const auto BOX = HL ? HL->getSurfaceBoxGlobal() : std::nullopt;
            if (BOX)
                USERLOCAL = g_pInputManager->getMouseCoordsInternal() - BOX->pos();
        }

        auto       t    = sc<uint32_t>(Time::millis(Time::steadyNow()));
        const auto EACH = [&POINTERS](auto&& fn) {
            for (const auto& p : POINTERS) {
                fn(*p);
                p->sendFrame();
            }
        };

        if (SHARED)
            EACH([](CWLPointerResource& p) { p.sendLeave(); });
        EACH([&](CWLPointerResource& p) {
            p.sendEnter(surf, local);
            p.sendMotion(t, local);
        });
        if (c.wheel) {
            // one frame per detent, as a wheel sends it; the discrete forms
            // and the relative direction precede their axis event (wl_pointer:
            // "always followed by exactly one axis event"); each send gates
            // on the pointer's version (discrete 5-7, value120 8+, direction 9+)
            const auto [AXIS, SIGN] = *c.wheel;
            for (int i = 0; i < c.count; ++i) {
                ++t;
                EACH([&](CWLPointerResource& p) {
                    p.sendAxisSource(WL_POINTER_AXIS_SOURCE_WHEEL);
                    p.sendAxisDiscrete(AXIS, SIGN);
                    p.sendAxisValue120(AXIS, SIGN * WHEEL_STEP_120);
                    p.sendAxisRelativeDirection(AXIS, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
                    p.sendAxis(t, AXIS, SIGN * WHEEL_STEP);
                });
            }
        } else {
            for (int i = 0; i < c.count; ++i) {
                ++t;
                EACH([&](CWLPointerResource& p) { p.sendButton(t, c.button, WL_POINTER_BUTTON_STATE_PRESSED); });
                ++t;
                EACH([&](CWLPointerResource& p) { p.sendButton(t, c.button, WL_POINTER_BUTTON_STATE_RELEASED); });
            }
        }
        EACH([](CWLPointerResource& p) { p.sendLeave(); });
        if (SHARED)
            EACH([&](CWLPointerResource& p) { p.sendEnter(USER, USERLOCAL); });

        return "ok";
    }

    // A paced scroll: each step after the first is its own act() on the
    // event loop, checked afresh (the window may close, hide or move, the
    // user's pointer may come into the app). A busy app is waited out for
    // up to BUSY_MS; any other error ends the run. One run at a time, and
    // nothing else meanwhile: what a script scrolls is what it asked for.
    struct SRun {
        SAction             step; // count 1
        int                 total  = 0;
        int                 done   = 0;
        int                 waited = 0; // ms spent on a busy app for the next step
        SP<CEventLoopTimer> timer;
    };
    static std::optional<SRun> run;
    static std::string         lastStop; // why the last run ended early, if it did

    static void endRun() {
        if (run && run->timer && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(run->timer);
        run.reset();
    }

    static void tick() {
        if (!run)
            return;
        const auto R = act(run->step);
        if (R == "ok") {
            ++run->done;
            run->waited = 0;
        } else if (R.starts_with("error: busy") && run->waited + run->step.ms <= BUSY_MS) {
            run->waited += run->step.ms;
        } else {
            lastStop = std::format("the last scroll stopped after {}/{}: {}", run->done, run->total, R);
            endRun();
            return;
        }
        if (run->done >= run->total) {
            endRun();
            return;
        }
        run->timer->updateTimeout(std::chrono::milliseconds(run->step.ms));
    }

    static std::string status() {
        if (run)
            return std::format("scrolling {}/{}", run->done, run->total);
        return lastStop.empty() ? "idle" : "idle; " + lastStop;
    }

    static std::string handle(const std::string& request) {
        const auto W = words(request);
        if (W.size() == 2 && W[0] == NAME && W[1] == "status")
            return status();
        const auto C = parse(W);
        if (!C)
            return C.error();
        if (run)
            return std::format("error: busy: still scrolling ({}/{}); retry later", run->done, run->total);
        if (!C->wheel || C->count == 1 || C->ms == 0)
            return act(*C);

        // the first step now, so its errors are the reply; the rest paced
        SAction step = *C;
        step.count   = 1;
        const auto R = act(step);
        if (R != "ok")
            return R;
        lastStop.clear();
        run.emplace(SRun{.step = step, .total = C->count, .done = 1});
        run->timer = makeShared<CEventLoopTimer>(std::chrono::milliseconds(step.ms), [](SP<CEventLoopTimer>, void*) { tick(); }, nullptr);
        g_pEventLoopManager->addTimer(run->timer);
        return "ok";
    }
}

namespace {
    HANDLE                     PHANDLE = nullptr;
    SP<IPC::Socket1::SCommand> ctlCmd;
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();
    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[vclick] Version mismatch: rebuild the plugin against the running Hyprland", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[vclick] version mismatch");
    }

    // a prefix command: the handler gets the full request, the command word included
    ctlCmd = HyprlandAPI::registerHyprCtlCommand(PHANDLE, IPC::Socket1::SCommand{.name = NVClick::NAME, .match = IPC::Socket1::COMMAND_MATCH_PREFIX,
                                                                                  .handler = [](const IPC::Socket1::SRequest& request) {
                                                                                      return IPC::Socket1::SResponse{NVClick::handle(request.command)};
                                                                                  }});

    return {NVClick::NAME, "click or scroll inside a window without the user's pointer", NVClick::AUTHOR, NVClick::VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    NVClick::endRun(); // a pending step must never call into an unloaded plugin
    if (ctlCmd)
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, ctlCmd);
    ctlCmd.reset();
}
