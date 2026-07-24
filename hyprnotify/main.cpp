// hyprnotify — awesome's naughty as a native Hyprland plugin.
//
// The compositor itself is the org.freedesktop.Notifications daemon: no
// external process, no layer surface — notification cards are drawn by the
// renderer in the top-right of the focused monitor, styled like the old
// naughty boxes (flat dark cards, thin frame, big icons).
//
// - Notify/CloseNotification/GetCapabilities/GetServerInformation (spec
//   1.3) plus the NotificationClosed, ActionInvoked and ActivationToken
//   signals. Capabilities: actions, action-icons, body, body-markup,
//   body-hyperlinks, body-images, icon-static, persistence, sound. New
//   cards stack newest-on-top; a replace keeps its slot; the model caps at
//   max_notifs — overflow evicts the oldest non-critical card (critical
//   last) with NotificationClosed.
// - Markup: body and title render the whitelisted Pango subset (b, i, u,
//   span, br); other tags are dropped, a stray '<'/'&' survives literally,
//   and malformed markup falls back to plain text.
// - Images: image-data/image_data/icon_data pixmaps, image-path/app_icon/
//   desktop-entry as file paths (file:// too) OR freedesktop icon NAMES
//   resolved against the GTK theme (then hicolor, then pixmaps) — decoded
//   by hyprgraphics (PNG/JPEG/WEBP/BMP/AVIF/JXL + SVG), downscaled once.
//   Wide images render card-width as a hero; iconless cards draw a random
//   face from fallback_icon_dir; <img src> in the body renders as a
//   thumbnail row.
// - The "value" hint draws a progress bar (the volume/brightness OSD);
//   replaces_id updates a card in place, keeping its stack slot.
// - Urgency: low/normal use the timeout defaults below, critical never
//   expires and takes the urgent frame + progress color. An explicit
//   expire_timeout wins; 0 means sticky.
// - Actions: non-default actions render as clickable buttons (icons under
//   the action-icons hint); a click emits ActionInvoked and dismisses
//   unless the resident hint holds the card. <a href> body links open via
//   xdg-open. Clicks: left invokes the action / opens the link / fires the
//   default then dismisses (minting an ActivationToken so the sender can
//   raise itself); right dismisses; middle sweeps the visible stack. The
//   cards own the pointer over them — hover never leaks to the window
//   beneath, and the hovered frame/button warms as the click affordance.
// - Sound: sound-file/sound-name play through a libcanberra player
//   (sound_command, empty disables); suppress-sound mutes one arrival.
// - DND (naughty.suspend): hl.plugin.hyprnotify.suspend() toggles; cards
//   collect silently with timeouts held, resume renders the queue with
//   fresh timeouts, newest first. `hyprctl hyprnotify count` answers the
//   pending total (the lockscreen bell).
// - History (persistence): a closed card is retained (max_history) unless
//   it is transient or a progress/OSD card; hl.plugin.hyprnotify.recall()
//   (or `hyprctl hyprnotify recall`) pops the most recent back onto the
//   stack with a fresh timeout; `hyprctl hyprnotify history` counts them.
// - Session lock: cards never render above the lockscreen (the built-in
//   overlay does — these are the user's notifications, not the
//   compositor's), input listeners guard-and-reset first, and whatever
//   outlives the lock repaints at unlock.
//
// Colors/fonts/metrics arrive from theme.lua via hl.config plugin values —
// the C++ defaults just mirror the theme (the old dunstrc hand-mirrored it;
// this closes that loop).
//
// DBus is event-driven: sd-bus's fds live in the wayland event loop as
// removable sources (EXIT pulls them before the connection dies), so idle
// costs zero wakeups and a Notify lands the same loop iteration; a timer
// carries only sd-bus's rare internal timeouts and the deferred post-emit
// drain (sd-bus dispatch is not re-entrant — never drain synchronously
// from an emit). Textures follow the texture rule (see hyprnotify.hpp):
// built one frame ahead from the event loop, cached per card, and a
// replace only rebuilds what actually changed — a volume sweep re-rasters
// one body line per step, nothing else; image-data pixmaps are downscaled
// once, hashed for dedup, and freed after upload.
//
// The code is split by concern — see hyprnotify.hpp for the module map.

#include "common/lifecycle.hpp"
#include "common/order.hpp"

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
static SP<SHyprCtlCommand>     ctlCount;
static NHyprCommon::CHop       pendingSuspend;

