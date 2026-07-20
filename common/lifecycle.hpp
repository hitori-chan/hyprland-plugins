// common/lifecycle.hpp — a plugin's event listeners and deferred event-loop
// hops, torn down in the only safe order: listeners first, so no event
// firing mid-teardown can re-queue a hop that would outlive the .so (a
// pending doLater across unload calls into dlclosed code).
//
// CHop instances self-register into a .so-local registry, so hops living in
// other translation units are reset by the same resetAll() — and once
// teardown began, arm() is a no-op everywhere: even a per-object listener
// a module clears later cannot re-arm one.
#pragma once

#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>

#include <functional>
#include <vector>

namespace NHyprCommon {

    inline std::vector<class CHop*>& hopRegistry() {
        static std::vector<CHop*> R;
        return R;
    }
    inline bool& tearingDown() {
        static bool B = false;
        return B;
    }

    // one deferred event-loop hop; re-arming cancels the pending callback
    class CHop {
      public:
        CHop() {
            hopRegistry().push_back(this);
        }
        ~CHop() {
            std::erase(hopRegistry(), this);
        }
        CHop(const CHop&)            = delete;
        CHop& operator=(const CHop&) = delete;

        void  arm(std::function<void()> fn) {
            if (tearingDown() || !g_pEventLoopManager)
                return;
            m_lock = g_pEventLoopManager->doLaterLock(std::move(fn));
        }
        void reset() {
            m_lock.reset();
        }
        // still true after the callback ran — "was ever armed", the same
        // once-only signal the raw UP gave via operator bool
        bool armed() const {
            return !!m_lock;
        }

      private:
        UP<SEventLoopDoLaterLock> m_lock;
    };

    // the bundle PLUGIN_INIT fills and PLUGIN_EXIT resets with one call
    class CLifecycle {
      public:
        template <typename S, typename F>
        void listen(S& signal, F&& fn) {
            m_listeners.emplace_back(signal.listen(std::forward<F>(fn)));
        }
        void init() {
            tearingDown() = false;
        }
        void resetAll() {
            tearingDown() = true;
            m_listeners.clear(); // listeners before the hops they arm
            for (auto* H : hopRegistry())
                H->reset();
        }

      private:
        std::vector<Hyprutils::Signal::CHyprSignalListener> m_listeners;
    };

} // namespace NHyprCommon
