// awesome/test/test_battery.cpp — the Pixel pill (shell/battery_pill.hpp):
// BatteryMeasurePolicy's widths at the bar's 13 px, and the raster's parts
// where they belong — the fill to ceil(level * 24 / 100) units, the digits
// inside the body, the attribution's ring cut out of it, a full body. Then
// the note's words (shell/battery_state.hpp), state by state.
#include "../shell/battery_pill.hpp"

#include "harness.hpp"

using namespace NAwesome::Shell::Battery;

namespace {
    struct SPx {
        int r, g, b, a;
    };
    // ARGB32 is premultiplied BGRA in memory on little-endian
    SPx at(cairo_surface_t* s, int x, int y) {
        const auto* D = cairo_image_surface_get_data(s) + y * cairo_image_surface_get_stride(s) + x * 4;
        return {D[2], D[1], D[0], D[3]};
    }
    cairo_surface_t* pill(int level, eAttribution a, SRgba fill = {1, 1, 1, 1}) {
        return rasterPill({.level = level, .attribution = a, .heightPx = 13, .fill = fill, .glyph = {1, 1, 1, 1}});
    }
} // namespace

bool test_battery() {
    bool ok = true;

    // 13 px: one device px a unit. The cap 1 off a 24 wide body; an
    // attribution overlaps the body by a fifth of its width instead.
    ok &= AW_CHECK(geometry(13, eAttribution::None).bodyW == 24 && geometry(13, eAttribution::None).width == 27);
    ok &= AW_CHECK(geometry(13, eAttribution::Charging).glyphX == 22 && geometry(13, eAttribution::Charging).width == 30);
    ok &= AW_CHECK(geometry(13, eAttribution::PowerSave).width == 31);
    ok &= AW_CHECK(geometry(13, eAttribution::Unknown).width == 29);
    ok &= AW_CHECK(geometry(26, eAttribution::None).bodyW == 48 && geometry(26, eAttribution::None).width == 53);
    ok &= AW_CHECK(fillWidth(0, 1) == 0 && fillWidth(1, 1) == 1 && fillWidth(62, 1) == 15 && fillWidth(100, 1) == 24);

    {
        // 62 %: the fill to x 15, the track past it, black digits between
        auto* S = pill(62, eAttribution::None);
        ok &= AW_CHECK(cairo_image_surface_get_width(S) == 27 && cairo_image_surface_get_height(S) == 13);
        ok &= AW_CHECK(at(S, 2, 6).a == 255 && at(S, 2, 6).r == 255);  // the fill
        ok &= AW_CHECK(at(S, 21, 6).a > 120 && at(S, 21, 6).a < 160); // the track, 0.55
        int dark = 0;
        for (int y = 2; y < 11; y++)
            for (int x = 4; x < 20; x++)
                dark += at(S, x, y).a > 200 && at(S, x, y).r < 140;
        ok &= AW_CHECK(dark > 15); // "62" in the body
        ok &= AW_CHECK(at(S, 25, 6).a > 100); // the cap
        cairo_surface_destroy(S);
    }
    {
        // charging: the bolt in a ring cut out of a full-width fill
        auto* S = pill(96, eAttribution::Charging, {0.2, 0.8, 0.35, 1});
        ok &= AW_CHECK(cairo_image_surface_get_width(S) == 30);
        ok &= AW_CHECK(at(S, 21, 7).a < 64);  // the ring, left of the bolt's point
        ok &= AW_CHECK(at(S, 25, 6).a == 255); // the bolt itself
        cairo_surface_destroy(S);
    }
    {
        // full: the whole body and the cap take the fill
        auto* S = pill(100, eAttribution::None, {1, 0, 0, 1});
        ok &= AW_CHECK(at(S, 22, 6).r == 255 && at(S, 22, 6).g == 0);
        ok &= AW_CHECK(at(S, 25, 6).r > 200 && at(S, 25, 6).g == 0);
        cairo_surface_destroy(S);
    }
    {
        // unknown: no digits on the dimmer track, the question beside it
        auto* S = pill(-1, eAttribution::Unknown);
        int   dark = 0;
        for (int y = 0; y < 13; y++)
            for (int x = 0; x < 20; x++)
                dark += at(S, x, y).a > 0 && at(S, x, y).r < at(S, x, y).a * 0.9;
        ok &= AW_CHECK(dark == 0);
        ok &= AW_CHECK(at(S, 10, 6).a > 100 && at(S, 10, 6).a < 125); // 0.45
        cairo_surface_destroy(S);
    }
    // the note: the ladder's order, the time only with a rate
    ok &= AW_CHECK(noteText({.level = 76, .minutes = 192}) == "76% · 3:12 left");
    ok &= AW_CHECK(noteText({.level = 76}) == "76%");
    ok &= AW_CHECK(noteText({.level = 63, .power = ePower::CHARGING, .minutes = 40, .fullAt = 22 * 60 + 40}) == "63% · Charging · full at 22:40");
    ok &= AW_CHECK(noteText({.level = 63, .power = ePower::CHARGING}) == "63% · Charging");
    ok &= AW_CHECK(noteText({.level = 100, .power = ePower::FULL}) == "100% · Charged");
    ok &= AW_CHECK(noteText({.level = 92, .power = ePower::HOLDING}) == "92% · Not charging");
    ok &= AW_CHECK(noteText({.level = 34, .powerSave = true, .minutes = 125}) == "34% · Power Saver · 2:05 left");
    ok &= AW_CHECK(noteText({.level = 78, .defender = true, .power = ePower::HOLDING, .limit = 80}) == "Held at 80% to spare the battery");
    ok &= AW_CHECK(noteText({.level = 78, .defender = true}) == "Held at 78% to spare the battery");
    ok &= AW_CHECK(noteText({.level = 50, .unknown = true, .minutes = 10}) == "Battery status unknown");
    ok &= AW_CHECK(noteText({.level = -1}) == "Battery status unknown");
    ok &= AW_CHECK(durationText(0) == "0:00" && durationText(605) == "10:05" && durationText(1000000) == "99:59");
    ok &= AW_CHECK(clockText(0) == "00:00" && clockText(9 * 60 + 5) == "09:05" && clockText(1440 + 61) == "01:01");
    return ok;
}
