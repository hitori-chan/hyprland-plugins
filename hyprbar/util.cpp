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

    bool hasBattery() {
        return !batteryDir.empty();
    }

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

    // ---- battery alerts (the old scripts/battery-watch.sh, folded in) ----

    // The alerts' waifu: one random ~/pic/waifu image, sticky for the session
    // (the per-tag waifu-icon.sh cache the script used, now just a static).
    static const std::string& batteryWaifu() {
        static const std::string ICON = []() -> std::string {
            const char* HOME = std::getenv("HOME");
            if (!HOME)
                return "";
            std::vector<std::string> found;
            std::error_code          ec;
            auto                     it = std::filesystem::recursive_directory_iterator(
                std::string{HOME} + "/pic/waifu", std::filesystem::directory_options::follow_directory_symlink | std::filesystem::directory_options::skip_permission_denied, ec);
            for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (!it->is_regular_file(ec))
                    continue;
                const auto EXT = lower(it->path().extension().string());
                if (EXT == ".png" || EXT == ".jpg" || EXT == ".jpeg" || EXT == ".gif" || EXT == ".webp" || EXT == ".bmp" || EXT == ".svg")
                    found.push_back(it->path().string());
            }
            return found.empty() ? "" : found[std::random_device{}() % found.size()];
        }();
        return ICON;
    }

    // battery-watch.sh's exact alerts, edge-triggered off the same uevents as
    // the gauge (the minute tick is the failsafe). Never called at INIT: the
    // first minute tick covers a login-low, after hyprnotify is up — hyprbar
    // loads before the notification daemon does.
    void checkBatteryAlerts() {
        if (batteryDir.empty())
            return;

        std::ifstream cf(batteryDir + "/capacity"), sf(batteryDir + "/status");
        std::string   cap, status;
        if (!cf || !std::getline(cf, cap) || !sf || !std::getline(sf, status))
            return;

        int capN = 0;
        try {
            capN = std::stoi(cap);
        } catch (...) { return; }

        constexpr int      WARN = 15, CRIT = 7;
        static std::string lastStatus;
        static bool        warned = false, critical = false;

        // urgency 0/1/2; 9990 = the script's pinned replace-in-place id
        const auto NOTIFY = [](uint8_t urgency, int32_t timeoutMs, const char* summary, const std::string& body) {
            Tray::notify("battery", 9990, batteryWaifu(), summary, body, urgency, timeoutMs);
        };

        if (!lastStatus.empty() && status != lastStatus) {
            if (status == "Charging")
                NOTIFY(0, 3000, "Battery", "AC connected — " + cap + "%");
            else if (status == "Discharging")
                NOTIFY(0, 3000, "Battery", "AC disconnected — " + cap + "%");
        }
        lastStatus = status;

        if (status == "Discharging") {
            if (capN <= CRIT && !critical) {
                NOTIFY(2, 0, "Battery critical", cap + "% — plug in now");
                // warned latches too: landing straight in the critical band
                // (login, resume) must not let the next tick's "Battery low"
                // replace the sticky critical card — one pinned id
                warned = critical = true;
            } else if (capN <= WARN && !warned) {
                NOTIFY(1, 6000, "Battery low", cap + "%");
                warned = true;
            }
        } else
            warned = critical = false;
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
