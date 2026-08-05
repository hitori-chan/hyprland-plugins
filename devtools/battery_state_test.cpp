#include "hyprbar/battery_state.hpp"

#include <cstdio>

using NHyprbar::Battery::eAttribution;
using NHyprbar::Battery::eColor;
using NHyprbar::Battery::isPowerSaverProfile;
using NHyprbar::Battery::measuredWidth;
using NHyprbar::Battery::statusIsPlugged;
using NHyprbar::Battery::visualState;

static bool expect(bool condition, const char* name) {
    if (condition)
        return true;
    std::fprintf(stderr, "FAIL: %s\n", name);
    return false;
}

int main() {
    bool ok = true;

    ok &= expect(isPowerSaverProfile("power-saver"), "power-saver profile enables Battery Saver");
    ok &= expect(!isPowerSaverProfile("low-power"), "ACPI low-power is not Battery Saver");
    ok &= expect(statusIsPlugged("Charging") && statusIsPlugged("Full") && statusIsPlugged("Not charging"), "powered states are plugged");
    ok &= expect(!statusIsPlugged("Discharging") && !statusIsPlugged("Unknown"), "cell and unknown states are not plugged");
    ok &= expect(measuredWidth(13, eAttribution::None) == 27, "Pixel cap state measures 27 px at the native viewport height");
    ok &= expect(measuredWidth(13, eAttribution::Charging) == 30 && measuredWidth(13, eAttribution::Defender) == 30,
                 "Pixel bolt and defender states measure 30 px at the native viewport height");
    ok &= expect(measuredWidth(13, eAttribution::PowerSave) == 31, "Pixel plus state measures 31 px at the native viewport height");
    ok &= expect(measuredWidth(13, eAttribution::Unknown) == 29, "Pixel question state measures 29 px at the native viewport height");

    const auto IDLE = visualState(67, false, false, false, false);
    ok &= expect(IDLE.attribution == eAttribution::None && IDLE.color == eColor::None, "unplugged idle has no attribution");

    const auto SAVE = visualState(67, false, true, false, false);
    ok &= expect(SAVE.attribution == eAttribution::PowerSave && SAVE.color == eColor::Warning, "Battery Saver uses the plus and warning color");

    const auto LOW = visualState(20, false, false, false, false);
    ok &= expect(LOW.attribution == eAttribution::None && LOW.color == eColor::Error, "low discharging battery is error colored without an attribution");

    const auto DEFEND = visualState(80, false, false, true, true);
    ok &= expect(DEFEND.attribution == eAttribution::Defender && DEFEND.color == eColor::Active, "charge defender keeps active power color");

    const auto CHARGING = visualState(80, false, false, false, true);
    ok &= expect(CHARGING.attribution == eAttribution::Charging && CHARGING.color == eColor::Active, "powered battery uses charging attribution");

    const auto SAVING_CHARGING = visualState(80, false, true, false, true);
    ok &= expect(SAVING_CHARGING.attribution == eAttribution::PowerSave && SAVING_CHARGING.color == eColor::Warning,
                 "Battery Saver stays yellow while powered");

    const auto UNKNOWN = visualState(-1, true, true, true, true);
    ok &= expect(UNKNOWN.attribution == eAttribution::Unknown && UNKNOWN.color == eColor::None,
                 "unknown state overrides every attribution and uses the default color");

    const auto UNREADABLE = visualState(-1, false, false, false, false);
    ok &= expect(UNREADABLE.attribution == eAttribution::None && UNREADABLE.color == eColor::None,
                 "missing level never becomes a critical zero-percent battery");

    if (!ok)
        return 1;
    std::puts("battery state tests passed");
    return 0;
}
