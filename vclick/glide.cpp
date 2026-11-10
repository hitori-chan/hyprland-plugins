// The way there. A glide sends the app's pointer one motion per tick (the
// profile's rate) along the planned path (path.cpp), each tick checked
// afresh: the window may close or hide; the user's pointer may come into the
// app, and then the app's pointer is the user's: the glide stops there, and
// a click or scroll goes on as a jump. A surface boundary on the way (a
// subsurface, a popup) is crossed with a leave and an enter, as a real
// pointer crosses it. At the end: the dwell, then the input itself from the
// pointer already there (Pointer::dispatch), or, for a move, the hover held.
#include "vclick.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/protocols/core/Seat.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

namespace VClick::Glide {
    static Path::SProfile pace;

    const Path::SProfile& profile() {
        return pace;
    }

    std::string loadProfile(const std::string& path) {
        const auto P = Path::readProfile(path.empty() ? Path::defaultProfilePath() : path);
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

    struct SGlide {
        SAction                             action;
        PHLWINDOWREF                        window;
        std::vector<Path::SSeg>             segs;
        double                              moveMs = 0, dwellMs = 0;
        Time::steady_tp                     start;
        bool                                arrived = false;
        WP<CWLSurfaceResource>              cur; // the surface the app's pointer is in
        std::vector<WP<CWLPointerResource>> pointers;
        wl_client*                          pointersClient = nullptr; // whose pointers those are
        int                                 period         = 8;
        SP<CEventLoopTimer>                 timer;
    };
    static std::optional<SGlide> glide;

    bool active() {
        return glide.has_value();
    }

    // the app's pointers, looked up again only when the app changes or one
    // went away: an app may hold thousands of resources, a tick runs 125
    // times a second
    static std::vector<SP<CWLPointerResource>> pointers(wl_client* client) {
        std::vector<SP<CWLPointerResource>> out;
        bool                                fresh = glide->pointersClient == client;
        for (const auto& p : glide->pointers) {
            const auto P = p.lock();
            if (!P || !P->good()) {
                fresh = false;
                break;
            }
            out.emplace_back(P);
        }
        if (!fresh || out.empty()) {
            out = Pointer::pointersOf(client);
            glide->pointers.assign(out.begin(), out.end());
            glide->pointersClient = client;
        }
        return out;
    }

    static Result moveTo(const PHLWINDOW& w, const Vector2D& pos) {
        Vector2D   local;
        const auto SURF = Pointer::surfaceAt(w, Path::clampTo(pos, Pointer::windowSize(w)), local);
        if (!SURF)
            return fail("error: the window has no surface");
        const auto PTRS = pointers(SURF->client());
        if (PTRS.empty())
            return fail("error: the app has no pointer (it bound no wl_pointer)");

        const auto CUR = glide->cur.lock();
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
        glide->cur = SURF;
        Cursor::moveTo(pos);
        return {};
    }

    static void end(bool leave) {
        if (!glide)
            return;
        if (glide->timer && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(glide->timer);
        const auto CUR = glide->cur.lock();
        if (leave && CUR && !Pointer::userIn(CUR->client())) {
            for (const auto& p : glide->pointers) {
                if (const auto P = p.lock(); P && P->good()) {
                    P->sendLeave();
                    P->sendFrame();
                }
            }
        }
        glide.reset();
        Cursor::linger();
    }

    void stop() {
        end(true);
    }

    static void tick() {
        if (!glide)
            return;
        const auto  W = glide->window.lock();
        std::string why;
        if (g_pCompositor->m_isShuttingDown)
            why = "the compositor is shutting down";
        else if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked())
            why = "the session is locked";
        else if (!W || !Desktop::View::validMapped(W) || W->isHidden())
            why = "the window closed or was hidden";
        if (!why.empty()) {
            noteStop("the last glide stopped: " + why);
            end(true);
            return;
        }

        if (const auto CLIENT = Pointer::clientOf(W); CLIENT && Pointer::userIn(CLIENT)) {
            // the user's pointer came into the app: the seat has already
            // taken its pointer (a leave, then an enter for the user's
            // surface), so no leave; a click or scroll goes on as a jump,
            // which steps out of the user's surface and back in
            const auto A = glide->action;
            end(false);
            if (A.move) {
                noteStop("the last move stopped: the user's pointer came into the app");
                return;
            }
            const auto R = Pointer::dispatch(A);
            noteStop(R ? "the last glide finished as a jump: the user's pointer came into the app" : "the last glide stopped: " + R.error());
            return;
        }

        const double EL = std::chrono::duration<double, std::milli>(Time::steadyNow() - glide->start).count();
        Result       r;
        if (EL < glide->moveMs)
            r = moveTo(W, Path::sample(glide->segs, EL));
        else if (!glide->arrived) {
            r              = moveTo(W, glide->action.pos); // the last point exactly
            glide->arrived = true;
        } else if (EL >= glide->moveMs + glide->dwellMs) {
            const auto A    = glide->action;
            const auto SURF = glide->cur.lock();
            Pointer::rememberPos(W, A.pos);
            end(false);
            if (A.move) {
                Pointer::hold(SURF);
                return;
            }
            if (const auto R = Pointer::dispatch(A); !R)
                noteStop("the last glide's input failed: " + R.error());
            return;
        }
        if (!r) {
            noteStop("the last glide stopped: " + r.error());
            end(true);
            return;
        }
        glide->timer->updateTimeout(std::chrono::milliseconds(glide->period));
    }

    std::optional<Result> start(const SAction& c) {
        auto T = Pointer::resolve(c.selector, c.pos);
        if (!T)
            return fail(T.error());
        if (T->shared) {
            if (c.move)
                return fail("error: busy: the user's pointer is in this app; a move would take it from them");
            return std::nullopt;
        }
        if (!T->inbox)
            return std::nullopt;

        const auto W    = T->window;
        const auto SIZE = Pointer::windowSize(W);
        const auto LAST = Pointer::lastPos(W);
        const auto FROM = Path::clampTo(LAST ? *LAST : SIZE / 2, SIZE);
        std::uniform_real_distribution<double> jitter{0.7, 1.3};

        glide.emplace(SGlide{.action = c, .window = W, .segs = Path::plan(FROM, c.pos, SIZE, pace, rng())});
        glide->moveMs  = Path::duration(glide->segs);
        glide->dwellMs = c.move ? 0 : std::clamp(pace.dwellMs * jitter(rng()), 0.0, 400.0);
        glide->period  = std::clamp(sc<int>(std::lround(1000.0 / pace.rateHz)), 4, 16);
        glide->start   = Time::steadyNow();

        Cursor::prepare();
        Cursor::show(W, FROM);
        const auto R = moveTo(W, FROM);
        if (!R) {
            glide.reset();
            Cursor::hide();
            return R;
        }
        clearStop();
        Pointer::hold(nullptr); // the glide has the app's pointer now
        glide->timer = makeShared<CEventLoopTimer>(std::chrono::milliseconds(glide->period), [](SP<CEventLoopTimer>, void*) { tick(); }, nullptr);
        g_pEventLoopManager->addTimer(glide->timer);
        return Result{};
    }
}
