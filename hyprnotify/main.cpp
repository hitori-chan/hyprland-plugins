// hyprnotify — Android's notification system on the freedesktop spec, drawn
// natively by the compositor.
//
// The compositor itself is the org.freedesktop.Notifications daemon: no
// external process, no layer surface. Two surfaces share one card model:
//
// - POPUPS (banners): glass cards top-right on the focused monitor. The
//   anatomy is Android's — an icon column (content avatar wearing the
//   identity's 13px corner badge; identity alone otherwise; nothing =
//   text-only), an "App • age" header, title, body, progress, and the
//   card's actions as tinted text buttons. Hovering reveals the ✕.
// - THE CENTER (F12 / the bar's bell / `hyprctl hyprnotify center`): two
//   views, like Android. The SHADE holds RESIDENT live cards — a banner
//   timeout emits reason 1 EXPIRED once and hides only the popup; the card
//   stays as a shade row until dismissed or acted on. HISTORY (behind ⏱)
//   holds dismissed/app-closed entries only. Every row is the same
//   two-state card (the chevron folds; live arrives expanded and auto-folds
//   with its banner; expanded rows show the notification's ORIGINAL actions
//   — history included, best-effort with the original id, entry consumed;
//   recall = left-click, delete = right-click). ≥2 same-app rows fold into
//   the three-state group model in both views. The bottom bar is
//   ⏱ · a context-sensitive Clear button · ⊖ DND.
//
// Model rules: x-canonical-append joins same app+summary into one growing
// conversation card (~8KB cap, oldest lines drop); the OSD id band
// 9990-9999 replaces in place and never appends, groups, or retires;
// critical bypasses DND; ignore_dbusclose gates only the bus
// CloseNotification path; transient/progress cards vanish entirely on
// expiry. `hyprctl hyprnotify state` reports center/live/hist/dnd; the
// org.hitori.hyprnotify bus interface carries Toggle/State for the bar's
// bell (the sanctioned cross-plugin channel — the bus, never symbols).
//
// Everything follows the texture rule (warm/draw split, see ui.hpp), spends
// zero wakeups idle, and defers every input-driven mutation off the
// emission. The code is split by concern — see hyprnotify.hpp's module map.

#include "common/icons.hpp"
#include "common/lifecycle.hpp"
#include "common/order.hpp"
#include "common/theme.hpp"

#include "hyprnotify.hpp"

#include <spawn.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace NHyprnotify;

HANDLE PHANDLE = nullptr;

namespace NHyprnotify {
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
    static std::vector<pid_t>      spawnOrphans; // couldn't-watch children (no pidfd/loop); WNOHANG-swept on the next spawn

    static int                     onChildExit(int, uint32_t, void* data) {
        auto* c = (SChild*)data;
        waitpid(c->pid, nullptr, WNOHANG);
        wl_event_source_remove(c->src);
        close(c->fd);
        std::erase_if(children, [&](const auto& U) { return U.get() == c; });
        return 0;
    }

    void spawnDetached(std::vector<const char*> argv) {
        if (argv.empty() || !argv[0])
            return;
        std::erase_if(spawnOrphans, [](pid_t p) { return waitpid(p, nullptr, WNOHANG) != 0; });
        if (argv.back())
            argv.push_back(nullptr); // execv needs the null terminator

        pid_t pid = -1;
        if (posix_spawnp(&pid, argv[0], nullptr, nullptr, const_cast<char* const*>(argv.data()), environ) != 0)
            return;

        const int FD = (int)syscall(SYS_pidfd_open, pid, 0);
        if (FD < 0 || !g_pCompositor) {
            // no pidfd/loop to watch it: reap now, or hand a not-yet-exited
            // child to the sweep above rather than leak a zombie
            if (waitpid(pid, nullptr, WNOHANG) == 0)
                spawnOrphans.push_back(pid);
            return;
        }
        auto c = makeUnique<SChild>();
        c->pid = pid;
        c->fd  = FD;
        c->src = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, FD, WL_EVENT_READABLE, onChildExit, c.get());
        children.push_back(std::move(c));
    }

    static void reapChildren() {
        for (auto& c : children) {
            if (c->src)
                wl_event_source_remove(c->src);
            if (c->fd >= 0)
                close(c->fd);
            if (c->pid > 0)
                waitpid(c->pid, nullptr, WNOHANG);
        }
        children.clear();
        for (pid_t p : spawnOrphans)
            waitpid(p, nullptr, WNOHANG);
        spawnOrphans.clear();
    }
}

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
            NHyprnotify::Bus::toggleSuspend();
    });
    return 0;
}

