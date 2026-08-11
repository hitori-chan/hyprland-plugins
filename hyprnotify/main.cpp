// hyprnotify — Android's notification system on the freedesktop spec, drawn
// natively by the compositor.
//
// The compositor itself is the org.freedesktop.Notifications daemon: no
// external process, no layer surface. Two surfaces share one card model:
//
// - POPUPS (banners): glass cards top-right on the focused monitor. The
//   anatomy is Android's conversation container — ONE bounded identity column:
//   a chat avatar with the app badge, or one unbadged application icon for
//   every other notification. Then the "App • age"
//   header, title, body,
//   progress, and the card's actions as tinted text buttons. Hovering a card
//   HOLDS its timeout while the pointer remains over it.
// - THE SHADE (the bar's bell / `hyprctl hyprnotify center`): ONE list
//   of live cards, Android's notification shade. No lifecycle sections and no
//   history — a dismissed card is gone. Ranking is Android's without the
//   dividers (critical, marked conversations, the rest of them, normal,
//   silent; newest first inside each). Declared groups win over automatic
//   application/section groups, which form at four; conversations remain
//   distinct children. The top item receives deterministic system expansion,
//   and the count pill alone toggles user expansion. A row's body always fires
//   its primary exactly as the popup does. Right dismisses; middle remains a
//   native desktop gesture in the shade and parks banners from a popup. The
//   footer is compact DND and a full-track global "Clear all".
//   The shade leaves keyboard input to the focused client, except for a Reply
//   field explicitly armed by pointer click. A row's long-press turns it into
//   a manage panel that stages Pixel's Priority / Default / Silent choice;
//   Done commits it and eligible singleton hold panels own Snooze. Rules
//   persist across relogs, and the footer's muted-count control never lets a
//   standing rule hide.
//
// Model rules: only an explicit (application, conversation ID) pair merges
// structured messages into one bounded card; visible text and legacy
// im.*/call.* categories never identify a chat. The OSD id band 9990-9999
// replaces in place and never groups; critical bypasses DND;
// ignore_dbusclose gates only the bus CloseNotification path;
// transient/progress cards vanish entirely on expiry. `hyprctl hyprnotify
// state` reports center/live/dnd; the
// org.hitori.hyprnotify bus interface carries Toggle/State for the
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
    // The target compositor installs SA_NOCLDWAIT, so exit can release an
    // already-execed helper after removing its callback instead of waiting
    // indefinitely for a broken xdg-open or sound command.
    struct SChild {
        pid_t            pid = -1;
        int              fd  = -1;
        wl_event_source* src = nullptr;
    };
    static std::vector<UP<SChild>> children;
    static std::vector<pid_t>      spawnOrphans; // couldn't-watch children; polled only while non-empty
    static SP<CEventLoopTimer>     orphanTick;
    constexpr size_t               MAX_CHILDREN = 64;

    static size_t trackedChildren() {
        return children.size() + spawnOrphans.size();
    }

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

    static void reapOrphans();

    static int                     onChildExit(int, uint32_t, void* data) {
        auto* c = (SChild*)data;
        NHyprCommon::reapPid(c->pid); // pidfd readiness is authoritative
        wl_event_source_remove(c->src);
        close(c->fd);
        std::erase_if(children, [&](const auto& U) { return U.get() == c; });
        return 0;
    }

    void spawnDetached(std::vector<const char*> argv) {
        if (argv.empty() || !argv[0])
            return;
        reapOrphans();
        if (trackedChildren() >= MAX_CHILDREN)
            return; // bounded admission: do not create an unowned child
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
            if (NHyprCommon::reapPid(pid) == 0)
                rememberOrphan(pid);
            return;
        }
        auto c = makeUnique<SChild>();
        c->pid = pid;
        c->fd  = FD;
        c->src = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, FD, WL_EVENT_READABLE, onChildExit, c.get());
        if (!c->src) {
            close(FD);
            if (NHyprCommon::reapPid(pid) == 0)
                rememberOrphan(pid);
            return;
        }
        children.push_back(std::move(c));
    }

    static void reapOrphans() {
        std::erase_if(spawnOrphans, [](pid_t p) {
            const pid_t RESULT = NHyprCommon::reapPid(p);
            return RESULT > 0 || (RESULT < 0 && errno == ECHILD);
        });
        armOrphanTick();
    }

    static void releaseChildren() {
        for (auto& c : children) {
            if (c->src)
                wl_event_source_remove(c->src);
            if (c->fd >= 0)
                close(c->fd);
            if (c->pid > 0)
                NHyprCommon::reapPid(c->pid);
        }
        children.clear();
        reapOrphans();
        // No callback can reference these helpers now. SA_NOCLDWAIT lets the
        // compositor reap a later exit without retaining plugin code.
        spawnOrphans.clear();
        armOrphanTick();
    }
}

