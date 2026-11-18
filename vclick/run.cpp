// The one timer: a request's steps (timeline.cpp), run in order. The steps
// that take no time run at once, so a request that needs none answers with
// its own error. On the way there and on a held button's way each tick is
// checked afresh: the window may close or hide; the user's pointer may come
// into the app, and then the app's pointer is the user's: the way stops
// there, and a click or scroll goes on at once. A surface boundary on the way
// (a subsurface, a popup) is crossed with a leave and an enter, as a real
// pointer crosses it. Wherever a way ends, early or not, is where the next
// one in that window starts, and in the human mode where the cursor rests.
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
#include <cstdint>
#include <format>
#include <random>

namespace VClick::Run {
    using Timeline::EStep;

    static Profile::SProfile pace;

    const Profile::SProfile& profile() {
        return pace;
    }

    std::string loadProfile(const std::string& path) {
        const auto P = Profile::read(path.empty() ? Profile::defaultPath() : path);
        if (!P) {
            pace = {};
            return P.error() + ": the built-in defaults";
        }
        pace = *P;
        return "loaded " + pace.source;
    }

    static std::mt19937& rng() {
        static std::mt19937 r{std::random_device{}()};
        return r;
    }

    struct SRun {
        uint64_t                     id = 0;
        SAction                      action;
        PHLWINDOWREF                 window;
        std::vector<Timeline::SStep> steps;
        size_t                       step = 0;
        Time::steady_tp              since;             // where the step's time counts from
        bool                         arrived  = false;  // the way's last point went
        bool                         open     = false;  // on a way: the cursor is pinned to it
        bool                         held     = false;  // a button is down the whole way
        bool                         replying = false;  // inside the request: an error is its answer
        bool                         walked   = false;  // it came by a way: an error says so
        WP<CWLSurfaceResource>       cur;               // the surface the app's pointer is in
        Vector2D                     at;                // where it is, in window coords
        Pointer::CPointers           pointers;
        int                          period = 8;
        int                          done = 0, total = 0, waited = 0; // a paced wheel run
        SP<CEventLoopTimer>          timer;
    };
    static std::optional<SRun> run;
    static uint64_t            lastId = 0;

    bool active() {
        return run.has_value();
    }

    static const Timeline::SStep& current() {
        return run->steps[run->step];
    }

    bool moving() {
        return run && run->open;
    }

    std::optional<std::string> status() {
        if (!run || run->step >= run->steps.size())
            return std::nullopt;
        switch (current().kind) {
            case EStep::WHEEL:
            case EStep::PACE: return std::format("scrolling {}/{}", run->done, run->total);
            case EStep::STROKE: return Stroke::status();
            case EStep::HOLD:
                if (run->action.verb == EVerb::CLICK)
                    return "clicking";
                return "dragging";
            case EStep::REST:
                if (run->action.verb == EVerb::CLICK && run->done > 0)
                    return "clicking";
                [[fallthrough]];
            default: return Pointer::heldButton() ? "dragging" : "moving";
        }
    }

    static void arm(int ms);

    // the next tick, no later than ms from the step's start
    static void armWithin(double ms) {
        const double EL = std::chrono::duration<double, std::milli>(Time::steadyNow() - run->since).count();
        arm(std::clamp(sc<int>(std::ceil(ms - EL)), 1, run->period));
    }

    static void arm(int ms) {
        if (run->timer) {
            run->timer->updateTimeout(std::chrono::milliseconds(ms));
            return;
        }
        run->timer = makeShared<CEventLoopTimer>(
            std::chrono::milliseconds(ms),
            [ID = run->id](SP<CEventLoopTimer>, void*) {
                if (run && run->id == ID)
                    tick();
            },
            nullptr);
        g_pEventLoopManager->addTimer(run->timer);
    }

