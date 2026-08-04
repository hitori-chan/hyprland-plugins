// hyprosd — the awesome volume/brightness OSD as a native Hyprland plugin.
//
// The XF86 audio/brightness keys call hl.plugin.hyprosd.* and a card with
// the value bar answers (ids 9992 brightness / 9993 volume / 9995 mic,
// replaced in place like the old scripts pinned them). Replaces
// scripts/osd.sh.
//
// - Brightness is fork-free: current/max read from /sys/class/backlight,
//   ±5% linear steps (the shown percent IS current/max — the old
//   exponential curve made "95%" mean 81% output), floor 2 raw so the
//   panel never goes black, written through logind
//   Session.SetBrightness on the system bus — the session owner needs no
//   root and no udev rule.
// - Volume/mic go through wpctl (PipeWire stays out of the process): the
//   set spawns, its pidfd tells the event loop when it's done, then the
//   get spawns with its stdout on a pipe the event loop drains — two
//   short forks per keypress instead of the script's shell pipeline, and
//   render/input never wait on any of it.
// - Cards carry the `value` hint (the daemon's 4px bar) and no explicit icon:
//   hyprnotify supplies its deterministic generic app mark.
// - Feedback rides the plugin's own event-loop-integrated session-bus
//   connection (hyprbar's tray pattern; the daemon's API is the bus name,
//   never its symbols). Bus death turns the cards off; the keys keep
//   working.
//
// Everything lives in NHyprosd so no symbol can collide with another
// plugin's at dlopen time.

#include "common/busclient.hpp"
#include "common/lifecycle.hpp"
#include "common/process.hpp"
#include "wpctl.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/helpers/time/Time.hpp>

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sdbus-c++/sdbus-c++.h>
#include <spawn.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern char** environ;

namespace NHyprosd {

    HANDLE        PHANDLE = nullptr;

    constexpr size_t MAX_ACTION_QUEUE     = 128;
    constexpr size_t MAX_ACTIVE_CHAINS    = 16;
    constexpr size_t MAX_ORPHANS          = 32;
    constexpr size_t MAX_TRACKED_CHILDREN = 32;

    // ---- bus links (hyprpad's chassis, one per bus) ----

    static NHyprCommon::CBusLink               sessionBus; // the cards' Notify sender
    static NHyprCommon::CBusLink               systemBus;  // logind SetBrightness

    static NHyprCommon::CLifecycle             g_lifecycle; // no bus listeners here — it owns the hops

    static std::unique_ptr<sdbus::IProxy>      notifyProxy; // on sessionBus
    static std::unique_ptr<sdbus::IProxy>      logindProxy; // on systemBus

