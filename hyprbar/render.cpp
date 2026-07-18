// hyprbar/render.cpp — the bar itself, the text cache, the paint context, the pass element

#include "hyprbar.hpp"

namespace NHyprbar {

    static const char* KANJI[9] = {"一", "二", "三", "四", "五", "六", "七", "八", "九"};

    // Each entry remembers the warm generation that last wanted it. Evicting
    // whatever the CURRENT layout doesn't name would thrash: sloppy focus
    // re-keys two task labels (colFg <-> colActive) for every window the cursor
    // crosses, so the variant it just left would be rebuilt on the way back —
    // a pango render + upload each time. A grace window keeps those hot and
    // still bounds the map against title churn.
    struct SCachedTex {
        SP<ITexture> tex;
        uint64_t     gen = 0;
    };
    static std::unordered_map<std::string, SCachedTex> texCache;
    static uint64_t                                    texGen         = 0;
    static constexpr uint64_t                          TEX_CACHE_LIFE = 32; // warms an unused texture survives

    // per monitor: a fingerprint of the task labels the strip shows — see
    // the tasklist in renderBar
    static std::unordered_map<uint64_t, size_t> lastTaskFp;

    // Built ONLY by the warm pass — see the texture rule in hyprbar.hpp. A miss
    // during a draw returns null (that one label is missing for one frame)
    // rather than building, which would paint nothing anyway AND swallow every
    // later draw in the element.
    SP<ITexture> textTex(const std::string& text, const CHyprColor& col, int pt, int maxWidth, const std::string& font) {
        // key on the RESOLVED font: every caller passes "", and keying the
        // empty string made old-font textures permanent hits across a
        // plugin:hyprbar:font change
        const std::string& F = font.empty() ? cfg.font->value() : font;

        char               meta[48]; // the non-text key parts in one stack write — no to_string churn per call
        const int          METALEN = std::snprintf(meta, sizeof(meta), "|%llx|%d|%d|", (unsigned long long)col.getAsHex(), pt, maxWidth);

        static std::string KEY; // reused; main thread only
        KEY.clear();
        KEY += text;
        KEY.append(meta, METALEN > 0 ? (size_t)METALEN : 0);
        KEY += F;

        if (const auto IT = texCache.find(KEY); IT != texCache.end()) {
            IT->second.gen = texGen;
            return IT->second.tex;
        }

        if (!warming) {
            texStale = true;
            return nullptr;
        }

        auto tex      = g_pHyprRenderer->renderText(text, col, pt, false, F, maxWidth);
        texCache[KEY] = {tex, texGen};
        return tex;
    }

    // awesome's awful.layout: the ordered registry (order matters, like
    // awful.layout.layouts) and each workspace's index — per tag, exactly
    // awesome's model. Future layouts append here and take real effect
    // wherever they get implemented; the bar carries the state and the icon
    // (~/.config/hypr/icons/<name>.png).
    static const std::vector<const char*>          LAYOUTS = {"floating"};
    static std::unordered_map<WORKSPACEID, size_t> wsLayout;
    // keyed by the LAYOUTS literals themselves — pointer identity, no
    // per-frame string
    static std::unordered_map<const char*, SP<ITexture>> layoutTexs;
    static std::unordered_set<const char*>               layoutTexTried;

    static const char*                                   currentLayout(WORKSPACEID ws) {
        const auto IT = wsLayout.find(ws);
        return LAYOUTS[(IT == wsLayout.end() ? 0 : IT->second) % LAYOUTS.size()];
    }

    void layoutInc(int dir) {
        const auto MON = Desktop::focusState() ? Desktop::focusState()->monitor() : nullptr;
        if (!MON || !MON->m_activeWorkspace)
            return;
        const int64_t N   = (int64_t)LAYOUTS.size();
        auto&         IDX = wsLayout[MON->m_activeWorkspace->m_id];
        IDX               = (size_t)(((int64_t)IDX + dir % N + N) % N);
        barChanged();
    }

