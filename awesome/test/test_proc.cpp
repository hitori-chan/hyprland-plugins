// awesome/test/test_proc.cpp — Proc::spawn under the compositor's own
// process state: SIGTERM blocked, SIGPIPE ignored, SA_NOCLDWAIT. The child
// must start clean, and its exit status must survive the auto-reap.
#include "../core/proc.hpp"

#include "harness.hpp"

#include <poll.h>
#include <string>

namespace {
    std::string runCapture(const char* const* argv, int* status) {
        int pfd[2];
        if (pipe2(pfd, O_CLOEXEC) != 0)
            return {};
        const auto CHILD = NAwesome::Proc::spawn(argv, {.stdoutFd = pfd[1]});
        close(pfd[1]);
        std::string out;
        char        buf[256];
        for (ssize_t n; (n = read(pfd[0], buf, sizeof(buf))) > 0;)
            out.append(buf, (size_t)n);
        close(pfd[0]);
        if (CHILD.pidfd >= 0) {
            pollfd p{.fd = CHILD.pidfd, .events = POLLIN};
            poll(&p, 1, 5000);
            // the pidfd turns readable at exit; under SA_NOCLDWAIT the status
            // is published when the auto-reap completes, a moment later
            std::optional<int> got;
            for (int i = 0; i < 200 && !(got = NAwesome::Proc::exitStatus(CHILD.pidfd)); i++)
                usleep(5000);
            *status = got.value_or(-12345);
            NAwesome::Proc::reap(CHILD.pidfd);
            close(CHILD.pidfd);
        }
        return out;
    }
}

bool test_proc() {
    // the compositor's state, inherited by anything that execs from it
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM);
    sigprocmask(SIG_BLOCK, &blocked, nullptr);
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa{};
    sa.sa_handler = SIG_IGN;
    sa.sa_flags   = SA_NOCLDWAIT;
    sigaction(SIGCHLD, &sa, nullptr);

    // an fd the child must not see (not CLOEXEC, like some compositor fds)
    const int LEAK = open("/dev/null", O_RDONLY);

    {
        const char* ARGV[] = {"sh", "-c", "grep -E '^Sig(Blk|Ign):' /proc/self/status; ls /proc/self/fd | wc -l; exit 7", nullptr};
        int         status = -1;
        const auto  OUT    = runCapture(ARGV, &status);
        AW_CHECK(OUT.find("SigBlk:\t0000000000000000") != std::string::npos); // SIGTERM unblocked
        AW_CHECK(OUT.find("SigIgn:\t0000000000000000") != std::string::npos); // SIGPIPE/SIGCHLD default
        // 0, 1, 2 and the fd ls itself opens: the leaked fd is closed
        AW_CHECK(OUT.find("\n4\n") != std::string::npos);
        AW_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 7); // survives SA_NOCLDWAIT
    }
    {
        const char* ARGV[] = {"/nonexistent/helper", nullptr};
        const auto  CHILD  = NAwesome::Proc::spawn(ARGV);
        AW_CHECK(!CHILD);
    }

    close(LEAK);
    sigprocmask(SIG_UNBLOCK, &blocked, nullptr);
    signal(SIGPIPE, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    return true;
}