    static void busInit(NHyprCommon::CBusLink& L, bool system, const char* label) {
        L.onLost = [label](const std::string& err) {
            HyprlandAPI::addNotification(PHANDLE, std::string{"[hyprosd] "} + label + " bus lost: " + err, CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
        };
        L.dropOwned = [&L]() {
            if (&L == &sessionBus)
                notifyProxy.reset();
            else
                logindProxy.reset();
        };
        try {
            L.open(system);
            L.sync(); // set the initial mask/timeout
        } catch (const std::exception& E) {
            HyprlandAPI::addNotification(PHANDLE, std::string{"[hyprosd] no "} + label + " bus: " + E.what(), CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
        }
    }

    // ---- the cards ----

    static void notify(uint32_t id, const char* summary, const std::string& body, int value) {
        if (!sessionBus.conn())
            return;
        try {
            if (!notifyProxy)
                notifyProxy = sdbus::createProxy(*sessionBus.conn(), sdbus::ServiceName{"org.freedesktop.Notifications"}, sdbus::ObjectPath{"/org/freedesktop/Notifications"});
            std::map<std::string, sdbus::Variant> hints{{"urgency", sdbus::Variant{uint8_t{0}}}, {"x-hitori-osd", sdbus::Variant{true}}};
            if (value >= 0)
                hints.emplace("value", sdbus::Variant{int32_t{value}});
            notifyProxy->callMethodAsync("Notify")
                .onInterface("org.freedesktop.Notifications")
                .withArguments(std::string{"osd"}, id, std::string{}, std::string{summary}, body, std::vector<std::string>{}, hints, 1200)
                .uponReplyInvoke([](std::optional<sdbus::Error>, uint32_t) {});
            sessionBus.pollSoon(); // flush the send from the event loop, never from here
        } catch (...) {} // broker gone: teardown is already pending, drop the card
    }

    // ---- brightness (sysfs + logind, zero forks) ----

    static std::string backlightDev; // /sys/class/backlight/<dev>, name only
    static int         backlightMax = 0;

    static void        findBacklight() {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator("/sys/class/backlight", ec)) {
            std::ifstream m(e.path() / "max_brightness");
            if (m && (m >> backlightMax) && backlightMax > 0) {
                backlightDev = e.path().filename();
                return;
            }
        }
    }

    // A keypress bases its step on what the previous one just asked for:
    // logind's write is asynchronous, so a fast repeat would read stale
    // sysfs and re-step from the same value. Half a second of trust, then
    // sysfs is the truth again (external tools, resume).
    static int             lastSetRaw = -1;
    static Time::steady_tp lastSetAt;
    static uint64_t        brightnessGeneration = 0;
    static uint64_t        volumeGeneration     = 0;
    static uint64_t        micGeneration        = 0;
    static uint64_t        volumeFeedback       = 0;
    static uint64_t        micFeedback          = 0;

    static void            brightnessStep(int dir) {
        const uint64_t GENERATION = ++brightnessGeneration;
        if (backlightDev.empty() || !systemBus.conn())
            return;

        int raw = -1;
        if (lastSetRaw >= 0 && Time::steadyNow() - lastSetAt < std::chrono::milliseconds(500))
            raw = lastSetRaw;
        else {
            std::ifstream b("/sys/class/backlight/" + backlightDev + "/brightness");
            if (!b || !(b >> raw) || raw < 0)
                return;
        }

        // ±5% of max, linear, floored at 2 raw (the old brightnessctl -n2)
        const int STEP = std::max(1, (int)std::lround(backlightMax * 0.05));
        raw            = std::clamp(raw + dir * STEP, 2, backlightMax);

        const int PCT = (int)std::lround(100.0 * raw / backlightMax);
        try {
            if (!logindProxy)
                logindProxy = sdbus::createProxy(*systemBus.conn(), sdbus::ServiceName{"org.freedesktop.login1"}, sdbus::ObjectPath{"/org/freedesktop/login1/session/auto"});
            // the card waits for logind's ack: a refused write must not
            // flash a percent that never applied (the reply lands on this
            // event loop; sending from a dispatch callback is fine, only
            // draining is not)
            logindProxy->callMethodAsync("SetBrightness")
                .onInterface("org.freedesktop.login1.Session")
                .withArguments(std::string{"backlight"}, backlightDev, (uint32_t)raw)
                .uponReplyInvoke([PCT, GENERATION](std::optional<sdbus::Error> err) {
                    if (!err && GENERATION == brightnessGeneration)
                        notify(9992, "Brightness", std::to_string(PCT) + "%", PCT);
                    else if (GENERATION == brightnessGeneration)
                        lastSetRaw = -1; // logind refused: drop the trust window so the next press re-reads sysfs
                });
            systemBus.pollSoon();
        } catch (...) { return; }

        lastSetRaw = raw;
        lastSetAt  = Time::steadyNow();
    }

    // ---- volume / mic (wpctl, sequenced on the event loop) ----

    enum eAction : uint8_t {
        VOL_UP,
        VOL_DOWN,
        VOL_MUTE,
        MIC_MUTE, // the wpctl four — ARGV below indexes them
        BRI_UP,
        BRI_DOWN
    };

    // set-child done -> spawn the get-child with its stdout on a pipe ->
    // pipe EOF -> parse + card. Chains overlap freely under key repeat:
    // every set runs (each IS a step), late gets just show the final state.
    struct SChain {
        bool             mic    = false;
        uint64_t         generation = 0;
        pid_t            setPid = -1, getPid = -1;
        int              pidFd = -1, outFd = -1;
        wl_event_source* pidSrc = nullptr;
        wl_event_source* outSrc = nullptr;
        std::string      out;
        bool             outputTruncated = false;
    };
    static std::vector<UP<SChain>> chains;
    static std::vector<pid_t>      orphans; // children without a live event source
    static SP<CEventLoopTimer>     orphanTick;

    static size_t trackedChildren() {
        size_t count = orphans.size();
        for (const auto& C : chains)
            count += (C->setPid > 0) + (C->getPid > 0);
        return count;
    }

    static bool canTrackChild(bool newChain = true) {
        return (!newChain || chains.size() < MAX_ACTIVE_CHAINS) && orphans.size() < MAX_ORPHANS && trackedChildren() < MAX_TRACKED_CHILDREN;
    }

    static void                    reapOrphans() {
        std::erase_if(orphans, [](pid_t p) {
            const pid_t RESULT = NHyprCommon::reapPid(p);
            return RESULT > 0 || (RESULT < 0 && errno == ECHILD);
        });
        if (orphanTick && g_pEventLoopManager)
            orphanTick->updateTimeout(orphans.empty() ? std::nullopt : std::optional{std::chrono::milliseconds(100)});
    }

    static void                    rememberOrphan(pid_t pid) {
        if (pid <= 0 || std::find(orphans.begin(), orphans.end(), pid) != orphans.end())
            return;
        // Every spawn is admitted through canTrackChild(). This guard keeps a
        // late fallback bounded if the ownership path changes in the future.
        if (orphans.size() >= MAX_ORPHANS)
            return;
        orphans.push_back(pid);
        reapOrphans();
    }

    static void                    chainDone(SChain* c) {
        if (c->pidSrc)
            wl_event_source_remove(c->pidSrc);
        if (c->outSrc)
            wl_event_source_remove(c->outSrc);
        if (c->pidFd >= 0)
            close(c->pidFd);
        if (c->outFd >= 0)
            close(c->outFd);
        // a child that closed its stdout a hair before exiting isn't a zombie
        // yet; hand it to the orphan list to re-reap rather than leak it
        const auto detach = [](pid_t& pid) {
            if (pid <= 0)
                return;
            const pid_t CHILD = std::exchange(pid, -1);
            const pid_t RESULT = NHyprCommon::reapPid(CHILD);
            if (RESULT == 0)
                rememberOrphan(CHILD);
        };
        detach(c->setPid);
        detach(c->getPid);
        std::erase_if(chains, [&](const auto& U) { return U.get() == c; });
    }

    static pid_t spawn(const std::vector<const char*>& argv, posix_spawn_file_actions_t* fa) {
        pid_t pid = -1;
        if (posix_spawnp(&pid, argv[0], fa, nullptr, const_cast<char* const*>(argv.data()), environ) != 0)
            return -1;
        return pid;
    }

    static int onGetOut(int fd, uint32_t mask, void* data) {
        auto* c = (SChain*)data;
        char  buf[256];
        for (;;) {
            const auto N = read(fd, buf, sizeof(buf));
            if (N > 0) {
                constexpr size_t CAP = 4096;
                const size_t      KEEP = c->out.size() < CAP ? std::min<size_t>((size_t)N, CAP - c->out.size()) : 0;
                if (KEEP)
                    c->out.append(buf, KEEP);
                if (KEEP != (size_t)N)
                    c->outputTruncated = true;
                continue;
            }
            if (N < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return 0; // more later
            break;        // EOF or error: the child is done talking
        }

        if (c->outputTruncated) {
            chainDone(c);
            return 0;
        }

        const auto READBACK = Wpctl::parseReadback(c->out);
        const bool MUTED    = READBACK && READBACK->muted;
        const int  PCT      = READBACK ? (READBACK->value >= 1.0 ? 100 : (int)std::lround(READBACK->value * 100.0)) : -1;
        uint64_t&  FEEDBACK = c->mic ? micFeedback : volumeFeedback;
        const bool NEWER    = READBACK && c->generation > FEEDBACK;

        // no parseable readback (no default device, wpctl error): no card —
        // asserting "live"/a percent for a state that never changed lies
        if (NEWER)
            FEEDBACK = c->generation;
        if (NEWER && c->mic) {
            if (MUTED || PCT >= 0)
                notify(9995, "Microphone", MUTED ? "muted" : "live", -1);
        } else if (NEWER && MUTED)
            notify(9993, "Volume", "muted", -1);
        else if (NEWER && PCT >= 0)
            notify(9993, "Volume", std::to_string(PCT) + "%", std::min(PCT, 100));

        chainDone(c);
        return 0;
    }

    static int onSetDone(int, uint32_t, void* data) {
        auto* c = (SChain*)data;
        wl_event_source_remove(c->pidSrc);
        c->pidSrc = nullptr;
        close(c->pidFd);
        c->pidFd = -1;
        // Hyprland installs SA_NOCLDWAIT, so no child exit status remains to
        // reap. The pidfd is the completion signal; the following readback is
        // the authoritative state exposed to the user.
        c->setPid = -1;

        if (!canTrackChild(false)) {
            chainDone(c);
            return 0;
        }

        int pfd[2];
        if (pipe2(pfd, O_CLOEXEC | O_NONBLOCK) != 0) {
            chainDone(c);
            return 0;
        }
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, pfd[1], 1);
        c->getPid = spawn({"wpctl", "get-volume", c->mic ? "@DEFAULT_AUDIO_SOURCE@" : "@DEFAULT_AUDIO_SINK@", nullptr}, &fa);
        posix_spawn_file_actions_destroy(&fa);
        close(pfd[1]);
        if (c->getPid < 0) {
            close(pfd[0]);
            chainDone(c);
            return 0;
        }
        c->outFd  = pfd[0];
        c->outSrc = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, c->outFd, WL_EVENT_READABLE, onGetOut, c);
        if (!c->outSrc) {
            close(c->outFd);
            c->outFd = -1;
            if (NHyprCommon::reapPid(c->getPid) == 0)
                rememberOrphan(c->getPid);
            c->getPid = -1;
            chainDone(c);
        }
        return 0;
    }

