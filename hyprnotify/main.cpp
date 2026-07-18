// hyprnotify — awesome's naughty as a native Hyprland plugin.
//
// The compositor itself is the org.freedesktop.Notifications daemon: no
// external process, no layer surface — notification cards are drawn by the
// renderer in the top-right of the focused monitor, styled like the old
// naughty boxes (flat dark cards, thin frame, big icons).
//
// - Notify/CloseNotification/GetCapabilities/GetServerInformation (spec
//   1.3) plus the NotificationClosed, ActionInvoked and ActivationToken
//   signals; capabilities are "actions", "body", "icon-static". Markup is
//   stripped (never advertised); the app name renders as a small kicker,
//   the summary bold, the body word-wraps to the card and the tail line
//   ellipsizes at the height cap. New cards stack newest-on-top; a replace
//   keeps its slot. The model caps at max_notifs — overflow evicts the
//   oldest non-critical card (critical last) with NotificationClosed.
// - Images: image-data/image_data/icon_data pixmaps, image-path and
//   app_icon file paths (file:// too) — decoded by hyprgraphics
//   (PNG/JPEG/WEBP/BMP/AVIF/JXL + SVG), downscaled once at load. Bare theme
//   icon NAMES resolve to nothing, like naughty's path-only icons. Wide
//   images (a screenshot preview) render card-width as a hero; iconless
//   cards draw a random face from fallback_icon_dir, rolled per card and
//   held across in-place replaces (a volume sweep keeps its face).
// - The "value" hint draws a progress bar (the volume/brightness OSD);
//   replaces_id updates a card in place, keeping its stack slot.
// - Urgency: low/normal use the timeout defaults below, critical never
//   expires and takes the urgent frame + progress color. An explicit
//   expire_timeout wins; 0 means sticky.
// - Clicks: left mints an xdg-activation token (ActivationToken, so the
//   sender can raise itself), invokes the client's default action, then
//   dismisses; right dismisses the card; middle sweeps the visible stack.
//   The cards own the pointer over them — hover never leaks to the window
//   beneath, and the hovered frame warms as the click affordance.
// - DND (naughty.suspend): hl.plugin.hyprnotify.suspend() toggles; cards
//   collect silently with timeouts held, resume renders the queue with
//   fresh timeouts, newest first. `hyprctl hyprnotify count` answers the
//   pending total (the lockscreen bell).
// - Session lock: cards never render above the lockscreen (the built-in
//   overlay does — these are the user's notifications, not the
//   compositor's), input listeners guard-and-reset first, and whatever
//   outlives the lock repaints at unlock.
// - No history, no sound, no markup: naughty had none, and the dunst-era
//   history binds were dropped with dunst itself.
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

#include "hyprnotify.hpp"

using namespace NHyprnotify;

HANDLE PHANDLE = nullptr;

namespace NHyprnotify {
    SNotifyConfig cfg;
}

static Hyprutils::Signal::CHyprSignalListener              lRender, lButton, lMove;
static std::vector<Hyprutils::Signal::CHyprSignalListener> lDamage;
static SP<SHyprCtlCommand>                                 ctlCount;
static UP<SEventLoopDoLaterLock>                           pendingSuspend;

