// The app's own pointer. The input goes to the target client's wl_pointer
// objects (enter, motion, press and release or wheel steps, leave), never
// through the seat: the user's pointer focus, cursor position and image,
// held buttons and keyboard focus stay the user's. The one shared resource
// is the client's pointer itself: when the user's pointer is in the same
// client (one Firefox process owns all its windows), the input steps out of
// the user's surface and back in under the user's cursor (docs/design.md).
// In the mouse mode the app's pointer stays where the input left it (the
// app keeps the hover, as under a mouse at rest); in the tap mode it leaves.
// A held button (press ... release) is the app's implicit grab: every
// motion goes to the surface the press began in, and nothing else of the
// seat's ever learns of it (docs/design.md, a held button).
#include "vclick.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/ViewHitTester.hpp>
#include <hyprland/src/desktop/state/ViewQuery.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/protocols/core/DataDevice.hpp>
#include <hyprland/src/protocols/core/Seat.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <utility>

namespace VClick {
    std::optional<std::string> blocked() {
        if (g_pCompositor->m_isShuttingDown)
            return "error: the compositor is shutting down";
        // the lockscreen is no reason: the input goes only to the window it
        // names (never a lock surface, which resolve refuses), and the seat
        // stays the lockscreen's (docs/design.md, under the lockscreen)
        return std::nullopt;
    }
}

namespace VClick::Pointer {
    // One wheel detent as a real mouse wheel sends it (libinput: 15 degrees,
    // value120 120), never scaled by input:scroll_factor: a script's step is
    // the same on every setup.
    constexpr double WHEEL_STEP     = 15.0;
    constexpr int    WHEEL_STEP_120 = 120;

    PHLWINDOW windowOf(std::string_view selector) {
        return Desktop::viewState()->query().selector(std::string{selector}).runWindow();
    }

    Vector2D windowPos(const PHLWINDOW& w) {
        return w->position(Desktop::View::IGeometric::GEOMETRIC_GOAL);
    }

    Vector2D windowSize(const PHLWINDOW& w) {
        return w->size(Desktop::View::IGeometric::GEOMETRIC_GOAL);
    }

    SP<CWLSurfaceResource> mainSurface(const PHLWINDOW& w) {
        return w && w->wlSurface() ? w->wlSurface()->resource() : nullptr;
    }

    wl_client* clientOf(const PHLWINDOW& w) {
        const auto SURF = mainSurface(w);
        return SURF ? SURF->client() : nullptr;
    }

    // the compositor's own hit test: popups, then the subsurface tree; a
    // point no input region takes falls back to the main surface, as for
    // the real pointer
    SP<CWLSurfaceResource> surfaceAt(const PHLWINDOW& w, const Vector2D& pos, Vector2D& local) {
        auto surf = Desktop::viewState()->hitTest().windowSurfaceAt(windowPos(w) + pos, w, local);
        if (!surf) {
            surf  = mainSurface(w);
            local = pos + w->contentOffset();
        }
        return surf;
    }

