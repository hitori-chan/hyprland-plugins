// hyprnotify — Android's notification system on the freedesktop spec, drawn
// natively by the compositor.
//
// The compositor itself is the org.freedesktop.Notifications daemon: no
// external process, no layer surface. Two surfaces share one card model:
//
// - POPUPS (banners): glass cards top-right on the focused monitor. The
//   anatomy is Android's conversation container — ONE icon column, the
//   avatar leading (the content image; for a chat, the sender's face — a
//   rolled fallback face when a card is iconless) with the app identity
//   badged on its bottom-right corner; a wide content image goes hero,
//   full-width, instead. Then the "App • age" header, title, body,
//   progress, and the card's actions as tinted text buttons. Hovering
//   reveals the ✕ and HOLDS the timeout.
// - THE SHADE (F12 / the bar's bell / `hyprctl hyprnotify center`): ONE list
//   of live cards, Android's notification shade. No lifecycle sections and no
//   history — a dismissed card is gone. Ranking is Android's without the
//   dividers (critical, marked conversations, the rest of them, normal,
//   silent; newest first inside each), an app's cards bundle at four or more
//   (GroupHelper's AUTOGROUP_AT_COUNT) and conversations never bundle.
//   Rows open by DEFAULT: an expansion budget walks the page and opens each
//   row while the panel has room, so the shade is readable with no clicks at
//   all — so the left click is spent ACTING, not revealing: a row IS its
//   banner, and clicking its body fires the card's primary exactly as the
//   popup does. The chevron is the only fold target. Right dismisses,
//   middle sweeps; the footer is ⊖ DND · a global "Clear all". While it is
//   open it owns the nav keys (↑↓ select, space folds, enter fires the
//   primary, delete dismisses, m/s/p manage, esc closes) and nothing else.
//   Hovering the bar's bell PEEKS it open unpinned, so a glance costs no
//   click. A row's ⋮ turns it into a manage panel: snooze durations, mute
//   durations, mark the sender — every verb named, the rules persist across
//   relogs, and the footer's ⊘ N never lets a standing one hide.
//
// Model rules: the conversation merge joins one chat's messages into one
// growing card (~8KB cap, oldest lines drop) — fd.o's im.*/call.* categories,
// or x-canonical-append; the OSD id band 9990-9999 replaces in place and
// never appends or groups; critical bypasses DND; ignore_dbusclose gates only
// the bus CloseNotification path; transient/progress cards vanish entirely on
// expiry. `hyprctl hyprnotify state` reports center/live/dnd; the
// org.hitori.hyprnotify bus interface carries Toggle/Peek/State for the
// bar's bell (the sanctioned cross-plugin channel — the bus, never symbols).
//
// Everything follows the texture rule (warm/draw split, see ui.hpp), spends
// zero wakeups idle, and defers every input-driven mutation off the
// emission. The code is split by concern — see hyprnotify.hpp's module map.

#include "common/icons.hpp"
#include "common/lifecycle.hpp"
#include "common/order.hpp"
#include "common/process.hpp"
#include "common/theme.hpp"

#include "hyprnotify.hpp"

