// awesome/core/schema.cpp — the table itself.
//
// The defaults ARE the glass·ink tokens: the C++ side carries the material
// so a config that sets nothing still looks right. Colors are raw hex in
// the fork's byte order (#AABBGGRR as typed in config); six-digit values
// are normalized to full alpha here so the tables read as the docs do.
//
// Field order: key, desc, kind, ival, dval, sval, cval, imin, imax, dmin, dmax.
#include "schema.hpp"

#include <algorithm>
#include <cmath>

namespace NAwesome {

    Schema::Schema() {
        m_specs = {
            // ---- shell ----
            {"plugin:awesome:shell:height", "bar height in logical px (reserve it: monitor reserved top)", Kind::Int, 26, 0, {}, 0, 16, 64},
            {"plugin:awesome:shell:font_size", "text size in logical px (monitor scale applies at raster time)", Kind::Int, 12, 0, {}, 0, 6, 48},
            {"plugin:awesome:shell:tray_spacing", "px between tray icons", Kind::Int, 10, 0, {}, 0, 0, 64},
            {"plugin:awesome:shell:font", "font family", Kind::String, 0, 0, "Roboto"},
            {"plugin:awesome:shell:terminal", "terminal that runs Terminal=true launcher entries", Kind::String, 0, 0, "alacritty"},
            {"plugin:awesome:shell:col_bg", "bar panel", Kind::Color, 0, 0, {}, 0xff132732},
            {"plugin:awesome:shell:col_fg", "bar text", Kind::Color, 0, 0, {}, 0xffeef3f5},
            {"plugin:awesome:shell:col_muted", "legacy fallback color", Kind::Color, 0, 0, {}, 0xffd1dde1},
            {"plugin:awesome:shell:col_focus", "launcher selection", Kind::Color, 0, 0, {}, 0xff9acbff},
            {"plugin:awesome:shell:col_active", "active workspace/task", Kind::Color, 0, 0, {}, 0xff9acbff},
            {"plugin:awesome:shell:col_active_bg", "active/hover state", Kind::Color, 0, 0, {}, 0x339acbff},
            {"plugin:awesome:shell:col_on_active", "content on primary", Kind::Color, 0, 0, {}, 0xff102333},
            {"plugin:awesome:shell:col_empty", "disabled content", Kind::Color, 0, 0, {}, 0x61d1dde1},
            {"plugin:awesome:shell:col_urgent", "urgent content", Kind::Color, 0, 0, {}, 0xffffb4ab},
            {"plugin:awesome:shell:col_urgent_bg", "urgent container", Kind::Color, 0, 0, {}, 0xff93000a},
            {"plugin:awesome:shell:col_square_sel", "focused-window marker", Kind::Color, 0, 0, {}, 0xff9acbff},
            {"plugin:awesome:shell:col_square_unsel", "occupied marker", Kind::Color, 0, 0, {}, 0xffd1dde1},
            {"plugin:awesome:shell:col_frame", "menu outline", Kind::Color, 0, 0, {}, 0x33e0f0f8},
            {"plugin:awesome:shell:col_charging", "battery fill charging/defender", Kind::Color, 0, 0, {}, 0x18cc47},
            {"plugin:awesome:shell:col_low", "battery fill low", Kind::Color, 0, 0, {}, 0xff0e01},
            {"plugin:awesome:shell:col_powersave", "battery fill power save", Kind::Color, 0, 0, {}, 0xffc917},
            // ---- notify ----
            {"plugin:awesome:notify:font", "font family", Kind::String, 0, 0, "IBM Plex Sans"},
            {"plugin:awesome:notify:font_size", "body text size in logical px (the type roles derive from it)", Kind::Int, 12, 0, {}, 0, 6, 48},
            {"plugin:awesome:notify:width", "popup card width in logical px", Kind::Int, 348, 0, {}, 0, 128, 1024},
            {"plugin:awesome:notify:max_height", "popup card height cap in logical px", Kind::Int, 300, 0, {}, 0, 96, 1024},
            {"plugin:awesome:notify:max_icon", "popup icon column in logical px (shade rows raster at this cap)", Kind::Int, 44, 0, {}, 0, 16, 256},
            {"plugin:awesome:notify:margin", "inter-card gap in logical px", Kind::Int, 6, 0, {}, 0, 0, 64},
            {"plugin:awesome:notify:offset_y", "popups' and the shade's distance from the monitor top", Kind::Int, 34, 0, {}, 0, 0, 512},
            {"plugin:awesome:notify:timeout_low", "ephemeral timeout in ms (low urgency, transient, progress cards)", Kind::Int, 4000, 0, {}, 0, 0, 600000},
            {"plugin:awesome:notify:timeout_normal", "normal banner timeout in ms, then it retreats to the shade; 0 = sticky", Kind::Int, 5000, 0, {}, 0, 0, 600000},
            {"plugin:awesome:notify:coalesce_popups", "1 = at most one live popup per app; same-app extras land silent in the shade", Kind::Int, 1, 0, {}, 0, 0, 1},
            {"plugin:awesome:notify:max_notifs", "model cap; overflow evicts the oldest non-critical card", Kind::Int, 50, 0, {}, 0, 1, 512},
            {"plugin:awesome:notify:ignore_dbusclose", "ignore app-initiated CloseNotification (dunst's knob)", Kind::Int, 0, 0, {}, 0, 0, 1},
            {"plugin:awesome:notify:rounding", "card radius in logical px (panel +6 and rows -2 derive)", Kind::Int, 16, 0, {}, 0, 0, 128},
            {"plugin:awesome:notify:rounding_power", "corner superellipse exponent", Kind::Double, 0, 3.0, {}, 0, 0, 1 << 20, 1.0, 8.0},
            {"plugin:awesome:notify:sound_command", "libcanberra player for sound hints; empty disables", Kind::String, 0, 0, "canberra-gtk-play"},
            {"plugin:awesome:notify:fallback_icon_dir", "iconless cards draw a random identity face from this directory", Kind::String, 0, 0, ""},
            {"plugin:awesome:notify:col_bg", "glass fill (the alpha IS the glass)", Kind::Color, 0, 0, {}, 0x9e0f1218},
            {"plugin:awesome:notify:col_fg", "body text", Kind::Color, 0, 0, {}, 0xffe4e8ee},
            {"plugin:awesome:notify:col_title", "card titles", Kind::Color, 0, 0, {}, 0xffeef1f5},
            {"plugin:awesome:notify:col_kicker", "header/age/secondary text", Kind::Color, 0, 0, {}, 0xff98a2ac},
            {"plugin:awesome:notify:col_frame", "hairlines", Kind::Color, 0, 0, {}, 0x17dcebff},
            {"plugin:awesome:notify:col_urgent", "critical ring/progress/urgent fills", Kind::Color, 0, 0, {}, 0xff8a5c},
            {"plugin:awesome:notify:col_highlight", "the accent: progress, actions, selections", Kind::Color, 0, 0, {}, 0xff32d6ff},
            {"plugin:awesome:notify:col_link", "body hyperlinks", Kind::Color, 0, 0, {}, 0xff7db4ff},
            // ---- windows ----
            {"plugin:awesome:windows:edge", "distance from an output edge that arms a snap zone", Kind::Int, 16, 0, {}, 0, 0, 128},
            {"plugin:awesome:windows:snap_distance", "magnetic pull distance", Kind::Int, 8, 0, {}, 0, 0, 128},
            {"plugin:awesome:windows:col_frame", "snap preview outline", Kind::Color, 0, 0, {}, 0xff9acbff},
        };
    }