    // ---- the battery (Android's expressive battery, drawn natively) ----
    //
    // A 1:1 transcription of SystemUI's Compose battery — the pill Pixels
    // ship on Android 16 QPR2/17, not yet in public AOSP. Decompiled from
    // SystemUIGoogle (statusbar/pipeline/battery: BatteryFrame, BatteryGlyph,
    // BatteryMeasurePolicy, UnifiedBattery.kt; Apache-2.0), paths embedded
    // verbatim. Its model, on a 24x13 viewport: borderless round-rect body
    // (r=4) as a translucent white track (0.55 with digits, 0.45 without),
    // a LEFT-anchored fill clipped to ceil(level*24/100) units, digit
    // GLYPHS (bespoke vector paths, not a font) in black 0.75 centered as a
    // row with 0.8 gaps, and to the right either the D cap (1 unit off, in
    // the track color) or, while charging, the bolt overlapping the body by
    // 20% of its width — white over a 2-unit black stroke. At 100% the whole
    // body takes the fill color. The canvas is always the widest (bolt)
    // state so the body never shifts when charging flips.
    struct SGlyphPath {
        double      w, h;
        const char* d;
    };
    // BatteryGlyph digits 0-9 (w, h, path), verbatim
    static const SGlyphPath BATT_DIGITS[10] = {
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
    static const SGlyphPath BATT_BOLT{
        8.0, 9.0,
        "M7.672,3.375L4.302,3.375L5.038,0.563C5.113,0.281 4.91,0 4.633,0C4.515,0 4.398,0.056 4.324,0.146L0.09,5.051C-0.102,5.276 0.047,5.625 "
        "0.335,5.625L3.705,5.625L2.969,8.438C2.895,8.719 3.097,9 3.374,9C3.492,9 3.609,8.944 3.684,8.854L7.917,3.949C8.109,3.724 7.96,3.375 7.672,3.375Z"};
    static const SGlyphPath BATT_CAP{1.5, 6.03,
                                     "M0.3333,-0.0037L0,-0.0037L0,6.0234L0.3333,6.0234C0.9777,6.0234 1.5,5.5011 1.5,4.8567L1.5,1.163C1.5,0.5187 0.9777,-0.0037 0.3333,-0.0037Z"};

    // just enough SVG for the paths above: absolute M/L/H/V/C/Z
    static void playPath(cairo_t* CR, const char* d) {
        const char* p   = d;
        char        cmd = 0;
        double      x = 0, y = 0;
        const auto  num = [&]() {
            while (*p == ',' || *p == ' ')
                p++;
            char*  e;
            double v = std::strtod(p, &e);
            p        = e;
            return v;
        };
        while (*p) {
            if (*p == ',' || *p == ' ') {
                p++;
                continue;
            }
            if (std::isalpha((unsigned char)*p)) {
                cmd = *p++;
                if (cmd == 'Z') {
                    cairo_close_path(CR);
                    cmd = 0;
                    continue;
                }
            }
            switch (cmd) {
                case 'M':
                    x = num(), y = num();
                    cairo_move_to(CR, x, y);
                    cmd = 'L'; // SVG: further pairs after M are lineto
                    break;
                case 'L':
                    x = num(), y = num();
                    cairo_line_to(CR, x, y);
                    break;
                case 'H':
                    x = num();
                    cairo_line_to(CR, x, y);
                    break;
                case 'V':
                    y = num();
                    cairo_line_to(CR, x, y);
                    break;
                case 'C': {
                    const double C1X = num(), C1Y = num(), C2X = num(), C2Y = num();
                    x = num(), y = num();
                    cairo_curve_to(CR, C1X, C1Y, C2X, C2Y, x, y);
                    break;
                }
                default: return; // unknown command: stop rather than misdraw
            }
        }
    }

    static SP<ITexture> batteryPill(int percent, bool charging, double hPx, const CHyprColor& fill) {
        const double S    = hPx / 13.0; // one viewport unit
        const int    LVL  = std::clamp(percent, 0, 100);
        const bool   FULL = LVL >= 100;
        const int    CW = (int)std::ceil(30.4 * S), CH = (int)std::ceil(13.0 * S);

        auto*        SURF = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, CW, CH);
        auto*        CR   = cairo_create(SURF);

        const auto   body = [&]() { // bodyPathSpec: RoundRect 24x13 r=4
            cairo_new_sub_path(CR);
            cairo_arc(CR, 20 * S, 4 * S, 4 * S, -M_PI / 2, 0);
            cairo_arc(CR, 20 * S, 9 * S, 4 * S, 0, M_PI / 2);
            cairo_arc(CR, 4 * S, 9 * S, 4 * S, M_PI / 2, M_PI);
            cairo_arc(CR, 4 * S, 4 * S, 4 * S, M_PI, 3 * M_PI / 2);
            cairo_close_path(CR);
        };
        const auto glyphAt = [&](const SGlyphPath& G, double ux, double uy) {
            cairo_save(CR);
            cairo_translate(CR, ux * S, uy * S);
            cairo_scale(CR, S, S);
            playPath(CR, G.d);
            cairo_restore(CR);
        };

        // DarkTheme: track white 0.55 (digits always shown); full = the fill
        const auto trackColor = [&]() {
            if (FULL)
                cairo_set_source_rgba(CR, fill.r, fill.g, fill.b, fill.a);
            else
                cairo_set_source_rgba(CR, 1, 1, 1, 0.55);
        };
        trackColor();
        body();
        cairo_fill(CR);

        if (!FULL && LVL > 0) { // fill: the same round rect under a clip at ceil(level*24/100) units
            cairo_save(CR);
            cairo_rectangle(CR, 0, 0, std::ceil(LVL * 24.0 / 100.0) * S, 13 * S);
            cairo_clip(CR);
            cairo_set_source_rgba(CR, fill.r, fill.g, fill.b, fill.a);
            body();
            cairo_fill(CR);
            cairo_restore(CR);
        }

        { // the level's digit glyphs: a centered row, 0.8-unit gaps, black 0.75
            const auto TXT   = std::to_string(LVL);
            double     total = 0;
            for (size_t i = 0; i < TXT.size(); i++)
                total += BATT_DIGITS[TXT[i] - '0'].w + (i ? 0.8 : 0.0);
            double x = (24.0 - total) / 2.0;
            cairo_set_source_rgba(CR, 0, 0, 0, 0.75);
            for (const char C : TXT) {
                const auto& G = BATT_DIGITS[C - '0'];
                glyphAt(G, x, (13.0 - G.h) / 2.0);
                cairo_fill(CR);
                x += G.w + 0.8;
            }
        }

        if (!charging) { // the D cap, 1 unit off the body, in the track color
            trackColor();
            glyphAt(BATT_CAP, 24.0 + std::round(S) / S, (13.0 - BATT_CAP.h) / 2.0);
            cairo_fill(CR);
        } else { // the bolt, overlapping the body by 20% of its width: white
                 // over a 2-unit black stroke so it reads on any fill
            glyphAt(BATT_BOLT, 24.0 - 0.2 * BATT_BOLT.w, (13.0 - BATT_BOLT.h) / 2.0);
            cairo_set_source_rgba(CR, 0, 0, 0, 1);
            cairo_set_line_width(CR, 2.0 * S);
            cairo_stroke_preserve(CR);
            cairo_set_source_rgba(CR, 1, 1, 1, 1);
            cairo_fill(CR);
        }

        cairo_surface_flush(SURF);
        auto tex = g_pHyprRenderer->createTexture(SURF);
        cairo_destroy(CR);
        cairo_surface_destroy(SURF);
        return tex;
    }