using namespace NHyprnotify;

static NHyprCommon::CLifecycle    g_lifecycle;
static SP<IPC::Socket1::SCommand> ctlCmd;

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

// The shade toggle: the bar's bell over the bus and
// `hyprctl hyprnotify center` both funnel here, deferred and accumulating
// like suspend.
static int               centerPresses = 0;
static NHyprCommon::CHop pendingCenter;

namespace NHyprnotify {
    void queueCenterToggle() {
        if (!g_pEventLoopManager)
            return;
        if (++centerPresses > 1)
            return;
        pendingCenter.arm([]() {
            if (!(std::exchange(centerPresses, 0) & 1))
                return;
            setCenter(!centerVisible());
        });
    }
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

    // Pixel/AOSP semantic defaults from common/theme.hpp. Config keeps every
    // role independently themeable without forking render paths.
    namespace Th = NHyprCommon::Theme;
    cfg.font            = makeShared<Config::Values::CStringValue>("plugin:hyprnotify:font", "font family", Th::FONT);
    cfg.fontSize        = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:font_size", "body text size in logical px (the type roles derive from it)", 12);
    cfg.width           = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:width", "popup card width in logical px", 380);
    cfg.maxHeight       = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:max_height", "popup card height cap in logical px", 300);
    cfg.maxIcon         = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:max_icon", "popup identity cell in logical px", 40);
    cfg.margin          = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:margin", "inter-card gap in logical px", 8);
    cfg.offsetY         = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:offset_y", "popups' and the center's distance from the monitor top", 34);
    cfg.timeoutLow      = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:timeout_low", "ephemeral timeout in ms (low urgency, transient, progress cards)", 4000);
    cfg.timeoutNormal   = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:timeout_normal", "normal-urgency banner timeout in ms, then it retreats to the center; 0 = sticky (critical always sticks)", 5000);
    cfg.quietFullscreen = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:quiet_fullscreen", "opt-in hold while a fullscreen window owns the monitor; the card still lands in the shade", 0);
    cfg.snoozeSeconds   = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:snooze_seconds", "how long a snoozed card stays out of sight before it alerts again", 900);
    cfg.rounding        = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:rounding", "outer notification radius in logical px", DEFAULT_ROUNDING);
    cfg.roundingPower   = makeShared<Config::Values::CFloatValue>("plugin:hyprnotify:rounding_power", "corner superellipse exponent", DEFAULT_ROUNDING_POWER);
    cfg.coalescePopups  = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:coalesce_popups", "1 = at most one live popup per app; same-app extras land silent in the center (0 = a banner per message)", 1);
    cfg.maxNotifs       = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:max_notifs", "model cap; overflow evicts the oldest non-critical card", 50);
    cfg.ignoreDbusClose = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:ignore_dbusclose", "ignore app-initiated CloseNotification (dunst's knob)", 0);
    cfg.colBg           = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_bg", "shade/panel glass container", Th::PANEL);
    cfg.colFg           = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_fg", "on-surface body text", Th::ON_SURFACE);
    cfg.colTitle        = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_title", "strong on-surface titles", Th::ON_SURFACE_STRONG);
    cfg.colKicker       = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_kicker", "on-surface-variant metadata", Th::ON_SURFACE_VARIANT);
    cfg.colFrame        = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_frame", "outline/hairlines", Th::OUTLINE);
    cfg.colUrgent       = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_urgent", "error role for critical content", Th::ERROR);
    cfg.colHighlight    = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_highlight", "primary actions, progress, and focus", Th::PRIMARY);
    cfg.colLink         = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_link", "body hyperlinks", Th::PRIMARY);
    cfg.colSurface      = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_surface", "notification and row surface container", Th::SURFACE);
    cfg.colSurfaceHigh  = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_surface_high", "raised control surface container", Th::SURFACE_HIGH);
    cfg.colState        = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_state", "hover and selected state layer", Th::STATE);
    cfg.colOnHighlight  = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_on_highlight", "content on a primary fill", Th::ON_PRIMARY);
    cfg.colOnUrgent     = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_on_urgent", "content on an error fill", Th::ON_ERROR);
    cfg.soundCommand    = makeShared<Config::Values::CStringValue>("plugin:hyprnotify:sound_command", "libcanberra player for sound hints; empty disables", "canberra-gtk-play");

    for (const auto& V : {cfg.fontSize, cfg.width, cfg.maxHeight, cfg.maxIcon, cfg.margin, cfg.offsetY, cfg.timeoutLow, cfg.timeoutNormal, cfg.coalescePopups, cfg.quietFullscreen, cfg.snoozeSeconds, cfg.rounding,
                          cfg.maxNotifs, cfg.ignoreDbusClose})
        HyprlandAPI::addConfigValueV2(PHANDLE, V);
    HyprlandAPI::addConfigValueV2(PHANDLE, cfg.roundingPower);
    for (const auto& V : {cfg.font, cfg.soundCommand})
        HyprlandAPI::addConfigValueV2(PHANDLE, V);
    for (const auto& V : {cfg.colBg, cfg.colFg, cfg.colTitle, cfg.colKicker, cfg.colFrame, cfg.colUrgent, cfg.colHighlight, cfg.colLink, cfg.colSurface, cfg.colSurfaceHigh,
                          cfg.colState, cfg.colOnHighlight, cfg.colOnUrgent})
        HyprlandAPI::addConfigValueV2(PHANDLE, V);

    Policy::init(); // the user's rules load before the first arrival is judged
    Model::init();  // and the expiry timer stands before anything can arrive
    Bus::init();
    iconsInit();
    orphanTick = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { reapOrphans(); }, nullptr);
    g_pEventLoopManager->addTimer(orphanTick);
    renderInit();
    // the lockscreen bell reads count; the stress gate reads state
    ctlCmd =
        HyprlandAPI::registerHyprCtlCommand(PHANDLE, IPC::Socket1::SCommand{.name = "hyprnotify", .match = IPC::Socket1::COMMAND_MATCH_PREFIX, .handler = [](const IPC::Socket1::SRequest& request) {
            const auto& COMMAND = request.command;
            if (COMMAND.ends_with("count"))
                return IPC::Socket1::SResponse{std::to_string(notifs.size())};
            if (COMMAND.ends_with("center")) {
                queueCenterToggle();
                return IPC::Socket1::SResponse{"ok"};
            }
            if (COMMAND.ends_with("state"))
                return IPC::Socket1::SResponse{Model::stateString()};
            if (COMMAND.ends_with("badge"))
                return IPC::Socket1::SResponse{Model::badgeString()};
            if (COMMAND.ends_with("policy"))
                return IPC::Socket1::SResponse{Policy::stateString()};
            if (COMMAND.ends_with("snoozed"))
                return IPC::Socket1::SResponse{std::to_string(Model::snoozedCount())};
            if (COMMAND.ends_with("clear")) { // the scripted reset
                static NHyprCommon::CHop pendingClear;
                pendingClear.arm([]() { Model::dismissAllLive(); });
                return IPC::Socket1::SResponse{"ok"};
            }
            return IPC::Socket1::SResponse{"unknown request"};
        }});
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprnotify", "suspend", luaSuspend);

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
        resetDesktopIconCache();
        resetGeneratedAvatarCache();
        textCacheClear();
        if (!notifs.empty() || centerVisible())
            notifChanged(); // one coalesced warm covers every re-keyed cache
    });

    return {"hyprnotify", "Android's notification shade for Hyprland", "hitori", VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_lifecycle.resetAll(); // listeners first, then every hop
    suspendPresses = 0;
    centerPresses  = 0;
    if (ctlCmd)
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, ctlCmd);
    ctlCmd.reset();
    Bus::exit();    // the connection first: nothing may arrive mid-teardown
    iconsExit();    // no timer or retained worker resource may outlive us
    Model::exit();  // then the cards, and their textures with them
    Policy::exit(); // the rules outlive all of it, so they flush last
    inputExit();
    replyExit();
    releaseChildren(); // remove callbacks without waiting on external helpers
    if (orphanTick && g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(orphanTick);
    orphanTick.reset();
    centerExit();
    renderExit();
}