    static void wpctlAction(eAction a) {
        static const std::vector<const char*> ARGV[] = {
            {"wpctl", "set-volume", "-l", "1.0", "@DEFAULT_AUDIO_SINK@", "5%+", nullptr},
            {"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "5%-", nullptr},
            {"wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle", nullptr},
            {"wpctl", "set-mute", "@DEFAULT_AUDIO_SOURCE@", "toggle", nullptr},
        };

        if (!canTrackChild())
            return;

        const pid_t PID = spawn(ARGV[a], nullptr);
        if (PID < 0)
            return;

        const bool MIC = a == MIC_MUTE;
        auto c         = makeUnique<SChain>();
        c->mic         = MIC;
        c->generation  = ++(MIC ? micGeneration : volumeGeneration);
        c->setPid = PID;
        c->pidFd  = (int)syscall(SYS_pidfd_open, PID, 0);
        if (c->pidFd < 0) {
            // no pidfd (EMFILE, ancient kernel): never block the loop on a
            // reap — the orphan list re-reaps it; the card is skipped
            rememberOrphan(PID);
            return;
        }
        fcntl(c->pidFd, F_SETFD, FD_CLOEXEC); // SYS_pidfd_open takes no CLOEXEC flag; keep it out of concurrent spawns
        c->pidSrc = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, c->pidFd, WL_EVENT_READABLE, onSetDone, c.get());
        if (!c->pidSrc) {
            close(c->pidFd);
            c->pidFd = -1;
            if (NHyprCommon::reapPid(PID) == 0)
                rememberOrphan(PID);
            return;
        }
        chains.push_back(std::move(c));
    }

