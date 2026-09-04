// hyprpad — the old awesome touchpad module as a native Hyprland plugin.
//
// The touchpad turns off while an external (USB/Bluetooth) mouse is present
// and back on when it's unplugged; XF86TouchpadToggle flips it by hand
// (hl.plugin.hyprpad.toggle()). Replaces scripts/touchpad-auto.sh.
//
// Fully in-process — no udev, no forks:
// - Hotplug rides the compositor's own device signals: aquamarine's
//   newPointer plus a destroy listener per pointer. Plugin listeners fire
//   BEFORE the compositor's own (dynamic before static in hyprutils), so
//   the device list is stale mid-signal — handlers only (re)arm a settle
//   timer, which also coalesces one plug's burst into a single re-check.
// - External mouse = a non-virtual, non-touchpad pointer whose libinput
//   bus type is USB or Bluetooth. The touchpad is the m_isTouchpad entry
//   (the compositor's own capability predicate — a pointer with a size),
//   not a name substring; its m_hlName is what hl.device keys on.
// - The flip is Config::Lua::mgr()->eval("hl.device({...})") — the code
//   `hyprctl eval` reaches, minus the fork + socket round-trip. It writes
//   the compositor's per-device config store, so nothing fights the next
//   config re-apply. A reload wipes that runtime state: config.reloaded
//   forgets appliedState and re-checks.
// - Auto re-checks are change-detected against the last applied state: an
//   unrelated hotplug re-checks but applies nothing.
// - Feedback is one async D-Bus Notify (replaces-id 9991, with explicit
//   touchpad identities) on the plugin's own
//   event-loop-integrated session-bus connection — hyprbar's tray pattern; the
//   notification daemon's API is the bus name, never its symbols. If the
//   bus dies the cards stop; the flip keeps working.
//
// Everything lives in NHyprpad so no symbol can collide with another
// plugin's at dlopen time.

#include "common/busclient.hpp"
#include "common/lifecycle.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/config/lua/ConfigManager.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>