    // Every wl_pointer the client bound, across all its wl_seat bindings (a
    // toolkit and the app may each bind one; the seat sends real input to
    // all of them). The seat protocol's own list is private; the client's
    // resource list is public libwayland.
    std::vector<SP<CWLPointerResource>> pointersOf(wl_client* client) {
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

    bool userIn(wl_client* client) {
        const auto USER = g_pSeatManager->m_state.pointerFocus.lock();
        return USER && USER->client() == client;
    }

    uint32_t nowMs() {
        return sc<uint32_t>(Time::millis(Time::steadyNow()));
    }

    std::expected<STarget, std::string> resolve(std::string_view selector, const Vector2D& pos) {
        if (const auto WHY = blocked())
            return std::unexpected(*WHY);

        STarget t;
        t.window = windowOf(selector);
        if (!t.window)
            return std::unexpected("error: window not found");
        if (!Desktop::View::validMapped(t.window) || t.window->isHidden())
            return std::unexpected("error: the window is not mapped");
        // the hit test RASSERTs on an X11 window: that aborts the compositor
        if (t.window->backend().isX11())
            return std::unexpected("error: XWayland windows are not supported");

        // x, y: logical px from the window box's top-left, the origin of a
        // window capture (grim -T)
        const auto SIZE = windowSize(t.window);
        t.inbox         = pos.x >= 0 && pos.y >= 0 && pos.x < SIZE.x && pos.y < SIZE.y;
        if (!t.inbox && !t.window->hasPopupAt(windowPos(t.window) + pos))
            return std::unexpected(std::format("error: {:g},{:g} is outside the window ({:g}x{:g})", pos.x, pos.y, SIZE.x, SIZE.y));

        t.surf = surfaceAt(t.window, pos, t.local);
        if (!t.surf || (g_pSessionLockManager && g_pSessionLockManager->isSurfaceSessionLock(t.surf)))
            return std::unexpected("error: the window has no surface");
        t.pointers = pointersOf(t.surf->client());
        if (t.pointers.empty())
            return std::unexpected("error: the app has no pointer (it bound no wl_pointer)");
        t.user   = g_pSeatManager->m_state.pointerFocus.lock();
        t.shared = t.user && t.user->client() == t.surf->client();
        return t;
    }

    static void each(const std::vector<SP<CWLPointerResource>>& pointers, const auto& fn) {
        for (const auto& p : pointers) {
            fn(*p);
            p->sendFrame();
        }
    }

    // --- the hover a move keeps ------------------------------------------------
    //
    // A move leaves the app's pointer where it went: the app keeps the hover.
    // The seat ends it whenever the user's pointer changes surface (it sends
    // every app's pointer a leave); vclick ends it at its next input into
    // another app, and on unload.
    static WP<CWLSurfaceResource> hovered;

    void hover(const SP<CWLSurfaceResource>& surf) {
        hovered = surf;
    }

    void unhover(wl_client* keep) {
        const auto SURF = hovered.lock();
        hovered.reset();
        if (!SURF)
            return;
        if (SURF->client() == keep) {
            hovered = SURF; // the next input continues in this app
            return;
        }
        if (userIn(SURF->client()))
            return; // the user's pointer came in: the seat owns that pointer now
        each(pointersOf(SURF->client()), [](CWLPointerResource& p) { p.sendLeave(); });
    }

    // a hover kept in another surface of the same app ends before the
    // pointer enters this one: a leave, then the enter, as a mouse crosses
    static void leaveHeldOther(const SP<CWLSurfaceResource>& surf) {
        const auto HOVERED = hovered.lock();
        if (!HOVERED || HOVERED == surf || HOVERED->client() != surf->client())
            return;
        hovered.reset();
        each(pointersOf(HOVERED->client()), [](CWLPointerResource& p) { p.sendLeave(); });
    }

    static std::vector<std::pair<PHLWINDOWREF, Vector2D>> positions;

    void rememberPos(const PHLWINDOW& w, const Vector2D& pos) {
        std::erase_if(positions, [&](const auto& e) { return !e.first.lock() || e.first.lock() == w; });
        positions.emplace_back(w, pos);
    }

    std::optional<Vector2D> lastPos(const PHLWINDOW& w) {
        for (const auto& [ref, pos] : positions)
            if (ref.lock() == w)
                return pos;
        return std::nullopt;
    }

    // --- one burst -------------------------------------------------------------

    static Result act(const SAction& c) {
        auto T = resolve(c.selector, c.pos);
        if (!T)
            return fail(T.error());
        const auto& POINTERS = T->pointers;

        // The user's pointer. Another client never hears of this input. The
        // same client has one pointer: it leaves the user's surface and
        // re-enters it under the cursor — never under a press (a leave
        // releases the client's held buttons), a constraint or a drag.
        const auto USER   = T->user;
        const bool SHARED = T->shared;
        if (SHARED && (g_pInputManager->hasHeldButtons() || g_pInputManager->isConstrained() || (PROTO::data && PROTO::data->dndActive())))
            return fail("error: busy: the user's pointer is pressed, constrained or dragging in this app; retry later");
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

        auto t = nowMs();
        if (SHARED)
            each(POINTERS, [](CWLPointerResource& p) { p.sendLeave(); });
        else
            leaveHeldOther(T->surf);
        // a pointer already in this surface (the end of a glide, a held
        // move) gets no second enter: sendEnter is a no-op there
        each(POINTERS, [&](CWLPointerResource& p) {
            p.sendEnter(T->surf, T->local);
            p.sendMotion(t, T->local);
        });
        if (c.wheel) {
            // one frame per detent, as a wheel sends it; the discrete forms
            // and the relative direction precede their axis event (wl_pointer:
            // "always followed by exactly one axis event"); each send gates
            // on the pointer's version (discrete 5-7, value120 8+, direction 9+)
            const auto [AXIS, SIGN] = *c.wheel;
            for (int i = 0; i < c.count; ++i) {
                ++t;
                each(POINTERS, [&](CWLPointerResource& p) {
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
                each(POINTERS, [&](CWLPointerResource& p) { p.sendButton(t, c.button, WL_POINTER_BUTTON_STATE_PRESSED); });
                ++t;
                each(POINTERS, [&](CWLPointerResource& p) { p.sendButton(t, c.button, WL_POINTER_BUTTON_STATE_RELEASED); });
            }
        }
        if (c.stay && !SHARED) {
            hovered = T->surf; // the app keeps the hover, as under a mouse at rest
            return {};
        }
        each(POINTERS, [](CWLPointerResource& p) { p.sendLeave(); });
        if (SHARED)
            each(POINTERS, [&](CWLPointerResource& p) { p.sendEnter(USER, USERLOCAL); });
        if (hovered.lock() == T->surf)
            hovered.reset(); // that leave ended it
        return {};
    }

    // a move without a glide: straight there, and stay
    static Result moveNow(const SAction& c) {
        auto T = resolve(c.selector, c.pos);
        if (!T)
            return fail(T.error());
        if (T->shared)
            return fail("error: busy: the user's pointer is in this app; a move would take it from them");
        const auto t = nowMs();
        leaveHeldOther(T->surf);
        each(T->pointers, [&](CWLPointerResource& p) {
            p.sendEnter(T->surf, T->local);
            p.sendMotion(t, T->local);
        });
        hovered = T->surf;
        return {};
    }

    // --- a held button ---------------------------------------------------------
    //
    // press ... release: the app hears one press, the motions vclick sends
    // meanwhile, and the release. Its implicit grab: every motion goes to the
    // press's surface, in its coordinates, wherever the point is. The press's
    // serial is struck from the seat's record at once, so the app cannot make
    // it an interactive move or resize, which the compositor would drive with
    // the user's real mouse; a drag-and-drop it begins is refused in
    // main.cpp's hook, for the same reason. A leave releases it (the
    // compositor sends the release first), and Hyprland leaves every app's
    // pointer whenever the user's pointer changes surface: that ends it.
    bool dragGuard = false;

    struct SHeld {
        PHLWINDOWREF           window;
        WP<CWLSurfaceResource> surf; // the press's: the implicit grab
        wl_client*             client = nullptr;
        uint32_t               button = 0;
        bool                   stay   = false; // the mode at the press
    };
    static std::optional<SHeld> held;
    static struct {
        wl_client*      client = nullptr;
        Time::steady_tp at;
    } letGo; // where and when vclick last let a button go

    std::optional<uint32_t> heldButton() {
        return held ? std::optional{held->button} : std::nullopt;
    }

    PHLWINDOW heldWindow() {
        return held ? held->window.lock() : nullptr;
    }

    bool holdsIn(wl_client* client) {
        if (held && held->client == client)
            return true;
        return letGo.client == client && Time::steadyNow() - letGo.at < std::chrono::milliseconds(GRACE_MS);
    }

    std::string buttonName(uint32_t button) {
        switch (button) {
            case BTN_LEFT: return "left";
            case BTN_RIGHT: return "right";
            case BTN_MIDDLE: return "middle";
            case BTN_SIDE: return "side";
            case BTN_EXTRA: return "extra";
            default: return std::to_string(button);
        }
    }

    // the release, to the press's surface while the app's pointer is still
    // vclick's (a leave the seat sent has released it already)
    static void letGoOf() {
        if (!held)
            return;
        if (const auto SURF = held->surf.lock(); SURF && !userIn(SURF->client())) {
            const auto T = nowMs();
            each(pointersOf(SURF->client()), [&](CWLPointerResource& p) { p.sendButton(T, held->button, WL_POINTER_BUTTON_STATE_RELEASED); });
        }
        letGo = {held->client, Time::steadyNow()};
        held.reset();
    }

    void seatMoved() {
        if (!held || g_pCompositor->m_isShuttingDown)
            return;
        const auto SURF = held->surf.lock();
        // the seat's leave let the button go already; nothing to send
        letGo = {held->client, Time::steadyNow()};
        held.reset();
        hovered.reset();
        noteStop(std::string{"the hold ended: "} +
                 (SURF && userIn(SURF->client()) ? "the user's pointer came into the app" : "the user's pointer changed surface, and the seat let the button go"));
    }

    std::optional<std::string> holdLost() {
        if (!held)
            return std::nullopt;
        const auto  W    = held->window.lock();
        const auto  SURF = held->surf.lock();
        std::string why;
        if (SURF && userIn(SURF->client()))
            why = "the user's pointer came into the app"; // the seat's leave let it go
        else if (!SURF || !W || !Desktop::View::validMapped(W) || W->isHidden())
            why = "the window closed or was hidden";
        else
            return std::nullopt;
        letGoOf();
        unhover();
        noteStop("the hold ended: " + why);
        return why;
    }

    void dropHold(const std::string& why) {
        if (!held) { // let go of under a second ago: the app read the last motion late
            noteStop("the app began a drag-and-drop as the button went up, which only the user's mouse can drive; refused");
            return;
        }
        const bool STAY = held->stay;
        Glide::abandon(); // a held move's way ends where it is
        letGoOf();
        if (!STAY) {
            unhover();
            Cursor::rest(false);
        }
        noteStop("the hold ended: " + why);
    }

    // the press's surface, and a window point in its coordinates
    SP<CWLSurfaceResource> heldAt(const Vector2D& pos, Vector2D& local) {
        const auto W    = held ? held->window.lock() : nullptr;
        const auto SURF = held ? held->surf.lock() : nullptr;
        if (!W || !SURF)
            return nullptr;
        const auto HL  = Desktop::View::CWLSurface::fromResource(SURF);
        const auto BOX = HL ? HL->getSurfaceBoxGlobal() : std::nullopt;
        local          = BOX ? windowPos(W) + pos - BOX->pos() : pos + W->contentOffset();
        return SURF;
    }

    Result heldMotion(const Vector2D& pos) {
        if (const auto WHY = holdLost())
            return fail("error: the hold ended: " + *WHY);
        Vector2D   local;
        const auto SURF = heldAt(pos, local);
        if (!SURF)
            return fail("error: no button is held");
        const auto T = nowMs();
        each(pointersOf(SURF->client()), [&](CWLPointerResource& p) { p.sendMotion(T, local); });
        return {};
    }

    Result release() {
        if (holdLost() || !held)
            return fail("error: no button is held");
        letGoOf();
        return {};
    }

    static Result pressNow(const SAction& c) {
        if (!dragGuard)
            return fail("error: no drag-and-drop guard (the initiateDrag hook is out): a held button could hand a drag to the user's mouse");
        auto T = resolve(c.selector, c.pos);
        if (!T)
            return fail(T.error());
        if (T->shared)
            return fail("error: busy: the user's pointer is in this app; a press would take it from them");
        auto t = nowMs();
        leaveHeldOther(T->surf);
        each(T->pointers, [&](CWLPointerResource& p) {
            p.sendEnter(T->surf, T->local);
            p.sendMotion(t, T->local);
        });
        ++t;
        each(T->pointers, [&](CWLPointerResource& p) {
            p.sendButton(t, c.button, WL_POINTER_BUTTON_STATE_PRESSED);
            g_pSeatManager->clearPointerButtonSerials(p.m_owner.lock(), T->surf, c.button); // no interactive move or resize from it
        });
        hovered = T->surf;
        held    = SHeld{.window = T->window, .surf = T->surf, .client = T->surf->client(), .button = c.button, .stay = c.stay};
        clearStop();
        if (!c.stay) {
            Cursor::prepare(T->window);
            Cursor::show(T->window, c.pos); // the tap mode: shown while the button is down
        }
        return {};
    }

    // --- a paced scroll --------------------------------------------------------
    //
    // Each step after the first is its own burst on the event loop, checked
    // afresh (the window may close, hide or move, the user's pointer may
    // come into the app). A busy app is waited out for up to BUSY_MS; any
    // other error ends the run. One run at a time, and nothing else
    // meanwhile: what a script scrolls is what it asked for.
    struct SRun {
        SAction             step; // count 1
        int                 total  = 0;
        int                 done   = 0;
        int                 waited = 0; // ms spent on a busy app for the next step
        SP<CEventLoopTimer> timer;
    };
    static std::optional<SRun> run;

    static void endRun() {
        if (run && run->timer && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(run->timer);
        run.reset();
    }

    static void tick() {
        if (!run)
            return;
        const auto R = act(run->step);
        if (R) {
            ++run->done;
            run->waited = 0;
        } else if (busy(R) && run->waited + run->step.ms <= BUSY_MS) {
            run->waited += run->step.ms;
        } else {
            noteStop(std::format("the last scroll stopped after {}/{}: {}", run->done, run->total, R.error()));
            endRun();
            return;
        }
        if (run->done >= run->total) {
            endRun();
            return;
        }
        run->timer->updateTimeout(std::chrono::milliseconds(run->step.ms));
    }

    std::optional<std::string> scrollStatus() {
        if (!run)
            return std::nullopt;
        return std::format("scrolling {}/{}", run->done, run->total);
    }

    Result dispatch(const SAction& c) {
        if (c.verb == EVerb::MOVE)
            return held ? heldMotion(c.pos) : moveNow(c);
        if (c.verb == EVerb::PRESS)
            return pressNow(c);
        if (c.verb == EVerb::DRAG) {
            if (const auto R = pressNow(c); !R)
                return R;
            return Glide::travel(c);
        }
        if (!c.wheel || c.count == 1 || c.ms == 0)
            return act(c);

        // the first step now, so its errors are the reply; the rest paced
        SAction step = c;
        step.count   = 1;
        if (const auto R = act(step); !R)
            return R;
        clearStop();
        run.emplace(SRun{.step = step, .total = c.count, .done = 1});
        run->timer = makeShared<CEventLoopTimer>(std::chrono::milliseconds(step.ms), [](SP<CEventLoopTimer>, void*) { tick(); }, nullptr);
        g_pEventLoopManager->addTimer(run->timer);
        return {};
    }

    void reset() {
        endRun();
        letGoOf(); // the release, then the leave
        unhover();
        positions.clear();
        letGo = {};
    }
}