    // ---- the Lua face (hl.plugin.hyprosd.*) ----

    // Actions queue and drain from the event loop, never inside the bind's
    // input emission; a queue rather than one deferred slot so a key-repeat
    // burst never coalesces two steps into one.
    static std::vector<uint8_t> queued;
    static NHyprCommon::CHop    pendingDrain;

    static void                 enqueue(eAction a) {
        if (!g_pEventLoopManager)
            return; // an unarmable drain must not let the queue grow
        if (queued.size() >= MAX_ACTION_QUEUE)
            return; // bounded backpressure under a key-repeat storm
        queued.push_back(a);
        pendingDrain.arm([]() {
            reapOrphans();
            for (const auto A : queued) {
                if (A == BRI_UP || A == BRI_DOWN)
                    brightnessStep(A == BRI_UP ? 1 : -1);
                else
                    wpctlAction((eAction)A);
            }
            queued.clear();
        });
    }

    static int luaVolumeUp(lua_State*) {
        enqueue(VOL_UP);
        return 0;
    }
    static int luaVolumeDown(lua_State*) {
        enqueue(VOL_DOWN);
        return 0;
    }
    static int luaMute(lua_State*) {
        enqueue(VOL_MUTE);
        return 0;
    }
    static int luaMicMute(lua_State*) {
        enqueue(MIC_MUTE);
        return 0;
    }
    static int luaBrightnessUp(lua_State*) {
        enqueue(BRI_UP);
        return 0;
    }
    static int luaBrightnessDown(lua_State*) {
        enqueue(BRI_DOWN);
        return 0;
    }

} // namespace NHyprosd

using namespace NHyprosd;

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprosd] Version mismatch: rebuild the plugin against the running Hyprland", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hyprosd] version mismatch");
    }

    g_lifecycle.init();

    busInit(sessionBus, false, "session");
    busInit(systemBus, true, "system");
    orphanTick = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { reapOrphans(); }, nullptr);
    g_pEventLoopManager->addTimer(orphanTick);
    findBacklight();

    HyprlandAPI::addLuaFunction(PHANDLE, "hyprosd", "volume_up", luaVolumeUp);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprosd", "volume_down", luaVolumeDown);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprosd", "mute", luaMute);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprosd", "mic_mute", luaMicMute);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprosd", "brightness_up", luaBrightnessUp);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprosd", "brightness_down", luaBrightnessDown);

    return {"hyprosd", "the awesome volume/brightness OSD", "hitori", "1.2.4"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_lifecycle.resetAll(); // every hop, in one place
    queued.clear();
    while (!chains.empty())
        chainDone(chains.back().get());
    reapOrphans();
    // Event sources and descriptors are gone. The exact target installs
    // SA_NOCLDWAIT, so a helper that exits later cannot become a zombie and
    // must not hold compositor teardown hostage.
    orphans.clear();
    if (orphanTick && g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(orphanTick);
    orphanTick.reset();
    backlightDev.clear();
    backlightMax = 0;
    lastSetRaw   = -1;
    brightnessGeneration = 0;
    volumeGeneration     = 0;
    micGeneration        = 0;
    volumeFeedback       = 0;
    micFeedback          = 0;
    sessionBus.close(); // fd sources out BEFORE the connections die
    systemBus.close();
}
