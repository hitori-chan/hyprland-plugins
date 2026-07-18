// hyprbar/util.cpp — small shared helpers: geometry, damage, colors, clock/battery state

#include "hyprbar.hpp"

namespace NHyprbar {

    bool               warming = false, texStale = false; // the texture rule — see hyprbar.hpp

    std::string        clockText;
    int                batteryPercent  = -1;
    bool               batteryCharging = false;
    static std::string batteryDir; // /sys/class/power_supply/BATx, empty = none

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

    std::string letterOf(const std::string& s) {
        if (s.empty())
            return "?";
        size_t n = 1;
        while (n < s.size() && (s[n] & 0xC0) == 0x80)
            n++;
        std::string L = s.substr(0, n);
        if (n == 1)
            L[0] = std::toupper((unsigned char)L[0]);
        return L;
    }

    // ---- clock / battery ----

    bool hasBattery() {
        return !batteryDir.empty();
    }

    void findBattery() {
        batteryDir.clear();
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator("/sys/class/power_supply", ec)) {
            std::ifstream t(e.path() / "type");
            std::string   type;
            if (!t || !std::getline(t, type) || type != "Battery")
                continue;
            // peripheral batteries (HID mice, headsets) also read "Battery";
            // their scope says "Device" — the system pack is "System" or none
            std::ifstream sc(e.path() / "scope");
            std::string   scope;
            if (sc && std::getline(sc, scope) && scope == "Device")
                continue;
            batteryDir = e.path();
            break;
        }
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

        int  pc       = -1;
        bool charging = false;
        if (!batteryDir.empty()) {
            std::ifstream cf(batteryDir + "/capacity"), sf(batteryDir + "/status");
            std::string   cap, status;
            if (cf && std::getline(cf, cap)) {
                try {
                    pc = std::clamp(std::stoi(cap), 0, 100);
                } catch (...) {}
                // every plugged state (Charging/Full/Not charging) colors the
                // pill; only Discharging runs on the cell
                charging = sf && std::getline(sf, status) && status != "Discharging";
            }
        }
        if (batteryPercent != pc || batteryCharging != charging) {
            batteryPercent  = pc;
            batteryCharging = charging;
            changed         = true;
        }

        return changed;
    }

    // ---- battery alerts (the old scripts/battery-watch.sh, folded in) ----

    // Edge-triggered off the same uevents as the gauge (the minute tick is
    // the failsafe); thresholds are Android's lines (20 low / 5 critical),
    // matching the pill's amber/crimson bands. Never called at INIT: the
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

        constexpr int      WARN = 20, CRIT = 5;
        static std::string lastStatus;
        static bool        warned = false, critical = false;

        // urgency 0/1/2; 9990 = the script's pinned replace-in-place id.
        // No icon: the daemon's fallback_icon_dir rolls the card its face.
        const auto NOTIFY = [](uint8_t urgency, int32_t timeoutMs, const char* summary, const std::string& body) {
            Tray::notify("battery", 9990, "", summary, body, urgency, timeoutMs);
        };

        // ACPI transitions/resume report transient "Unknown" — it must not
        // count as an edge (spurious AC cards) nor reset the latches (a
        // duplicate low card, or a 3s transient replacing the sticky critical)
        const bool KNOWN = status == "Charging" || status == "Discharging" || status == "Full" || status == "Not charging";

        if (KNOWN && !lastStatus.empty() && status != lastStatus) {
            if (status == "Charging")
                NOTIFY(0, 3000, "Battery", "AC connected — " + cap + "%");
            else if (status == "Discharging")
                NOTIFY(0, 3000, "Battery", "AC disconnected — " + cap + "%");
        }
        if (KNOWN)
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
        } else if (KNOWN)
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
