// awesome/core/hop.hpp — the plugin's deferral path (crash class 6).
//
// Workspace and focus changes never run inside an input emission: they arm
// a hop, which runs on the next event-loop turn. One mechanism, a registry,
// and a single teardown: every Hop instance self-registers, PLUGIN_EXIT
// resets them all AFTER the listeners are gone (a listener firing
// mid-teardown could otherwise re-queue a hop that outlives the .so), and
// once teardown began, arm() is a no-op everywhere — even a per-object
// listener a module clears later cannot re-arm one.
//
// Multiple hops coexist (the warm, a focus toggle, a store flush): each
// owns its doLater lock, and re-arming ONE cancels only that one — the
// press accumulators in the modules rely on exactly that.
#pragma once

#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>

#include <functional>
#include <vector>

namespace NAwesome {

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

    // the .so-wide teardown, in the only safe order: hops after the
    // listeners that armed them. Called once from PLUGIN_EXIT.
    inline void resetHops() {
        tearingDown() = true;
        for (auto* H : hopRegistry())
            H->reset();
    }
    inline void beginSession() {
        tearingDown() = false;
    }

} // namespace NAwesome
