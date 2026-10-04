// awesome/core/proc.hpp — the one way the plugin starts a process.
//
// The compositor's own process state must never leak into a helper:
// SIGTERM is blocked and SIGPIPE ignored there, and both survive exec (a
// cancelled helper then ignores its kill, and a reader that closes the
// pipe leaves the producer running to its own bound); the compositor's
// fds are not all CLOEXEC; the NOFILE soft limit is raised. A full fork()
// also copies a compositor-sized page table per spawn (volume keys repeat
// at ~25 Hz). spawn() uses (pidfd_)posix_spawnp: vfork semantics, an
// empty signal mask, default dispositions, stdin/stderr on /dev/null,
// every fd above 2 closed, and a race-free pidfd (SA_NOCLDWAIT reaps the
// child the moment it exits, so a pid opened afterwards may be gone or
// reused).
#pragma once

#include <csignal>
#include <fcntl.h>
#include <algorithm>
#include <optional>
#include <spawn.h>
#include <sys/ioctl.h>
// glibc 2.44's <sys/pidfd.h> has two header bugs: it tests _PIDFD_H but
// never defines it (a second inclusion redefines its structs — include it
// ONLY through here), and it lacks __BEGIN_DECLS (C++ would reference
// mangled pidfd_getpid/pidfd_send_signal: the .so links, dlopen fails)
extern "C" {
#include <sys/pidfd.h>
}
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace NAwesome::Proc {

    struct SOptions {
        int  stdoutFd   = -1;    // dup2'd onto stdout; -1 = /dev/null
        bool ownGroup   = false; // its own process group: kill(-pid) reaches its children
        bool userFacing = false; // an app the user runs (browser, launcher entry): the session's soft NOFILE
        bool pidfd      = true;  // hand back a pidfd (readable at exit); false = pid only
    };

    struct SChild {
        pid_t pid   = -1; // -1 with a valid pidfd: it already exited (and was reaped)
        int   pidfd = -1; // owned by the caller

        explicit operator bool() const {
            return pid > 0 || pidfd >= 0;
        }
    };

    // The soft NOFILE a user session starts with (systemd's default): the
    // compositor raised its own to the hard limit, and select()-based apps
    // break above FD_SETSIZE.
    inline constexpr rlim_t SESSION_NOFILE = 1024;

    // argv: null-terminated, argv[0] looked up in PATH.
    inline SChild spawn(const char* const* argv, const SOptions& opt = {}) {
        if (!argv || !argv[0])
            return {};

        posix_spawn_file_actions_t fa;
        posix_spawnattr_t          attr;
        if (posix_spawn_file_actions_init(&fa) != 0)
            return {};
        if (posix_spawnattr_init(&attr) != 0) {
            posix_spawn_file_actions_destroy(&fa);
            return {};
        }

        sigset_t mask, defaults;
        sigemptyset(&mask);
        sigemptyset(&defaults);
        for (int sig : {SIGPIPE, SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGCHLD, SIGALRM, SIGUSR1, SIGUSR2, SIGTTIN, SIGTTOU, SIGTSTP, SIGXCPU, SIGXFSZ})
            sigaddset(&defaults, sig);

        short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
        if (opt.ownGroup)
            flags |= POSIX_SPAWN_SETPGROUP;

        // one setflags call: a second one REPLACES the first's flags (the
        // old "SETSIGMASK does not work here" note was that bug)
        bool ok = posix_spawnattr_setsigmask(&attr, &mask) == 0 && posix_spawnattr_setsigdefault(&attr, &defaults) == 0 &&
            (!opt.ownGroup || posix_spawnattr_setpgroup(&attr, 0) == 0) && posix_spawnattr_setflags(&attr, flags) == 0;

        ok = ok && posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0) == 0;
        if (opt.stdoutFd >= 0)
            ok = ok && posix_spawn_file_actions_adddup2(&fa, opt.stdoutFd, STDOUT_FILENO) == 0;
        else
            ok = ok && posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) == 0;
        ok = ok && posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0;
        ok = ok && posix_spawn_file_actions_addclosefrom_np(&fa, STDERR_FILENO + 1) == 0;

        SChild child;
        if (ok) {
            auto* const ARGV = const_cast<char* const*>(argv);
            if (opt.pidfd) {
                if (pidfd_spawnp(&child.pidfd, argv[0], &fa, &attr, ARGV, environ) == 0)
                    child.pid = pidfd_getpid(child.pidfd);
                else
                    child.pidfd = -1;
            } else if (posix_spawnp(&child.pid, argv[0], &fa, &attr, ARGV, environ) != 0)
                child.pid = -1;
        }

        posix_spawn_file_actions_destroy(&fa);
        posix_spawnattr_destroy(&attr);

        if (child.pid > 0 && opt.userFacing) {
            rlimit cur{};
            if (getrlimit(RLIMIT_NOFILE, &cur) == 0 && cur.rlim_cur > SESSION_NOFILE) {
                const rlimit SESSION{std::min(SESSION_NOFILE, cur.rlim_max), cur.rlim_max};
                prlimit(child.pid, RLIMIT_NOFILE, &SESSION, nullptr);
            }
        }
        return child;
    }

    // Reap without blocking, by pidfd (a no-op when SA_NOCLDWAIT already
    // did): never by pid, which the kernel may have handed out again.
    inline void reap(int pidfd) {
        if (pidfd < 0)
            return;
        siginfo_t info{};
        waitid(P_PIDFD, pidfd, &info, WEXITED | WNOHANG);
    }

    // The exit status of a child whose pidfd turned readable. SA_NOCLDWAIT
    // reaps it, so waitpid() has nothing; the pidfd keeps the status
    // (PIDFD_INFO_EXIT, Linux 6.15+) once the auto-reap completed — a
    // moment after the pidfd wakes. nullopt = not (yet) known.
    inline std::optional<int> exitStatus(int pidfd) {
        if (pidfd < 0)
            return std::nullopt;
        pidfd_info info{};
        info.mask = PIDFD_INFO_EXIT;
        if (ioctl(pidfd, PIDFD_GET_INFO, &info) == 0 && (info.mask & PIDFD_INFO_EXIT))
            return info.exit_code;
        return std::nullopt;
    }

} // namespace NAwesome::Proc