#include <cerrno>
#include <spawn.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace NHyprnotify {
    HANDLE PHANDLE = nullptr;
    SNotifyConfig cfg;

    // ---- detached child spawning (hyperlink open, notification sound) ----
    //
    // A child per pidfd, reaped by an event-loop source when it dies: no
    // blocking, no zombies, and EXIT pulls the sources before the loop goes.
    struct SChild {
        pid_t            pid = -1;
        int              fd  = -1;
        wl_event_source* src = nullptr;
    };
    static std::vector<UP<SChild>> children;
    static std::vector<pid_t>      spawnOrphans; // couldn't-watch children; polled only while non-empty
    static SP<CEventLoopTimer>     orphanTick;

    static void armOrphanTick() {
        if (!orphanTick || !g_pEventLoopManager)
            return;
        orphanTick->updateTimeout(spawnOrphans.empty() ? std::nullopt : std::optional{std::chrono::milliseconds(100)});
    }

    static void rememberOrphan(pid_t pid) {
        if (pid <= 0 || std::ranges::find(spawnOrphans, pid) != spawnOrphans.end())
            return;
        spawnOrphans.push_back(pid);
        armOrphanTick();
    }

    static void reapOrphans(bool block = false);

    static int                     onChildExit(int, uint32_t, void* data) {
        auto* c = (SChild*)data;
        NHyprCommon::waitPid(c->pid, true);
        wl_event_source_remove(c->src);
        close(c->fd);
        std::erase_if(children, [&](const auto& U) { return U.get() == c; });
        return 0;
    }

    void spawnDetached(std::vector<const char*> argv) {
        if (argv.empty() || !argv[0])
            return;
        reapOrphans();
        if (argv.back())
            argv.push_back(nullptr); // execv needs the null terminator

        pid_t pid = -1;
        if (posix_spawnp(&pid, argv[0], nullptr, nullptr, const_cast<char* const*>(argv.data()), environ) != 0)
            return;

        const int FD = (int)syscall(SYS_pidfd_open, pid, 0);
        if (FD < 0 || !g_pCompositor) {
            // no pidfd/loop to watch it: close the descriptor if one was
            // opened, then keep ownership until a non-blocking reap wins
            if (FD >= 0)
                close(FD);
            if (NHyprCommon::waitPid(pid, false) == 0)
                rememberOrphan(pid);
            return;
        }
        auto c = makeUnique<SChild>();
        c->pid = pid;
        c->fd  = FD;
        c->src = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, FD, WL_EVENT_READABLE, onChildExit, c.get());
        if (!c->src) {
            close(FD);
            if (NHyprCommon::waitPid(pid, false) == 0)
                rememberOrphan(pid);
            return;
        }
        children.push_back(std::move(c));
    }

    static void reapOrphans(bool block) {
        std::erase_if(spawnOrphans, [block](pid_t p) {
            const pid_t RESULT = NHyprCommon::waitPid(p, block);
            return RESULT > 0 || (RESULT < 0 && errno == ECHILD);
        });
        if (!block)
            armOrphanTick();
    }

    static void reapChildren(bool block = false) {
        for (auto& c : children) {
            if (c->src)
                wl_event_source_remove(c->src);
            if (c->fd >= 0)
                close(c->fd);
            if (c->pid > 0) {
                const pid_t RESULT = NHyprCommon::waitPid(c->pid, block);
                if (!block && RESULT == 0)
                    rememberOrphan(c->pid);
            }
        }
        children.clear();
        reapOrphans(block);
    }
}

using namespace NHyprnotify;

static NHyprCommon::CLifecycle g_lifecycle;
static SP<SHyprCtlCommand>     ctlCmd;

// hl.plugin.hyprnotify.suspend() — the DND chord. Deferred out of the bind's
// input emission (the resume reflows and repaints). Presses ACCUMULATE:
// overwriting the lock cancels the unfired toggle, and two presses in one
// dispatch would net zero instead of two toggles.
static NHyprCommon::CHop pendingSuspend;
static int               suspendPresses = 0;
static int               luaSuspend(lua_State*) {
    if (!g_pEventLoopManager)
        return 0; // presses must not accumulate with no drain to run them
    if (++suspendPresses > 1)
        return 0; // a drain is already queued
    pendingSuspend.arm([]() {
        if (std::exchange(suspendPresses, 0) & 1)
            NHyprnotify::Model::toggleSuspend();
    });
    return 0;
}

// The shade toggle: F12's user bind (hl.plugin.hyprnotify.center()), the
// bar's bell over the bus, and `hyprctl hyprnotify center` all funnel here —
// deferred and accumulating like suspend.
static int               centerPresses = 0;
static NHyprCommon::CHop pendingCenter;

static NHyprCommon::CHop pendingPeek;
static int               peekWant = -1; // the latest bell hover state, -1 = nothing pending

namespace NHyprnotify {
    void queueCenterToggle() {
        if (!g_pEventLoopManager)
            return;
        if (++centerPresses > 1)
            return;
        pendingCenter.arm([]() {
            if (!(std::exchange(centerPresses, 0) & 1))
                return;
            // a click on a shade the pointer PEEKED open keeps it, rather than
            // closing what the user has not chosen to open yet
            if (centerPeeking())
                centerPin();
            else
                setCenter(!centerVisible());
        });
    }

    // the bell's hover; only the newest state matters, so re-arming overwrites
    void queueCenterPeek(bool onBell) {
        peekWant = onBell ? 1 : 0;
        pendingPeek.arm([]() {
            if (const int W = std::exchange(peekWant, -1); W >= 0)
                centerPeek(W != 0);
        });
    }
}