    static Result moveTo(const PHLWINDOW& w, const Vector2D& pos) {
        Vector2D   local;
        const auto AT = Path::clampTo(pos, Pointer::windowSize(w));
        // a held button is the app's implicit grab: no surface is crossed
        const auto SURF = Pointer::heldButton() ? Pointer::heldAt(AT, local) : Pointer::surfaceAt(w, AT, local);
        if (!SURF)
            return fail("error: the window has no surface");
        const auto PTRS = run->pointers.of(SURF->client());
        if (PTRS.empty())
            return fail("error: the app has no pointer (it bound no wl_pointer)");

        const auto CUR = run->cur.lock();
        const auto T   = Pointer::nowMs();
        for (const auto& p : PTRS) {
            if (CUR && CUR != SURF) {
                p->sendLeave();
                p->sendFrame();
            }
            // after the seat's leave (the user's pointer changed surface) the
            // enter goes again; otherwise it is a no-op
            p->sendEnter(SURF, local);
            p->sendMotion(T, local);
            p->sendFrame();
        }
        run->cur = SURF;
        run->at  = AT;
        Cursor::moveTo(pos);
        return {};
    }

    // the way is over: where it ended is remembered, and the cursor rests there
    static void closeWay() {
        if (!run->open)
            return;
        run->open = false;
        if (const auto W = run->window.lock())
            Pointer::rememberPos(W, run->at);
        Cursor::rest(run->action.stay || Pointer::heldButton()); // a held button keeps the hand on the mouse
    }

