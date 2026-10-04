// awesome/core/jobs.cpp — see jobs.hpp.
#include "jobs.hpp"
#include "proc.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <wayland-server-core.h>

namespace NAwesome {

    namespace {
        using namespace std::chrono_literals;

        struct SBudget {
            size_t                    live;
            std::chrono::milliseconds deadline; // 0 = never killed
        };
        // CONTROL matches the system module's chain cap (MAX_ACTIVE_CHAINS):
        // a readback chain holds at most one control child at a time
        constexpr SBudget BUDGET[Jobs::CLASS_COUNT] = {
            {16, 5s},  // CONTROL
            {4, 30s},  // SOUND
            {32, 0ms}, // OPEN: bounded by user clicks; a browser may live for hours
        };

        // the retained-output cap is also the work cap (see onPipeReadable)
        constexpr size_t MAX_PIPED_OUTPUT = 4096;

        void killChild(int pidfd) {
            if (pidfd >= 0)
                pidfd_send_signal(pidfd, SIGKILL, nullptr, 0);
        }
    }

    Jobs& Jobs::inst() {
        static Jobs J;
        return J;
    }

    size_t Jobs::live(eClass cls) const {
        return std::ranges::count_if(m_children, [cls](const auto& c) { return c->cls == cls; });
    }

    bool Jobs::spawn(eClass cls, std::vector<const char*> argv) {
        return spawnTracked(cls, argv, makeUnique<SChild>());
    }

    bool Jobs::spawnChecked(eClass cls, std::vector<const char*> argv, std::function<void(int)> onExit) {
        auto c    = makeUnique<SChild>();
        c->onExit = std::move(onExit);
        return spawnTracked(cls, argv, std::move(c));
    }

    bool Jobs::spawnPiped(eClass cls, std::vector<const char*> argv, std::function<void(SPipedResult)> onDone) {
        if (!onDone)
            return false;
        int pfd[2];
        if (pipe2(pfd, O_CLOEXEC | O_NONBLOCK) != 0)
            return false;
        auto c     = makeUnique<SChild>();
        c->outFd   = pfd[0];
        c->onPiped = std::move(onDone);
        // spawnTracked dup2s the write end onto the child's stdout; it lives
        // in the child only
        const bool OK = spawnTracked(cls, argv, std::move(c), pfd[1]);
        close(pfd[1]);
        return OK;
    }

