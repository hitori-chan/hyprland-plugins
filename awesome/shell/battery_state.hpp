// awesome/shell/battery_state.hpp — the battery's display state, pure: Pixel
// SystemUI's attribution ladder and its colour (charging green, power save
// yellow, low red), and the note the pill shows under the pointer. Its
// drawing is shell/battery_pill.hpp.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
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

    // ---- the note ----
    //
    // One line under the pill after a rest: the level, the state the ladder
    // picked, and how long the charge lasts or when it is full ("76% · 3:12
    // left", "63% · Charging · full at 22:40"). The time is left out while
    // no rate is known.
    enum class ePower : uint8_t {
        BATTERY,  // discharging
        CHARGING, // taking charge
        FULL,     // on the cable, charged
        HOLDING,  // on the cable, not charging, no limit to blame (a transient)
    };

    struct SNote {
        int    level     = -1;
        bool   unknown   = false;
        bool   powerSave = false;
        bool   defender  = false;
        ePower power     = ePower::BATTERY;
        int    minutes   = -1; // BATTERY: to empty; CHARGING: to full; -1 unknown
        int    fullAt    = -1; // CHARGING: the minute of the day it is full at; -1 unknown
        int    limit     = -1; // the charge limit a defender holds at; -1 unknown
    };

    // "3:12": hours and minutes, capped at 99:59
    inline std::string durationText(int minutes) {
        const int M = std::clamp(minutes, 0, 99 * 60 + 59);
        return std::to_string(M / 60) + (M % 60 < 10 ? ":0" : ":") + std::to_string(M % 60);
    }

    // "22:40": a minute of the day on the clock
    inline std::string clockText(int minuteOfDay) {
        const int M = ((minuteOfDay % 1440) + 1440) % 1440;
        return (M / 60 < 10 ? "0" : "") + std::to_string(M / 60) + (M % 60 < 10 ? ":0" : ":") + std::to_string(M % 60);
    }

    // the ladder's order: unknown, then the defender's hold (its level is
    // the limit, said once), then the level with power save and the power
    inline std::string noteText(const SNote& n) {
        if (n.unknown || n.level < 0)
            return "Battery status unknown";
        if (n.defender)
            return "Held at " + std::to_string(n.limit >= 0 ? n.limit : n.level) + "% to spare the battery";
        std::string out = std::to_string(n.level) + "%";
        if (n.powerSave)
            out += " · Power Saver";
        switch (n.power) {
            case ePower::BATTERY:
                if (n.minutes >= 0)
                    out += " · " + durationText(n.minutes) + " left";
                break;
            case ePower::CHARGING:
                out += " · Charging";
                if (n.fullAt >= 0)
                    out += " · full at " + clockText(n.fullAt);
                break;
            case ePower::FULL: out += " · Charged"; break;
            case ePower::HOLDING: out += " · Not charging"; break;
        }
        return out;
    }

} // namespace NAwesome::Shell::Battery
