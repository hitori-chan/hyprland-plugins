// awesome/system/pad.cpp — the touchpad policy.
//
// The touchpad turns off while an external (USB/Bluetooth) mouse is
// present and back on when it's unplugged; XF86TouchpadToggle flips it by
// hand. The feedback card posts straight into the notify model (id 9991).
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
// - The flip is Config::Lua::mgr()->eval("hl.device({...})") — it writes
//   the compositor's per-device config store, so nothing fights the next
//   config re-apply. A reload wipes that runtime state: config.reloaded
//   forgets appliedState and re-checks.
// - Auto re-checks are change-detected against the last applied state: an
//   unrelated hotplug re-checks but applies nothing.

#include "notify/model.hpp"

#include "core/jobs.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/lua/ConfigManager.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>

#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/input/Input.hpp>
#include <libinput.h>
#include <linux/input.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace NAwesome::System {

    inline constexpr uint32_t ID_TOUCHPAD = 9991;
    inline constexpr int      PAD_TIMEOUT = 1500;

    constexpr auto SETTLE = std::chrono::milliseconds(400);

    static SP<CEventLoopTimer> settle;
    // per live pointer, rebuilt on every device re-check — dynamic lifetime,
    // deliberately not in the supervisor's listener registry (it is rebuilt,
    // not cleared)
    static std::vector<Hyprutils::Signal::CHyprSignalListener> lDestroy;

    // The last state and actual touchpad object this module applied it to. A
    // replacement must receive policy even when the desired boolean matches.
    static int          appliedState = -1;
    static WP<IPointer> appliedTouchpad;

    static void postPad(const char* icon, const char* body, int timeoutMs) {
        Notify::Model::postCard({.id = ID_TOUCHPAD, .icon = icon, .summary = "Touchpad", .body = body, .expireTimeout = timeoutMs});
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
            appliedState    = -1;
            appliedTouchpad.reset();
            postPad("input-touchpad-symbolic", "not found", PAD_TIMEOUT);
            return;
        }
        // the manager lives in a unique pointer: its weak can NEVER lock()
        // (hyprutils forbids promoting unique to shared) — use it in place
        const auto MGR = Config::Lua::mgr();
        if (!MGR)
            return;
        if (const auto ERR = MGR->eval("hl.device({ name = \"" + luaq(target->m_hlName) + "\", enabled = " + (on ? "true" : "false") + " })")) {
            HyprlandAPI::addNotification(supervisor().handle(), "[awesome] hl.device failed: " + *ERR, CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
            return; // appliedState untouched: the next check retries
        }
        appliedState    = on ? 1 : 0;
        appliedTouchpad = target;
        // symbolic, not the plain names: "touchpad-disabled" ships in the
        // HighContrast theme only (unprobed) and would render iconless
        postPad(on ? "input-touchpad-symbolic" : "touchpad-disabled-symbolic", on ? "enabled" : "disabled", PAD_TIMEOUT);
    }

    static void autoApply() {
        const auto TOUCHPAD = touchpad();
        if (!TOUCHPAD) {
            appliedState    = -1;
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

    void padInit() {
        settle = makeShared<CEventLoopTimer>(
            std::nullopt,
            [](SP<CEventLoopTimer>, void*) {
                watchPointers();
                autoApply();
            },
            nullptr);
        g_pEventLoopManager->addTimer(settle);

        if (g_pCompositor && g_pCompositor->m_aqBackend)
            supervisor().listen(g_pCompositor->m_aqBackend->events.newPointer, [](const SP<Aquamarine::IPointer>&) {
                if (settle)
                    settle->updateTimeout(SETTLE);
            });

        // a reload wiped the runtime hl.device state: forget what was applied
        // and re-check
        supervisor().listen(Event::bus()->m_events.config.reloaded, []() {
            appliedState    = -1;
            appliedTouchpad.reset();
            if (settle)
                settle->updateTimeout(SETTLE);
        });

        // the login check rides the settle timer too: by the time it fires the
        // device list is populated and the notification surface is up
        settle->updateTimeout(SETTLE);
    }

    // queued, never a lone doLaterLock: two flips can arm in one dispatch
    // and overwriting the lock cancels the unfired one; only the parity
    // survives the drain (an even batch nets to no change)
    static CHopQueue<char, 16> toggles([](std::vector<char>& batch) {
        if (batch.size() % 2 == 0)
            return;
        if (settle)
            settle->updateTimeout(std::nullopt);
        const bool CURRENT = touchpadEnabled().value_or(appliedState == 1);
        applyEnabled(!CURRENT);
    });

    void padExit() {
        toggles.reset();
        lDestroy.clear();
        appliedState    = -1;
        appliedTouchpad.reset();
        if (settle && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(settle);
        settle.reset();
    }

    // The manual flip (XF86TouchpadToggle). Deferred out of the bind's input
    // emission; the manual flip also cancels a pending auto re-check so it
    // isn't overridden a beat later.
    void padToggle() {
        toggles.push(0);
    }

    // the gate's probe: the touchpad's live state, not the policy's memory
    std::string padState() {
        const auto E = touchpadEnabled();
        if (!E)
            return "none";
        return *E ? "on" : "off";
    }

} // namespace NAwesome::System