#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/input/Input.hpp>
#include <libinput.h>
#include <linux/input.h>
#include <poll.h>
#include <sdbus-c++/sdbus-c++.h>
#include <wayland-server-core.h>

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace NHyprpad {

    HANDLE PHANDLE = nullptr;

    constexpr auto                                             SETTLE = std::chrono::milliseconds(400);

    static SP<CEventLoopTimer>                                 settle;
    static NHyprCommon::CHop                                   pendingToggle;
    static NHyprCommon::CLifecycle                             g_lifecycle;
    // per live pointer, rebuilt on every device re-check — dynamic lifetime,
    // deliberately not in the lifecycle bundle
    static std::vector<Hyprutils::Signal::CHyprSignalListener> lDestroy;

    // The last state and actual touchpad object this plugin applied it to. A
    // replacement must receive policy even when the desired boolean matches.
    static int          appliedState = -1;
    static WP<IPointer> appliedTouchpad;

    // ---- the feedback cards' bus link (hyprbar's tray pattern, send-only) ----

    static std::unique_ptr<sdbus::IProxy> notifyProxy;
    static NHyprCommon::CBusLink          g_bus; // feedback cards' session-bus link

    static void notify(const char* icon, const std::string& body, bool timed) {
        if (!g_bus.conn())
            return;
        try {
            if (!notifyProxy)
                notifyProxy = sdbus::createProxy(*g_bus.conn(), sdbus::ServiceName{"org.freedesktop.Notifications"}, sdbus::ObjectPath{"/org/freedesktop/Notifications"});
            notifyProxy->callMethodAsync("Notify")
                .onInterface("org.freedesktop.Notifications")
                .withArguments(std::string{"osd"}, uint32_t{9991}, std::string{icon}, std::string{"Touchpad"}, body, std::vector<std::string>{},
                               std::map<std::string, sdbus::Variant>{{"urgency", sdbus::Variant{uint8_t{0}}}, {"x-hitori-osd", sdbus::Variant{true}}}, timed ? 1500 : -1)
                .uponReplyInvoke([](std::optional<sdbus::Error>, uint32_t) {});
            g_bus.pollSoon(); // flush the send from the event loop, never from here
        } catch (...) {} // broker gone: teardown is already pending, drop the card
    }

    // ---- the device side ----

    // escape for a double-quoted Lua string literal
    static std::string luaq(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (const char C : s) {
            if (C == '\\' || C == '"')
                out += '\\';
            out += C;
        }
        return out;
    }

    static SP<IPointer> touchpad() {
        if (!g_pInputManager)
            return nullptr;
        for (const auto& P : g_pInputManager->m_pointers)
            if (P->m_isTouchpad)
                return P;
        return nullptr;
    }

    // m_connected is the compositor's actual pointer attachment state. It is
    // updated together with libinput's send-events mode by setPointerConfigs,
    // so a manual toggle can read the live state even when no automatic pass
    // has populated appliedState yet.
    static std::optional<bool> touchpadEnabled() {
        const auto TOUCHPAD = touchpad();
        return TOUCHPAD ? std::optional<bool>{TOUCHPAD->m_connected} : std::nullopt;
    }

    static bool externalMousePresent() {
        if (!g_pInputManager)
            return false;
        for (const auto& P : g_pInputManager->m_pointers) {
            if (P->isVirtual() || P->m_isTouchpad)
                continue;
            const auto  AQ = P->aq();
            auto* const H  = AQ ? AQ->getLibinputHandle() : nullptr;
            if (!H)
                continue;
            const auto BUS = libinput_device_get_id_bustype(H);
            if (BUS == BUS_USB || BUS == BUS_BLUETOOTH)
                return true;
        }
        return false;
    }

    static void applyEnabled(bool on, SP<IPointer> target = nullptr) {
        if (!target)
            target = touchpad();
        if (!target) {
            appliedState = -1;
            appliedTouchpad.reset();
            notify("input-touchpad", "not found", false);
            return;
        }
        // the manager lives in a unique pointer: its weak can NEVER lock()
        // (hyprutils forbids promoting unique to shared) — use it in place
        const auto MGR = Config::Lua::mgr();
        if (!MGR)
            return;
        if (const auto ERR = MGR->eval("hl.device({ name = \"" + luaq(target->m_hlName) + "\", enabled = " + (on ? "true" : "false") + " })")) {
            HyprlandAPI::addNotification(PHANDLE, "[hyprpad] hl.device failed: " + *ERR, CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
            return; // appliedState untouched: the next check retries
        }
        appliedState    = on ? 1 : 0;
        appliedTouchpad = target;
        notify(on ? "input-touchpad" : "touchpad-disabled", on ? "enabled" : "disabled", true);
    }

    static void autoApply() {
        const auto TOUCHPAD = touchpad();
        if (!TOUCHPAD) {
            appliedState = -1;
            appliedTouchpad.reset();
            return; // nothing to auto-manage — the "not found" card belongs to the manual toggle,
                    // else every mouse hotplug re-spams it (WANT is 0/1, appliedState stuck at -1)
        }
        const int WANT = externalMousePresent() ? 0 : 1;
        if (WANT != appliedState || appliedTouchpad.lock() != TOUCHPAD)
            applyEnabled(WANT == 1, TOUCHPAD);
    }

    // Removal is observable in-process: every pointer's destroy signal arms
    // the settle timer. Rebuilt each re-check — aq() is already dead inside
    // a destroy emission, so handlers must never touch the device.
    static void watchPointers() {
        lDestroy.clear();
        if (!g_pInputManager)
            return;
        lDestroy.reserve(g_pInputManager->m_pointers.size());
        for (const auto& P : g_pInputManager->m_pointers)
            lDestroy.push_back(P->m_events.destroy.listen([]() {
                if (settle)
                    settle->updateTimeout(SETTLE);
            }));
    }

    // hl.plugin.hyprpad.toggle() — XF86TouchpadToggle. Deferred out of the
    // bind's input emission; the manual flip also cancels a pending auto
    // re-check so it isn't overridden a beat later.
    //
    // queue+drain, never a lone doLaterLock: two flips can arm in one
    // dispatch and overwriting the lock cancels the unfired one; only the
    // parity survives the drain (an even batch nets to no change)
    static std::vector<int> g_toggleQueue; // one entry per flip
    static bool             toggleQueued = false;

    static int luaToggle(lua_State*) {
        if (g_toggleQueue.size() < 16)
            g_toggleQueue.push_back(0);
        if (toggleQueued)
            return 0;
        toggleQueued = true;
        pendingToggle.arm([]() {
            const int N = (int)g_toggleQueue.size();
            g_toggleQueue.clear();
            toggleQueued = false;
            if (N % 2 == 0)
                return;
            if (settle)
                settle->updateTimeout(std::nullopt);
            const bool CURRENT = touchpadEnabled().value_or(appliedState == 1);
            applyEnabled(!CURRENT);
        });
        return 0;
    }

} // namespace NHyprpad

using namespace NHyprpad;

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprpad] Version mismatch: rebuild the plugin against the running Hyprland", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hyprpad] version mismatch");
    }

    g_bus.onLost    = [](const std::string& err) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprpad] bus lost, feedback cards off: " + err, CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
    };
    g_bus.dropOwned = []() { notifyProxy.reset(); };
    try {
        g_bus.open(false);
        g_bus.sync(); // set the initial mask/timeout
    } catch (const std::exception& E) {
        HyprlandAPI::addNotification(PHANDLE, std::string{"[hyprpad] no session bus, feedback cards off: "} + E.what(), CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
        g_bus.teardown();
    }

    settle = makeShared<CEventLoopTimer>(
        std::nullopt,
        [](SP<CEventLoopTimer>, void*) {
            watchPointers();
            autoApply();
        },
        nullptr);
    g_pEventLoopManager->addTimer(settle);

    g_lifecycle.init();
    if (g_pCompositor && g_pCompositor->m_aqBackend)
        g_lifecycle.listen(g_pCompositor->m_aqBackend->events.newPointer, [](const SP<Aquamarine::IPointer>&) {
            if (settle)
                settle->updateTimeout(SETTLE);
        });

    // a reload wiped the runtime hl.device state: forget what was applied
    // and re-check (replaces the old core.lua config.reloaded hook)
    g_lifecycle.listen(Event::bus()->m_events.config.reloaded, []() {
        appliedState = -1;
        appliedTouchpad.reset();
        if (settle)
            settle->updateTimeout(SETTLE);
    });

    HyprlandAPI::addLuaFunction(PHANDLE, "hyprpad", "toggle", luaToggle);

    // the login check rides the settle timer too: by the time it fires the
    // device list is populated and the notification daemon is up
    settle->updateTimeout(SETTLE);

    return {"hyprpad", "the awesome touchpad module", "hitori", "1.1.6"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_lifecycle.resetAll(); // listeners first, then every hop
    lDestroy.clear();
    appliedState = -1;
    appliedTouchpad.reset();
    g_bus.close(); // fd sources out BEFORE the connection dies
    if (settle && g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(settle);
    settle.reset();
}
