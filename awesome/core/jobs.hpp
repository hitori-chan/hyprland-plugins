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

        // EXIT: pull every source, close every fd, reap.
        void teardown();

      private:
        Jobs() = default;
        struct SChild {
            pid_t                  pid  = -1;
            int                    fd   = -1;
            void*                  src  = nullptr; // wl_event_source*
            std::function<void(int)> onExit;
        };
        static int onChildExit(int, uint32_t, void* data);
        static int onChildExitChecked(int, uint32_t, void* data);

        std::vector<UP<SChild>> m_children;
        std::vector<pid_t>      m_orphans; // couldn't-watch children; WNOHANG-swept on the next spawn
    };

} // namespace NAwesome
