// Quiet automatically: two remembered switches, both off until the user
// turns them on from the bell's menu. While the focused window is
// fullscreen, or while a display or a region of one is shared, banners and
// sounds hold: an arrival lands in the shade and the bell dots, as an app
// with banners off does, and the soft banners already up retreat there
// when the quiet begins (a share's once it has held a second). Critical
// punches through, as through DND; over a share it shows without its words.
// A window share never shows a card, so it never quiets anything.
#include "model.hpp"

#include "../core/state.hpp"

#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/screenshare/ScreenshareManager.hpp>
#include <hyprland/src/state/MonitorState.hpp>

namespace NAwesome::Notify::Quiet {

    // A share as the cards last rendered it, watched only while its switch
    // is on, and whether it outlasted a poll: the banners already up retreat
    // only then (a screenshot's half second never moves them). The
    // compositor reports a share from its first copied frame and calls it
    // over half a second after its last, while a still screen keeps a frame
    // waiting and copies none: while one is held, a poll rechecks.
    static bool                shared = false, settled = false;
    static eWhy                shown  = eWhy::NONE; // what the open center's status line last said
    static SP<CEventLoopTimer> sharePoll;
    constexpr auto             POLL = std::chrono::seconds(1);

    bool on(eOption o) {
        return StateStore::inst().data().options.on(o);
    }

    static bool fullscreenFocused() {
        const auto W = Desktop::focusState() ? Desktop::focusState()->window() : nullptr;
        return W && Fullscreen::controller()->getFullscreenModes(W).internal == Fullscreen::FSMODE_FULLSCREEN;
    }

    // A monitor's (or a region's) capture with frames flowing, or one frame
    // waiting for a still screen to change: the compositor's own test for a
    // consumer. A live session alone proves nothing: a wlr-screencopy
    // client keeps its sessions until it disconnects, and the portal never
    // does.
    static bool shareLive() {
        if (!Screenshare::mgr())
            return false;
        for (const auto& M : State::monitorState()->monitors()) {
            if (!M)
                continue;
            const auto S = Screenshare::mgr()->outputCopyFBState(M);
            if (S.sharingSessions > 0 || S.pendingMonitorFrames > 0 || S.pendingRegionFrames > 0)
                return true;
        }
        return false;
    }

    eWhy why() {
        if (on(eOption::QUIET_SHARING) && shareLive())
            return eWhy::SHARING;
        return on(eOption::QUIET_FULLSCREEN) && fullscreenFocused() ? eWhy::FULLSCREEN : eWhy::NONE;
    }

    bool now() {
        return why() != eWhy::NONE;
    }

    bool hushed() {
        return shared;
    }

    static void poll();
    static void armPoll(bool arm) {
        if (!arm) {
            if (sharePoll)
                sharePoll->updateTimeout(std::nullopt);
            return;
        }
        if (!sharePoll) {
            sharePoll = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { poll(); }, nullptr);
            g_pEventLoopManager->addTimer(sharePoll);
        }
        sharePoll->updateTimeout(POLL);
    }

    void refresh() {
        if (NAwesome::compositorShuttingDown())
            return;
        const bool SHARED = on(eOption::QUIET_SHARING) && shareLive();
        if (SHARED != shared) {
            shared  = SHARED;
            settled = false;
            armPoll(shared);
            notifChanged(); // a critical banner's words go, or come back
        }
        if ((on(eOption::QUIET_FULLSCREEN) && fullscreenFocused()) || settled)
            Model::lowerBanners({});
        if (const auto WHY = why(); WHY != shown) {
            shown = WHY;
            if (centerVisible())
                notifChanged(); // its status line follows
        }
    }

    static void poll() {
        settled = shared; // it held a whole poll
        refresh();
        if (shared)
            armPoll(true);
    }

    void set(eOption o, bool enable) {
        if (o != eOption::QUIET_FULLSCREEN && o != eOption::QUIET_SHARING)
            return;
        if (StateStore::inst().data().options.set(o, enable ? 1 : 0))
            StateStore::inst().dirty();
        refresh();
        notifChanged(); // turned off over a share: the words come back
    }

    void exit() {
        if (sharePoll && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(sharePoll);
        sharePoll.reset();
        shared = settled = false;
        shown            = eWhy::NONE;
    }

} // namespace NAwesome::Notify::Quiet