// hl.plugin.hyprnotify.recall() / `hyprctl hyprnotify recall` — history-pop.
static int               recallPresses = 0;
static NHyprCommon::CHop pendingRecall;
static void              queueRecall() {
    if (!g_pEventLoopManager)
        return; // as luaSuspend
    if (++recallPresses > 1)
        return;
    pendingRecall.arm([]() {
        for (int i = std::exchange(recallPresses, 0); i > 0; i--)
            NHyprnotify::Bus::recall();
    });
}
static int luaRecall(lua_State*) {
    queueRecall();
    return 0;
}

// The center toggle: F12's user bind (hl.plugin.hyprnotify.center()), the
// bar's bell over the bus, and `hyprctl hyprnotify center` all funnel here —
// deferred and accumulating like suspend.
static int               centerPresses = 0;
static NHyprCommon::CHop pendingCenter;

namespace NHyprnotify {
    void queueCenterToggle() {
        if (!g_pEventLoopManager)
            return;
        if (++centerPresses > 1)
            return;
        pendingCenter.arm([]() {
            if (std::exchange(centerPresses, 0) & 1)
                setCenter(!centerVisible());
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
    cfg.timeoutLow      = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:timeout_low", "low-urgency banner timeout in ms", 4000);
    cfg.timeoutNormal   = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:timeout_normal", "normal-urgency banner timeout in ms (critical never expires)", 8000);
    cfg.rounding        = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:rounding", "card radius in logical px (panel +6 and rows -2 derive)", Th::RAD_CARD);
    cfg.roundingPower   = makeShared<Config::Values::CFloatValue>("plugin:hyprnotify:rounding_power", "corner superellipse exponent", (float)Th::ROUNDING_POWER);
    cfg.maxNotifs       = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:max_notifs", "model cap; overflow evicts the oldest non-critical card", 50);
    cfg.maxHistory      = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:max_history", "history cap; 0 disables history", 20);
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

    for (const auto& V : {cfg.fontSize, cfg.width, cfg.maxHeight, cfg.maxIcon, cfg.margin, cfg.offsetY, cfg.timeoutLow, cfg.timeoutNormal, cfg.rounding, cfg.maxNotifs,
                          cfg.maxHistory, cfg.ignoreDbusClose})
        HyprlandAPI::addConfigValueV2(PHANDLE, V);
    HyprlandAPI::addConfigValueV2(PHANDLE, cfg.roundingPower);
    for (const auto& V : {cfg.font, cfg.soundCommand})
        HyprlandAPI::addConfigValueV2(PHANDLE, V);
    for (const auto& V : {cfg.colBg, cfg.colFg, cfg.colTitle, cfg.colKicker, cfg.colFrame, cfg.colUrgent, cfg.colHighlight, cfg.colLink})
        HyprlandAPI::addConfigValueV2(PHANDLE, V);

    Bus::init();
    renderInit();

    // the lockscreen bell reads count; the stress gate reads state
    ctlCmd =
        HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{.name = "hyprnotify", .exact = false, .fn = [](eHyprCtlOutputFormat, std::string request) -> std::string {
                                                                         if (request.ends_with("count"))
                                                                             return std::to_string(notifs.size());
                                                                         if (request.ends_with("history"))
                                                                             return std::to_string(Bus::historySize());
                                                                         if (request.ends_with("recall")) {
                                                                             queueRecall();
                                                                             return "ok";
                                                                         }
                                                                         if (request.ends_with("center")) {
                                                                             queueCenterToggle();
                                                                             return "ok";
                                                                         }
                                                                         if (request.ends_with("state"))
                                                                             return Bus::stateString();
                                                                         if (request.ends_with("clear")) { // dismiss + wipe: the scripted reset
                                                                             static NHyprCommon::CHop pendingClear;
                                                                             pendingClear.arm([]() {
                                                                                 Bus::dismissAllLive();
                                                                                 Bus::clearHistory();
                                                                             });
                                                                             return "ok";
                                                                         }
                                                                         return "unknown request";
                                                                     }});
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprnotify", "suspend", luaSuspend);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprnotify", "recall", luaRecall);
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
        if (!notifs.empty() || centerVisible())
            notifChanged(); // a live theme reload re-keys the texture caches
    });

    return {"hyprnotify", "Android's notification shade for Hyprland", "hitori", VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_lifecycle.resetAll(); // listeners first, then every hop
    suspendPresses = 0;
    recallPresses  = 0;
    centerPresses  = 0;
    if (ctlCmd)
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, ctlCmd);
    ctlCmd.reset();
    Bus::exit(); // closes the model; its textures die with it
    inputExit();
    reapChildren();
    renderExit();
}