static int luaCenter(lua_State*) {
    queueCenterToggle();
    return 0;
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprnotify] Version mismatch: rebuild the plugin against the running Hyprland", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hyprnotify] version mismatch");
    }

    // a notification-card click must never reach the window beneath it
    NHyprCommon::mustLoadBefore(PHANDLE, "hyprnotify", {"hyprmax", "hyprclick"});

    // Defaults are the glass·ink tokens (common/theme.hpp); theme.lua
    // overrides them through the same values as always.
    namespace Th = NHyprCommon::Theme;
    cfg.font            = makeShared<Config::Values::CStringValue>("plugin:hyprnotify:font", "font family", Th::FONT);
    cfg.fontSize        = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:font_size", "body text size in logical px (the type roles derive from it)", 12);
    cfg.width           = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:width", "popup card width in logical px", 348);
    cfg.maxHeight       = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:max_height", "popup card height cap in logical px", 300);
    cfg.maxIcon         = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:max_icon", "popup icon column in logical px", 44);
    cfg.margin          = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:margin", "inter-card gap in logical px", 6);
    cfg.offsetY         = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:offset_y", "popups' and the center's distance from the monitor top", 34);
    cfg.timeoutLow      = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:timeout_low", "ephemeral timeout in ms (low urgency, transient, progress cards)", 4000);
    cfg.timeoutNormal   = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:timeout_normal", "normal-urgency banner timeout in ms, then it retreats to the center; 0 = sticky (critical always sticks)", 5000);
    cfg.quietFullscreen = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:quiet_fullscreen", "hold banners back while a fullscreen window owns the monitor; the card still lands in the shade", 1);
    cfg.snoozeSeconds   = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:snooze_seconds", "how long a snoozed card stays out of sight before it alerts again", 900);
    cfg.rounding        = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:rounding", "card radius in logical px (panel +6 and rows -2 derive)", Th::RAD_CARD);
    cfg.roundingPower   = makeShared<Config::Values::CFloatValue>("plugin:hyprnotify:rounding_power", "corner superellipse exponent", (float)Th::ROUNDING_POWER);
    cfg.coalescePopups  = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:coalesce_popups", "1 = at most one live popup per app; same-app extras land silent in the center (0 = a banner per message)", 1);
    cfg.maxNotifs       = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:max_notifs", "model cap; overflow evicts the oldest non-critical card", 50);
    cfg.ignoreDbusClose = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:ignore_dbusclose", "ignore app-initiated CloseNotification (dunst's knob)", 0);
    cfg.colBg           = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_bg", "glass fill (alpha is the glass)", Th::GLASS);
    cfg.colFg           = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_fg", "body text", Th::INK);
    cfg.colTitle        = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_title", "card titles", Th::TITLE);
    cfg.colKicker       = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_kicker", "header/age/secondary text", Th::SUB);
    cfg.colFrame        = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_frame", "hairlines", Th::LINE);
    cfg.colUrgent       = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_urgent", "critical ring/progress/urgent fills", Th::URGENT);
    cfg.colHighlight    = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_highlight", "the accent: progress, actions, selections", Th::ACCENT);
    cfg.colLink         = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_link", "body hyperlinks", Th::LINK);
    cfg.soundCommand    = makeShared<Config::Values::CStringValue>("plugin:hyprnotify:sound_command", "libcanberra player for sound hints; empty disables", "canberra-gtk-play");
    cfg.fallbackIconDir = makeShared<Config::Values::CStringValue>("plugin:hyprnotify:fallback_icon_dir", "iconless cards draw a random identity face from this directory", "");

    for (const auto& V : {cfg.fontSize, cfg.width, cfg.maxHeight, cfg.maxIcon, cfg.margin, cfg.offsetY, cfg.timeoutLow, cfg.timeoutNormal, cfg.coalescePopups, cfg.quietFullscreen, cfg.snoozeSeconds, cfg.rounding,
                          cfg.maxNotifs, cfg.ignoreDbusClose})
        HyprlandAPI::addConfigValueV2(PHANDLE, V);
    HyprlandAPI::addConfigValueV2(PHANDLE, cfg.roundingPower);
    for (const auto& V : {cfg.font, cfg.fallbackIconDir, cfg.soundCommand})
        HyprlandAPI::addConfigValueV2(PHANDLE, V);
    for (const auto& V : {cfg.colBg, cfg.colFg, cfg.colTitle, cfg.colKicker, cfg.colFrame, cfg.colUrgent, cfg.colHighlight, cfg.colLink})
        HyprlandAPI::addConfigValueV2(PHANDLE, V);

    Policy::init(); // the user's rules load before the first arrival is judged
    Model::init();  // and the expiry timer stands before anything can arrive
    Bus::init();
    orphanTick = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { reapOrphans(); }, nullptr);
    g_pEventLoopManager->addTimer(orphanTick);
    renderInit();
    centerInit();

    // the lockscreen bell reads count; the stress gate reads state
    ctlCmd =
        HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{.name = "hyprnotify", .exact = false, .fn = [](eHyprCtlOutputFormat, std::string request) -> std::string {
                                                                         if (request.ends_with("count"))
                                                                             return std::to_string(notifs.size());
                                                                         if (request.ends_with("center")) {
                                                                             queueCenterToggle();
                                                                             return "ok";
                                                                         }
                                                                         if (request.ends_with("state"))
                                                                             return Model::stateString();
                                                                         if (request.ends_with("badge"))
                                                                             return Model::badgeString();
                                                                         if (request.ends_with("policy"))
                                                                             return Policy::stateString();
                                                                         if (request.ends_with("snoozed"))
                                                                             return std::to_string(Model::snoozedCount());
                                                                         if (request.ends_with("clear")) { // the scripted reset
                                                                             static NHyprCommon::CHop pendingClear;
                                                                             pendingClear.arm([]() { Model::dismissAllLive(); });
                                                                             return "ok";
                                                                         }
                                                                         return "unknown request";
                                                                     }});
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprnotify", "suspend", luaSuspend);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprnotify", "center", luaCenter); // F12 is the reserved bind

    g_lifecycle.init();
    g_lifecycle.listen(Event::bus()->m_events.render.stage, [](eRenderStage stage) { onRenderStage(stage); });
    g_lifecycle.listen(Event::bus()->m_events.render.preChecks, [](PHLMONITOR mon) { onRenderPreChecks(mon); });
    g_lifecycle.listen(Event::bus()->m_events.input.mouse.button, [](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { onMouseButton(e, info); });
    g_lifecycle.listen(Event::bus()->m_events.input.mouse.move, [](Vector2D pos, Event::SCallbackInfo& info) { onMouseMove(pos, info); });
    g_lifecycle.listen(Event::bus()->m_events.input.mouse.axis, [](IPointer::SAxisEvent e, Event::SCallbackInfo& info) { onMouseAxis(e, info); });
    g_lifecycle.listen(Event::bus()->m_events.input.keyboard.key, [](IKeyboard::SKeyEvent e, Event::SCallbackInfo& info) { onKey(e, info); });

    // Everything lives on the FOCUSED monitor: monitor.focused is the one
    // desktop event layout depends on (rawMonitorFocus early-outs same-monitor
    // flips, so sloppy focus costs nothing). It fires BEFORE m_focusMonitor is
    // assigned — the warm must stay deferred, which notifChanged guarantees.
    auto& EV = Event::bus()->m_events;
    g_lifecycle.listen(EV.monitor.focused, [](PHLMONITOR) {
        if (!notifs.empty() || centerVisible())
            notifChanged();
    });
    g_lifecycle.listen(EV.monitor.layoutChanged, []() {
        if (!notifs.empty() || centerVisible())
            notifChanged();
    });
    g_lifecycle.listen(EV.config.reloaded, []() {
        NHyprCommon::resetIconNameCache();
        resetFallbackCache();
        if (!notifs.empty() || centerVisible())
            notifChanged(); // a live theme reload re-keys the texture caches
    });

    return {"hyprnotify", "Android's notification shade for Hyprland", "hitori", VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_lifecycle.resetAll(); // listeners first, then every hop
    suspendPresses = 0;
    centerPresses  = 0;
    peekWant       = -1;
    if (ctlCmd)
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, ctlCmd);
    ctlCmd.reset();
    Bus::exit();    // the connection first: nothing may arrive mid-teardown
    Model::exit();  // then the cards, and their textures with them
    Policy::exit(); // the rules outlive all of it, so they flush last
    inputExit();
    replyExit();
    reapChildren(true); // plugin exit owns the helpers; reap before its code can unload
    if (orphanTick && g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(orphanTick);
    orphanTick.reset();
    centerExit();
    renderExit();
}
