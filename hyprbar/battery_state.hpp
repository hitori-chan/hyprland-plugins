// hyprbar/battery_state.hpp — pure Android-aligned battery icon state
#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace NHyprbar::Battery {

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

    // BatteryMeasurePolicy rounds positive child dimensions independently,
    // using the JVM/Kotlin round-to-nearest rule (a .5 tie rounds upward).
    constexpr int pixelRound(double value) {
        return static_cast<int>(value + 0.5);
    }

    constexpr double attributionViewportWidth(eAttribution attribution) {
        switch (attribution) {
            case eAttribution::Charging:
            case eAttribution::Defender: return 8.0;
            case eAttribution::PowerSave: return 8.5;
            case eAttribution::Unknown: return 6.0;
            case eAttribution::None: return 0.0;
        }
        return 0.0;
    }

    constexpr int measuredWidth(int heightPx, eAttribution attribution) {
        const double SCALE = heightPx / 13.0;
        const int    BODY  = pixelRound(24.0 * SCALE);
        if (attribution == eAttribution::None)
            return BODY + pixelRound(SCALE) + pixelRound(1.5 * SCALE);
        const int ATTRIBUTION = pixelRound(attributionViewportWidth(attribution) * SCALE);
        return BODY + pixelRound(ATTRIBUTION * 0.8);
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

} // namespace NHyprbar::Battery
