// awesome/shell/battery_state.hpp — the battery's display state, pure: Pixel
// SystemUI's attribution ladder and its colour (charging green, power save
// yellow, low red). Its drawing is shell/battery_pill.hpp.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace NAwesome::Shell::Battery {

    enum class eAttribution : uint8_t {
        None = 0,
        Charging,
        Defender,
        PowerSave,
        Unknown,
    };

    enum class eColor : uint8_t {
        None = 0,
        Active,
        Warning,
        Error,
    };

    struct SVisualState {
        eAttribution attribution = eAttribution::None;
        eColor       color       = eColor::None;
    };

    // Linux battery status has no separate plugged bit. These are the only
    // states that establish it; transient Unknown must not imply charging.
    constexpr bool statusIsPlugged(std::string_view status) {
        return status == "Charging" || status == "Full" || status == "Not charging";
    }

    // Power Profiles Daemon's explicit user-selected energy-saving profile.
    // ACPI platform_profile is a hardware tuning hint and is intentionally
    // not an Android Battery Saver signal.
    constexpr bool isPowerSaverProfile(std::string_view profile) {
        return profile == "power-saver";
    }

    // Pixel SystemUI's BatteryInteractor and BatteryViewModel: unknown state
    // wins the attribution ladder, and the selected attribution owns the color.
    constexpr SVisualState visualState(int level, bool unknown, bool powerSave, bool defender, bool charging) {
        eAttribution attribution = eAttribution::None;
        if (unknown)
            attribution = eAttribution::Unknown;
        else if (powerSave)
            attribution = eAttribution::PowerSave;
        else if (defender)
            attribution = eAttribution::Defender;
        else if (charging)
            attribution = eAttribution::Charging;

        eColor color = eColor::None;
        if (attribution == eAttribution::Charging || attribution == eAttribution::Defender)
            color = eColor::Active;
        else if (attribution == eAttribution::PowerSave)
            color = eColor::Warning;
        else if (attribution == eAttribution::None && level >= 0 && level <= 20)
            color = eColor::Error;

        return {.attribution = attribution, .color = color};
    }

} // namespace NAwesome::Shell::Battery
