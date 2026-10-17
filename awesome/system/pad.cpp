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
//   bus type is USB or Bluetooth, once it has moved or clicked: a
//   Unifying/Bolt receiver with only a keyboard paired, a gaming
//   keyboard's mouse interface or a presenter exposes a pointer that never
//   does. The touchpad is the m_isTouchpad entry (the compositor's own
//   capability predicate — a pointer with a size), the built-in one
//   before an external trackpad, not a name substring; its m_hlName is
//   what hl.device keys on.
// - A manual flip holds until the pointer set changes (a hotplug): every
//   config save reloads, and the automatic pass used to put the mouse
//   policy back over the user's own choice.
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

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace NAwesome::System {

    inline constexpr uint32_t ID_TOUCHPAD = 9991;
    inline constexpr int      PAD_TIMEOUT = 1500;

    constexpr auto SETTLE = std::chrono::milliseconds(400);

    static SP<CEventLoopTimer> settle;
    // per live pointer, rebuilt on every device re-check — dynamic lifetime,
    // deliberately not in the supervisor's listener registry (it is rebuilt,
    // not cleared): every pointer's destroy, and an unproven external
    // pointer's first motion or click
    static std::vector<Hyprutils::Signal::CHyprSignalListener> lDestroy, lProve;
    static std::vector<WP<IPointer>>                           proven; // external pointers seen acting as a mouse
    static bool                                                hotplugged  = false; // the pointer set changed since the last check
    static int                                                 manualState = -1;    // the user's own flip, until a hotplug; -1 none

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

    static bool onExternalBus(const SP<IPointer>& P) {
        const auto  AQ = P->aq();
        auto* const H  = AQ ? AQ->getLibinputHandle() : nullptr;
        if (!H)
            return false;
        const auto BUS = libinput_device_get_id_bustype(H);
        return BUS == BUS_USB || BUS == BUS_BLUETOOTH;
    }

    // the built-in touchpad, else any (an external trackpad alone)
    static SP<IPointer> touchpad() {
        if (!g_pInputManager)
            return nullptr;
        SP<IPointer> any;
        for (const auto& P : g_pInputManager->m_pointers) {
            if (!P->m_isTouchpad)
                continue;
            if (!onExternalBus(P))
                return P;
            if (!any)
                any = P;
        }
        return any;
    }

    // m_connected is the compositor's actual pointer attachment state. It is
    // updated together with libinput's send-events mode by setPointerConfigs,
    // so a manual toggle can read the live state even when no automatic pass
    // has populated appliedState yet.
    static std::optional<bool> touchpadEnabled() {
        const auto TOUCHPAD = touchpad();
        return TOUCHPAD ? std::optional<bool>{TOUCHPAD->m_connected} : std::nullopt;
    }

    static bool isExternalMouse(const SP<IPointer>& P) {
        return !P->isVirtual() && !P->m_isTouchpad && onExternalBus(P);
    }

    static bool isProven(const SP<IPointer>& P) {
        return std::ranges::any_of(proven, [&](const auto& W) { return W.lock() == P; });
    }

    static bool externalMousePresent() {
        if (!g_pInputManager)
            return false;
        return std::ranges::any_of(g_pInputManager->m_pointers, [](const auto& P) { return isExternalMouse(P) && isProven(P); });
    }

    // The state the user was last SHOWN: an automatic re-apply that changes
    // nothing they can see (a config reload wipes the runtime hl.device
    // state, and every config save reloads) stays silent; a manual toggle
    // always answers.
    static int announcedState = -1;

    static void applyEnabled(bool on, SP<IPointer> target = nullptr, bool manual = false) {
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
        if (!manual && announcedState == appliedState)
            return;
        announcedState = appliedState;
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
        const int WANT = manualState >= 0 ? manualState : externalMousePresent() ? 0 : 1;
        if (WANT == appliedState && appliedTouchpad.lock() == TOUCHPAD)
            return;
        // already there (every config save reloads, and the config's own
        // state is usually the wanted one): record it without the eval —
        // hl.device re-runs the device setup, an XKB keymap compile
        if (touchpadEnabled() == (WANT == 1)) {
            appliedState    = WANT;
            appliedTouchpad = TOUCHPAD;
            return;
        }
        applyEnabled(WANT == 1, TOUCHPAD);
    }

    static void hotplug() {
        hotplugged = true;
        if (settle)
            settle->updateTimeout(SETTLE);
    }

    // an unproven external pointer acted: it is a mouse now. The listener
    // stays until the re-check rebuilds them (a listener must not drop
    // itself mid-emission); later events find it proven and do nothing.
    static void markProven(const WP<IPointer>& wp) {
        const auto P = wp.lock();
        if (!P || isProven(P))
            return;
        proven.push_back(wp);
        if (settle)
            settle->updateTimeout(SETTLE);
    }

    // Removal is observable in-process: every pointer's destroy signal arms
    // the settle timer. Rebuilt each re-check — aq() is already dead inside
    // a destroy emission, so handlers must never touch the device. An
    // unproven external pointer is also watched for its first motion or
    // click (proven ones cost nothing per motion).
    static void watchPointers() {
        lDestroy.clear();
        lProve.clear();
        std::erase_if(proven, [](const auto& W) { return W.expired(); });
        if (!g_pInputManager)
            return;
        lDestroy.reserve(g_pInputManager->m_pointers.size());
        for (const auto& P : g_pInputManager->m_pointers) {
            lDestroy.push_back(P->m_events.destroy.listen([]() { hotplug(); }));
            if (!isExternalMouse(P) || isProven(P))
                continue;
            lProve.push_back(P->m_pointerEvents.motion.listen([wp = WP<IPointer>{P}](const IPointer::SMotionEvent&) { markProven(wp); }));
            lProve.push_back(P->m_pointerEvents.button.listen([wp = WP<IPointer>{P}](const IPointer::SButtonEvent&) { markProven(wp); }));
        }
    }

    void padInit() {
        settle = makeShared<CEventLoopTimer>(
            std::nullopt,
            [](SP<CEventLoopTimer>, void*) {
                if (std::exchange(hotplugged, false))
                    manualState = -1; // a new pointer set: the policy is automatic again
                watchPointers();
                autoApply();
            },
            nullptr);
        g_pEventLoopManager->addTimer(settle);

        if (g_pCompositor && g_pCompositor->m_aqBackend)
            supervisor().listen(g_pCompositor->m_aqBackend->events.newPointer, [](const SP<Aquamarine::IPointer>&) { hotplug(); });

        // a reload wiped the runtime hl.device state: forget what was applied
        // and re-check (a manual flip is re-applied, not forgotten)
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
        hotplugged = false; // the flip comes after it: the user's choice wins
        // the cancelled re-check also attached the destroy listeners: a
        // mouse plugged in just before this toggle must still be heard
        // leaving
        watchPointers();
        const bool CURRENT = touchpadEnabled().value_or(appliedState == 1);
        applyEnabled(!CURRENT, nullptr, true);
        if (appliedState == (CURRENT ? 0 : 1))
            manualState = appliedState; // held until a hotplug, across config reloads
    });

    void padExit() {
        toggles.reset();
        lDestroy.clear();
        lProve.clear();
        proven.clear();
        hotplugged  = false;
        manualState = -1;
        appliedState    = -1;
        announcedState  = -1;
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