    // per physical height, so mixed-scale monitors don't evict each other
    struct SPill {
        SP<ITexture> tex;
        uint64_t     key = 0;
    };
    static std::unordered_map<int, SPill> pillCache;

    static bool                           inRenderBar = false; // a render is on the stack: never build textures

    static UP<SEventLoopDoLaterLock>      pendingRewarm;

    // Back out to the event loop to build what a draw found missing, then
    // repaint. Deferred because we are inside the render when we notice.
    static void scheduleWarmRepaint() {
        if (!g_pEventLoopManager)
            return;
        pendingRewarm = g_pEventLoopManager->doLaterLock([]() { barChanged(); });
    }

    // ---- the paint context (hyprbar.hpp) ----

    CBox SPaint::toPhys(const CBox& global) const {
        return CBox{global}.translate(-mon->m_position).scale(scale).round();
    }

    void SPaint::rect(const CBox& global, const CHyprColor& c, int round) const {
        if (warm)
            return;
        g_pHyprOpenGL->renderRect(toPhys(global), c, {.round = round});
    }

    void SPaint::border(const CBox& global, const CHyprColor& c, int round, int sizePx) const {
        if (warm)
            return;
        g_pHyprOpenGL->renderBorder(toPhys(global), Config::CGradientValueData{c}, {.round = round, .borderSize = sizePx});
    }

