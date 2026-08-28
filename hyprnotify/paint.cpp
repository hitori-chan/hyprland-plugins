// hyprnotify/paint.cpp — the paint context (glass, shadow, textures), the
// shared card recipes, the type scale and the motion curves. The config
// gates and color memos live in common/glass.hpp.

#include "ui.hpp"

#include <cairo/cairo.h>

#include <cmath>
#include <numbers>

namespace NHyprnotify {

    namespace {
        std::unordered_map<uint64_t, SP<ITexture>> controlIcons;

        uint64_t                                   controlIconKey(eControlIcon icon, int px, int glyphPx, const CHyprColor& color) {
            return ((uint64_t)icon << 56) ^ ((uint64_t)(uint32_t)px << 44) ^ ((uint64_t)(uint32_t)glyphPx << 32) ^ color.getAsHex();
        }

        void strokeIcon(cairo_t* cr, eControlIcon icon, double px) {
            const double S = px / 24.0;
            const double PI = std::numbers::pi;
            cairo_set_line_width(cr, std::max(1.0, 2.0 * S));
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

            // AOSP HEAD SystemUI's current 24dp OSD marks:
            // ic_brightness_full, ic_volume_media(_mute), ic_mic_26dp/off.
            const auto mediaVolume = [&]() {
                cairo_move_to(cr, 12.0 * S, 3.0 * S);
                cairo_line_to(cr, 12.01 * S, 13.55 * S);
                cairo_curve_to(cr, 11.42 * S, 13.21 * S, 10.74 * S, 13.0 * S, 10.01 * S, 13.0 * S);
                cairo_curve_to(cr, 7.79 * S, 13.0 * S, 6.0 * S, 14.79 * S, 6.0 * S, 17.0 * S);
                cairo_curve_to(cr, 6.0 * S, 19.21 * S, 7.79 * S, 21.0 * S, 10.01 * S, 21.0 * S);
                cairo_curve_to(cr, 12.22 * S, 21.0 * S, 14.0 * S, 19.21 * S, 14.0 * S, 17.0 * S);
                cairo_line_to(cr, 14.0 * S, 7.0 * S);
                cairo_line_to(cr, 18.0 * S, 7.0 * S);
                cairo_line_to(cr, 18.0 * S, 3.0 * S);
                cairo_close_path(cr);
                cairo_fill(cr);
            };
            const auto strike = [&]() {
                cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
                cairo_set_line_width(cr, std::max(2.0, 4.0 * S));
                cairo_move_to(cr, 3.0 * S, 3.0 * S);
                cairo_line_to(cr, 21.0 * S, 21.0 * S);
                cairo_stroke(cr);
                cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
                cairo_set_line_width(cr, std::max(1.0, 2.0 * S));
                cairo_move_to(cr, 2.8 * S, 2.8 * S);
                cairo_line_to(cr, 21.2 * S, 21.2 * S);
                cairo_stroke(cr);
            };
            const auto mic = [&]() {
                cairo_arc(cr, 12.0 * S, 5.0 * S, 3.0 * S, PI, 0);
                cairo_line_to(cr, 15.0 * S, 11.0 * S);
                cairo_arc(cr, 12.0 * S, 11.0 * S, 3.0 * S, 0, PI);
                cairo_close_path(cr);
                cairo_fill(cr);
                cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
                cairo_set_line_width(cr, std::max(1.0, 2.0 * S));
                cairo_move_to(cr, 12.0 * S, 5.0 * S);
                cairo_line_to(cr, 12.0 * S, 11.0 * S);
                cairo_stroke(cr);
                cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
                cairo_arc(cr, 12.0 * S, 11.0 * S, 6.0 * S, 0, PI);
                cairo_move_to(cr, 6.0 * S, 11.0 * S);
                cairo_curve_to(cr, 6.0 * S, 14.31 * S, 8.69 * S, 17.0 * S, 12.0 * S, 17.0 * S);
                cairo_curve_to(cr, 15.31 * S, 17.0 * S, 18.0 * S, 14.31 * S, 18.0 * S, 11.0 * S);
                cairo_move_to(cr, 12.0 * S, 17.0 * S);
                cairo_line_to(cr, 12.0 * S, 21.0 * S);
                cairo_move_to(cr, 9.0 * S, 21.0 * S);
                cairo_line_to(cr, 15.0 * S, 21.0 * S);
                cairo_stroke(cr);
            };
            const auto touchpad = [&]() {
                cairo_rectangle(cr, 4.0 * S, 4.0 * S, 16.0 * S, 16.0 * S);
                cairo_stroke(cr);
                cairo_move_to(cr, 4.0 * S, 15.5 * S);
                cairo_line_to(cr, 20.0 * S, 15.5 * S);
                cairo_stroke(cr);
            };
            const auto battery = [&]() {
                cairo_new_sub_path(cr);
                cairo_arc(cr, 17.0 * S, 7.0 * S, 2.0 * S, -PI / 2, 0);
                cairo_arc(cr, 17.0 * S, 17.0 * S, 2.0 * S, 0, PI / 2);
                cairo_arc(cr, 6.0 * S, 17.0 * S, 2.0 * S, PI / 2, PI);
                cairo_arc(cr, 6.0 * S, 7.0 * S, 2.0 * S, PI, 3 * PI / 2);
                cairo_close_path(cr);
                cairo_stroke(cr);
                cairo_rectangle(cr, 19.0 * S, 9.0 * S, 2.5 * S, 6.0 * S);
                cairo_fill(cr);
            };
            const auto dnd = [&]() {
                cairo_arc(cr, 12.0 * S, 12.0 * S, 8.0 * S, 0, 2 * PI);
                cairo_stroke(cr);
                cairo_move_to(cr, 7.0 * S, 12.0 * S);
                cairo_line_to(cr, 17.0 * S, 12.0 * S);
                cairo_stroke(cr);
            };
            const auto snooze = [&]() {
                // the v13 hold-menu mark (demo mi-snooze, the ROM stroke Z on
                // its clock ring) — kept as one cached 24dp recipe so font
                // fallback cannot alter it
                cairo_arc(cr, 11.3 * S, 12.4 * S, 7.55 * S, 0, 2 * PI);
                cairo_set_line_width(cr, std::max(1.0, 2.1 * S));
                cairo_stroke(cr);

                cairo_set_line_width(cr, std::max(1.0, 2.4 * S));
                cairo_move_to(cr, 5.9 * S, 2.3 * S);
                cairo_line_to(cr, 2.9 * S, 5.3 * S);
                cairo_stroke(cr);
                cairo_move_to(cr, 16.7 * S, 2.3 * S);
                cairo_line_to(cr, 19.7 * S, 5.3 * S);
                cairo_stroke(cr);

                cairo_set_line_width(cr, std::max(1.0, 2.3 * S));
                cairo_move_to(cr, 8.5 * S, 9.2 * S);
                cairo_line_to(cr, 14.2 * S, 9.2 * S);
                cairo_line_to(cr, 8.5 * S, 15.6 * S);
                cairo_line_to(cr, 14.2 * S, 15.6 * S);
                cairo_stroke(cr);
            };
            const auto alert = [&]() {
                                cairo_move_to(cr, 18.000 * S, 17.000 * S);
                                cairo_line_to(cr, 18.000 * S, 11.000 * S);
                                cairo_curve_to(cr, 18.000 * S, 7.930 * S, 16.370 * S, 5.360 * S, 13.500 * S, 4.680 * S);
                                cairo_line_to(cr, 13.500 * S, 4.000 * S);
                                cairo_curve_to(cr, 13.500 * S, 3.170 * S, 12.830 * S, 2.500 * S, 12.000 * S, 2.500 * S);
                                cairo_curve_to(cr, 11.170 * S, 2.500 * S, 10.500 * S, 3.170 * S, 10.500 * S, 4.000 * S);
                                cairo_line_to(cr, 10.500 * S, 4.680 * S);
                                cairo_curve_to(cr, 7.640 * S, 5.360 * S, 6.000 * S, 7.920 * S, 6.000 * S, 11.000 * S);
                                cairo_line_to(cr, 6.000 * S, 17.000 * S);
                                cairo_line_to(cr, 4.000 * S, 17.000 * S);
                                cairo_line_to(cr, 4.000 * S, 19.000 * S);
                                cairo_line_to(cr, 20.000 * S, 19.000 * S);
                                cairo_line_to(cr, 20.000 * S, 17.000 * S);
                                cairo_line_to(cr, 18.000 * S, 17.000 * S);
                                cairo_move_to(cr, 16.000 * S, 17.000 * S);
                                cairo_line_to(cr, 8.000 * S, 17.000 * S);
                                cairo_line_to(cr, 8.000 * S, 11.000 * S);
                                cairo_curve_to(cr, 8.000 * S, 8.520 * S, 9.510 * S, 6.500 * S, 12.000 * S, 6.500 * S);
                                cairo_curve_to(cr, 14.490 * S, 6.500 * S, 16.000 * S, 8.520 * S, 16.000 * S, 11.000 * S);
                                cairo_line_to(cr, 16.000 * S, 17.000 * S);
                                cairo_move_to(cr, 10.000 * S, 20.000 * S);
                                cairo_line_to(cr, 14.000 * S, 20.000 * S);
                                cairo_curve_to(cr, 14.000 * S, 21.100 * S, 13.100 * S, 22.000 * S, 12.000 * S, 22.000 * S);
                                cairo_curve_to(cr, 10.900 * S, 22.000 * S, 10.000 * S, 21.100 * S, 10.000 * S, 20.000 * S);
                                cairo_move_to(cr, 22.000 * S, 11.000 * S);
                                cairo_line_to(cr, 20.000 * S, 11.000 * S);
                                cairo_curve_to(cr, 20.000 * S, 8.260 * S, 18.770 * S, 5.810 * S, 16.840 * S, 4.160 * S);
                                cairo_line_to(cr, 18.250 * S, 2.750 * S);
                                cairo_curve_to(cr, 20.540 * S, 4.770 * S, 22.000 * S, 7.710 * S, 22.000 * S, 11.000 * S);
                                cairo_move_to(cr, 5.750 * S, 2.750 * S);
                                cairo_line_to(cr, 7.160 * S, 4.160 * S);
                                cairo_curve_to(cr, 5.230 * S, 5.810 * S, 4.000 * S, 8.260 * S, 4.000 * S, 11.000 * S);
                                cairo_line_to(cr, 2.000 * S, 11.000 * S);
                                cairo_curve_to(cr, 2.000 * S, 7.710 * S, 3.460 * S, 4.770 * S, 5.750 * S, 2.750 * S);
            };
            const auto alertSilent = [&]() {
                                cairo_move_to(cr, 12.000 * S, 22.000 * S);
                                cairo_curve_to(cr, 13.100 * S, 22.000 * S, 14.000 * S, 21.100 * S, 14.000 * S, 20.000 * S);
                                cairo_line_to(cr, 10.000 * S, 20.000 * S);
                                cairo_curve_to(cr, 10.000 * S, 21.100 * S, 10.900 * S, 22.000 * S, 12.000 * S, 22.000 * S);
                                cairo_move_to(cr, 16.000 * S, 16.000 * S);
                                cairo_line_to(cr, 2.810 * S, 2.810 * S);
                                cairo_line_to(cr, 1.390 * S, 4.220 * S);
                                cairo_line_to(cr, 6.240 * S, 9.070 * S);
                                cairo_curve_to(cr, 6.090 * S, 9.680 * S, 6.000 * S, 10.330 * S, 6.000 * S, 11.000 * S);
                                cairo_line_to(cr, 6.000 * S, 17.000 * S);
                                cairo_line_to(cr, 4.000 * S, 17.000 * S);
                                cairo_line_to(cr, 4.000 * S, 19.000 * S);
                                cairo_line_to(cr, 16.170 * S, 19.000 * S);
                                cairo_line_to(cr, 19.780 * S, 22.610 * S);
                                cairo_line_to(cr, 21.190 * S, 21.200 * S);
                                cairo_line_to(cr, 16.000 * S, 16.000 * S);
                                cairo_move_to(cr, 8.000 * S, 17.000 * S);
                                cairo_line_to(cr, 8.010 * S, 10.840 * S);
                                cairo_line_to(cr, 14.170 * S, 17.000 * S);
                                cairo_line_to(cr, 8.000 * S, 17.000 * S);
                                cairo_move_to(cr, 12.000 * S, 6.500 * S);
                                cairo_curve_to(cr, 14.490 * S, 6.500 * S, 16.000 * S, 8.520 * S, 16.000 * S, 11.000 * S);
                                cairo_line_to(cr, 16.000 * S, 13.170 * S);
                                cairo_line_to(cr, 18.000 * S, 15.170 * S);
                                cairo_line_to(cr, 18.000 * S, 11.000 * S);
                                cairo_curve_to(cr, 18.000 * S, 7.930 * S, 16.370 * S, 5.360 * S, 13.500 * S, 4.680 * S);
                                cairo_line_to(cr, 13.500 * S, 4.000 * S);
                                cairo_curve_to(cr, 13.500 * S, 3.170 * S, 12.830 * S, 2.500 * S, 12.000 * S, 2.500 * S);
                                cairo_curve_to(cr, 11.170 * S, 2.500 * S, 10.500 * S, 3.170 * S, 10.500 * S, 4.000 * S);
                                cairo_line_to(cr, 10.500 * S, 4.680 * S);
                                cairo_curve_to(cr, 9.720 * S, 4.860 * S, 9.050 * S, 5.200 * S, 8.460 * S, 5.630 * S);
                                cairo_line_to(cr, 9.930 * S, 7.100 * S);
                                cairo_curve_to(cr, 10.510 * S, 6.730 * S, 11.200 * S, 6.500 * S, 12.000 * S, 6.500 * S);
            };
            const auto priority = [&]() {
                // the ROM hold-menu priority mark (demo mi-priority: two
                // stroked chevrons)
                cairo_set_line_width(cr, std::max(1.0, 3.0 * S));
                cairo_move_to(cr, 3.0 * S, 4.5 * S);
                cairo_line_to(cr, 11.0 * S, 12.0 * S);
                cairo_line_to(cr, 3.0 * S, 19.5 * S);
                cairo_stroke(cr);
                cairo_move_to(cr, 11.0 * S, 4.5 * S);
                cairo_line_to(cr, 19.0 * S, 12.0 * S);
                cairo_line_to(cr, 11.0 * S, 19.5 * S);
                cairo_stroke(cr);
            };

            switch (icon) {
                case eControlIcon::APPS:
                    for (int y = 0; y < 3; y++)
                        for (int x = 0; x < 3; x++)
                            cairo_rectangle(cr, (5.0 + x * 6.0) * S, (5.0 + y * 6.0) * S, 3.0 * S, 3.0 * S);
                    cairo_fill(cr);
                    break;
                case eControlIcon::BATTERY:
                    battery();
                    break;
                case eControlIcon::BRIGHTNESS:
                    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
                    cairo_move_to(cr, 18.667 * S, 9.241 * S);
                    cairo_line_to(cr, 18.667 * S, 5.333 * S);
                    cairo_line_to(cr, 14.759 * S, 5.333 * S);
                    cairo_line_to(cr, 12.0 * S, 2.575 * S);
                    cairo_line_to(cr, 9.242 * S, 5.333 * S);
                    cairo_line_to(cr, 5.334 * S, 5.333 * S);
                    cairo_line_to(cr, 5.334 * S, 9.241 * S);
                    cairo_line_to(cr, 2.575 * S, 12.0 * S);
                    cairo_line_to(cr, 5.334 * S, 14.758 * S);
                    cairo_line_to(cr, 5.334 * S, 18.666 * S);
                    cairo_line_to(cr, 9.242 * S, 18.666 * S);
                    cairo_line_to(cr, 12.0 * S, 21.425 * S);
                    cairo_line_to(cr, 14.759 * S, 18.666 * S);
                    cairo_line_to(cr, 18.667 * S, 18.666 * S);
                    cairo_line_to(cr, 18.667 * S, 14.758 * S);
                    cairo_line_to(cr, 21.425 * S, 12.0 * S);
                    cairo_close_path(cr);
                    cairo_move_to(cr, 17.0 * S, 14.066 * S);
                    cairo_line_to(cr, 17.0 * S, 17.0 * S);
                    cairo_line_to(cr, 14.067 * S, 17.0 * S);
                    cairo_line_to(cr, 12.0 * S, 19.066 * S);
                    cairo_line_to(cr, 9.934 * S, 17.0 * S);
                    cairo_line_to(cr, 7.0 * S, 17.0 * S);
                    cairo_line_to(cr, 7.0 * S, 14.066 * S);
                    cairo_line_to(cr, 4.934 * S, 12.0 * S);
                    cairo_line_to(cr, 7.0 * S, 9.933 * S);
                    cairo_line_to(cr, 7.0 * S, 7.0 * S);
                    cairo_line_to(cr, 9.934 * S, 7.0 * S);
                    cairo_line_to(cr, 12.0 * S, 4.933 * S);
                    cairo_line_to(cr, 14.067 * S, 7.0 * S);
                    cairo_line_to(cr, 17.0 * S, 7.0 * S);
                    cairo_line_to(cr, 17.0 * S, 9.933 * S);
                    cairo_line_to(cr, 19.067 * S, 12.0 * S);
                    cairo_close_path(cr);
                    cairo_fill(cr);
                    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
                    cairo_arc(cr, 12.0 * S, 12.0 * S, 3.25 * S, 0, 2 * PI);
                    cairo_fill(cr);
                    break;
                case eControlIcon::VOLUME:
                    mediaVolume();
                    break;
                case eControlIcon::VOLUME_MUTED:
                    mediaVolume();
                    strike();
                    break;
                case eControlIcon::MICROPHONE:
                    mic();
                    break;
                case eControlIcon::MICROPHONE_MUTED:
                    mic();
                    strike();
                    break;
                case eControlIcon::TOUCHPAD:
                    touchpad();
                    break;
                case eControlIcon::TOUCHPAD_DISABLED:
                    touchpad();
                    cairo_move_to(cr, 4.0 * S, 4.0 * S);
                    cairo_line_to(cr, 20.0 * S, 20.0 * S);
                    cairo_stroke(cr);
                    break;
                case eControlIcon::DO_NOT_DISTURB:
                    dnd();
                    break;
                case eControlIcon::SNOOZE:
                    snooze();
                    break;
                case eControlIcon::PRIORITY: priority(); break;
                case eControlIcon::NOTIFICATION_ALERT: alert(); break;
                case eControlIcon::NOTIFICATION_SILENT: alertSilent(); break;
                // the v13 chevrons: Material's 960-box paths, scaled to the
                // box (the 24-box S above is only right for 24dp glyphs)
                case eControlIcon::EXPAND_MORE: {
                    const double G = px / 960.0;
                    cairo_move_to(cr, 480.0 * G, 616.0 * G);
                    cairo_line_to(cr, 184.0 * G, 320.0 * G);
                    cairo_line_to(cr, 248.0 * G, 256.0 * G);
                    cairo_line_to(cr, 480.0 * G, 488.0 * G);
                    cairo_line_to(cr, 712.0 * G, 256.0 * G);
                    cairo_line_to(cr, 776.0 * G, 320.0 * G);
                    cairo_close_path(cr);
                    cairo_fill(cr);
                    break;
                }
                case eControlIcon::EXPAND_LESS: {
                    const double G = px / 960.0;
                    cairo_move_to(cr, 480.0 * G, 344.0 * G);
                    cairo_line_to(cr, 776.0 * G, 640.0 * G);
                    cairo_line_to(cr, 712.0 * G, 704.0 * G);
                    cairo_line_to(cr, 480.0 * G, 472.0 * G);
                    cairo_line_to(cr, 248.0 * G, 704.0 * G);
                    cairo_line_to(cr, 184.0 * G, 640.0 * G);
                    cairo_close_path(cr);
                    cairo_fill(cr);
                    break;
                }
                case eControlIcon::HISTORY: {
                    // demo mi-history (CCW arc + arrowhead + clock hands)
                                cairo_move_to(cr, 13.000 * S, 3.000 * S);
                                cairo_curve_to(cr, 8.030 * S, 3.000 * S, 4.000 * S, 7.030 * S, 4.000 * S, 12.000 * S);
                                cairo_line_to(cr, 1.000 * S, 12.000 * S);
                                cairo_line_to(cr, 4.890 * S, 15.890 * S);
                                cairo_line_to(cr, 4.960 * S, 16.030 * S);
                                cairo_line_to(cr, 9.000 * S, 12.000 * S);
                                cairo_line_to(cr, 6.000 * S, 12.000 * S);
                                cairo_curve_to(cr, 6.000 * S, 8.130 * S, 9.130 * S, 5.000 * S, 13.000 * S, 5.000 * S);
                                cairo_curve_to(cr, 16.870 * S, 5.000 * S, 20.000 * S, 8.130 * S, 20.000 * S, 12.000 * S);
                                cairo_curve_to(cr, 20.000 * S, 15.870 * S, 16.870 * S, 19.000 * S, 13.000 * S, 19.000 * S);
                                cairo_curve_to(cr, 11.070 * S, 19.000 * S, 9.320 * S, 18.210 * S, 8.060 * S, 16.940 * S);
                                cairo_line_to(cr, 6.640 * S, 18.360 * S);
                                cairo_curve_to(cr, 8.270 * S, 19.990 * S, 10.510 * S, 21.000 * S, 13.000 * S, 21.000 * S);
                                cairo_curve_to(cr, 17.970 * S, 21.000 * S, 22.000 * S, 16.970 * S, 22.000 * S, 12.000 * S);
                                cairo_curve_to(cr, 22.000 * S, 7.030 * S, 17.970 * S, 3.000 * S, 13.000 * S, 3.000 * S);
                                cairo_move_to(cr, 12.000 * S, 8.000 * S);
                                cairo_line_to(cr, 12.000 * S, 13.000 * S);
                                cairo_line_to(cr, 16.280 * S, 15.540 * S);
                                cairo_line_to(cr, 17.000 * S, 14.330 * S);
                                cairo_line_to(cr, 13.500 * S, 12.250 * S);
                                cairo_line_to(cr, 13.500 * S, 8.000 * S);
                                cairo_line_to(cr, 12.000 * S, 8.000 * S);
                }
                case eControlIcon::GEAR: {
                    // demo mi-gear
                                cairo_move_to(cr, 19.140 * S, 12.940 * S);
                                cairo_curve_to(cr, 19.180 * S, 12.640 * S, 19.200 * S, 12.330 * S, 19.200 * S, 12.000 * S);
                                cairo_curve_to(cr, 19.200 * S, 11.680 * S, 19.180 * S, 11.360 * S, 19.130 * S, 11.060 * S);
                                cairo_line_to(cr, 21.160 * S, 9.480 * S);
                                cairo_curve_to(cr, 21.340 * S, 9.340 * S, 21.390 * S, 9.070 * S, 21.280 * S, 8.870 * S);
                                cairo_line_to(cr, 19.360 * S, 5.550 * S);
                                cairo_curve_to(cr, 19.240 * S, 5.330 * S, 18.990 * S, 5.260 * S, 18.770 * S, 5.330 * S);
                                cairo_line_to(cr, 16.380 * S, 6.290 * S);
                                cairo_curve_to(cr, 15.880 * S, 5.910 * S, 15.350 * S, 5.590 * S, 14.760 * S, 5.350 * S);
                                cairo_line_to(cr, 14.400 * S, 2.810 * S);
                                cairo_curve_to(cr, 14.360 * S, 2.570 * S, 14.160 * S, 2.400 * S, 13.920 * S, 2.400 * S);
                                cairo_line_to(cr, 10.080 * S, 2.400 * S);
                                cairo_curve_to(cr, 9.840 * S, 2.400 * S, 9.650 * S, 2.570 * S, 9.610 * S, 2.810 * S);
                                cairo_line_to(cr, 9.250 * S, 5.350 * S);
                                cairo_curve_to(cr, 8.660 * S, 5.590 * S, 8.120 * S, 5.920 * S, 7.630 * S, 6.290 * S);
                                cairo_line_to(cr, 5.240 * S, 5.330 * S);
                                cairo_curve_to(cr, 5.020 * S, 5.250 * S, 4.770 * S, 5.330 * S, 4.650 * S, 5.550 * S);
                                cairo_line_to(cr, 2.740 * S, 8.870 * S);
                                cairo_curve_to(cr, 2.620 * S, 9.080 * S, 2.660 * S, 9.340 * S, 2.860 * S, 9.480 * S);
                                cairo_line_to(cr, 4.890 * S, 11.060 * S);
                                cairo_curve_to(cr, 4.840 * S, 11.360 * S, 4.800 * S, 11.690 * S, 4.800 * S, 12.000 * S);
                                cairo_curve_to(cr, 4.800 * S, 12.310 * S, 4.820 * S, 12.640 * S, 4.870 * S, 12.940 * S);
                                cairo_line_to(cr, 2.840 * S, 14.520 * S);
                                cairo_curve_to(cr, 2.660 * S, 14.660 * S, 2.610 * S, 14.930 * S, 2.720 * S, 15.130 * S);
                                cairo_line_to(cr, 4.640 * S, 18.450 * S);
                                cairo_curve_to(cr, 4.760 * S, 18.670 * S, 5.010 * S, 18.740 * S, 5.230 * S, 18.670 * S);
                                cairo_line_to(cr, 7.620 * S, 17.710 * S);
                                cairo_curve_to(cr, 8.120 * S, 18.090 * S, 8.650 * S, 18.410 * S, 9.240 * S, 18.650 * S);
                                cairo_line_to(cr, 9.600 * S, 21.190 * S);
                                cairo_curve_to(cr, 9.650 * S, 21.430 * S, 9.840 * S, 21.600 * S, 10.080 * S, 21.600 * S);
                                cairo_line_to(cr, 13.920 * S, 21.600 * S);
                                cairo_curve_to(cr, 14.160 * S, 21.600 * S, 14.360 * S, 21.430 * S, 14.390 * S, 21.190 * S);
                                cairo_line_to(cr, 14.750 * S, 18.650 * S);
                                cairo_curve_to(cr, 15.340 * S, 18.410 * S, 15.880 * S, 18.090 * S, 16.370 * S, 17.710 * S);
                                cairo_line_to(cr, 18.760 * S, 18.670 * S);
                                cairo_curve_to(cr, 18.980 * S, 18.750 * S, 19.230 * S, 18.670 * S, 19.350 * S, 18.450 * S);
                                cairo_line_to(cr, 21.270 * S, 15.130 * S);
                                cairo_curve_to(cr, 21.390 * S, 14.910 * S, 21.340 * S, 14.660 * S, 21.150 * S, 14.520 * S);
                                cairo_line_to(cr, 19.140 * S, 12.940 * S);
                                cairo_move_to(cr, 12.000 * S, 15.600 * S);
                                cairo_curve_to(cr, 10.020 * S, 15.600 * S, 8.400 * S, 13.980 * S, 8.400 * S, 12.000 * S);
                                cairo_curve_to(cr, 8.400 * S, 10.020 * S, 10.020 * S, 8.400 * S, 12.000 * S, 8.400 * S);
                                cairo_curve_to(cr, 13.980 * S, 8.400 * S, 15.600 * S, 10.020 * S, 15.600 * S, 12.000 * S);
                                cairo_curve_to(cr, 15.600 * S, 13.980 * S, 13.980 * S, 15.600 * S, 12.000 * S, 15.600 * S);
                }
                case eControlIcon::DND_BELL_GEAR: {
                    // demo mi-dnd2 (bell + gear, the 960-box ROM mark)
                    const double G = px / 960.0;
                                cairo_move_to(cr, 480.000 * G, 864.000 * G);
                                cairo_curve_to(cr, 460.000 * G, 864.000 * G, 443.000 * G, 857.000 * G, 429.000 * G, 843.000 * G);
                                cairo_curve_to(cr, 415.000 * G, 829.000 * G, 408.000 * G, 812.000 * G, 408.000 * G, 792.000 * G);
                                cairo_line_to(cr, 552.000 * G, 792.000 * G);
                                cairo_curve_to(cr, 552.000 * G, 812.000 * G, 545.000 * G, 829.000 * G, 531.000 * G, 843.000 * G);
                                cairo_curve_to(cr, 517.000 * G, 857.000 * G, 500.000 * G, 864.000 * G, 480.000 * G, 864.000 * G);
                                cairo_move_to(cr, 192.000 * G, 744.000 * G);
                                cairo_line_to(cr, 192.000 * G, 672.000 * G);
                                cairo_line_to(cr, 240.000 * G, 672.000 * G);
                                cairo_line_to(cr, 240.000 * G, 432.000 * G);
                                cairo_curve_to(cr, 240.000 * G, 373.333 * G, 258.500 * G, 321.667 * G, 295.500 * G, 277.000 * G);
                                cairo_curve_to(cr, 332.500 * G, 232.333 * G, 380.333 * G, 205.333 * G, 439.000 * G, 196.000 * G);
                                cairo_curve_to(cr, 435.667 * G, 207.333 * G, 433.333 * G, 218.667 * G, 432.000 * G, 230.000 * G);
                                cairo_curve_to(cr, 430.667 * G, 241.333 * G, 430.000 * G, 252.667 * G, 430.000 * G, 264.000 * G);
                                cairo_curve_to(cr, 430.000 * G, 303.333 * G, 437.833 * G, 339.833 * G, 453.500 * G, 373.500 * G);
                                cairo_curve_to(cr, 469.167 * G, 407.167 * G, 490.333 * G, 436.333 * G, 517.000 * G, 461.000 * G);
                                cairo_curve_to(cr, 543.667 * G, 485.667 * G, 574.333 * G, 504.333 * G, 609.000 * G, 517.000 * G);
                                cairo_curve_to(cr, 643.667 * G, 529.667 * G, 680.667 * G, 534.333 * G, 720.000 * G, 531.000 * G);
                                cairo_line_to(cr, 720.000 * G, 672.000 * G);
                                cairo_line_to(cr, 768.000 * G, 672.000 * G);
                                cairo_line_to(cr, 768.000 * G, 744.000 * G);
                                cairo_line_to(cr, 192.000 * G, 744.000 * G);
                                cairo_move_to(cr, 664.000 * G, 456.000 * G);
                                cairo_line_to(cr, 652.000 * G, 400.000 * G);
                                cairo_curve_to(cr, 642.667 * G, 396.667 * G, 633.833 * G, 392.833 * G, 625.500 * G, 388.500 * G);
                                cairo_curve_to(cr, 617.167 * G, 384.167 * G, 609.333 * G, 378.667 * G, 602.000 * G, 372.000 * G);
                                cairo_line_to(cr, 547.000 * G, 389.000 * G);
                                cairo_line_to(cr, 515.000 * G, 334.000 * G);
                                cairo_line_to(cr, 556.000 * G, 294.000 * G);
                                cairo_curve_to(cr, 554.000 * G, 284.667 * G, 553.000 * G, 275.000 * G, 553.000 * G, 265.000 * G);
                                cairo_curve_to(cr, 553.000 * G, 255.000 * G, 554.000 * G, 245.333 * G, 556.000 * G, 236.000 * G);
                                cairo_line_to(cr, 515.000 * G, 197.000 * G);
                                cairo_line_to(cr, 547.000 * G, 141.000 * G);
                                cairo_line_to(cr, 601.000 * G, 157.000 * G);
                                cairo_curve_to(cr, 608.333 * G, 149.667 * G, 616.333 * G, 143.667 * G, 625.000 * G, 139.000 * G);
                                cairo_curve_to(cr, 633.667 * G, 134.333 * G, 642.667 * G, 130.667 * G, 652.000 * G, 128.000 * G);
                                cairo_line_to(cr, 665.000 * G, 72.000 * G);
                                cairo_line_to(cr, 729.000 * G, 72.000 * G);
                                cairo_line_to(cr, 742.000 * G, 128.000 * G);
                                cairo_curve_to(cr, 751.333 * G, 131.333 * G, 760.500 * G, 135.167 * G, 769.500 * G, 139.500 * G);
                                cairo_curve_to(cr, 778.500 * G, 143.833 * G, 786.333 * G, 149.667 * G, 793.000 * G, 157.000 * G);
                                cairo_line_to(cr, 847.000 * G, 142.000 * G);
                                cairo_line_to(cr, 879.000 * G, 197.000 * G);
                                cairo_line_to(cr, 839.000 * G, 235.000 * G);
                                cairo_curve_to(cr, 841.000 * G, 244.333 * G, 841.833 * G, 254.167 * G, 841.500 * G, 264.500 * G);
                                cairo_curve_to(cr, 841.167 * G, 274.833 * G, 840.000 * G, 284.667 * G, 838.000 * G, 294.000 * G);
                                cairo_line_to(cr, 879.000 * G, 333.000 * G);
                                cairo_line_to(cr, 847.000 * G, 388.000 * G);
                                cairo_line_to(cr, 792.000 * G, 372.000 * G);
                                cairo_curve_to(cr, 784.667 * G, 378.667 * G, 776.833 * G, 384.167 * G, 768.500 * G, 388.500 * G);
                                cairo_curve_to(cr, 760.167 * G, 392.833 * G, 751.333 * G, 396.667 * G, 742.000 * G, 400.000 * G);
                                cairo_line_to(cr, 728.000 * G, 456.000 * G);
                                cairo_line_to(cr, 664.000 * G, 456.000 * G);
                                cairo_move_to(cr, 697.000 * G, 336.000 * G);
                                cairo_curve_to(cr, 717.000 * G, 336.000 * G, 734.000 * G, 329.000 * G, 748.000 * G, 315.000 * G);
                                cairo_curve_to(cr, 762.000 * G, 301.000 * G, 769.000 * G, 284.000 * G, 769.000 * G, 264.000 * G);
                                cairo_curve_to(cr, 769.000 * G, 244.000 * G, 762.000 * G, 227.000 * G, 748.000 * G, 213.000 * G);
                                cairo_curve_to(cr, 734.000 * G, 199.000 * G, 717.000 * G, 192.000 * G, 697.000 * G, 192.000 * G);
                                cairo_curve_to(cr, 677.000 * G, 192.000 * G, 660.000 * G, 199.000 * G, 646.000 * G, 213.000 * G);
                                cairo_curve_to(cr, 632.000 * G, 227.000 * G, 625.000 * G, 244.000 * G, 625.000 * G, 264.000 * G);
                                cairo_curve_to(cr, 625.000 * G, 284.000 * G, 632.000 * G, 301.000 * G, 646.000 * G, 315.000 * G);
                                cairo_curve_to(cr, 660.000 * G, 329.000 * G, 677.000 * G, 336.000 * G, 697.000 * G, 336.000 * G);
                }
                case eControlIcon::SEND:
                    // demo mi-send (the reply field's paper plane)
                    cairo_move_to(cr, 2.01 * S, 21.0 * S);
                    cairo_line_to(cr, 23.0 * S, 12.0 * S);
                    cairo_line_to(cr, 2.01 * S, 3.0 * S);
                    cairo_line_to(cr, 2.0 * S, 10.0 * S);
                    cairo_line_to(cr, 17.0 * S, 12.0 * S);
                    cairo_line_to(cr, 2.0 * S, 14.0 * S);
                    cairo_close_path(cr);
                    cairo_fill(cr);
                    break;
                case eControlIcon::MEDIA:
                    // demo mi-cam (the preview's media glyph)
                    cairo_move_to(cr, 17.0 * S, 10.5 * S);
                    cairo_line_to(cr, 17.0 * S, 7.0 * S);
                    cairo_curve_to(cr, 17.0 * S, 6.45 * S, 16.55 * S, 6.0 * S, 16.0 * S, 6.0 * S);
                    cairo_line_to(cr, 4.0 * S, 6.0 * S);
                    cairo_curve_to(cr, 3.45 * S, 6.0 * S, 3.0 * S, 6.45 * S, 3.0 * S, 7.0 * S);
                    cairo_line_to(cr, 3.0 * S, 17.0 * S);
                    cairo_curve_to(cr, 3.0 * S, 17.55 * S, 3.45 * S, 18.0 * S, 4.0 * S, 18.0 * S);
                    cairo_line_to(cr, 16.0 * S, 18.0 * S);
                    cairo_curve_to(cr, 16.55 * S, 18.0 * S, 17.0 * S, 17.55 * S, 17.0 * S, 17.0 * S);
                    cairo_line_to(cr, 17.0 * S, 13.5 * S);
                    cairo_line_to(cr, 21.0 * S, 17.5 * S);
                    cairo_line_to(cr, 21.0 * S, 6.5 * S);
                    cairo_line_to(cr, 17.0 * S, 10.5 * S);
                    cairo_close_path(cr);
                    cairo_fill(cr);
                    break;
                case eControlIcon::CHECK:
                    cairo_move_to(cr, 9.0 * S, 16.17 * S);
                    cairo_line_to(cr, 4.83 * S, 12.0 * S);
                    cairo_line_to(cr, 3.41 * S, 13.41 * S);
                    cairo_line_to(cr, 9.0 * S, 19.0 * S);
                    cairo_line_to(cr, 21.0 * S, 7.0 * S);
                    cairo_line_to(cr, 19.59 * S, 5.59 * S);
                    cairo_close_path(cr);
                    cairo_fill(cr);
                    break;
            }
        }
    }