    static void end(bool leave) {
        if (!run)
            return;
        if (run->timer && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(run->timer);
        if (run->open) {
            const auto CUR = run->cur.lock();
            if (leave && CUR && !Pointer::userIn(CUR->client()) && !Pointer::heldButton()) {
                for (const auto& p : run->pointers.last()) {
                    if (const auto P = p.lock(); P && P->good()) {
                        P->sendLeave();
                        P->sendFrame();
                    }
                }
            }
            closeWay();
        }
        run.reset();
    }

    void stop() {
        end(true);
    }

    void abandon() {
        end(false);
    }

    // a step failed: inside the request the error is the answer, later it is noted
    static Result failed(const Result& r, std::string_view note) {
        const bool REPLY = run->replying;
        end(false);
        if (REPLY)
            return r;
        noteStop(std::format("{}{}", note, r.error()));
        return {};
    }

    // the clicks not yet pressed, as one burst
    static SAction remaining() {
        SAction left = run->action;
        left.count   = std::max(1, run->total - run->done);
        return left;
    }

    static Result inputFailed(const Result& r) {
        return failed(r, run->walked ? "the last glide's input failed: " : "");
    }

    // the steps from here that take no time, up to one that does
    static Result advance() {
        while (run) {
            if (run->step >= run->steps.size()) {
                end(false);
                return {};
            }
            const auto& S = current();
            if (S.kind != EStep::WAY && S.kind != EStep::REST)
                closeWay();

            switch (S.kind) {
                case EStep::WAY:
                case EStep::REST: arm(run->period); return {};
                case EStep::HOLD: armWithin(S.ms); return {};
                case EStep::TRAVEL: {
                    const auto W = Pointer::heldWindow();
                    if (!W)
                        return inputFailed(fail("error: no button is held"));
                    Vector2D local;
                    run->window  = W;
                    run->held    = true;
                    run->open    = true;
                    run->arrived = false;
                    run->walked  = true;
                    run->at      = run->action.pos;
                    run->cur     = Pointer::heldAt(run->action.pos, local);
                    run->since   = Time::steadyNow();
                    Cursor::prepare(W);
                    Cursor::show(W, run->action.pos);
                    arm(run->period);
                    return {};
                }
                case EStep::PACE:
                    if (run->done == 1)
                        clearStop();
                    arm(sc<int>(S.ms));
                    return {};
                case EStep::STROKE: {
                    const auto R = run->action.verb == EVerb::PINCH ? Stroke::pinch(run->action) : Stroke::scroll(run->action);
                    if (!R)
                        return inputFailed(R);
                    arm(run->period);
                    return {};
                }
                case EStep::WHEEL: {
                    SAction one = run->action;
                    one.count   = 1;
                    const auto R = Pointer::burst(one);
                    if (R) {
                        ++run->done;
                        run->waited = 0;
                    } else if (run->done == 0) {
                        return inputFailed(R);
                    } else if (busy(R) && run->waited + run->action.ms <= BUSY_MS) {
                        run->waited += run->action.ms; // the app is busy: this step again, at the same pace
                        arm(run->action.ms);
                        return {};
                    } else
                        return failed(R, std::format("the last scroll stopped after {}/{}: ", run->done, run->total));
                    break;
                }
                case EStep::BURST: {
                    if (const auto R = Pointer::burst(run->action); !R)
                        return inputFailed(R);
                    break;
                }
                case EStep::PLACE: {
                    if (Pointer::heldButton()) {
                        if (const auto R = Pointer::heldMotion(run->action.pos); !R)
                            return inputFailed(R);
                    } else if (const auto T = Pointer::place(run->action); !T)
                        return inputFailed(fail(T.error()));
                    break;
                }
                case EStep::HOVER: Pointer::hover(run->cur.lock()); break;
                case EStep::PRESS: {
                    const bool CLICK = run->action.verb == EVerb::CLICK;
                    if (const auto R = Pointer::press(run->action, CLICK); !R) {
                        if (!CLICK)
                            return inputFailed(R);
                        // it can't hold after all (the user's pointer just came in): at once
                        const auto LEFT = remaining();
                        end(false);
                        if (const auto B = Pointer::burst(LEFT); !B)
                            noteStop("the last glide's input failed: " + B.error());
                        return {};
                    }
                    if (CLICK)
                        ++run->done;
                    run->window = Pointer::heldWindow(); // a run that came at once has none yet: its hold checks this one
                    run->since  = Time::steadyNow();
                    break;
                }
                case EStep::RELEASE: {
                    if (const auto R = Pointer::release(); !R)
                        return failed(R, "the last drag's release failed: ");
                    if (!run->action.stay && run->step + 1 == run->steps.size()) {
                        Pointer::unhover();
                        Cursor::rest(false);
                    }
                    run->since = Time::steadyNow();
                    break;
                }
            }
            ++run->step;
        }
        return {};
    }

    static Timeline::SRequest requestOf(const SAction& c) {
        const auto W = Pointer::windowOf(c.selector);
        return {.verb   = c.verb,
                .style  = *c.style,
                .pos    = c.pos,
                .to     = c.to,
                .bounds = W ? Pointer::windowSize(W) : Vector2D{},
                .size   = c.size,
                .count  = c.count,
                .ms     = c.ms,
                .px     = c.px,
                .hold   = Pointer::dragGuard};
    }

    // the request's steps, from the first: what takes no time happens here
    static Result begin(const SAction& c, const Timeline::SRequest& q, const PHLWINDOW& w) {
        run.emplace(SRun{.id = ++lastId, .action = c, .window = w, .steps = Timeline::compose(q, pace, rng())});
        run->period   = std::clamp(sc<int>(std::lround(1000.0 / pace.rateHz)), 4, 16);
        run->total    = c.count;
        run->since    = Time::steadyNow();
        run->replying = true;
        run->held     = Pointer::heldButton().has_value();
        if (q.from) {
            run->open   = true;
            run->walked = true;
            run->at     = *q.from;
            Cursor::prepare(w);
            Cursor::show(w, *q.from);
            if (const auto R = moveTo(w, *q.from); !R) {
                run.reset();
                Cursor::hide();
                return R;
            }
            clearStop();
            if (!Pointer::heldButton())
                Pointer::hover(nullptr); // the glide has the app's pointer now
        }
        const auto R = advance();
        if (run)
            run->replying = false;
        return R;
    }

    Result request(const SAction& asked) {
        SAction c = asked;
        if (*c.style == EStyle::HUMAN && !c.paced)
            c.ms = sc<int>(std::lround(pace.wheelMs));
        auto q = requestOf(c);
        if (*c.style == EStyle::PERFORMANCE)
            return begin(c, q, nullptr);
        auto T = Pointer::resolve(c.selector, c.pos);
        if (!T)
            return fail(T.error());
        if (T->shared && c.verb != EVerb::CLICK && c.verb != EVerb::SCROLL)
            return fail(std::format("error: busy: the user's pointer is in this app; a {} would take it from them", verbName(c.verb)));
        if (T->shared || !T->inbox) // goes at once: it can't take the app's pointer, or a popup is out of the window's box
            return begin(c, q, nullptr);
        const auto SIZE = Pointer::windowSize(T->window);
        const auto LAST = Pointer::lastPos(T->window);
        q.from          = Path::clampTo(LAST ? *LAST : SIZE / 2, SIZE);
        return begin(c, q, T->window);
    }

    // the checks of a way and its rest; false: the run ended here
    static bool walkOn(const PHLWINDOW& w) {
        if (run->held && (Pointer::holdLost() || !Pointer::heldButton())) { // noted: the window went, the seat let go
            end(false);
            return false;
        }
        std::string why;
        if (const auto WHY = blocked())
            why = WHY->substr(7); // past "error: "
        else if (!w || !Desktop::View::validMapped(w) || w->isHidden())
            why = "the window closed or was hidden";
        if (!why.empty()) {
            noteStop("the last glide stopped: " + why);
            end(true);
            return false;
        }
        const auto CLIENT = Pointer::clientOf(w);
        if (!CLIENT || !Pointer::userIn(CLIENT))
            return true;

        // the user's pointer came into the app: the seat has already taken
        // its pointer (a leave, then an enter for the user's surface), so no
        // leave; a click or scroll goes on at once, which steps out of the
        // user's surface and back in; a move or a press would take that
        // pointer, so it stops
        const auto A = run->action.verb == EVerb::CLICK ? remaining() : run->action;
        end(false);
        Cursor::userMoved(); // their pointer has the app's now: the cursor goes
        if (A.verb != EVerb::CLICK && A.verb != EVerb::SCROLL) {
            noteStop(std::format("the last {} stopped: the user's pointer came into the app", verbName(A.verb)));
            return false;
        }
        const auto R = begin(A, requestOf(A), nullptr);
        if (R) {
            Pointer::rememberPos(w, A.pos);
            if (A.stay)
                Cursor::place(w, A.pos); // taken: the user's pointer is in the app
        }
        noteStop(R ? "the last glide finished at once: the user's pointer came into the app" : "the last glide stopped: " + R.error());
        return false;
    }

    void tick() {
        if (!run)
            return;
        const auto& S = current();
        switch (S.kind) {
            case EStep::WAY:
            case EStep::TRAVEL:
            case EStep::REST: {
                const auto W = run->window.lock();
                if (!walkOn(W))
                    return;
                const double EL = std::chrono::duration<double, std::milli>(Time::steadyNow() - run->since).count();
                Result       r;
                if (S.kind == EStep::REST || (EL >= S.ms && run->arrived)) {
                    if (EL < S.ms)
                        break;
                    run->since += std::chrono::duration_cast<Time::steady_dur>(std::chrono::duration<double, std::milli>(S.ms));
                    run->arrived = false;
                    ++run->step;
                    std::ignore = advance();
                    return;
                }
                if (EL < S.ms)
                    r = moveTo(W, Path::sample(S.way, EL));
                else {
                    r            = moveTo(W, S.to); // the last point exactly
                    run->arrived = true;
                }
                if (!r) {
                    noteStop("the last glide stopped: " + r.error());
                    end(true);
                    return;
                }
                break;
            }
            case EStep::HOLD: {
                const auto W    = run->window.lock();
                const auto WHY  = Pointer::holdLost(); // noted, unless the seat only ended a click sooner
                const bool GONE = !W || !Desktop::View::validMapped(W) || W->isHidden() || blocked();
                if (WHY || !Pointer::heldButton() || GONE) {
                    if (run->action.verb != EVerb::CLICK || GONE) {
                        end(false);
                        return;
                    }
                    // the press and the seat's release were a whole click: the rest go at once
                    const bool MORE = run->done < run->total;
                    const auto LEFT = remaining();
                    end(false);
                    if (MORE)
                        if (const auto R = Pointer::burst(LEFT); !R)
                            noteStop("the last glide's input failed: " + R.error());
                    return;
                }
                const double EL = std::chrono::duration<double, std::milli>(Time::steadyNow() - run->since).count();
                if (EL >= S.ms) {
                    ++run->step;
                    std::ignore = advance();
                    return;
                }
                armWithin(S.ms);
                return;
            }
            case EStep::STROKE:
                if (!Stroke::tick()) {
                    ++run->step;
                    std::ignore = advance();
                    return;
                }
                break;
            case EStep::PACE:
                ++run->step;
                std::ignore = advance();
                return;
            default: // a busy wheel step, again
                std::ignore = advance();
                return;
        }
        arm(run->period);
    }
}
