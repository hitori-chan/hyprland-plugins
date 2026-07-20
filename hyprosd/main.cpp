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
// - Cards carry the `value` hint (the daemon's 4px bar) and no icon: the
//   daemon's fallback_icon_dir rolls each card its face.
// - Feedback rides the plugin's own event-loop-integrated session-bus
//   connection (hyprbar's tray pattern; the daemon's API is the bus name,
//   never its symbols). Bus death turns the cards off; the keys keep
//   working.
//
// Everything lives in NHyprosd so no symbol can collide with another
// plugin's at dlopen time.

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
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern char** environ;

HANDLE        PHANDLE = nullptr;

namespace NHyprosd {

    // ---- bus links (hyprpad's chassis, one per bus) ----

    struct SBusLink {
        std::unique_ptr<sdbus::IConnection> conn;
        SP<CEventLoopTimer>                 poll; // sd-bus timeout carrier + deferred-drain kicker, normally disarmed
        wl_event_source*                    src    = nullptr;
        wl_event_source*                    evtSrc = nullptr;
        UP<SEventLoopDoLaterLock>           pendingDrop;
        const char*                         label = "";
    };
    static SBusLink                            sessionBus{.label = "session"};
    static SBusLink                            systemBus{.label = "system"};

    static std::unique_ptr<sdbus::IProxy>      notifyProxy; // on sessionBus
    static std::unique_ptr<sdbus::IProxy>      logindProxy; // on systemBus

    // sd-bus dispatch is not re-entrant: never drain from a send site, park
    // a near tick on the timer instead
    static void pollSoon(SBusLink& L) {
        if (L.poll)
            L.poll->updateTimeout(std::chrono::milliseconds(2));
    }

    static void busTeardown(SBusLink& L) {
        if (L.src)
            wl_event_source_remove(L.src);
        if (L.evtSrc)
            wl_event_source_remove(L.evtSrc);
        L.src = L.evtSrc = nullptr;
        if (L.poll)
            L.poll->updateTimeout(std::nullopt);
        if (&L == &sessionBus)
            notifyProxy.reset();
        else
            logindProxy.reset();
        L.conn.reset();
    }

    static void syncBus(SBusLink& L) {
        if (!L.conn)
            return;
        try {
            int n = 0;
            while (n++ < 64 && L.conn->processPendingEvent()) {}
            const auto PD = L.conn->getEventLoopPollData();
            if (L.src)
                wl_event_source_fd_update(L.src, ((PD.events & POLLIN) ? WL_EVENT_READABLE : 0) | ((PD.events & POLLOUT) ? WL_EVENT_WRITABLE : 0));
            const auto REL = PD.getRelativeTimeout();
            if (n > 64)
                L.poll->updateTimeout(std::chrono::milliseconds(2));
            else if (REL == std::chrono::microseconds::max())
                L.poll->updateTimeout(std::nullopt);
            else
                L.poll->updateTimeout(std::max(std::chrono::duration_cast<std::chrono::milliseconds>(REL), std::chrono::milliseconds(1)));
        } catch (const std::exception& E) {
            // the bus died under us; an escape would unwind through the event
            // loop's C frames. Only the first failing source tears down.
            if (L.conn && g_pEventLoopManager && !L.pendingDrop) {
                HyprlandAPI::addNotification(PHANDLE, std::string{"[hyprosd] "} + L.label + " bus lost: " + E.what(), CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
                L.pendingDrop = g_pEventLoopManager->doLaterLock([&L]() { busTeardown(L); });
            }
        }
    }

    static int onBusFd(int, uint32_t, void* data) {
        syncBus(*(SBusLink*)data);
        return 0;
    }

    static bool busInit(SBusLink& L, bool system) {
        try {
            L.conn = system ? sdbus::createSystemBusConnection() : sdbus::createSessionBusConnection();
            L.poll = makeShared<CEventLoopTimer>(std::nullopt, [&L](SP<CEventLoopTimer>, void*) { syncBus(L); }, nullptr);
            g_pEventLoopManager->addTimer(L.poll);
            const auto PD = L.conn->getEventLoopPollData();
            L.src         = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, PD.fd, WL_EVENT_READABLE, onBusFd, &L);
            L.evtSrc      = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, PD.eventFd, WL_EVENT_READABLE, onBusFd, &L);
            syncBus(L); // set the initial mask/timeout
            return true;
        } catch (const std::exception& E) {
            HyprlandAPI::addNotification(PHANDLE, std::string{"[hyprosd] no "} + L.label + " bus: " + E.what(), CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
            busTeardown(L);
            return false;
        }
    }

