// A hand's stroke on a touchpad, over time, as one sends it: a finger's
// scroll (wl_pointer axis events from the finger source, no discrete steps,
// then axis_stop as it lifts) or a two-finger pinch (the pointer-gestures
// protocol: begin, updates, end), a frame a tick at the profile's rate. The
// way is straight, its speed the profile's bell over Fitts's time for the
// fingers' travel (path.cpp); each tick sends what the time since the start
// says is done, so a scroll's deltas and a pinch's turns sum exactly to the
// request (wl_fixed, 1/256) and a pinch ends exactly on its scale, whatever
// the timer's jitter. The fingers then rest (LIFT_MS) before they lift: a
// toolkit flings a scroll at the speed of its last 150 ms (GTK's), so none
// follows. Each tick checks afresh, as a glide's: the window may close or
// hide, and the user's pointer may come into the app, which takes the app's
// pointer: the stroke stops there, a pinch cancelled.
#include "vclick.hpp"

#include <hyprland/protocols/pointer-gestures-unstable-v1.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/protocols/core/Seat.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

namespace VClick::Stroke {
    constexpr std::string_view PINCH       = "zwp_pointer_gesture_pinch_v1";
    constexpr size_t           MAX_PINCHES = 8;   // an app binds one per wl_pointer
    constexpr double           PINCH_PX    = 150; // the fingers' travel for a doubling or a quarter turn

    struct SStroke {
        SAction                action;
        PHLWINDOWREF           window;
        WP<CWLSurfaceResource> surf;
        wl_client*             client = nullptr;
        Vector2D               local;
        Path::SSeg             way;          // x: the share done
        double                 done  = 0;    // of the way
        int32_t                total = 0;    // a scroll's px or a pinch's turn, in wl_fixed units (1/256), signed
        int32_t                sent  = 0;    // of those
        std::vector<uint32_t>  pinches;      // the app's pinch gesture objects, by id
        bool                   left = false; // the seat left the app's pointer: enter again
        Time::steady_tp        start;
        int                    period = 8;
        Pointer::CPointers     pointers;
        SP<CEventLoopTimer>    timer;
    };
    static std::optional<SStroke> stroke;

    static bool pinching() {
        return stroke->action.verb == EVerb::PINCH;
    }

    static std::string progress() {
        return std::format("{}/{:g} px", std::lround(std::abs(stroke->sent) / 256.0), stroke->action.px);
    }

    // the app's pinch gesture objects, by id: the protocol's own list is
    // private, and one may go any time, so each send looks it up again
    static std::vector<uint32_t> pinchesOf(wl_client* client) {
        std::vector<uint32_t> out;
        wl_client_for_each_resource(
            client,
            [](wl_resource* r, void* data) -> wl_iterator_result {
                auto& ids = *sc<std::vector<uint32_t>*>(data);
                if (std::string_view{wl_resource_get_class(r)} == PINCH && wl_resource_get_user_data(r) && ids.size() < MAX_PINCHES)
                    ids.emplace_back(wl_resource_get_id(r));
                return WL_ITERATOR_CONTINUE;
            },
            &out);
        return out;
    }

    static void eachPinch(const auto& fn) {
        for (const auto ID : stroke->pinches) {
            const auto R = wl_client_get_object(stroke->client, ID);
            if (R && std::string_view{wl_resource_get_class(R)} == PINCH && wl_resource_get_user_data(R))
                fn(*sc<CZwpPointerGesturePinchV1*>(wl_resource_get_user_data(R)));
        }
    }

    static uint32_t serial() {
        const auto SEAT = g_pSeatManager->seatResourceForClient(stroke->client);
        return SEAT ? g_pSeatManager->nextSerial(SEAT) : 0;
    }

    // the seat left every app's pointer as the user's changed surface: the
    // app's goes in again, alone in its frame or with what follows
    static void enterAgain(const SP<CWLSurfaceResource>& surf, bool frame) {
        if (!std::exchange(stroke->left, false))
            return;
        for (const auto& p : stroke->pointers.of(stroke->client)) {
            p->sendEnter(surf, stroke->local);
            if (frame)
                p->sendFrame();
        }
    }