    SP<ITexture> controlIcon(eControlIcon icon, int physicalPx, const CHyprColor& color, double glyphRatio) {
        const int  PX  = std::clamp(physicalPx, 8, 256);
        const int  GPX = std::clamp((int)std::lround(PX * std::clamp(glyphRatio, 0.1, 1.0)), 8, PX);
        const auto KEY = controlIconKey(icon, PX, GPX, color);
        if (const auto IT = controlIcons.find(KEY); IT != controlIcons.end())
            return IT->second;
        if (!warmGate.mayBuild() || !g_pHyprRenderer)
            return {};

        auto* SURF = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, PX, PX);
        auto* CR   = cairo_create(SURF);
        cairo_set_source_rgba(CR, color.r, color.g, color.b, color.a);
        cairo_translate(CR, (PX - GPX) / 2.0, (PX - GPX) / 2.0);
        strokeIcon(CR, icon, GPX);
        cairo_destroy(CR);
        cairo_surface_flush(SURF);
        auto TEX = g_pHyprRenderer->createTexture(SURF);
        cairo_surface_destroy(SURF);
        if (!TEX)
            return {};
        if (controlIcons.size() >= 128)
            controlIcons.erase(controlIcons.begin());
        controlIcons.emplace(KEY, TEX);
        return TEX;
    }

    void controlIconCacheClear() {
        controlIcons.clear();
    }

    double damageMargin(PHLMONITOR m) {
        // hairlines ride outside boxes, glass grows by the blur radius, and
        // shadows reach further still (their range covers the no-blur case)
        return (m ? std::ceil(m->m_scale) : 1.0) + 1.0 + std::max(liveBlurNeeded() ? blurRadius() : 0.0, 26.0);
    }

    // ---- the radius family ----

    float rPow() {
        return (float)cfg.roundingPower->value();
    }
    int rPanel(double scale) {
        return (int)std::lround(PIXEL_SHADE_RADIUS * scale);
    }
    int rRow(double scale) {
        return std::max(0, (int)std::lround(std::max(0, (int)cfg.rounding->value()) * scale));
    }
    int rJoint(double scale) {
        return std::max(0, (int)std::lround(PIXEL_INTERNAL_RADIUS * scale));
    }

    // ---- motion ----

    float easeOutCubic(float t) {
        const float U = 1 - t;
        return 1 - U * U * U;
    }
    float easeOutBack(float t) { // the spatial overshoot (damping ~.75)
        const float U = t - 1;
        return 1 + 2.2f * U * U * U + 1.2f * U * U;
    }
    float animT(const Time::steady_tp& since, int ms) {
        const auto EL = std::chrono::duration_cast<std::chrono::milliseconds>(Time::steadyNow() - since).count();
        return std::clamp((float)EL / (float)ms, 0.f, 1.f);
    }

    // ---- the type scale (physical pt) ----

    SType typeScale(double scale) {
        const double FS = (double)cfg.fontSize->value();
        const auto   PT = [&](double logical) { return std::max(1, (int)std::lround(logical * scale)); };
        // the v13 roles off the 12px base: header/kicker 11, title 15,
        // body 13, small 11, actions 13, bar 13
        return SType{PT(FS - 1), PT(FS + 3), PT(FS + 1), PT(FS - 1), PT(FS + 1), PT(FS + 1)};
    }

    // ---- the paint context ----

    CBox SPaint::toPhys(const CBox& global) const {
        return CBox{global}.translate(Vector2D{-monPos.x, -monPos.y + dy}).scale(scale).round();
    }

    void SPaint::rect(const CBox& global, const CHyprColor& c, int round, float rp) const {
        if (warm)
            return;
        const auto B = toPhys(global);
        if (B.w <= 0 || B.h <= 0)
            return;
        g_pHyprOpenGL->renderRect(B, c.modifyA(c.a * alpha), {.round = round, .roundingPower = rp});
    }

    void SPaint::lineH(const CBox& global, const CHyprColor& c) const {
        if (warm)
            return;
        // one logical px, full width, at the box top: the kid separator
        const auto B = toPhys(CBox{global.x, global.y, global.w, std::max(1.0, global.h)});
        if (B.w <= 0)
            return;
        g_pHyprOpenGL->renderRect({B.x, B.y, B.w, (double)std::max(1, (int)std::lround(1 * scale))}, c.modifyA(c.a * alpha), {});
    }

    void SPaint::glass(const CBox& global, const CHyprColor& c, int round, float rp) const {
        if (warm)
            return;
        const auto B = toPhys(global);
        if (B.w <= 0 || B.h <= 0)
            return;
        g_pHyprOpenGL->renderRect(B, c.modifyA(c.a * alpha), {.round = round, .roundingPower = rp, .blur = blurOn() && c.a < 1.f, .blurA = alpha});
    }

    void SPaint::shadow(const CBox& global, int round, float rp, int range) const {
        if (warm)
            return;
        const auto B = toPhys(global);
        if (B.w <= 0 || B.h <= 0)
            return;
        static Config::CGradientValueData GRAD{CHyprColor{Theme::SHADOW}};
        g_pHyprOpenGL->renderRoundedShadow(B, round, rp, (int)std::lround(range * scale), GRAD, alpha);
    }

    void SPaint::ring(const CBox& global, const CHyprColor& c, int round, float rp, double px) const {
        if (warm)
            return;
        const auto B = toPhys(global);
        if (B.w <= 0 || B.h <= 0)
            return;
        // the gradient ctor heap-allocates and OkLab-converts — memoize per color
        static std::unordered_map<uint64_t, Config::CGradientValueData> grads;
        const auto                                                      KEY = c.getAsHex();
        auto                                                            IT  = grads.find(KEY);
        if (IT == grads.end())
            IT = grads.emplace(KEY, Config::CGradientValueData{c}).first;
        g_pHyprOpenGL->renderBorder(B, IT->second, {.round = round, .roundingPower = rp, .borderSize = std::max(1, (int)std::lround(px * scale)), .a = alpha});
    }

    void SPaint::tex(const SP<ITexture>& t, double gx, double gy) const {
        if (warm || !t || t->m_texID == 0)
            return;
        const auto P = toPhys(CBox{gx, gy, 1, 1});
        g_pHyprOpenGL->renderTexture(t, CBox{(double)P.x, (double)P.y, t->m_size.x, t->m_size.y}, {.a = alpha});
    }

    void SPaint::texClipped(const SP<ITexture>& t, double gx, double gy, const CBox& clip) const {
        if (warm || !t || t->m_texID == 0 || clip.empty())
            return;
        const auto P = toPhys(CBox{gx, gy, 1, 1});
        g_pHyprOpenGL->renderTexture(t, CBox{(double)P.x, (double)P.y, t->m_size.x, t->m_size.y}, {.a = alpha, .clipRegion = CRegion{toPhys(clip)}});
    }

    void SPaint::texFit(const SP<ITexture>& t, const CBox& cell, int round, float rp) const {
        if (warm || !t || t->m_texID == 0)
            return;
        const auto   B  = toPhys(cell);
        const double TW = t->m_size.x, TH = t->m_size.y;
        if (TW <= 0 || TH <= 0 || B.w <= 0 || B.h <= 0)
            return;
        const double S = std::min(B.w / TW, B.h / TH);
        const double W = TW * S, H = TH * S;
        CBox         F{B.x + (B.w - W) / 2.0, B.y + (B.h - H) / 2.0, W, H};
        const int    R = round > 0 ? std::min(round, (int)std::lround(std::min(W, H) / 2.0)) : 0;
        g_pHyprOpenGL->renderTexture(t, F.round(), {.a = alpha, .round = R, .roundingPower = rp});
    }

    void SPaint::texCover(const SP<ITexture>& t, const CBox& cell, int round, float rp) const {
        if (warm || !t || t->m_texID == 0)
            return;
        const auto   B  = toPhys(cell);
        const double TW = t->m_size.x, TH = t->m_size.y;
        if (TW <= 0 || TH <= 0 || B.w <= 0 || B.h <= 0)
            return;

        Vector2D     uv0{0, 0}, uv1{1, 1};
        const double TEX_AR = TW / TH, CELL_AR = B.w / B.h;
        if (TEX_AR > CELL_AR) {
            const double SPAN = CELL_AR / TEX_AR;
            uv0.x             = (1.0 - SPAN) / 2.0;
            uv1.x             = 1.0 - uv0.x;
        } else if (TEX_AR < CELL_AR) {
            const double SPAN = TEX_AR / CELL_AR;
            uv0.y             = (1.0 - SPAN) / 2.0;
            uv1.y             = 1.0 - uv0.y;
        }
        g_pHyprOpenGL->renderTexture(t, B,
                                     {.a = alpha, .round = round, .roundingPower = rp, .allowCustomUV = true, .primarySurfaceUVTopLeft = uv0, .primarySurfaceUVBottomRight = uv1});
    }

    // ---- shared card recipes (popups and center rows drifted apart once —
    //      the badge and the progress pill draw from ONE place now) ----

    static bool ready(const SP<ITexture>& texture) {
        return texture && texture->m_texID != 0;
    }

    // The v13 lead icon (2026-08-18 captures + demo sc7): an explicit
    // conversation icon wins; a 2+-sender group wears the APP icon; a 1:1
    // conversation wears the peer's avatar (with the app badge). `avatar`
    // marks the badge case.
    struct SLeadIcon {
        SP<ITexture> tex;
        bool         avatar = false;
    };

    static SLeadIcon conversationLead(const SNotif& n) {
        if (!n.conversation)
            return {};
        if (ready(n.conversationTex))
            return {n.conversationTex, false};
        if (n.conversationKind == "group" && n.participants.size() >= 2) {
            if (ready(n.identTex))
                return {n.identTex, false};
            if (ready(n.iconTex))
                return {n.iconTex, false};
            return {};
        }
        if (!n.participants.empty() && ready(n.participants.front().avatarTex))
            return {n.participants.front().avatarTex, true};
        if (ready(n.iconTex))
            return {n.iconTex, false};
        return {};
    }

    bool hasLeadIcon(const SNotif& n) {
        return ready(conversationLead(n).tex) || ready(n.identTex);
    }

    void paintProgress(const SPaint& P, double x, double y, double w, int pct, bool critical) {
        const int PR = (int)std::lround(PROGRESS_H / 2 * P.scale);
        P.rect(CBox{x, y, w, PROGRESS_H}, surfaceHigh(), PR);
        if (pct > 0)
            P.rect(CBox{x, y, std::max(w * pct / 100.0, PROGRESS_H), PROGRESS_H}, critical ? color(cfg.colUrgent) : color(cfg.colHighlight), PR);
    }

    // the pixel-parity expand affordance when no count rides it: a ~32px
    // circular grey disc with the chevron (the ROM card's top-right button)
    void paintChevronButton(const SPaint& P, double x, double y, bool open, bool hov) {
        P.rect(CBox{x, y, CHEV_BTN_D, CHEV_BTN_D}, hov ? v13RaisedH() : v13Chip(), (int)std::lround(CHEV_BTN_D / 2 * P.scale), 2.f);
        const auto CHEV = controlIcon(open ? eControlIcon::EXPAND_LESS : eControlIcon::EXPAND_MORE, (int)std::lround(CHEV_BTN_GLYPH * P.scale), hov ? v13On() : v13On82());
        if (CHEV)
            P.texFit(CHEV, CBox{x + (CHEV_BTN_D - CHEV_BTN_GLYPH) / 2, y + (CHEV_BTN_D - CHEV_BTN_GLYPH) / 2, CHEV_BTN_GLYPH, CHEV_BTN_GLYPH}, 0);
    }

    // v13: EVERY lead icon is a 40dp circle (demo .c-icon img{border-radius:
    // 50%}) — app identities crop to the circle as well. The app badge still
    // overhangs a 1:1 avatar; importance is shown in the hold menu, not as an
    // icon ring (the v13 cards are stroke-free).
    void paintIconColumn(const SPaint& P, const SNotif& n, const CBox& cell, bool withBadge, float rp) {
        auto LEAD = conversationLead(n);
        if (!ready(LEAD.tex)) {
            if (!ready(n.identTex))
                return;
            LEAD = {n.identTex, false};
        }
        const int R = (int)std::lround(cell.w / 2 * P.scale);
        P.texCover(LEAD.tex, cell, R, 2.f);
        if (!withBadge || !LEAD.avatar || !ready(n.identTex))
            return;
        const double D = cell.w * BADGE_D, IN = D * BADGE_INSET;
        const CBox   BB{cell.x + cell.w * (1 + BADGE_PROT) - D, cell.y + cell.h * (1 + BADGE_PROT) - D, D, D};
        // The rim's job is to cut the app glyph free of the avatar it sits on
        // (AOSP tints conversation_badge_background to the card background;
        // over glass a near-white disc separates the glyph on any avatar).
        P.rect(BB, CHyprColor{Theme::BADGE_RIM}, (int)std::lround(D / 2 * P.scale), 2.f);
        P.texFit(n.identTex, CBox{BB.x + IN, BB.y + IN, D - 2 * IN, D - 2 * IN}, (int)std::lround((D / 2 - IN) * P.scale), 2.f);
    }

    // The 15px circular mini avatar of a collapsed preview line (demo
    // .c-prev .mini): the sender's avatar, or a chip disc with the initial
    // when no avatar resolved (demo .mini.init).
    void paintMiniAvatar(const SPaint& P, const SNotif& n, const std::string& sender, const CBox& cell) {
        const int R = (int)std::lround(cell.w / 2 * P.scale);
        const auto IT = std::ranges::find_if(n.participants, [&](const auto& item) { return item.key == sender || item.name == sender; });
        if (IT != n.participants.end() && ready(IT->avatarTex)) {
            P.texCover(IT->avatarTex, cell, R, 2.f);
            return;
        }
        P.rect(cell, v13Chip(), R, 2.f);
        std::string init;
        if (!sender.empty()) {
            size_t b = 0;
            while (b < sender.size() && (sender[b] & 0xC0) == 0x80)
                ++b;
            init = Pixel::firstCodepoint(sender, b);
        }
        if (init.empty())
            init = "?";
        const auto GLYPH = cachedText(init, v13On(), (int)std::lround(10 * P.scale), (int)cell.w + 8, -1, 0, false, 600);
        if (GLYPH && GLYPH->tex)
            P.tex(GLYPH->tex, cell.x + (cell.w - GLYPH->tex->m_size.x / P.scale) / 2, cell.y + (cell.h - GLYPH->tex->m_size.y / P.scale) / 2);
    }

    void paintParticipantAvatar(const SPaint& P, const SNotif& n, const std::string_view participant, const CBox& cell) {
        const auto IT = std::ranges::find_if(n.participants, [&](const auto& item) { return item.key == participant; });
        if (IT == n.participants.end() || !ready(IT->avatarTex))
            return;
        P.texCover(IT->avatarTex, cell, (int)std::lround(cell.w / 2 * P.scale), 2.f);
    }

} // namespace NHyprnotify