    // ---- the cards ----

    static void notify(uint32_t id, const char* summary, const std::string& body, int value) {
        if (!sessionBus.conn)
            return;
        try {
            if (!notifyProxy)
                notifyProxy = sdbus::createProxy(*sessionBus.conn, sdbus::ServiceName{"org.freedesktop.Notifications"}, sdbus::ObjectPath{"/org/freedesktop/Notifications"});
            std::map<std::string, sdbus::Variant> hints{{"urgency", sdbus::Variant{uint8_t{0}}}};
            if (value >= 0)
                hints.emplace("value", sdbus::Variant{int32_t{value}});
            notifyProxy->callMethodAsync("Notify")
                .onInterface("org.freedesktop.Notifications")
                .withArguments(std::string{"osd"}, id, std::string{}, std::string{summary}, body, std::vector<std::string>{}, hints, 1200)
                .uponReplyInvoke([](std::optional<sdbus::Error>, uint32_t) {});
            pollSoon(sessionBus); // flush the send from the event loop, never from here
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

    static void            brightnessStep(int dir) {
        if (backlightDev.empty() || !systemBus.conn)
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
                logindProxy = sdbus::createProxy(*systemBus.conn, sdbus::ServiceName{"org.freedesktop.login1"}, sdbus::ObjectPath{"/org/freedesktop/login1/session/auto"});
            // the card waits for logind's ack: a refused write must not
            // flash a percent that never applied (the reply lands on this
            // event loop; sending from a dispatch callback is fine, only
            // draining is not)
            logindProxy->callMethodAsync("SetBrightness")
                .onInterface("org.freedesktop.login1.Session")
                .withArguments(std::string{"backlight"}, backlightDev, (uint32_t)raw)
                .uponReplyInvoke([PCT](std::optional<sdbus::Error> err) {
                    if (!err)
                        notify(9992, "Brightness", std::to_string(PCT) + "%", PCT);
                    else
                        lastSetRaw = -1; // logind refused: drop the trust window so the next press re-reads sysfs
                });
            pollSoon(systemBus);
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
        pid_t            setPid = -1, getPid = -1;
        int              pidFd = -1, outFd = -1;
        wl_event_source* pidSrc = nullptr;
        wl_event_source* outSrc = nullptr;
        std::string      out;
    };
    static std::vector<UP<SChain>> chains;
    static std::vector<pid_t>      orphans; // capped-burst children still owed a waitpid

    static void                    reapOrphans() {
        std::erase_if(orphans, [](pid_t p) { return waitpid(p, nullptr, WNOHANG) != 0; });
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
        if (c->setPid > 0 && waitpid(c->setPid, nullptr, WNOHANG) == 0)
            orphans.push_back(c->setPid);
        if (c->getPid > 0 && waitpid(c->getPid, nullptr, WNOHANG) == 0)
            orphans.push_back(c->getPid);
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
                c->out.append(buf, N);
                continue;
            }
            if (N < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return 0; // more later
            break;        // EOF or error: the child is done talking
        }

        // wpctl get-volume: "Volume: 0.65" or "Volume: 0.65 [MUTED]" —
        // formatted through the user locale, so a comma decimal must parse too
        const bool MUTED = c->out.find("[MUTED]") != std::string::npos;
        int        pct   = -1;
        if (const auto P = c->out.find(':'); P != std::string::npos) {
            auto num = c->out.substr(P + 1);
            if (const auto CM = num.find(','); CM != std::string::npos)
                num[CM] = '.';
            pct = (int)std::lround(std::strtod(num.c_str(), nullptr) * 100.0);
        }

        // no parseable readback (no default device, wpctl error): no card —
        // asserting "live"/a percent for a state that never changed lies
        if (c->mic) {
            if (MUTED || pct >= 0)
                notify(9995, "Microphone", MUTED ? "muted" : "live", -1);
        } else if (MUTED)
            notify(9993, "Volume", "muted", -1);
        else if (pct >= 0)
            notify(9993, "Volume", std::to_string(pct) + "%", std::min(pct, 100));

        chainDone(c);
        return 0;
    }

    static int onSetDone(int, uint32_t, void* data) {
        auto* c = (SChain*)data;
        wl_event_source_remove(c->pidSrc);
        c->pidSrc = nullptr;
        close(c->pidFd);
        c->pidFd = -1;
        waitpid(c->setPid, nullptr, WNOHANG);
        c->setPid = -1;

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
        return 0;
    }

    static void wpctlAction(eAction a) {
        static const std::vector<const char*> ARGV[] = {
            {"wpctl", "set-volume", "-l", "1.0", "@DEFAULT_AUDIO_SINK@", "5%+", nullptr},
            {"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "5%-", nullptr},
            {"wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle", nullptr},
            {"wpctl", "set-mute", "@DEFAULT_AUDIO_SOURCE@", "toggle", nullptr},
        };

        const pid_t PID = spawn(ARGV[a], nullptr);
        if (PID < 0)
            return;

        // a runaway key repeat keeps stepping, only the readback is skipped;
        // the orphan list re-reaps them (never waitpid(-1): the Executor's
        // children are not ours to steal)
        if (chains.size() >= 16) {
            orphans.push_back(PID);
            return;
        }

        auto c    = makeUnique<SChain>();
        c->mic    = a == MIC_MUTE;
        c->setPid = PID;
        c->pidFd  = (int)syscall(SYS_pidfd_open, PID, 0);
        if (c->pidFd < 0) {
            // no pidfd (EMFILE, ancient kernel): never block the loop on a
            // reap — the orphan list re-reaps it; the card is skipped
            orphans.push_back(PID);
            return;
        }
        fcntl(c->pidFd, F_SETFD, FD_CLOEXEC); // SYS_pidfd_open takes no CLOEXEC flag; keep it out of concurrent spawns
        c->pidSrc = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, c->pidFd, WL_EVENT_READABLE, onSetDone, c.get());
        chains.push_back(std::move(c));
    }

    // ---- the Lua face (hl.plugin.hyprosd.*) ----

    // Actions queue and drain from the event loop, never inside the bind's
    // input emission; a queue rather than one deferred slot so a key-repeat
    // burst never coalesces two steps into one.
    static std::vector<uint8_t>      queued;
    static UP<SEventLoopDoLaterLock> pendingDrain;

    static void                      enqueue(eAction a) {
        if (!g_pEventLoopManager)
            return;
        queued.push_back(a);
        pendingDrain = g_pEventLoopManager->doLaterLock([]() {
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

    busInit(sessionBus, false);
    busInit(systemBus, true);
    findBacklight();

    HyprlandAPI::addLuaFunction(PHANDLE, "hyprosd", "volume_up", luaVolumeUp);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprosd", "volume_down", luaVolumeDown);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprosd", "mute", luaMute);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprosd", "mic_mute", luaMicMute);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprosd", "brightness_up", luaBrightnessUp);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprosd", "brightness_down", luaBrightnessDown);

    return {"hyprosd", "the awesome volume/brightness OSD", "hitori", "1.0.4"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    pendingDrain.reset(); // a pending doLater across unload calls into dlclosed code
    queued.clear();
    sessionBus.pendingDrop.reset();
    systemBus.pendingDrop.reset();
    while (!chains.empty())
        chainDone(chains.back().get()); // sources out, fds closed, children reaped as far as WNOHANG goes
    reapOrphans();
    orphans.clear();
    busTeardown(sessionBus); // fd sources out BEFORE the connections die
    busTeardown(systemBus);
    if (g_pEventLoopManager) {
        if (sessionBus.poll)
            g_pEventLoopManager->removeTimer(sessionBus.poll);
        if (systemBus.poll)
            g_pEventLoopManager->removeTimer(systemBus.poll);
    }
    sessionBus.poll.reset();
    systemBus.poll.reset();
}