// hl.plugin.hyprnotify.suspend() — the DND chord. Deferred out of the bind's
// input emission (the resume reflows and repaints the stack). Presses
// ACCUMULATE: overwriting the lock cancels the unfired toggle, and two
// presses in one dispatch would net zero instead of two toggles.
static int suspendPresses = 0;
static int luaSuspend(lua_State*) {
    if (!g_pEventLoopManager)
        return 0;
    if (++suspendPresses > 1)
        return 0; // a drain is already queued
    pendingSuspend = g_pEventLoopManager->doLaterLock([]() {
        if (std::exchange(suspendPresses, 0) & 1)
            NHyprnotify::Bus::toggleSuspend();
    });
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

    // Defaults mirror theme.lua; the config overwrites them from the theme.
    cfg.font          = makeShared<Config::Values::CStringValue>("plugin:hyprnotify:font", "font family", "Fira Code");
    cfg.fontSize      = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:font_size", "text size in logical px (monitor scale applies at raster time)", 12);
    cfg.width         = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:width", "card width in logical px", 340);
    cfg.maxHeight     = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:max_height", "card height cap in logical px", 260);
    cfg.maxIcon       = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:max_icon", "image box cap in logical px (the old naughty icon_size)", 100);
    cfg.margin        = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:margin", "screen-edge and inter-card gap in logical px", 4);
    cfg.offsetY       = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:offset_y", "first card's distance from the monitor top (clear the bar)", 30);
    cfg.timeoutLow      = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:timeout_low", "low-urgency timeout in ms", 4000);
    cfg.timeoutNormal   = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:timeout_normal", "normal-urgency timeout in ms (critical never expires)", 8000);
    cfg.rounding        = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:rounding", "corner radius in logical px", 1);
    cfg.maxNotifs       = makeShared<Config::Values::CIntValue>("plugin:hyprnotify:max_notifs", "model cap; overflow evicts the oldest non-critical card", 50);
    cfg.fallbackIconDir = makeShared<Config::Values::CStringValue>("plugin:hyprnotify:fallback_icon_dir", "iconless cards draw a random image from this directory", "");
    cfg.colBg           = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_bg", "card background", 0xff131313);
    cfg.colFg           = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_fg", "body text", 0xffaaaaaa);
    cfg.colTitle        = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_title", "summary line", 0xffdcdccc);
    cfg.colKicker       = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_kicker", "app-name kicker + hovered frame", 0xff8a97a8);
    cfg.colFrame        = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_frame", "card frame + progress trough", 0xff3f3f3f);
    cfg.colUrgent       = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_urgent", "critical frame/kicker + critical progress", 0xffc83f11);
    cfg.colHighlight    = makeShared<Config::Values::CColorValue>("plugin:hyprnotify:col_highlight", "progress bar fill", 0xff32d6ff);

    for (const auto& V : {cfg.fontSize, cfg.width, cfg.maxHeight, cfg.maxIcon, cfg.margin, cfg.offsetY, cfg.timeoutLow, cfg.timeoutNormal, cfg.rounding, cfg.maxNotifs})
        HyprlandAPI::addConfigValueV2(PHANDLE, V);
    for (const auto& V : {cfg.font, cfg.fallbackIconDir})
        HyprlandAPI::addConfigValueV2(PHANDLE, V);
    for (const auto& V : {cfg.colBg, cfg.colFg, cfg.colTitle, cfg.colKicker, cfg.colFrame, cfg.colUrgent, cfg.colHighlight})
        HyprlandAPI::addConfigValueV2(PHANDLE, V);

    Bus::init();

    // the lockscreen bell reads this: every pending card, DND queue included
    ctlCount = HyprlandAPI::registerHyprCtlCommand(
        PHANDLE, SHyprCtlCommand{.name = "hyprnotify", .exact = false, .fn = [](eHyprCtlOutputFormat, std::string request) -> std::string {
                     if (request.ends_with("count"))
                         return std::to_string(notifs.size());
                     return "unknown request";
                 }});
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprnotify", "suspend", luaSuspend);

    lRender = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) { onRenderStage(stage); });
    lButton = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { onMouseButton(e, info); });
    lMove   = Event::bus()->m_events.input.mouse.move.listen([](Vector2D pos, Event::SCallbackInfo& info) { onMouseMove(pos, info); });

    // The cards live on the FOCUSED monitor: monitor.focused is the one
    // desktop event layout depends on (rawMonitorFocus early-outs same-monitor
    // flips, so sloppy focus costs nothing). It fires BEFORE m_focusMonitor is
    // assigned — the warm must stay deferred, which notifChanged guarantees.
    auto& EV = Event::bus()->m_events;
    lDamage.push_back(EV.monitor.focused.listen([](PHLMONITOR) {
        if (!notifs.empty())
            notifChanged();
    }));
    lDamage.push_back(EV.monitor.layoutChanged.listen([]() {
        if (!notifs.empty())
            notifChanged();
    }));
    lDamage.push_back(EV.config.reloaded.listen([]() {
        resetFallbackCache();
        if (!notifs.empty())
            notifChanged(); // a live theme reload re-keys the texture caches
    }));

    return {"hyprnotify", "awesome's naughty: notifications drawn by the compositor", "hitori", VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    pendingSuspend.reset();
    suspendPresses = 0;
    if (ctlCount)
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, ctlCount);
    ctlCount.reset();
    Bus::exit(); // closes the model; its textures die with it
    inputExit();
    lRender.reset();
    lButton.reset();
    lMove.reset();
    lDamage.clear();
    renderExit();
}
