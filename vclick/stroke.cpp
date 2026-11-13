// A finger's stroke on a touchpad, as one sends it: wl_pointer axis events
// from the finger source (no discrete steps), a frame a tick at the
// profile's rate, then axis_stop as it lifts. The way is straight, its speed
// the profile's bell over Fitts's time for its length (path.cpp); each tick
// sends what the time since the start says is done, so the deltas sum
// exactly to the request (wl_fixed, 1/256 px) whatever the timer's jitter.
// The finger then rests (LIFT_MS) before it lifts: a toolkit flings with the
// speed of the last 150 ms (GTK's), so none follows. Each tick checks
// afresh, as a glide's: the window may close or hide, and the user's pointer
// may come into the app, which takes the app's pointer: the stroke stops.
#include "vclick.hpp"

#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/protocols/core/Seat.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

namespace VClick::Stroke {
    struct SStroke {
        SAction                action;
        PHLWINDOWREF           window;
        WP<CWLSurfaceResource> surf;
        wl_client*             client = nullptr;
        Vector2D               local;
        Path::SSeg             way;       // x: the share done
        int32_t                total = 0; // wl_fixed units (1/256 px), signed
        int32_t                sent  = 0; // of those
        Time::steady_tp        start;
        int                    period = 8;
        Pointer::CPointers     pointers;
        SP<CEventLoopTimer>    timer;
    };
    static std::optional<SStroke> stroke;

    static std::string            done() {
        return std::format("{}/{:g} px", std::lround(std::abs(stroke->sent) / 256.0), stroke->action.px);
    }

    // the finger lifts: the stop, then the app keeps the hover (keep) or
    // its pointer leaves; nothing once the pointer is not vclick's
    static void end(bool keep) {
        if (!stroke)
            return;
        if (stroke->timer && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(stroke->timer);
        const auto SURF = stroke->surf.lock();
        if (SURF && !blocked() && !Pointer::userIn(stroke->client)) {
            const auto T = Pointer::nowMs();
            for (const auto& p : stroke->pointers.of(stroke->client)) {
                p->sendEnter(SURF, stroke->local); // after the seat's leave, again; else a no-op
                p->sendAxisSource(WL_POINTER_AXIS_SOURCE_FINGER);
                p->sendAxisStop(T, stroke->action.wheel->axis);
                p->sendFrame();
            }
            Pointer::hover(SURF);
            if (!keep)
                Pointer::unhover();
        }
        stroke.reset();
    }

    static void tick() {
        if (!stroke)
            return;
        const auto  W    = stroke->window.lock();
        const auto  SURF = stroke->surf.lock();
        std::string why;
        if (const auto WHY = blocked())
            why = WHY->substr(7); // past "error: "
        else if (!W || !SURF || !Desktop::View::validMapped(W) || W->isHidden())
            why = "the window closed or was hidden";
        else if (Pointer::userIn(stroke->client))
            why = "the user's pointer came into the app"; // the seat took its pointer
        if (!why.empty()) {
            noteStop(std::format("the last scroll stopped after {}: {}", done(), why));
            end(false);
            return;
        }

        const double EL = std::chrono::duration<double, std::milli>(Time::steadyNow() - stroke->start).count();
        if (EL >= stroke->way.ms + LIFT_MS) {
            end(stroke->action.stay);
            return;
        }
        const int32_t NOW = EL >= stroke->way.ms ? stroke->total : sc<int32_t>(std::lround(stroke->total * Path::along(stroke->way, EL).x));
        if (NOW != stroke->sent) {
            const auto PTRS = stroke->pointers.of(stroke->client);
            if (PTRS.empty()) {
                noteStop(std::format("the last scroll stopped after {}: the app has no pointer", done()));
                end(false);
                return;
            }
            const auto T    = Pointer::nowMs();
            const auto AXIS = stroke->action.wheel->axis;
            for (const auto& p : PTRS) {
                p->sendEnter(SURF, stroke->local); // after the seat's leave (the user's pointer changed surface), again
                p->sendAxisSource(WL_POINTER_AXIS_SOURCE_FINGER);
                p->sendAxisRelativeDirection(AXIS, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
                p->sendAxis(T, AXIS, (NOW - stroke->sent) / 256.0);
                p->sendFrame();
            }
            stroke->sent = NOW;
        }
        stroke->timer->updateTimeout(std::chrono::milliseconds(stroke->period));
    }

    Result scroll(const SAction& c) {
        const auto T = Pointer::place(c);
        if (!T)
            return fail(T.error());
        const auto& PACE = Glide::profile();
        stroke.emplace(SStroke{.action = c,
                               .window = T->window,
                               .surf   = T->surf,
                               .client = T->surf->client(),
                               .local  = T->local,
                               .way    = Path::stroke(c.px, PACE),
                               .total  = sc<int32_t>(std::lround(c.px * 256)) * c.wheel->sign});
        stroke->period = std::clamp(sc<int>(std::lround(1000.0 / PACE.rateHz)), 4, 16);
        stroke->start  = Time::steadyNow();
        clearStop();
        stroke->timer = makeShared<CEventLoopTimer>(std::chrono::milliseconds(stroke->period), [](SP<CEventLoopTimer>, void*) { tick(); }, nullptr);
        g_pEventLoopManager->addTimer(stroke->timer);
        return {};
    }

    std::optional<std::string> status() {
        if (!stroke)
            return std::nullopt;
        return "scrolling " + done();
    }

    void reset() {
        end(false);
    }
}
