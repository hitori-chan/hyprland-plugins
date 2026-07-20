// common/order.hpp — load order IS behavior: event listeners fire in plugin
// load order, and hyprpm.toml's order encodes the input swallow chain (the
// bar eats its strip clicks before they count as window clicks; the
// immovable-maximized swallow wins over click-to-raise). A misordered toml
// used to invert that policy silently. The guard turns it into a load
// failure: a plugin asserts that none of the plugins required to load
// AFTER it are already in. Ordering only, never presence — a disabled
// plugin must not block the chain.
#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/plugins/PluginSystem.hpp>

#include <initializer_list>
#include <stdexcept>
#include <string>

namespace NHyprCommon {

    inline void mustLoadBefore(HANDLE handle, const char* self, std::initializer_list<const char*> later) {
        if (!g_pPluginSystem)
            return;
        for (const auto* P : g_pPluginSystem->getAllPlugins()) {
            for (const char* L : later) {
                if (P->m_name != L)
                    continue;
                const auto MSG = std::string{"["} + self + "] load order broken: " + L + " is already loaded but must come after " + self + " (fix hyprpm.toml order)";
                HyprlandAPI::addNotification(handle, MSG, CHyprColor{1.0, 0.2, 0.2, 1.0}, 10000);
                throw std::runtime_error(MSG);
            }
        }
    }

} // namespace NHyprCommon