    const Spec* Schema::find(std::string_view key) const {
        for (const auto& S : m_specs)
            if (S.key == key)
                return &S;
        return nullptr;
    }

    void Schema::set(std::string_view key, Raw r) {
        if (!find(key)) {
            ++m_unknown;
            return;
        }
        // string keys are admitted at the bound; everything else lands as
        // delivered (the getters clamp)
        BoundedString<MAX_STRING_KEY_BYTES> B;
        B.assignClipped(r.s);
        r.s = B.str();
        // assignment, not emplace: re-delivering a key replaces its value
        m_raw[std::string{key}] = std::move(r);
    }

    int Schema::getI(std::string_view key) const {
        const auto* S = spec(key);
        const auto IT = m_raw.find(std::string{key});
        const auto V  = S && IT != m_raw.end() ? IT->second.i : (S ? S->ival : 0);
        if (!S)
            return 0;
        return std::clamp(V, S->imin, S->imax);
    }

    double Schema::getD(std::string_view key) const {
        const auto* S = spec(key);
        if (!S)
            return 0.0;
        const auto IT = m_raw.find(std::string{key});
        const auto V  = IT != m_raw.end() ? IT->second.d : S->dval;
        if (!std::isfinite(V))
            return S->dval;
        return std::clamp(V, S->dmin, S->dmax);
    }

    uint32_t Schema::getColor(std::string_view key) const {
        const auto* S = spec(key);
        if (!S)
            return 0;
        const auto IT = m_raw.find(std::string{key});
        return IT != m_raw.end() ? IT->second.c : S->cval;
    }

    std::string_view Schema::getS(std::string_view key) const {
        const auto* S = spec(key);
        if (!S)
            return {};
        const auto IT = m_raw.find(std::string{key});
        return IT != m_raw.end() ? std::string_view{IT->second.s} : S->sval;
    }

} // namespace NAwesome
