// awesome/core/jobs.hpp — the plugin's one subprocess path: bounded,
// watched, generation-checked. Ported from hyprnotify's detached spawner,
// generalized: every shell-out in the plugin (sound, hyperlink open,
// wpctl, the launcher's Exec=) goes through here, nothing else forks.
//
// A child per pidfd, reaped by an event-loop source when it dies: no
// blocking, no zombies, and exit pulls the sources before the loop goes.
#pragma once

#include <hyprland/src/helpers/memory/Memory.hpp>
#include <wayland-server-core.h>

#include <cstdint>
#include <functional>
#include <vector>

#include <spawn.h>

namespace NAwesome {

    class Jobs {
      public:
        static Jobs& inst();

        // Fire-and-forget with a bounded live set. At the cap the spawn is
        // SKIPPED — a dropped action is the defined behavior, a fork storm
        // is not (the sound path is arrival-triggered, so a hostile sender
        // can hold a steady fork rate; one live tracked child at a time is
        // one fork).
        // Returns false when the spawn was skipped.
        bool spawn(std::vector<const char*> argv);

        // A child whose exit a callback consumes: readback chains (wpctl).
        // The callback runs on the event loop, after reap; it must be
        // generation-safe (the plugin may be gone — the callback captures
        // only what survives on the heap).
        bool spawnChecked(std::vector<const char*> argv, std::function<void(int)> onExit);

        // A child whose STDOUT and EXIT a callback consumes: the wpctl
        // readback's get-stage. stdout is drained on the event loop into a
        // bounded buffer; at the cap the read end closes (a noisy producer
        // takes SIGPIPE instead of holding the loop in a read loop). The
        // callback runs once — after the pipe EOF/cap AND the reap — and
        // must be generation-safe, like spawnChecked.
        struct SPipedResult {
            std::string out;
            bool        truncated = false; // the cap closed the pipe early
            int         status = 0;        // waitpid's status, best effort
        };
        bool spawnPiped(std::vector<const char*> argv, std::function<void(SPipedResult)> onDone);

        // EXIT: pull every source, close every fd, reap.
        void teardown();

      private:
        Jobs() = default;
        struct SChild {
            pid_t                        pid    = -1;
            int                          fd     = -1;  // the pidfd; -1 = none (piped children)
            int                          outFd  = -1;  // piped stdout read end; -1 = none
            void*                        src    = nullptr; // the pidfd's wl_event_source*
            void*                        outSrc = nullptr; // the pipe's wl_event_source*
            bool                         pipeDone = false; // EOF, error, or the cap
            bool                         truncated = false;
            std::string                  out;
            std::function<void(int)>     onExit;
            std::function<void(SPipedResult)> onPiped;
        };
        static int onChildExit(int, uint32_t, void* data);
        static int onChildExitChecked(int, uint32_t, void* data);
        static int onPipedOut(int, uint32_t, void* data); // pipe readable: the only edge (wpctl is single-process; its exit IS the EOF)
        static void finishPiped(SChild* c);               // the pipe is done: deliver, then release

        std::vector<UP<SChild>> m_children;
        std::vector<pid_t>      m_orphans; // couldn't-watch children; WNOHANG-swept on the next spawn
    };

} // namespace NAwesome
