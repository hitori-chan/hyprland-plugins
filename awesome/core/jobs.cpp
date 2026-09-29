// awesome/core/jobs.cpp — see jobs.hpp.
#include "jobs.hpp"

#include <hyprland/src/Compositor.hpp>

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

    bool Jobs::spawnChecked(std::vector<const char*> argv, std::function<void(int)> onExit) {
        if (argv.empty() || !argv[0])
            return false;
        std::erase_if(m_orphans, [](pid_t p) { return waitpid(p, nullptr, WNOHANG) != 0; });
        if (m_children.size() >= MAX_LIVE_CHILDREN)
            return false;
        if (argv.back())
            argv.push_back(nullptr); // execv needs the null terminator

        pid_t pid = -1;
        if (posix_spawnp(&pid, argv[0], nullptr, nullptr, const_cast<char* const*>(argv.data()), environ) != 0)
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

    void Jobs::teardown() {
        for (auto& c : m_children) {
            if (c->src)
                wl_event_source_remove((wl_event_source*)c->src);
            if (c->fd >= 0)
                close(c->fd);
            if (c->pid > 0)
                waitpid(c->pid, nullptr, WNOHANG);
        }
        m_children.clear();
        for (pid_t p : m_orphans)
            waitpid(p, nullptr, WNOHANG);
        m_orphans.clear();
    }

} // namespace NAwesome
