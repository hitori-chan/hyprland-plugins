// awesome/core/jobs.hpp — the plugin's helper processes: bounded per
// class, watched by pidfd, killed past a deadline. Every helper starts
// through Proc::spawn (core/proc.hpp); the launcher's Exec= goes through
// the compositor's own executor instead (exec rules, activation tokens).
//
// A child per pidfd, handled by an event-loop source when it dies: no
// blocking, no zombies, and teardown pulls every source before the loop
// goes.
#pragma once

#include <hyprland/src/helpers/memory/Memory.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class CEventLoopTimer;

namespace NAwesome {

    class Jobs {
      public:
        // Separate budgets: a burst of notification sounds must never take
        // the slots the volume keys need.
        enum eClass : uint8_t {
            CONTROL = 0, // short readback/control commands (wpctl): 16 live, killed after 5 s
            SOUND,       // one-shot players: 4 live, killed after 30 s
            OPEN,        // a user-facing opener (xdg-open may exec the browser itself):
                         // never tracked, never killed, NOFILE back to the session's
            CLASS_COUNT,
        };

        static Jobs& inst();

        // Fire-and-forget. At the class cap the spawn is SKIPPED — a dropped
        // action is the defined behavior, a fork storm is not (sound is
        // arrival-triggered: a hostile sender can hold a steady rate).
        // Returns false when the spawn was skipped or failed.
        bool spawn(eClass cls, std::vector<const char*> argv);

        // A child whose exit a callback consumes (status: the wait status,
        // -1 = unknown or killed at the deadline). The callback runs on the
        // event loop after the exit and must be generation-safe.
        bool spawnChecked(eClass cls, std::vector<const char*> argv, std::function<void(int)> onExit);

        // A child whose STDOUT and exit a callback consumes: stdout drains on
        // the event loop into a bounded buffer; at the cap the read end
        // closes and the child is killed. The callback runs once, after EOF
        // (or the cap, or the deadline).
        struct SPipedResult {
            std::string out;
            bool        truncated = false; // the cap or the deadline cut it short
            int         status    = -1;    // the wait status, -1 = unknown
        };
        bool spawnPiped(eClass cls, std::vector<const char*> argv, std::function<void(SPipedResult)> onDone);

        // EXIT: kill and release every tracked child, pull every source.
        void teardown();

      private:
        Jobs() = default;
        struct SChild {
            eClass                            cls      = CONTROL;
            int                               pidfd    = -1;
            int                               outFd    = -1;      // piped stdout read end
            void*                             src      = nullptr; // the pidfd's wl_event_source*
            void*                             outSrc   = nullptr; // the pipe's wl_event_source*
            std::chrono::steady_clock::time_point deadline;
            bool                              exited    = false;
            bool                              pipeDone  = false; // EOF, error, the cap, or the deadline
            bool                              truncated = false;
            std::string                       out;
            std::function<void(int)>          onExit;
            std::function<void(SPipedResult)> onPiped;
        };

        bool                spawnTracked(eClass cls, std::vector<const char*>& argv, UP<SChild> child, int stdoutFd = -1);
        size_t              live(eClass cls) const;
        void                release(SChild* c); // pull sources, close fds, erase
        void                maybeFinish(SChild* c);
        void                armDeadline();
        void                onDeadline();
        static int          onExitReadable(int, uint32_t, void* data);
        static int          onPipeReadable(int, uint32_t, void* data);

        std::vector<UP<SChild>> m_children;
        SP<CEventLoopTimer>     m_timer;
    };

} // namespace NAwesome