    void SPaint::tex(const SP<ITexture>& t, const CBox& physBox) const {
        if (warm || !t || t->m_texID == 0)
            return;
        g_pHyprOpenGL->renderTexture(t, physBox, {});
    }

    void SPaint::texIn(const SP<ITexture>& t, const CBox& cell) const {
        if (warm || !t || t->m_texID == 0)
            return;
        const auto B = toPhys(cell);
        CBox       b{B.x + (B.w - t->m_size.x) / 2.0, B.y + (B.h - t->m_size.y) / 2.0, t->m_size.x, t->m_size.y};
        g_pHyprOpenGL->renderTexture(t, b.round(), {});
    }

    // ---- rendering ----

    // One layout, two modes. WARM builds every texture and paints nothing;
    // DRAW paints and must never build.
    //
    // A texture returned by renderText()/createTexture() cannot be painted by
    // the frame that created it — wherever in that frame it was created — and
    // the miss silently swallows everything drawn after it too. Building them
    // lazily mid-draw therefore blanked the strip from the first miss onward
    // for exactly one frame. Since itemw is part of the texture cache key, one
    // window closing re-keys every task label at once: the whole tasklist
    // vanished on every open/close ("the bar blinks when I close a window").
    // So warmBars() runs this same layout from the EVENT LOOP, a frame ahead;
    // by the time draw() runs, everything is a cache hit.
    static void renderBar(PHLMONITOR mon, bool warm) {
        if (!mon)
            return;

        auto& hits = hitboxes[mon->m_id];
        hits.clear(); // capacity retained: no per-frame allocations

        const auto WS = mon->m_activeWorkspace;
        if (WS && Fullscreen::controller()->getFullscreenModes(WS).internal == Fullscreen::FSMODE_FULLSCREEN && !(Menubar::isOpen && Menubar::mon.lock() == mon)) {
            if (Menu::isOpen && Menu::mon.lock() == mon)
                Menu::close();
            return; // real fullscreen owns the whole output, like awesome —
                    // except the open menubar: awesome's is an ontop wibox
        }

        const double SCALE = mon->m_scale;
        const auto   MB    = mon->logicalBox();
        const double H     = barHeight();
        const int    PT    = std::max(6, (int)std::round((double)cfg.fontSize->value() * SCALE));

        const SPaint P{.mon = mon, .hits = &hits, .warm = warm, .scale = SCALE, .mb = MB, .h = H, .pt = PT};

        const auto   toPhys    = [&](const CBox& b) { return P.toPhys(b); };
        const auto   drawRect  = [&](const CBox& b, const CHyprColor& c) { P.rect(b, c); };
        const auto   drawTex   = [&](const SP<ITexture>& t, const CBox& b) { P.tex(t, b); };
        const auto   drawTexIn = [&](const SP<ITexture>& t, const CBox& b) { P.texIn(t, b); };

        drawRect(CBox{MB.x, MB.y, MB.w, H}, color(cfg.colBg));

        // -- the menubar: its own strip right BELOW the bar, the bar stays
        // visible (awesome's menubar is a separate wibox at the workarea top,
        // which sits under the wibar — it never replaced it) --
        Menubar::render(P);

        // ONE walk of the window list for all its consumers: per-workspace
        // urgency + occupancy (taglist) and this workspace's tasks (tasklist,
        // in arrival order).
        static std::vector<std::pair<uint64_t, PHLWINDOW>> tasks; // reused; main thread only
        bool                                               urgentWS[10]  = {};
        int                                                wsWindows[10] = {};
        tasks.clear();
        for (const auto& W : Desktop::windowState()->windows()) {
            if (W->m_isMapped && W->m_workspace) {
                const auto ID = W->m_workspace->m_id;
                if (ID >= 1 && ID <= 9) {
                    wsWindows[ID]++;
                    if (W->m_isUrgent)
                        urgentWS[ID] = true;
                }
            }
            if (isTaskOn(W, WS)) {
                const auto [SEQ, NEW] = winSeq.try_emplace(W.get(), winSeqNext);
                if (NEW)
                    winSeqNext++;
                tasks.emplace_back(SEQ->second, W);
            }
        }
        std::sort(tasks.begin(), tasks.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

        const auto FOCUS   = Desktop::focusState() ? Desktop::focusState()->window() : nullptr;
        const auto FOCUSWS = FOCUS && FOCUS->m_workspace ? FOCUS->m_workspace->m_id : WORKSPACE_INVALID;

        // one palette fetch per frame: color() memoizes the conversion but
        // still hashes per call
        const CHyprColor COLFG = color(cfg.colFg), COLACTIVE = color(cfg.colActive), COLACTIVEBG = color(cfg.colActiveBg), COLURGENT = color(cfg.colUrgent),
                         COLURGENTBG = color(cfg.colUrgentBg), COLSQSEL = color(cfg.colSquareSel), COLSQUNSEL = color(cfg.colSquareUnsel);

        // -- taglist (awesome's exact state matrix: the viewed tag gets the
        // focus colors, an urgent one the urgent colors, and everything else —
        // occupied or empty — the plain text color; occupancy shows as the
        // little corner square instead, filled when the tag holds the focused
        // window, hollow otherwise) --
        double     x  = MB.x;
        const auto SQ = std::round(H * 4.0 / 19.0); // the 4px square of a 19px wibar, scaled
        for (int i = 1; i <= 9; i++) {
            const int  WINDOWS = wsWindows[i];
            const bool ACTIVE  = WS && WS->m_id == i;

            CHyprColor fg    = COLFG;
            CHyprColor bg    = {};
            bool       hasBg = false;
            if (ACTIVE) {
                bg    = COLACTIVEBG;
                hasBg = true;
                fg    = COLACTIVE;
            } else if (urgentWS[i]) {
                bg    = COLURGENTBG;
                hasBg = true;
                fg    = COLURGENT;
            }

            // awesome's tag button width (text + 12), label centered
            const auto   TEX = textTex(KANJI[i - 1], fg, PT);
            const double TW  = TEX ? TEX->m_size.x / SCALE : H;
            const CBox   CELL{x, MB.y, TW + 12, H};

            if (hasBg)
                drawRect(CELL, bg);

            if (WINDOWS > 0) {
                if (FOCUSWS == i)
                    drawRect(CBox{x, MB.y, SQ, SQ}, COLSQSEL);
                else { // hollow
                    drawRect(CBox{x, MB.y, SQ, 1}, COLSQUNSEL);
                    drawRect(CBox{x, MB.y + SQ - 1, SQ, 1}, COLSQUNSEL);
                    drawRect(CBox{x, MB.y, 1, SQ}, COLSQUNSEL);
                    drawRect(CBox{x + SQ - 1, MB.y, 1, SQ}, COLSQUNSEL);
                }
            }

            drawTexIn(TEX, CELL);

            SHit h;
            h.box  = CELL;
            h.kind = SHit::TAG;
            h.tag  = i;
            hits.push_back(h);
            x += CELL.w;
        }

        // -- right side, laid out from the edge inwards; awesome's order is
        // [systray][battery][clock][layoutbox], so the layoutbox sits last --
        double right = MB.x + MB.w;

        { // layoutbox: the active workspace's layout icon; click/wheel cycles
            // the registry — with its single entry it is still the static
            // floating indicator it always was
            const char* NAME = currentLayout(WS ? WS->m_id : WORKSPACE_INVALID);
            auto&       TEX  = layoutTexs[NAME];
            if (!TEX && !layoutTexTried.contains(NAME)) {
                if (!warming)
                    texStale = true; // an icon is a texture too: warm builds it
                else {
                    layoutTexTried.insert(NAME);
                    if (const char* HOME = std::getenv("HOME"))
                        TEX = loadPng(std::string{HOME} + "/.config/hypr/icons/" + NAME + ".png");
                }
            }
            const CBox CELL{right - H, MB.y, H, H};
            if (TEX && TEX->m_texID != 0) {
                // 3px inset, the bar's icon rhythm
                const double S = std::round((H - 6) * SCALE);
                const auto   P = toPhys(CELL);
                CBox         b{P.x + (P.w - S) / 2.0, P.y + (P.h - S) / 2.0, S, S};
                drawTex(TEX, b.round());
            }
            SHit h;
            h.box  = CELL;
            h.kind = SHit::LAYOUT;
            hits.push_back(h);
            right -= H;
        }

        { // clock — 6px each side, the bar's text pad
            const auto   TEX = textTex(clockText, COLFG, PT);
            const double W   = TEX ? TEX->m_size.x / SCALE + 12 : 0;
            drawTexIn(TEX, CBox{right - W, MB.y, W, H});
            right -= W;
        }

        if (batteryPercent >= 0) { // Android's expressive battery: digits inside, the fill colored by state
            // Android sizes it 13sp beside 14sp status bar text: 13/14 of
            // the bar's font size, not the icon rhythm — the pill is meant
            // to read as text-line furniture, not as an icon
            const int PH = std::max(8, (int)std::round(PT * 13.0 / 14.0));
            // its ladder: charging -> green, <= 20 discharging -> error red,
            // else white (the DarkTheme Default fill)
            const CHyprColor FILL = batteryCharging ? color(cfg.colCharging) : batteryPercent <= 20 ? color(cfg.colLow) : CHyprColor{1.f, 1.f, 1.f, 1.f};
            const uint64_t   KEY  = ((uint64_t)batteryPercent << 40) ^ (batteryCharging ? 1ull << 47 : 0) ^ FILL.getAsHex();
            auto&            PILL = pillCache[PH];
            if (warm) {
                if (!PILL.tex || PILL.key != KEY) {
                    PILL.tex = batteryPill(batteryPercent, batteryCharging, PH, FILL);
                    PILL.key = KEY;
                }
            } else if (!PILL.tex || PILL.key != KEY)
                texStale = true; // level moved under a scissored repaint: warm + repaint

            const double PW = PILL.tex ? PILL.tex->m_size.x / SCALE : PT * 13.0 / 14.0 * 30.4 / 13.0;
            const double W  = 6 + PW + 6; // breathing room off the tray
            drawTexIn(PILL.tex, CBox{right - W + 6, MB.y, PW, H});
            right -= W;
        }

        // -- tray icons, spaced like awesome's systray_icon_spacing --
        bool firstTray = true;
        for (const auto& IT : Tray::items) {
            if (IT->status == "Passive")
                continue; // SNI: Passive means don't show the item
            if (!firstTray)
                right -= (double)cfg.traySpacing->value();
            firstTray = false;
            // The pixmap is a texture too, so the rule applies: rebuild it on
            // the warm only. A dirty item reaching a draw keeps its old icon
            // for this frame and asks for a repaint.
            if (IT->dirty) {
                if (!warming)
                    texStale = true;
                else {
                    IT->dirty = false;
                    IT->tex.reset();
                    if (!IT->pixels.empty())
                        IT->tex = g_pHyprRenderer->createTexture(DRM_FORMAT_ARGB8888, IT->pixels.data(), IT->pw * 4, Vector2D{(double)IT->pw, (double)IT->ph});
                    if ((!IT->tex || IT->tex->m_texID == 0) && !IT->iconName.empty())
                        IT->tex = trayIcon(IT->iconName, IT->themePath);
                }
            }

            const CBox CELL{right - H, MB.y, H, H};
            if (IT->tex && IT->tex->m_texID != 0) {
                // 3px inset: SNI pixmaps lack the internal padding XEmbed
                // icons carried, full-bleed reads as cramped
                const double S = std::round((H - 6) * SCALE);
                const auto   P = toPhys(CELL);
                CBox         b{P.x + (P.w - S) / 2.0, P.y + (P.h - S) / 2.0, S, S};
                drawTex(IT->tex, b.round());
            } else
                drawTexIn(textTex(letterOf(IT->iconName), color(cfg.colMuted), PT), CELL);

            SHit h;
            h.box     = CELL;
            h.kind    = SHit::TRAY;
            h.tray    = IT;
            h.anchorX = CELL.x + H / 2.0;
            h.mon     = mon;
            hits.push_back(h);
            right -= H;
        }

        // -- tasklist: the active workspace's windows, in arrival order
        // (collected in the single window walk above) --
        size_t taskFp = 0;
        {
            const double avail = right - 8 - x;
            if (!tasks.empty() && avail >= 40) {
                // awesome tasklist behavior: the windows split the WHOLE free
                // strip between taglist and tray — one window = one huge item
                const double ITEMW = avail / (double)tasks.size();

                for (const auto& [SEQ, W] : tasks) {
                    const CBox CELL{x, MB.y, ITEMW, H};
                    // the old theme: the focused task is cyan TEXT on the plain
                    // bar (tasklist_bg_focus = bg_normal, no box); urgent gets
                    // the urgent bg — and focus wins over urgent, like awesome
                    CHyprColor fg = COLFG;
                    if (W == FOCUS)
                        fg = COLACTIVE;
                    else if (W->m_isUrgent) {
                        drawRect(CELL, COLURGENTBG);
                        fg = COLURGENT;
                    }

                    // [4][icon][4][title] — awesome's item margins, icon on
                    // the bar's 3px-inset rhythm
                    const double ICON = H - 6;
                    double       tx   = x + 4;
                    if (const auto ITEX = appIcon(W->m_class); ITEX && ITEX->m_texID != 0) {
                        const auto P = toPhys(CBox{tx, MB.y + 3, ICON, ICON});
                        drawTex(ITEX, P);
                    } else
                        drawTexIn(textTex(letterOf(W->m_class), COLACTIVE, PT), CBox{tx, MB.y, ICON, H});
                    tx += ICON + 4;

                    static std::string LBL; // reused; main thread only
                    taskLabel(W, LBL);
                    taskFp         = taskFp * 1099511628211ULL + std::hash<std::string>{}(LBL);
                    const auto TEX = textTex(LBL, fg, PT, (int)std::round((ITEMW - (tx - x) - 4) * SCALE));
                    if (TEX && TEX->m_texID != 0) {
                        const auto P = toPhys(CBox{tx, MB.y, 1, H});
                        CBox       b{P.x, P.y + (P.h - TEX->m_size.y) / 2.0, TEX->m_size.x, TEX->m_size.y};
                        drawTex(TEX, b.round());
                    }

                    SHit h;
                    h.box    = CELL;
                    h.kind   = SHit::TASK;
                    h.window = W;
                    hits.push_back(h);
                    x += ITEMW;
                }
            }
        }
        // A label can flip without any bar event or strip damage (pin damages
        // only the window; the xdg maximized bit has no event at all): with
        // every variant still cached the next draw is correct but scissored
        // to the window's damage, so the strip on screen keeps the old text.
        // A warm stamps what the paint it precedes will show; a draw that
        // lays out anything else may have been clipped — repaint the strip.
        auto& FP = lastTaskFp[mon->m_id];
        if (warm)
            FP = taskFp;
        else if (FP != taskFp) {
            FP       = taskFp;
            texStale = true;
        }

        tasks.clear(); // don't keep strong window refs across frames

        // -- the open menu, panel by panel: the client list is fixed at 250
        // wide (the old rc's client_list width); dbusmenu levels size
        // themselves, and submenus cascade out beside their parent — like
        // the GTK menus these were under X11 --
        Menu::render(P);
    }

    // ---- pass element ----

    class CBarPassElement : public IPassElement {
      public:
        CBarPassElement(PHLMONITOR mon) : m_mon(mon) {}
        virtual ~CBarPassElement() = default;

        virtual std::vector<UP<IPassElement>> draw() override {
            inRenderBar = true;
            renderBar(m_mon.lock(), false);
            inRenderBar = false;

            // Something changed without warming first (a texture the warm never
            // enumerated). One label is missing for one frame; build it and
            // repaint. Never build here — that is the bug this all guards.
            if (texStale)
                scheduleWarmRepaint();

            return {};
        }
        virtual bool needsLiveBlur() override {
            return false;
        }
        virtual bool needsPrecomputeBlur() override {
            return false;
        }
        virtual std::optional<CBox> boundingBox() override {
            const auto MON = m_mon.lock();
            if (!MON)
                return std::nullopt;
            double h = barHeight();
            if (Menubar::isOpen && Menubar::mon.lock() == MON)
                h += barHeight(); // the prompt strip below the bar
            if (Menu::isOpen && Menu::mon.lock() == MON)
                h = MON->logicalBox().h; // cascades anchor anywhere below the bar — cover it all, an undersized box clips
            // monitor-local LOGICAL px — the pass scales by m_scale itself
            // (stock elements divide their physical boxes back down; see
            // CRenderPass::simplify)
            return CBox{0, 0, MON->logicalBox().w, h};
        }
        virtual const char* passName() override {
            return "CBarPassElement";
        }
        virtual ePassElementType type() override {
            return EK_CUSTOM;
        }

      private:
        PHLMONITORREF m_mon;
    };

    // Build every texture the NEXT frame will paint. Must run outside the
    // render cycle (see renderBar): a texture built during a frame cannot be
    // painted by that same frame, wherever in it it was built. Steady state is
    // all cache hits — a layout walk and nothing else.
    //
    // No-ops inside a render so callers never have to check: Menu::close()
    // damages, and renderBar itself can close the menu when a window goes
    // fullscreen.
    void warmBars(PHLMONITOR only) {
        if (warming || inRenderBar || !g_pCompositor)
            return;

        warming = true;
        if (!only)
            texGen++;

        for (const auto& M : State::monitorState()->monitors())
            if (!only || M == only)
                renderBar(M, true);

        // Bound the cache against title churn, but only drop what no warm has
        // wanted for a while — see SCachedTex. (The old size-capped full flush
        // is survivable now that a draw can never build, but it still meant one
        // warm rebuilding everything at once.) A scoped warm never enumerates
        // every monitor's textures, so it must not age or evict.
        if (!only && texGen > TEX_CACHE_LIFE)
            std::erase_if(texCache, [](const auto& E) { return E.second.gen + TEX_CACHE_LIFE < texGen; });

        warming  = false;
        texStale = false;
    }

    void onRenderStage(eRenderStage stage) {
        if (stage != RENDER_POST_WINDOWS)
            return;
        const auto MON = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!MON)
            return;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CBarPassElement>(MON));
    }

    void renderExit() {
        pendingRewarm.reset(); // its barChanged would touch the caches cleared below
        texCache.clear();
        lastTaskFp.clear();
        layoutTexs.clear();
        layoutTexTried.clear();
        wsLayout.clear();
        pillCache.clear();
    }

} // namespace NHyprbar
