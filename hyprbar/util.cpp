// hyprbar/util.cpp — small shared helpers: geometry, damage, colors, clock/battery state

#include "hyprbar.hpp"

namespace NHyprbar {

    bool               warming = false, texStale = false; // the texture rule — see hyprbar.hpp

    std::string        clockText, batteryText, batteryGlyphText;
    static std::string batteryDir; // /sys/class/power_supply/BATx, empty = none
    static std::string mainsDir;   // .../ACx — AC online switches the glyph, like the old widget

    // ---- helpers ----

    double barHeight() {
        return std::max((double)cfg.height->value(), 8.0);
    }

    void damageBars() {
        if (!g_pHyprRenderer)
            return;
        const double H = Menubar::isOpen ? barHeight() * 2 : barHeight();
        for (const auto& M : State::monitorState()->monitors()) {
            const auto MB = M->logicalBox();
            g_pHyprRenderer->damageBox(CBox{MB.x, MB.y, MB.w, H});
        }
    }

    CHyprColor color(const SP<Config::Values::CColorValue>& v) {
        struct SMemo {
            uint64_t   raw = 0;
            bool       set = false;
            CHyprColor col;
        };
        static std::unordered_map<const void*, SMemo> memo; // main thread only
        auto&                                         M = memo[v.get()];
        if (!M.set || M.raw != (uint64_t)v->value()) {
            M.raw = (uint64_t)v->value();
            M.set = true;
            M.col = CHyprColor{M.raw};
        }
        return M.col;
    }

    bool isTaskOn(const PHLWINDOW& w, const PHLWORKSPACE& ws) {
        return w && ws && w->m_isMapped && !w->isHidden() && w->m_workspace && w->m_workspace->m_id == ws->m_id;
    }

    // ---- clock / battery ----

    void findBattery() {
        batteryDir.clear();
        mainsDir.clear();
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator("/sys/class/power_supply", ec)) {
            std::ifstream t(e.path() / "type");
            std::string   type;
            if (t && std::getline(t, type)) {
                if (type == "Battery" && batteryDir.empty())
                    batteryDir = e.path();
                else if (type == "Mains" && mainsDir.empty())
                    mainsDir = e.path();
            }
        }
    }

    // The old awesome battery widget's glyphs: Material Icons Round, one
    // charging bolt on AC, else a gauge in 12.5% steps.
    static const char* batteryGlyph(int percent, bool ac) {
        if (ac)
            return "";
        if (percent <= 12)
            return "";
        if (percent <= 25)
            return "";
        if (percent <= 37)
            return "";
        if (percent <= 50)
            return "";
        if (percent <= 62)
            return "";
        if (percent <= 75)
            return "";
        if (percent <= 87)
            return "";
        return "";
    }

    bool refreshTexts() {
        bool       changed = false;

        char       buf[64];
        const auto NOW = std::time(nullptr);
        // awesome's default format, trimmed — padding is render.cpp's
        // explicit margin, not spaces baked into the text
        std::strftime(buf, sizeof(buf), "%a %b %d, %H:%M", std::localtime(&NOW));
        if (clockText != buf) {
            clockText = buf;
            changed   = true;
        }

        std::string glyph, bat;
        if (!batteryDir.empty()) {
            std::ifstream c(batteryDir + "/capacity");
            std::string   cap;
            if (c && std::getline(c, cap)) {
                bool ac = false;
                if (!mainsDir.empty()) {
                    std::ifstream o(mainsDir + "/online");
                    std::string   on;
                    ac = o && std::getline(o, on) && on == "1";
                }
                int pc = 0;
                try {
                    pc = std::stoi(cap);
                } catch (...) {}
                glyph = batteryGlyph(pc, ac);
                bat   = cap + "%";
            }
        }
        if (batteryText != bat || batteryGlyphText != glyph) {
            batteryText      = bat;
            batteryGlyphText = glyph;
            changed          = true;
        }

        return changed;
    }

    std::unordered_map<void*, uint64_t> winSeq;
    uint64_t                            winSeqNext = 0;

    // awesome's tasklist text: state markers, then the title. The stock set
    // is ▪ sticky, ⌃ ontop, ▴ above, ▾ below, + maximized, ⬌/⬍ maximized
    // h/v, ✈ floating — crosschecked against what exists here:
    //   ⌃  a PINNED window. Hyprland's pin is ontop AND sticky at once, but
    //      the user's Super+T was awesome's `c.ontop` toggle (its tasklist
    //      marker: ⌃) and nothing in their old config ever set sticky, so
    //      pin presents as the ontop it replaces — ▪ would read as wrong.
    //   +  maximized (awesome drew it bold; plain here — markup would need
    //      escaping every title).
    //   ✈  floating, in maximized's else exactly like awesome. The
    //      floating-only rule floats every window, so today every
    //      unmaximized task carries it; it starts discriminating when
    //      other layouts arrive.
    //   ▴/▾/⬌/⬍ have no Hyprland analog.
    void taskLabel(const PHLWINDOW& w, std::string& out) {
        out.clear();
        if (w->m_pinned)
            out += "⌃";
        // maximized: the configured xdg state where it's honest (floating —
        // hyprmax's per-window maximize speaks xdg only), the fullscreen
        // chain otherwise (tiled windows are told maximized as the CSD lie).
        // This runs per task per frame; the chain is a dozen virtual calls.
        bool maximized;
        if (!w->m_isX11 && w->m_isFloating && w->m_xdgSurface && w->m_xdgSurface->m_toplevel)
            maximized = std::ranges::contains(w->m_xdgSurface->m_toplevel->m_pendingApply.states, XDG_TOPLEVEL_STATE_MAXIMIZED);
        else
            maximized = Fullscreen::controller()->getFullscreenModes(w).internal == Fullscreen::FSMODE_MAXIMIZED;
        if (maximized)
            out += "+";
        else if (w->m_isFloating)
            out += "✈";
        out += w->m_title.empty() ? "<untitled>" : w->m_title;
    }

} // namespace NHyprbar