// hl.plugin.hyprnotify.suspend() — the DND chord. Deferred out of the bind's
// input emission (the resume reflows and repaints the stack). Presses
// ACCUMULATE: overwriting the lock cancels the unfired toggle, and two
// presses in one dispatch would net zero instead of two toggles.
static int suspendPresses = 0;
static int luaSuspend(lua_State*) {
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
// Deferred like suspend (a keybind or hyprctl dispatch must not reflow the
// model inline); presses accumulate so a burst pops that many.
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

    // Defaults mirror theme.lua; the config overwrites them from the theme.
    cfg.font            = makeShared<Config::Values::CStringValue>("plugin:hyprnotify:font", "font family", "Fira Code");
    cfg.fontSize        = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:font_size", "text size in logical px (monitor scale applies at raster time)", 12);
    cfg.width           = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:width", "card width in logical px", 340);
    cfg.maxHeight       = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:max_height", "card height cap in logical px", 260);
    cfg.maxIcon         = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:max_icon", "image box cap in logical px (the old naughty icon_size)", 64);
    cfg.margin          = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:margin", "screen-edge and inter-card gap in logical px", 4);
    cfg.offsetY         = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:offset_y", "first card's distance from the monitor top (clear the bar)", 30);
    cfg.timeoutLow      = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:timeout_low", "low-urgency timeout in ms", 4000);
    cfg.timeoutNormal   = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:timeout_normal", "normal-urgency timeout in ms (critical never expires)", 8000);
    cfg.rounding        = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:rounding", "corner radius in logical px", 1);
    cfg.maxNotifs       = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:max_notifs", "model cap; overflow evicts the oldest non-critical card", 50);
    cfg.maxHistory      = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:max_history", "retained-for-recall cap; 0 disables history", 20);
    cfg.fallbackIconDir = makeShared<Config::Values::CStringValue>("plugin:hyprnotify:fallback_icon_dir", "iconless cards draw a random image from this directory", "");
    cfg.colBg           = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_bg", "card background", 0xff131313);
    cfg.colFg           = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_fg", "body text", 0xffaaaaaa);
    cfg.colTitle        = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_title", "summary line", 0xffdcdccc);
    cfg.colKicker       = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_kicker", "app-name kicker + hovered frame", 0xff8a97a8);
    cfg.colFrame        = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_frame", "card frame + progress trough", 0xff3f3f3f);
    cfg.colUrgent       = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_urgent", "critical frame/kicker + critical progress", 0xffc83f11);
    cfg.colHighlight    = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_highlight", "progress bar fill", 0xff32d6ff);
    cfg.colLink         = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_link", "body hyperlinks", 0xff5e9fef);
    cfg.soundCommand    = makeShared<Config::Values::CStringValue>("plugin:hyprnotify:sound_command", "libcanberra player for sound hints; empty disables", "canberra-gtk-play");

    for (const auto& V : {cfg.fontSize, cfg.width, cfg.maxHeight, cfg.maxIcon, cfg.margin, cfg.offsetY, cfg.timeoutLow, cfg.timeoutNormal, cfg.rounding, cfg.maxNotifs, cfg.maxHistory})
        HyprlandAPI::addConfigValueV2(PHANDLE, V);
    for (const auto& V : {cfg.font, cfg.fallbackIconDir, cfg.soundCommand})
        HyprlandAPI::addConfigValueV2(PHANDLE, V);
    for (const auto& V : {cfg.colBg, cfg.colFg, cfg.colTitle, cfg.colKicker, cfg.colFrame, cfg.colUrgent, cfg.colHighlight, cfg.colLink})
        HyprlandAPI::addConfigValueV2(PHANDLE, V);

    Bus::init();

    // the lockscreen bell reads this: every pending card, DND queue included
    ctlCount =
        HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{.name = "hyprnotify", .exact = false, .fn = [](eHyprCtlOutputFormat, std::string request) -> std::string {
                                                                         if (request.ends_with("count"))
                                                                             return std::to_string(notifs.size());
                                                                         if (request.ends_with("history"))
                                                                             return std::to_string(Bus::historySize());
                                                                         if (request.ends_with("recall")) {
                                                                             queueRecall();
                                                                             return "ok";
                                                                         }
                                                                         return "unknown request";
                                                                     }});
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprnotify", "suspend", luaSuspend);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprnotify", "recall", luaRecall);

    g_lifecycle.init();
    g_lifecycle.listen(Event::bus()->m_events.render.stage, [](eRenderStage stage) { onRenderStage(stage); });
    g_lifecycle.listen(Event::bus()->m_events.render.preChecks, [](PHLMONITOR mon) { onRenderPreChecks(mon); });
    g_lifecycle.listen(Event::bus()->m_events.input.mouse.button, [](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { onMouseButton(e, info); });
    g_lifecycle.listen(Event::bus()->m_events.input.mouse.move, [](Vector2D pos, Event::SCallbackInfo& info) { onMouseMove(pos, info); });

    // The cards live on the FOCUSED monitor: monitor.focused is the one
    // desktop event layout depends on (rawMonitorFocus early-outs same-monitor
    // flips, so sloppy focus costs nothing). It fires BEFORE m_focusMonitor is
    // assigned — the warm must stay deferred, which notifChanged guarantees.
    auto& EV = Event::bus()->m_events;
    g_lifecycle.listen(EV.monitor.focused, [](PHLMONITOR) {
        if (!notifs.empty())
            notifChanged();
    });
    g_lifecycle.listen(EV.monitor.layoutChanged, []() {
        if (!notifs.empty())
            notifChanged();
    });
    g_lifecycle.listen(EV.config.reloaded, []() {
        resetFallbackCache();
        resetIconThemeCache();
        if (!notifs.empty())
            notifChanged(); // a live theme reload re-keys the texture caches
    });

    return {"hyprnotify", "awesome's naughty: notifications drawn by the compositor", "hitori", VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_lifecycle.resetAll(); // listeners first, then every hop
    suspendPresses = 0;
    recallPresses  = 0;
    if (ctlCount)
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, ctlCount);
    ctlCount.reset();
    Bus::exit(); // closes the model; its textures die with it
    inputExit();
    reapChildren();
    renderExit();
}
