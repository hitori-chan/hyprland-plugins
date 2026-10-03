// awesome/test/stubs.cpp — the compositor symbols the core headers
// reference that a headless test never exercises: the event-loop manager
// (core/hop.hpp's deferral path). arm() is a no-op on the null manager
// (hop.hpp guards it); Saver::flush() writes directly, so the lock is
// never created here.
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>

UP<SEventLoopDoLaterLock> CEventLoopManager::doLaterLock(const std::function<void()>&) {
    return nullptr;
}
