// common/process.hpp — EINTR-safe ownership primitives for plugin children.
#pragma once

#include <cerrno>
#include <sys/wait.h>

namespace NHyprCommon {

    inline pid_t reapPid(pid_t pid) noexcept {
        if (pid <= 0)
            return -1;
        for (;;) {
            const pid_t RESULT = waitpid(pid, nullptr, WNOHANG);
            if (RESULT < 0 && errno == EINTR)
                continue;
            return RESULT;
        }
    }

} // namespace NHyprCommon
