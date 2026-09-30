// awesome/core/jobs.cpp — see jobs.hpp.
#include "jobs.hpp"

#include <hyprland/src/Compositor.hpp>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace NAwesome {

    // one live tracked child at a time is one fork (see jobs.hpp).
    inline constexpr size_t MAX_LIVE_CHILDREN = 16;

    Jobs& Jobs::inst() {
        static Jobs J;
        return J;
    }

    // The compositor process runs with SIGTERM blocked (graceful-shutdown
    // handling), and children inherit the blocked mask: an unkillable helper
    // (kill(1) reports success) that outlives its purpose and pins a pool
    // slot for its full timeout. posix_spawnp with POSIX_SPAWN_SETSIGMASK
    // does not lift it on this glibc (verified: the child stays blocked).
    // The fork's own Executor does fork + mask-clear + exec; match it so
    // plugin helpers keep default signal semantics.
    static pid_t spawnChild(const std::vector<const char*>& argv, int outDup) {
        const pid_t pid = fork();
        if (pid < 0)
            return -1;
        if (pid == 0) {
            sigset_t mask;
            sigemptyset(&mask);
            sigprocmask(SIG_SETMASK, &mask, nullptr);
            if (outDup >= 0 && outDup != 1) {
                dup2(outDup, 1);
                if (outDup > 2)
                    close(outDup);
            }
            execvp(argv[0], const_cast<char* const*>(argv.data()));
            _exit(127);
        }
        return pid;
    }

    int Jobs::onChildExit(int, uint32_t, void* data) {
        auto* c = (SChild*)data;
        waitpid(c->pid, nullptr, WNOHANG);
        if (c->onExit)
            c->onExit(0); // the pid is reaped; delivery is the signal
        wl_event_source_remove((wl_event_source*)c->src);
        close(c->fd);
        auto& J = inst();
        std::erase_if(J.m_children, [&](const auto& U) { return U.get() == c; });
        return 0;
    }

    int Jobs::onChildExitChecked(int, uint32_t, void* data) {
        auto* c = (SChild*)data;
        int   status = 0;
        waitpid(c->pid, &status, WNOHANG);
        if (c->onExit)
            c->onExit(status);
        wl_event_source_remove((wl_event_source*)c->src);
        close(c->fd);
        auto& J = inst();
        std::erase_if(J.m_children, [&](const auto& U) { return U.get() == c; });
        return 0;
    }

    bool Jobs::spawn(std::vector<const char*> argv) {
        return spawnChecked(std::move(argv), nullptr);
    }

    // the retained-output cap is also the work cap (see onPipedOut)
    inline constexpr size_t MAX_PIPED_OUTPUT = 4096;

    bool Jobs::spawnChecked(std::vector<const char*> argv, std::function<void(int)> onExit) {
        if (argv.empty() || !argv[0])
            return false;
        std::erase_if(m_orphans, [](pid_t p) { return waitpid(p, nullptr, WNOHANG) != 0; });
        if (m_children.size() >= MAX_LIVE_CHILDREN)
            return false;
        if (argv.back())
            argv.push_back(nullptr); // execv needs the null terminator

        const pid_t pid = spawnChild(argv, -1);
        if (pid < 0)
            return false;

        const int FD = (int)syscall(SYS_pidfd_open, pid, 0);
        if (FD < 0 || !g_pCompositor) {
            // no pidfd/loop to watch it: reap now, or hand a not-yet-exited
            // child to the sweep above rather than leak a zombie
            if (waitpid(pid, nullptr, WNOHANG) == 0)
                m_orphans.push_back(pid);
            return false;
        }
        auto c = makeUnique<SChild>();
        c->pid    = pid;
        c->fd     = FD;
        c->onExit = std::move(onExit);
        c->src    = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, FD, WL_EVENT_READABLE,
                                          c->onExit ? &Jobs::onChildExitChecked : &Jobs::onChildExit, c.get());
        m_children.push_back(std::move(c));
        return true;
    }

    bool Jobs::spawnPiped(std::vector<const char*> argv, std::function<void(SPipedResult)> onDone) {
        if (argv.empty() || !argv[0] || !onDone)
            return false;
        std::erase_if(m_orphans, [](pid_t p) { return waitpid(p, nullptr, WNOHANG) != 0; });
        if (m_children.size() >= MAX_LIVE_CHILDREN)
            return false;

        int pfd[2];
        if (pipe2(pfd, O_CLOEXEC | O_NONBLOCK) != 0)
            return false;

        if (argv.back())
            argv.push_back(nullptr);
        const pid_t pid = spawnChild(argv, pfd[1]);
        close(pfd[1]); // the write end lives in the child only
        if (pid < 0) {
            close(pfd[0]);
            return false;
        }

        auto c = makeUnique<SChild>();
        c->pid     = pid;
        c->outFd   = pfd[0];
        c->onPiped = std::move(onDone);
        c->outSrc  = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, c->outFd, WL_EVENT_READABLE, &Jobs::onPipedOut, c.get());
        if (!c->outSrc) {
            close(c->outFd);
            if (waitpid(pid, nullptr, WNOHANG) == 0)
                m_orphans.push_back(pid);
            return false;
        }
        m_children.push_back(std::move(c));
        return true;
    }

    int Jobs::onPipedOut(int fd, uint32_t, void* data) {
        auto* c = (SChild*)data;
        char  buf[256];
        bool  done = false;
        for (;;) {
            const auto N = read(fd, buf, sizeof(buf));
            if (N > 0) {
                if (c->out.size() >= MAX_PIPED_OUTPUT || (size_t)N > MAX_PIPED_OUTPUT - c->out.size()) {
                    // the cap: stop reading NOW and close the read end — the
                    // producer takes SIGPIPE instead of holding this callback
                    c->truncated = true;
                    done = true;
                    break;
                }
                c->out.append(buf, (size_t)N);
                continue;
            }
            if (N < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return 0; // more later
            done = true; // EOF or error: the child is done talking
            break;
        }
        if (done)
            finishPiped(c);
        return 0;
    }

    void Jobs::finishPiped(SChild* c) {
        SPipedResult R;
        R.out       = std::move(c->out);
        R.truncated = c->truncated;
        if (c->pid > 0) {
            int status = -1;
            if (waitpid(c->pid, &status, WNOHANG) == c->pid)
                R.status = status;
            else if (errno == ECHILD)
                R.status = 0; // SA_NOCLDWAIT: the exit status is gone, the delivery is the signal
            else
                inst().m_orphans.push_back(c->pid); // still alive (the cap path): the next spawn sweeps it
        }
        if (c->outSrc)
            wl_event_source_remove((wl_event_source*)c->outSrc);
        if (c->outFd >= 0)
            close(c->outFd);
        if (c->fd >= 0)
            close(c->fd);
        auto& J = inst();
        std::erase_if(J.m_children, [&](const auto& U) { return U.get() == c; });
        if (c->onPiped)
            c->onPiped(std::move(R)); // after the release: the callback may spawn again
    }

    void Jobs::teardown() {
        for (auto& c : m_children) {
            if (c->src)
                wl_event_source_remove((wl_event_source*)c->src);
            if (c->outSrc)
                wl_event_source_remove((wl_event_source*)c->outSrc);
            if (c->fd >= 0)
                close(c->fd);
            if (c->outFd >= 0)
                close(c->outFd);
            if (c->pid > 0)
                waitpid(c->pid, nullptr, WNOHANG);
        }
        m_children.clear();
        for (pid_t p : m_orphans)
            waitpid(p, nullptr, WNOHANG);
        m_orphans.clear();
    }

} // namespace NAwesome