    bool Jobs::spawnTracked(eClass cls, std::vector<const char*>& argv, UP<SChild> c, int stdoutFd) {
        const auto FAIL = [&c]() {
            if (c && c->outFd >= 0)
                close(c->outFd);
            return false;
        };
        if (argv.empty() || !argv[0] || !g_pCompositor || !g_pEventLoopManager || cls >= CLASS_COUNT)
            return FAIL();
        if (live(cls) >= BUDGET[cls].live)
            return FAIL();
        if (argv.back())
            argv.push_back(nullptr); // execv needs the null terminator

        const auto CHILD = Proc::spawn(argv.data(), {.stdoutFd = stdoutFd, .ownGroup = cls == OPEN, .userFacing = cls == OPEN});
        if (CHILD.pidfd < 0)
            return FAIL();

        c->cls      = cls;
        c->pidfd    = CHILD.pidfd;
        c->deadline = BUDGET[cls].deadline.count() > 0 ? std::chrono::steady_clock::now() + BUDGET[cls].deadline : std::chrono::steady_clock::time_point::max();
        c->src      = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, c->pidfd, WL_EVENT_READABLE, &Jobs::onExitReadable, c.get());
        if (c->outFd >= 0)
            c->outSrc = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, c->outFd, WL_EVENT_READABLE, &Jobs::onPipeReadable, c.get());
        if (!c->src || (c->outFd >= 0 && !c->outSrc)) {
            // unwatchable: never leave a helper running unobserved (an
            // OPEN child is the user's app and keeps running)
            if (cls != OPEN)
                killChild(c->pidfd);
            auto* raw = c.get();
            m_children.push_back(std::move(c));
            release(raw);
            return false;
        }
        m_children.push_back(std::move(c));
        armDeadline();
        return true;
    }

    int Jobs::onExitReadable(int, uint32_t, void* data) {
        auto* c = (SChild*)data;
        // level-triggered: stop it firing again before anything else
        wl_event_source_remove((wl_event_source*)c->src);
        c->src    = nullptr;
        c->exited = true;
        inst().maybeFinish(c);
        return 0;
    }

    int Jobs::onPipeReadable(int fd, uint32_t, void* data) {
        auto* c = (SChild*)data;
        char  buf[512];
        for (;;) {
            const auto N = read(fd, buf, sizeof(buf));
            if (N > 0) {
                if ((size_t)N > MAX_PIPED_OUTPUT - std::min(MAX_PIPED_OUTPUT, c->out.size())) {
                    // the cap: stop reading NOW, close the read end, and end
                    // the producer (a closed pipe alone leaves it running)
                    c->truncated = true;
                    killChild(c->pidfd);
                    break;
                }
                c->out.append(buf, (size_t)N);
                continue;
            }
            if (N < 0 && errno == EINTR)
                continue;
            if (N < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return 0; // more later
            break;        // EOF or error: the child is done talking
        }
        wl_event_source_remove((wl_event_source*)c->outSrc);
        c->outSrc = nullptr;
        close(c->outFd);
        c->outFd    = -1;
        c->pipeDone = true;
        inst().maybeFinish(c);
        return 0;
    }

    // Deliver once the child exited AND its pipe (if any) is drained. The
    // callbacks and the output move out before release() frees the SChild,
    // and run after it: a callback may spawn again (m_children grows).
    void Jobs::maybeFinish(SChild* c) {
        if (!c->exited || (c->outFd >= 0 && !c->pipeDone))
            return;
        const int STATUS = Proc::exitStatus(c->pidfd).value_or(-1);
        Proc::reap(c->pidfd);
        auto         onExit  = std::move(c->onExit);
        auto         onPiped = std::move(c->onPiped);
        SPipedResult result{.out = std::move(c->out), .truncated = c->truncated, .status = STATUS};
        release(c);
        if (onPiped)
            onPiped(std::move(result));
        else if (onExit)
            onExit(STATUS);
    }

    void Jobs::release(SChild* c) {
        if (c->src)
            wl_event_source_remove((wl_event_source*)c->src);
        if (c->outSrc)
            wl_event_source_remove((wl_event_source*)c->outSrc);
        if (c->outFd >= 0)
            close(c->outFd);
        if (c->pidfd >= 0)
            close(c->pidfd);
        std::erase_if(m_children, [c](const auto& U) { return U.get() == c; });
    }

    void Jobs::armDeadline() {
        auto next = std::chrono::steady_clock::time_point::max();
        for (const auto& c : m_children)
            if (!c->exited)
                next = std::min(next, c->deadline);
        if (next == std::chrono::steady_clock::time_point::max()) {
            if (m_timer)
                m_timer->updateTimeout(std::nullopt);
            return;
        }
        if (!m_timer) {
            m_timer = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { Jobs::inst().onDeadline(); }, nullptr);
            g_pEventLoopManager->addTimer(m_timer);
        }
        m_timer->updateTimeout(std::max(std::chrono::steady_clock::duration::zero(), next - std::chrono::steady_clock::now()) + 1ms);
    }

    // Past the deadline: kill it. Its pidfd turns readable and the normal
    // exit path delivers (truncated, status of the kill).
    void Jobs::onDeadline() {
        const auto NOW = std::chrono::steady_clock::now();
        for (const auto& c : m_children)
            if (!c->exited && c->deadline <= NOW) {
                killChild(c->pidfd);
                c->truncated = true;
                c->deadline  = std::chrono::steady_clock::time_point::max(); // killed once
            }
        armDeadline();
    }

    void Jobs::teardown() {
        if (m_timer && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(m_timer);
        m_timer.reset();
        for (auto& c : m_children) {
            if (c->src)
                wl_event_source_remove((wl_event_source*)c->src);
            if (c->outSrc)
                wl_event_source_remove((wl_event_source*)c->outSrc);
            if (c->outFd >= 0)
                close(c->outFd);
            if (c->cls != OPEN && !c->exited)
                killChild(c->pidfd); // a helper must not outlive the plugin; the user's app may
            Proc::reap(c->pidfd);
            if (c->pidfd >= 0)
                close(c->pidfd);
        }
        m_children.clear();
    }

} // namespace NAwesome
