// awesome/system.cpp — the system module: the one CModule over its three
// units (audio.cpp — volume/mic; light.cpp — the display's and the
// keyboard's lights; pad.cpp — the touchpad policy) and the system bus
// they share. The feedback bus is the notify model, the Lua face is the
// flat hl.plugin.awesome.* names, the control center's doors are
// registered in core/controls.hpp.

#include "system.hpp"

#include "core/busclient.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

namespace NAwesome::System {

    CModule& module() {
        static CModule M;
        return M;
    }

    CBusLink& systemBus() {
        static CBusLink L;
        return L;
    }

    static void systemBusInit() {
        auto& L = systemBus();
        L.onLost = [](const std::string& err) {
            HyprlandAPI::addNotification(supervisor().handle(), "[awesome] system bus lost: " + err, CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
        };
        L.dropOwned = []() {
            lightBusDropped();
            audioBusDropped();
        };
        try {
            L.open(true);
            L.sync();
        } catch (const std::exception& E) {
            HyprlandAPI::addNotification(supervisor().handle(), std::string{"[awesome] no system bus, brightness off: "} + E.what(), CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
        }
    }

    namespace {
        // The flat face, nil-guarded in the user config like the old
        // namespaces: volume_up, volume_down, mute, mic_mute,
        // brightness_up, brightness_down, keyboard_light_up,
        // keyboard_light_down, touchpad_toggle.
        int luaVolumeUp(lua_State*) {
            enqueueAction(VOL_UP);
            return 0;
        }
        int luaVolumeDown(lua_State*) {
            enqueueAction(VOL_DOWN);
            return 0;
        }
        int luaMute(lua_State*) {
            enqueueAction(VOL_MUTE);
            return 0;
        }
        int luaMicMute(lua_State*) {
            enqueueAction(MIC_MUTE);
            return 0;
        }
        int luaBrightnessUp(lua_State*) {
            enqueueAction(BRI_UP);
            return 0;
        }
        int luaBrightnessDown(lua_State*) {
            enqueueAction(BRI_DOWN);
            return 0;
        }
        int luaKeyboardLightUp(lua_State*) {
            enqueueAction(KBD_UP);
            return 0;
        }
        int luaKeyboardLightDown(lua_State*) {
            enqueueAction(KBD_DOWN);
            return 0;
        }
        int luaTouchpadToggle(lua_State*) {
            padToggle();
            return 0;
        }
    }

    void CModule::init() {
        systemBusInit(); // before the units that talk on it
        lightInit();
        audioInit();
        padInit(); // the settle timer stands before any hotplug can land
        Controls::doors() = {
            .setLevel = setLevel,
            .step     = stepLevel,
            .setSwitch =
                [](Controls::eSwitch s, bool on) {
                    if (s == Controls::eSwitch::MIC)
                        setMic(on);
                    else if (s == Controls::eSwitch::TOUCHPAD)
                        padSet(on);
                    else if (s == Controls::eSwitch::ROOM)
                        lightFollowRoom(on);
                },
            .setMuted  = setMuted,
            .setOutput = setOutput,
            .refresh =
                []() {
                    lightRefresh();
                    audioRefresh();
                    padPublish();
                },
        };

        HANDLE H = supervisor().handle();
        HyprlandAPI::addLuaFunction(H, "awesome", "volume_up", luaVolumeUp);
        HyprlandAPI::addLuaFunction(H, "awesome", "volume_down", luaVolumeDown);
        HyprlandAPI::addLuaFunction(H, "awesome", "mute", luaMute);
        HyprlandAPI::addLuaFunction(H, "awesome", "mic_mute", luaMicMute);
        HyprlandAPI::addLuaFunction(H, "awesome", "brightness_up", luaBrightnessUp);
        HyprlandAPI::addLuaFunction(H, "awesome", "brightness_down", luaBrightnessDown);
        HyprlandAPI::addLuaFunction(H, "awesome", "keyboard_light_up", luaKeyboardLightUp);
        HyprlandAPI::addLuaFunction(H, "awesome", "keyboard_light_down", luaKeyboardLightDown);
        HyprlandAPI::addLuaFunction(H, "awesome", "touchpad_toggle", luaTouchpadToggle);
    }

    void CModule::teardown() {
        // this module tears down FIRST (reverse priority): the units go
        // out in the order their pending work can still fire — the settle
        // timer (pad) before the Jobs callbacks (audio) and the logind
        // replies (light), whose in-flight deliveries are voided by the
        // generation bumps inside their exits before the notify model dies
        // a beat later; then the bus (fd sources out BEFORE the connection
        // dies). The control center's doors close first and its values go
        // last.
        Controls::doors() = {};
        padExit();
        audioExit();
        lightExit();
        systemBus().close();
        Controls::state() = {};
    }

    // `hyprctl awesome <verb>`: the gate's probes for the touchpad policy
    // and the values the control center shows
    std::optional<std::string> CModule::handleCtl(const std::string& verb) const {
        if (verb == "pad")
            return padState();
#ifdef AWESOME_GATE
        if (verb == "controls") { // "display:P keyboard:P volume:P[m] mic:S touchpad:S room:S" (-1: none)
            const auto& S = Controls::state();
            const auto  L = [&](Controls::eLevel l) {
                const auto& V = S.levels[(size_t)l];
                return std::to_string(V.pct) + (V.muted ? "m" : "");
            };
            return "display:" + L(Controls::eLevel::DISPLAY) + " keyboard:" + L(Controls::eLevel::KEYBOARD) + " volume:" + L(Controls::eLevel::VOLUME) +
                " mic:" + std::to_string(S.switches[(size_t)Controls::eSwitch::MIC]) + " touchpad:" + std::to_string(S.switches[(size_t)Controls::eSwitch::TOUCHPAD]) + " room:" + std::to_string(S.switches[(size_t)Controls::eSwitch::ROOM]);
        }
        if (verb == "outputs") { // "41 *42": the sinks by id, the default starred
            std::string out;
            for (const auto& O : Controls::state().outputs)
                out += (out.empty() ? "" : " ") + std::string{O.current ? "*" : ""} + std::to_string(O.id);
            return out;
        }
#endif
        return std::nullopt;
    }

} // namespace NAwesome::System
