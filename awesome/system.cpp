// awesome/system.cpp — the system module: the one CModule over the two
// units (audio.cpp — volume/mic/brightness; pad.cpp — the touchpad
// policy). The feedback bus is the notify model, the Lua face is the flat
// hl.plugin.awesome.* names.

#include "system.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

namespace NAwesome::System {

    CModule& module() {
        static CModule M;
        return M;
    }

    namespace {
        // The flat face, nil-guarded in the user config like the old
        // namespaces: volume_up, volume_down, mute, mic_mute,
        // brightness_up, brightness_down, touchpad_toggle.
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
        int luaTouchpadToggle(lua_State*) {
            padToggle();
            return 0;
        }
    }

    void CModule::init() {
        audioInit(); // the system bus and the backlight scan first
        padInit(); // the settle timer stands before any hotplug can land

        HANDLE H = supervisor().handle();
        HyprlandAPI::addLuaFunction(H, "awesome", "volume_up", luaVolumeUp);
        HyprlandAPI::addLuaFunction(H, "awesome", "volume_down", luaVolumeDown);
        HyprlandAPI::addLuaFunction(H, "awesome", "mute", luaMute);
        HyprlandAPI::addLuaFunction(H, "awesome", "mic_mute", luaMicMute);
        HyprlandAPI::addLuaFunction(H, "awesome", "brightness_up", luaBrightnessUp);
        HyprlandAPI::addLuaFunction(H, "awesome", "brightness_down", luaBrightnessDown);
        HyprlandAPI::addLuaFunction(H, "awesome", "touchpad_toggle", luaTouchpadToggle);
    }

    void CModule::teardown() {
        // this module tears down FIRST (reverse priority): the units go
        // out in the order their pending work can still fire — the settle
        // timer (pad) before the Jobs callbacks (audio), whose in-flight
        // deliveries are voided by the generation bumps inside audioExit
        // before the notify model dies a beat later
        padExit();
        audioExit();
    }

    // `hyprctl awesome <verb>`: the gate's probe for the touchpad policy
    std::optional<std::string> CModule::handleCtl(const std::string& verb) const {
        if (verb == "pad")
            return padState();
        return std::nullopt;
    }

} // namespace NAwesome::System
