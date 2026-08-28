#include "hyprbar/battery_state.hpp"
#include "test.hpp"

using NHyprbar::Battery::eAttribution;
using NHyprbar::Battery::eColor;
using NHyprbar::Battery::isPowerSaverProfile;
using NHyprbar::Battery::measuredWidth;
using NHyprbar::Battery::statusIsPlugged;
using NHyprbar::Battery::visualState;

int main() {
    NHyprTest::CSuite suite{"battery_state_test"};

    suite.expect(isPowerSaverProfile("power-saver"), "power-saver profile enables Battery Saver");
    suite.expect(!isPowerSaverProfile("low-power"), "ACPI low-power is not Battery Saver");
    suite.expect(statusIsPlugged("Charging") && statusIsPlugged("Full") && statusIsPlugged("Not charging"), "powered states are plugged");
    suite.expect(!statusIsPlugged("Discharging") && !statusIsPlugged("Unknown"), "cell and unknown states are not plugged");
    suite.expect(measuredWidth(13, eAttribution::None) == 27, "Pixel cap state measures 27 px at the native viewport height");
    suite.expect(measuredWidth(13, eAttribution::Charging) == 30 && measuredWidth(13, eAttribution::Defender) == 30,
                 "Pixel bolt and defender states measure 30 px at the native viewport height");
    suite.expect(measuredWidth(13, eAttribution::PowerSave) == 31, "Pixel plus state measures 31 px at the native viewport height");
    suite.expect(measuredWidth(13, eAttribution::Unknown) == 29, "Pixel question state measures 29 px at the native viewport height");

    const auto IDLE = visualState(67, false, false, false, false);
    suite.expect(IDLE.attribution == eAttribution::None && IDLE.color == eColor::None, "unplugged idle has no attribution");

    const auto SAVE = visualState(67, false, true, false, false);
    suite.expect(SAVE.attribution == eAttribution::PowerSave && SAVE.color == eColor::Warning, "Battery Saver uses the plus and warning color");

    const auto LOW = visualState(20, false, false, false, false);
    suite.expect(LOW.attribution == eAttribution::None && LOW.color == eColor::Error, "low discharging battery is error colored without an attribution");

    const auto DEFEND = visualState(80, false, false, true, true);
    suite.expect(DEFEND.attribution == eAttribution::Defender && DEFEND.color == eColor::Active, "charge defender keeps active power color");

    const auto CHARGING = visualState(80, false, false, false, true);
    suite.expect(CHARGING.attribution == eAttribution::Charging && CHARGING.color == eColor::Active, "powered battery uses charging attribution");

    const auto SAVING_CHARGING = visualState(80, false, true, false, true);
    suite.expect(SAVING_CHARGING.attribution == eAttribution::PowerSave && SAVING_CHARGING.color == eColor::Warning, "Battery Saver stays yellow while powered");

    const auto UNKNOWN = visualState(-1, true, true, true, true);
    suite.expect(UNKNOWN.attribution == eAttribution::Unknown && UNKNOWN.color == eColor::None, "unknown state overrides every attribution and uses the default color");

    const auto UNREADABLE = visualState(-1, false, false, false, false);
    suite.expect(UNREADABLE.attribution == eAttribution::None && UNREADABLE.color == eColor::None, "missing level never becomes a critical zero-percent battery");

    return suite.finish();
}
