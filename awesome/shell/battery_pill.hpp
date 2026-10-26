// awesome/shell/battery_pill.hpp — the bar's battery glyph: Pixel's
// expressive battery, transcribed from SystemUI's Compose battery (Android
// 16 QPR2: BatteryFrame, BatteryGlyph, BatteryMeasurePolicy,
// UnifiedBattery.kt; Apache-2.0, the paths verbatim). On its 24 x 13
// viewport: a borderless round-rect body (r 4) as a translucent white
// track, the level a left-anchored fill clipped to ceil(level * 24 / 100)
// units, the number in BatteryGlyph's own digits (vector paths, not a
// font) centred as a row 0.8 apart, and to the right either the cap one
// unit off or the attribution glyph — the charging bolt, the charge-limit
// shield, the power-save plus, the unknown question — overlapping the body
// by a fifth of its width inside a 2-unit ring. Full, the whole body takes
// the fill. Pure cairo, so the unit harness rasters it.
//
// Two departures, both for the bar's glass: the ring is cut out of the body
// (the phone paints it black over its black status bar; over glass black is
// a halo), and the digit row starts on a whole device pixel.
#pragma once

#include "shell/battery_state.hpp"

#include <cairo/cairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace NAwesome::Shell::Battery {

    // BatteryMeasurePolicy rounds every positive child dimension on its own,
    // by the JVM's round-to-nearest (a .5 tie rounds up)
    constexpr int pixelRound(double v) {
        return static_cast<int>(v + 0.5);
    }

    // the viewport, in units
    inline constexpr double VIEW_W = 24, VIEW_H = 13, BODY_R = 4, CAP_GAP = 1, CAP_W = 1.5, CAP_H = 6.03, DIGIT_GAP = 0.8, RING = 2, OVERLAP = 0.2;
    // DarkTheme: the track 0.55 under digits, 0.45 with no level to show;
    // the digits black at 0.75
    inline constexpr double TRACK_ALPHA = 0.55, TRACK_ALPHA_UNKNOWN = 0.45, DIGIT_ALPHA = 0.75;

    // the attribution glyph's viewport (0 x 0: the cap instead)
    constexpr double attributionWidth(eAttribution a) {
        switch (a) {
            case eAttribution::Charging:
            case eAttribution::Defender: return 8.0;
            case eAttribution::PowerSave: return 8.5;
            case eAttribution::Unknown: return 6.0;
            case eAttribution::None: return 0.0;
        }
        return 0.0;
    }
    constexpr double attributionHeight(eAttribution a) {
        switch (a) {
            case eAttribution::Charging:
            case eAttribution::Defender: return 9.0;
            case eAttribution::PowerSave: return 8.5;
            case eAttribution::Unknown: return 10.0;
            case eAttribution::None: return 0.0;
        }
        return 0.0;
    }

    // device px, every child rounded on its own as BatteryMeasurePolicy does
    struct SGeometry {
        int bodyW = 0, bodyH = 0, capX = 0, capW = 0, capH = 0, glyphX = 0, glyphW = 0, glyphH = 0, width = 0;
    };

    constexpr SGeometry geometry(int heightPx, eAttribution a) {
        const double S = heightPx / VIEW_H;
        SGeometry    g;
        g.bodyW = pixelRound(VIEW_W * S);
        g.bodyH = heightPx;
        if (const double AW = attributionWidth(a); AW > 0) {
            g.glyphW = pixelRound(AW * S);
            g.glyphH = pixelRound(attributionHeight(a) * S);
            g.glyphX = pixelRound(g.bodyW - g.glyphW * OVERLAP);
            g.width  = g.bodyW + pixelRound(g.glyphW * (1 - OVERLAP));
        } else {
            g.capX  = g.bodyW + pixelRound(CAP_GAP * S);
            g.capW  = pixelRound(CAP_W * S);
            g.capH  = pixelRound(CAP_H * S);
            g.width = g.capX + g.capW;
        }
        return g;
    }

    // the fill's width in device px: ceil(level * 24 / 100) whole units
    constexpr double fillWidth(int level, double unitPx) {
        return level <= 0 ? 0.0 : std::ceil(std::min(level, 100) * VIEW_W / 100.0) * unitPx;
    }

    namespace Detail {
        struct SPathGlyph {
            double      w, h;
            const char* d;
        };

        // BatteryGlyph's digits 0-9 (w, h, path), its cap and its attribution
        // glyphs (the bolt, the defender shield, the power-save plus, the
        // question), verbatim
        inline constexpr SPathGlyph DIGITS[10] = {
            {7.07, 9.01,
             "M3.578,9.01C2.502,9.01 1.636,8.598 0.98,7.774C0.328,6.95 0.002,5.858 0.002,4.498C0.002,3.138 0.328,2.048 0.98,1.228C1.636,0.408 2.502,-0.002 3.578,-0.002C4.662,-0.002 "
             "5.532,0.408 6.188,1.228C6.844,2.048 7.172,3.138 7.172,4.498C7.172,5.858 6.844,6.95 6.188,7.774C5.532,8.598 4.662,9.01 3.578,9.01ZM3.59,7.456C4.214,7.456 4.684,7.19 "
             "5,6.658C5.32,6.122 5.48,5.402 5.48,4.498C5.48,3.594 5.32,2.878 5,2.35C4.684,1.818 4.214,1.552 3.59,1.552C2.966,1.552 2.494,1.818 2.174,2.35C1.854,2.878 1.694,3.594 "
             "1.694,4.498C1.694,5.402 1.854,6.122 2.174,6.658C2.494,7.19 2.966,7.456 3.59,7.456Z"},
            {3.89, 8.75,
             "M3.062,8.75C2.834,8.75 2.638,8.668 2.474,8.504C2.314,8.34 2.234,8.142 2.234,7.91L2.234,2.252L1.16,3.032C0.996,3.148 0.816,3.19 0.62,3.158C0.424,3.126 0.266,3.032 "
             "0.146,2.876C0.026,2.716 -0.018,2.534 0.014,2.33C0.046,2.126 0.146,1.966 0.314,1.85L2.642,0.17C2.698,0.126 2.76,0.086 2.828,0.05C2.9,0.014 2.992,-0.004 "
             "3.104,-0.004C3.328,-0.004 3.516,0.076 3.668,0.236C3.82,0.392 3.896,0.584 3.896,0.812L3.896,7.91C3.896,8.142 3.814,8.34 3.65,8.504C3.486,8.668 3.29,8.75 3.062,8.75Z"},
            {6.07, 8.8,
             "M0.922,8.8C0.662,8.8 0.442,8.714 0.262,8.542C0.086,8.366 -0.002,8.152 -0.002,7.9C-0.002,7.756 0.028,7.628 0.088,7.516C0.148,7.404 0.218,7.31 "
             "0.298,7.234L2.77,4.762C3.166,4.378 "
             "3.482,4.022 3.718,3.694C3.958,3.366 4.078,3.038 4.078,2.71C4.078,2.358 3.97,2.076 3.754,1.864C3.542,1.648 3.248,1.54 2.872,1.54C2.628,1.54 2.426,1.578 "
             "2.266,1.654C2.106,1.73 "
             "1.96,1.84 1.828,1.984C1.696,2.124 1.58,2.276 1.48,2.44C1.384,2.604 1.244,2.714 1.06,2.77C0.88,2.822 0.696,2.804 0.508,2.716C0.324,2.624 0.2,2.478 "
             "0.136,2.278C0.072,2.078 "
             "0.094,1.854 0.202,1.606C0.31,1.358 0.494,1.104 0.754,0.844C1.018,0.58 1.328,0.374 1.684,0.226C2.04,0.074 2.462,-0.002 2.95,-0.002C3.79,-0.002 4.47,0.242 "
             "4.99,0.73C5.514,1.218 "
             "5.776,1.842 5.776,2.602C5.776,3.094 5.664,3.544 5.44,3.952C5.216,4.36 4.848,4.806 4.336,5.29L2.308,7.258L2.326,7.3L5.314,7.3C5.522,7.3 5.7,7.374 5.848,7.522C5.996,7.67 "
             "6.07,7.846 6.07,8.05C6.07,8.254 5.996,8.43 5.848,8.578C5.7,8.726 5.522,8.8 5.314,8.8L0.922,8.8Z"},
            {6.18, 9.01,
             "M2.968,9.01C2.408,9.01 1.934,8.93 1.546,8.77C1.158,8.606 0.83,8.378 0.562,8.086C0.298,7.794 0.128,7.526 0.052,7.282C-0.024,7.038 -0.022,6.83 0.058,6.658C0.142,6.482 "
             "0.274,6.36 0.454,6.292C0.634,6.22 0.806,6.212 0.97,6.268C1.138,6.32 1.262,6.422 1.342,6.574C1.426,6.722 1.54,6.876 1.684,7.036C1.832,7.196 2.006,7.324 "
             "2.206,7.42C2.41,7.512 "
             "2.658,7.558 2.95,7.558C3.402,7.558 3.77,7.438 4.054,7.198C4.342,6.954 4.486,6.644 4.486,6.268C4.486,5.868 4.352,5.558 4.084,5.338C3.82,5.118 3.442,5.008 "
             "2.95,5.008L2.77,5.008C2.59,5.008 2.434,4.944 2.302,4.816C2.174,4.688 2.11,4.536 2.11,4.36C2.11,4.18 2.174,4.026 "
             "2.302,3.898C2.43,3.77 2.582,3.706 2.758,3.706L2.872,3.706C3.276,3.706 3.596,3.608 3.832,3.412C4.072,3.212 4.192,2.922 4.192,2.542C4.192,2.21 4.076,1.944 "
             "3.844,1.744C3.616,1.54 "
             "3.306,1.438 2.914,1.438C2.674,1.438 2.468,1.476 2.296,1.552C2.128,1.624 1.978,1.726 1.846,1.858C1.714,1.986 1.604,2.11 1.516,2.23C1.432,2.35 1.306,2.432 "
             "1.138,2.476C0.974,2.516 "
             "0.806,2.492 0.634,2.404C0.466,2.312 0.354,2.176 0.298,1.996C0.242,1.812 0.26,1.618 0.352,1.414C0.444,1.206 0.618,0.982 0.874,0.742C1.134,0.502 1.436,0.318 "
             "1.78,0.19C2.128,0.062 "
             "2.542,-0.002 3.022,-0.002C3.918,-0.002 4.61,0.218 5.098,0.658C5.586,1.098 5.83,1.652 5.83,2.32C5.83,2.804 5.708,3.204 5.464,3.52C5.22,3.836 4.872,4.064 "
             "4.42,4.204L4.42,4.252C4.972,4.376 "
             "5.402,4.618 5.71,4.978C6.018,5.338 6.172,5.806 6.172,6.382C6.172,7.126 5.89,7.75 5.326,8.254C4.766,8.758 3.98,9.01 2.968,9.01Z"},
            {6.91, 8.79,
             "M4.882,8.79C4.662,8.79 4.47,8.71 4.306,8.55C4.146,8.386 4.066,8.19 "
             "4.066,7.962L4.066,6.198L4.18,5.916L4.18,1.422L4.834,2.04L4.144,2.04L1.624,5.526L4.726,5.526L5.116,5.46L6.19,5.46C6.386,5.46 "
             "6.554,5.53 6.694,5.67C6.834,5.81 6.904,5.978 6.904,6.174C6.904,6.37 6.834,6.538 6.694,6.678C6.554,6.818 6.386,6.888 6.19,6.888L1,6.888C0.716,6.888 0.478,6.796 "
             "0.286,6.612C0.094,6.428 "
             "-0.002,6.202 -0.002,5.934C-0.002,5.802 0.018,5.692 0.058,5.604C0.098,5.516 0.146,5.432 0.202,5.352L3.724,0.48C3.82,0.348 3.946,0.236 4.102,0.144C4.262,0.048 4.436,-0 "
             "4.624,-0C4.928,-0 "
             "5.184,0.11 5.392,0.33C5.6,0.546 5.704,0.81 5.704,1.122L5.704,7.962C5.704,8.19 5.622,8.386 5.458,8.55C5.298,8.71 5.106,8.79 4.882,8.79Z"},
            {6.07, 8.8,
             "M2.94,8.794C2.392,8.794 1.92,8.71 1.524,8.542C1.132,8.374 0.8,8.14 0.528,7.84C0.26,7.54 0.096,7.27 0.036,7.03C-0.024,6.79 -0.008,6.586 0.084,6.418C0.176,6.25 "
             "0.306,6.134 "
             "0.474,6.07C0.642,6.006 0.808,5.998 0.972,6.046C1.14,6.094 1.266,6.188 1.35,6.328C1.434,6.468 1.542,6.618 1.674,6.778C1.81,6.934 1.976,7.06 2.172,7.156C2.368,7.252 "
             "2.606,7.3 "
             "2.886,7.3C3.346,7.3 3.716,7.164 3.996,6.892C4.276,6.62 4.416,6.256 4.416,5.8C4.416,5.36 4.278,5.006 4.002,4.738C3.726,4.47 3.38,4.336 2.964,4.336C2.728,4.336 2.522,4.38 "
             "2.346,4.468C2.17,4.552 2.03,4.632 1.926,4.708C1.77,4.808 1.608,4.876 1.44,4.912C1.276,4.948 1.094,4.926 0.894,4.846C0.682,4.762 0.516,4.614 0.396,4.402C0.28,4.19 "
             "0.236,3.968 "
             "0.264,3.736L0.624,0.904C0.652,0.656 0.766,0.444 0.966,0.268C1.166,0.088 1.392,-0.002 1.644,-0.002L4.89,-0.002C5.098,-0.002 5.274,0.072 5.418,0.22C5.566,0.364 5.64,0.536 "
             "5.64,0.736C5.64,0.94 5.566,1.114 5.418,1.258C5.274,1.402 5.098,1.474 4.89,1.474L1.956,1.474L1.692,3.538L1.734,3.55C1.95,3.374 2.194,3.238 2.466,3.142C2.742,3.042 "
             "3.052,2.992 "
             "3.396,2.992C4.16,2.992 4.796,3.256 5.304,3.784C5.816,4.312 6.072,4.99 6.072,5.818C6.072,6.698 5.782,7.414 5.202,7.966C4.626,8.518 3.872,8.794 2.94,8.794Z"},
            {6.5, 8.91,
             "M3.26,8.914C2.28,8.914 1.492,8.624 0.896,8.044C0.3,7.464 0.002,6.714 0.002,5.794C0.002,5.194 0.156,4.612 0.464,4.048C0.772,3.484 1.23,2.854 "
             "1.838,2.158L3.404,0.274C3.536,0.118 "
             "3.708,0.03 3.92,0.01C4.136,-0.014 4.324,0.038 4.484,0.166C4.66,0.306 4.758,0.486 4.778,0.706C4.802,0.926 4.74,1.118 4.592,1.282L3.548,2.548C3.352,2.78 3.034,3.04 "
             "2.594,3.328C2.158,3.612 "
             "1.764,4.438 1.412,5.806L0.164,5.788C0.368,4.808 0.816,4.094 1.508,3.646C2.2,3.198 2.948,2.974 3.752,2.974C4.524,2.974 5.174,3.242 5.702,3.778C6.234,4.314 6.5,4.996 "
             "6.5,5.824C6.5,6.696 "
             "6.198,7.43 5.594,8.026C4.994,8.618 4.216,8.914 3.26,8.914ZM3.254,7.456C3.718,7.456 4.096,7.31 4.388,7.018C4.68,6.722 4.826,6.332 4.826,5.848C4.826,5.372 4.678,4.988 "
             "4.382,4.696C4.09,4.404 "
             "3.714,4.258 3.254,4.258C2.79,4.258 2.41,4.402 2.114,4.69C1.818,4.978 1.67,5.364 1.67,5.848C1.67,6.328 1.818,6.716 2.114,7.012C2.41,7.308 2.79,7.456 3.254,7.456Z"},
            {6.11, 8.64,
             "M1.322,8.536C1.114,8.432 0.976,8.268 0.908,8.044C0.844,7.82 0.868,7.606 0.98,7.402L4.136,1.546L4.112,1.504L0.758,1.504C0.546,1.504 0.366,1.43 0.218,1.282C0.07,1.134 "
             "-0.004,0.958 "
             "-0.004,0.754C-0.004,0.546 0.07,0.368 0.218,0.22C0.366,0.072 0.546,-0.002 0.758,-0.002L5.06,-0.002C5.356,-0.002 5.604,0.094 5.804,0.286C6.008,0.478 6.11,0.716 "
             "6.11,1C6.11,1.108 "
             "6.094,1.206 6.062,1.294C6.03,1.378 5.996,1.456 5.96,1.528L2.438,8.182C2.334,8.386 2.172,8.522 1.952,8.59C1.732,8.662 1.522,8.644 1.322,8.536Z"},
            {6.31, 9.01,
             "M3.15,9.01C2.19,9.01 1.424,8.766 0.852,8.278C0.284,7.79 0,7.164 0,6.4C0,5.88 0.148,5.43 0.444,5.05C0.74,4.666 1.12,4.396 1.584,4.24L1.584,4.198C1.22,4.03 0.918,3.79 "
             "0.678,3.478C0.438,3.162 "
             "0.318,2.786 0.318,2.35C0.318,1.654 0.578,1.088 1.098,0.652C1.618,0.216 2.302,-0.002 3.15,-0.002C4.002,-0.002 4.686,0.216 5.202,0.652C5.722,1.088 5.982,1.654 "
             "5.982,2.35C5.982,2.786 "
             "5.86,3.166 5.616,3.49C5.372,3.81 5.072,4.046 4.716,4.198L4.716,4.24C5.18,4.396 5.56,4.658 5.856,5.026C6.156,5.394 6.306,5.85 6.306,6.394C6.306,7.162 6.02,7.79 "
             "5.448,8.278C4.876,8.766 "
             "4.11,9.01 3.15,9.01ZM3.15,7.6C3.618,7.6 3.986,7.476 4.254,7.228C4.522,6.98 4.656,6.658 4.656,6.262C4.656,5.866 4.522,5.54 4.254,5.284C3.99,5.024 3.622,4.894 "
             "3.15,4.894C2.69,4.894 "
             "2.324,5.02 2.052,5.272C1.784,5.524 1.65,5.85 1.65,6.25C1.65,6.646 1.784,6.97 2.052,7.222C2.32,7.474 2.686,7.6 3.15,7.6ZM3.15,3.67C3.534,3.67 3.838,3.564 "
             "4.062,3.352C4.29,3.14 "
             "4.404,2.862 4.404,2.518C4.404,2.174 4.29,1.898 4.062,1.69C3.834,1.482 3.53,1.378 3.15,1.378C2.77,1.378 2.466,1.482 2.238,1.69C2.01,1.898 1.896,2.174 "
             "1.896,2.518C1.896,2.862 "
             "2.01,3.14 2.238,3.352C2.466,3.564 2.77,3.67 3.15,3.67Z"},
            {6.49, 8.91,
             "M3.244,0.004C4.22,0.004 5.006,0.296 5.602,0.88C6.198,1.46 6.496,2.208 6.496,3.124C6.496,3.724 6.342,4.306 6.034,4.87C5.73,5.434 5.272,6.066 "
             "4.66,6.766L3.1,8.644C2.964,8.8 2.79,8.888 "
             "2.578,8.908C2.366,8.932 2.178,8.88 2.014,8.752C1.838,8.612 1.74,8.434 1.72,8.218C1.7,7.998 1.762,7.804 1.906,7.636L2.956,6.37C3.148,6.142 3.462,5.884 "
             "3.898,5.596C4.338,5.308 4.734,4.48 "
             "5.086,3.112L6.334,3.13C6.13,4.11 5.682,4.824 4.99,5.272C4.298,5.72 3.55,5.944 2.746,5.944C1.974,5.944 1.324,5.676 0.796,5.14C0.268,4.604 0.004,3.922 "
             "0.004,3.094C0.004,2.222 0.304,1.49 "
             "0.904,0.898C1.508,0.302 2.288,0.004 3.244,0.004ZM3.244,1.462C2.784,1.462 2.406,1.61 2.11,1.906C1.818,2.198 1.672,2.586 1.672,3.07C1.672,3.55 1.818,3.936 "
             "2.11,4.228C2.406,4.52 2.784,4.666 "
             "3.244,4.666C3.708,4.666 4.088,4.522 4.384,4.234C4.68,3.942 4.828,3.556 4.828,3.076C4.828,2.592 4.68,2.202 4.384,1.906C4.088,1.61 3.708,1.462 3.244,1.462Z"},
        };
        inline constexpr SPathGlyph BOLT{
            8.0, 9.0,
            "M7.672,3.375L4.302,3.375L5.038,0.563C5.113,0.281 4.91,0 4.633,0C4.515,0 4.398,0.056 4.324,0.146L0.09,5.051C-0.102,5.276 0.047,5.625 "
            "0.335,5.625L3.705,5.625L2.969,8.438C2.895,8.719 3.097,9 3.374,9C3.492,9 3.609,8.944 3.684,8.854L7.917,3.949C8.109,3.724 7.96,3.375 7.672,3.375Z"};
        inline constexpr SPathGlyph CAP{1.5, 6.03,
                                         "M0.3333,-0.0037L0,-0.0037L0,6.0234L0.3333,6.0234C0.9777,6.0234 1.5,5.5011 1.5,4.8567L1.5,1.163C1.5,0.5187 0.9777,-0.0037 0.3333,-0.0037Z"};
        inline constexpr SPathGlyph DEFEND{
            8.0, 9.0,
            "M-0.004,2.337C-0.004,2.09 0.145,1.865 0.373,1.787C1.048,1.551 2.487,0.989 3.642,0.124C3.745,0.045 3.87,0 3.996,0C4.122,0 4.247,0.045 "
            "4.35,0.124C5.504,0.989 6.944,1.551 7.619,1.787C7.859,1.865 8.007,2.09 7.996,2.337C7.801,7.461 4.236,9 3.996,9C3.756,9 0.191,7.461 "
            "-0.004,2.337ZM3.996,7.449C4.259,7.285 4.605,7.024 4.956,6.643C5.616,5.927 6.335,4.749 6.526,2.885C5.841,2.614 4.892,2.187 3.996,1.602L3.996,7.449Z"};
        inline constexpr SPathGlyph PLUS{
            8.5, 8.5,
            "M4.248,0C4.745,0 5.148,0.403 5.148,0.9V3.35H7.6C8.097,3.35 8.5,3.753 8.5,4.25C8.5,4.747 8.097,5.149 7.6,5.149H5.148V7.6C5.148,8.097 4.745,8.5 "
            "4.248,8.5C3.751,8.5 3.349,8.097 3.349,7.6V5.149H0.9C0.403,5.149 0,4.747 0,4.25C0,3.753 0.403,3.35 0.9,3.35H3.349V0.9C3.349,0.403 3.751,0 4.248,0Z"};
        inline constexpr SPathGlyph QUESTION{
            6.0, 10.0,
            "M2.85,6.438C2.591,6.438 2.363,6.356 2.167,6.193C1.975,6.025 1.879,5.823 1.879,5.588V5.557C1.879,5.209 1.958,4.911 2.117,4.663C2.276,4.416 2.545,4.143 2.925,3.845C3.276,3.572 3.537,3.346 3.708,3.166C3.883,2.985 3.971,2.792 3.971,2.587C3.971,2.31 3.869,2.091 3.664,1.932C3.464,1.768 3.188,1.687 2.837,1.687C2.616,1.687 2.418,1.722 2.242,1.794C2.067,1.865 1.919,1.961 1.798,2.083C1.677,2.205 1.568,2.322 1.472,2.435C1.38,2.545 1.242,2.62 1.059,2.662C0.879,2.7 0.687,2.67 0.482,2.574C0.282,2.477 0.14,2.32 0.057,2.102C-0.023,1.884 -0.019,1.668 0.069,1.454C0.161,1.24 0.34,1.015 0.608,0.78C0.879,0.541 1.207,0.352 1.591,0.214C1.975,0.071 2.426,0 2.944,0C3.866,0 4.605,0.231 5.161,0.692C5.72,1.15 6,1.739 6,2.461C6,2.897 5.889,3.287 5.668,3.631C5.451,3.971 5.098,4.315 4.61,4.663C4.326,4.869 4.127,5.049 4.015,5.205C3.902,5.36 3.835,5.546 3.814,5.765V5.777C3.781,5.945 3.679,6.098 3.507,6.237C3.336,6.371 3.117,6.438 2.85,6.438ZM2.837,10C2.495,10 2.205,9.885 1.967,9.654C1.733,9.423 1.616,9.14 1.616,8.804C1.616,8.477 1.733,8.2 1.967,7.974C2.205,7.747 2.495,7.634 2.837,7.634C3.18,7.634 3.47,7.747 3.708,7.974C3.95,8.2 4.071,8.477 4.071,8.804C4.071,9.14 3.952,9.423 3.714,9.654C3.476,9.885 3.184,10 2.837,10Z"};

        // the row's width in units: the digits and the 0.8 between them
        inline double rowWidth(const char* digits) {
            double w = 0;
            for (const char* c = digits; *c; c++)
                w += DIGITS[*c - '0'].w + (c != digits ? DIGIT_GAP : 0.0);
            return w;
        }

        // just enough SVG for the paths above: absolute M/L/H/V/C/Z
        inline void playPath(cairo_t* cr, const char* d) {
            const char* p   = d;
            char        cmd = 0;
            double      x = 0, y = 0;
            const auto  num = [&]() {
                while (*p == ',' || *p == ' ')
                    p++;
                char*        e;
                const double V = std::strtod(p, &e);
                p              = e;
                return V;
            };
            while (*p) {
                if (*p == ',' || *p == ' ') {
                    p++;
                    continue;
                }
                if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
                    cmd = *p++;
                    if (cmd == 'Z') {
                        cairo_close_path(cr);
                        cmd = 0;
                        continue;
                    }
                }
                switch (cmd) {
                    case 'M':
                        x = num(), y = num();
                        cairo_move_to(cr, x, y);
                        cmd = 'L'; // SVG: further pairs after M are lineto
                        break;
                    case 'L':
                        x = num(), y = num();
                        cairo_line_to(cr, x, y);
                        break;
                    case 'H':
                        x = num();
                        cairo_line_to(cr, x, y);
                        break;
                    case 'V':
                        y = num();
                        cairo_line_to(cr, x, y);
                        break;
                    case 'C': {
                        const double C1X = num(), C1Y = num(), C2X = num(), C2Y = num();
                        x = num(), y = num();
                        cairo_curve_to(cr, C1X, C1Y, C2X, C2Y, x, y);
                        break;
                    }
                    default: return; // unknown command: stop rather than misdraw
                }
            }
        }

        inline const SPathGlyph* attributionGlyph(eAttribution a) {
            switch (a) {
                case eAttribution::Charging: return &BOLT;
                case eAttribution::Defender: return &DEFEND;
                case eAttribution::PowerSave: return &PLUS;
                case eAttribution::Unknown: return &QUESTION;
                case eAttribution::None: return nullptr;
            }
            return nullptr;
        }

        // a glyph's path at (x, y) device px, scaled from its units
        inline void glyphPath(cairo_t* cr, const SPathGlyph& g, double x, double y, double scale) {
            cairo_save(cr);
            cairo_translate(cr, x, y);
            cairo_scale(cr, scale, scale);
            playPath(cr, g.d);
            cairo_restore(cr);
        }
    } // namespace Detail

    struct SRgba {
        double r = 1, g = 1, b = 1, a = 1;
    };

    struct SPillSpec {
        int          level       = -1; // -1: unknown, no digits
        eAttribution attribution = eAttribution::None;
        int          heightPx    = 13;
        SRgba        fill;  // the level: white, or the state's colour
        SRgba        glyph; // the attribution glyph: the bar's text colour
    };

    // The pill on a fresh ARGB32 surface of geometry().width x heightPx; the
    // caller owns it. Null if cairo cannot allocate.
    inline cairo_surface_t* rasterPill(const SPillSpec& spec) {
        const auto G    = geometry(spec.heightPx, spec.attribution);
        auto*      SURF = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, std::max(1, G.width), std::max(1, G.bodyH));
        if (cairo_surface_status(SURF) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(SURF);
            return nullptr;
        }
        auto*      CR   = cairo_create(SURF);
        const bool HAS  = spec.level >= 0;
        const int  LVL  = std::clamp(spec.level, 0, 100);
        const bool FULL = HAS && LVL >= 100;
        // one unit in device px: the body's rounded size over its viewport
        const double U = std::min(G.bodyW / VIEW_W, G.bodyH / VIEW_H);

        const auto   body = [&]() {
            const double R = BODY_R * U, W = VIEW_W * U, H = VIEW_H * U;
            cairo_new_sub_path(CR);
            cairo_arc(CR, W - R, R, R, -M_PI / 2, 0);
            cairo_arc(CR, W - R, H - R, R, 0, M_PI / 2);
            cairo_arc(CR, R, H - R, R, M_PI / 2, M_PI);
            cairo_arc(CR, R, R, R, M_PI, 3 * M_PI / 2);
            cairo_close_path(CR);
        };
        const auto  source = [&](const SRgba& c) { cairo_set_source_rgba(CR, c.r, c.g, c.b, c.a); };
        const SRgba TRACK{1, 1, 1, HAS ? TRACK_ALPHA : TRACK_ALPHA_UNKNOWN};

        // the body: the track, and the level over it (all of it when full)
        source(FULL ? spec.fill : TRACK);
        body();
        cairo_fill(CR);
        if (const double FW = fillWidth(LVL, U); HAS && !FULL && FW > 0) {
            cairo_save(CR);
            cairo_rectangle(CR, 0, 0, FW, G.bodyH);
            cairo_clip(CR);
            source(spec.fill);
            body();
            cairo_fill(CR);
            cairo_restore(CR);
        }

        // the digits: a centred row, each glyph centred on the body's height
        if (HAS) {
            char         txt[4];
            const int    N   = std::snprintf(txt, sizeof(txt), "%d", LVL);
            const double ROW = N > 0 ? Detail::rowWidth(txt) : 0;
            double       x   = std::round((VIEW_W - ROW) / 2 * U);
            cairo_set_source_rgba(CR, 0, 0, 0, DIGIT_ALPHA);
            for (int i = 0; i < N; i++) {
                const auto& D = Detail::DIGITS[txt[i] - '0'];
                Detail::glyphPath(CR, D, x, (VIEW_H - D.h) / 2 * U, U);
                cairo_fill(CR);
                x += (D.w + DIGIT_GAP) * U;
            }
        }

        if (const auto* GL = Detail::attributionGlyph(spec.attribution); GL && G.glyphW > 0) {
            // the glyph inside its ring, the ring cut out of the body
            const double S = std::min(G.glyphW / GL->w, G.glyphH / GL->h);
            Detail::glyphPath(CR, *GL, G.glyphX, pixelRound((G.bodyH - G.glyphH) / 2.0), S);
            cairo_set_line_width(CR, RING * S);
            cairo_set_operator(CR, CAIRO_OPERATOR_CLEAR);
            cairo_stroke_preserve(CR);
            cairo_set_operator(CR, CAIRO_OPERATOR_OVER);
            source(spec.glyph);
            cairo_fill(CR);
        } else if (G.capW > 0) {
            // the cap, in the track's colour (the fill's when full)
            const double S = std::min(G.capW / Detail::CAP.w, G.capH / Detail::CAP.h);
            Detail::glyphPath(CR, Detail::CAP, G.capX, pixelRound((G.bodyH - G.capH) / 2.0), S);
            source(FULL ? spec.fill : TRACK);
            cairo_fill(CR);
        }

        cairo_destroy(CR);
        cairo_surface_flush(SURF);
        return SURF;
    }

} // namespace NAwesome::Shell::Battery