    // the fingers lift: a scroll's stop or a pinch's end (cancelled: it
    // stopped early), then the app keeps the hover (keep) or its pointer
    // leaves; nothing into a pointer that is the user's now
    static void end(bool keep, bool cancelled) {
        if (!stroke)
            return;
        if (stroke->timer && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(stroke->timer);
        const auto SURF = stroke->surf.lock();
        if (SURF && !blocked()) {
            const bool MINE = !Pointer::userIn(stroke->client);
            const auto T    = Pointer::nowMs();
            if (MINE)
                enterAgain(SURF, pinching());
            if (pinching()) {
                const auto SERIAL = serial();
                eachPinch([&](CZwpPointerGesturePinchV1& g) { g.sendEnd(SERIAL, T, cancelled); });
            } else if (MINE) {
                for (const auto& p : stroke->pointers.of(stroke->client)) {
                    p->sendAxisSource(WL_POINTER_AXIS_SOURCE_FINGER);
                    p->sendAxisStop(T, stroke->action.wheel->axis);
                    p->sendFrame();
                }
            }
            if (MINE) {
                Pointer::hover(SURF);
                if (!keep)
                    Pointer::unhover();
            }
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
            noteStop(pinching() ? "the last pinch stopped: " + why : std::format("the last scroll stopped after {}: {}", progress(), why));
            end(false, true);
            return;
        }

        const double EL = std::chrono::duration<double, std::milli>(Time::steadyNow() - stroke->start).count();
        if (EL >= stroke->way.ms + LIFT_MS) {
            end(stroke->action.stay, false);
            return;
        }
        const double  U   = EL >= stroke->way.ms ? 1.0 : Path::along(stroke->way, EL).x;
        const int32_t NOW = U >= 1 ? stroke->total : sc<int32_t>(std::lround(stroke->total * U));
        const auto    T   = Pointer::nowMs();
        if (pinching() && U != stroke->done) {
            enterAgain(SURF, true);
            const auto SCALE = wl_fixed_from_double(1 + (stroke->action.scale - 1) * U); // the fingers' spread over the start's
            eachPinch([&](CZwpPointerGesturePinchV1& g) { g.sendUpdate(T, 0, 0, SCALE, NOW - stroke->sent); });
        } else if (!pinching() && NOW != stroke->sent) {
            enterAgain(SURF, false);
            const auto AXIS = stroke->action.wheel->axis;
            for (const auto& p : stroke->pointers.of(stroke->client)) {
                p->sendAxisSource(WL_POINTER_AXIS_SOURCE_FINGER);
                p->sendAxisRelativeDirection(AXIS, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
                p->sendAxis(T, AXIS, (NOW - stroke->sent) / 256.0);
                p->sendFrame();
            }
        }
        stroke->sent = NOW;
        stroke->done = U;
        stroke->timer->updateTimeout(std::chrono::milliseconds(stroke->period));
    }

    static void begin(const SAction& c, const Pointer::STarget& t, double travel, int32_t total) {
        const auto& PACE = Glide::profile();
        stroke.emplace(SStroke{.action = c, .window = t.window, .surf = t.surf, .client = t.surf->client(), .local = t.local, .way = Path::stroke(travel, PACE), .total = total});
        stroke->period = std::clamp(sc<int>(std::lround(1000.0 / PACE.rateHz)), 4, 16);
        stroke->start  = Time::steadyNow();
        clearStop();
        stroke->timer = makeShared<CEventLoopTimer>(std::chrono::milliseconds(stroke->period), [](SP<CEventLoopTimer>, void*) { tick(); }, nullptr);
        g_pEventLoopManager->addTimer(stroke->timer);
    }

    Result scroll(const SAction& c) {
        const auto T = Pointer::place(c);
        if (!T)
            return fail(T.error());
        begin(c, *T, c.px, sc<int32_t>(std::lround(c.px * 256)) * c.wheel->sign);
        return {};
    }

    Result pinch(const SAction& c) {
        const auto T = Pointer::place(c);
        if (!T)
            return fail(T.error());
        auto ids = pinchesOf(T->surf->client());
        if (ids.empty()) {
            if (!c.stay)
                Pointer::unhover();
            return fail("error: the app takes no pinch (it bound no zwp_pointer_gesture_pinch_v1)");
        }
        begin(c, *T, PINCH_PX * (std::abs(std::log2(c.scale)) + std::abs(c.deg) / 90), sc<int32_t>(std::lround(c.deg * 256)));
        stroke->pinches   = std::move(ids);
        const auto SERIAL = serial();
        const auto TIME   = Pointer::nowMs();
        eachPinch([&](CZwpPointerGesturePinchV1& g) { g.sendBegin(SERIAL, TIME, T->surf->getResource()->resource(), 2); });
        return {};
    }

    std::optional<std::string> status() {
        if (!stroke)
            return std::nullopt;
        return pinching() ? "pinching" : "scrolling " + progress();
    }

    void seatMoved() {
        if (stroke)
            stroke->left = true;
    }

    void reset() {
        end(false, true);
    }
}
