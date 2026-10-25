// awesome/shell/battery.cpp — the battery: gauge state from sysfs, the old
// battery-watch.sh alerts, and its widget (the level and the outline cell)
#include "shell/shell.hpp"
#include "shell/battery_state.hpp"

#include "core/glyph.hpp"
#include "core/theme.hpp"
#include "notify/model.hpp"

namespace NAwesome::Shell {

    static int         batteryPercent  = -1;    // -1 = unknown level; batteryDirs decides whether the widget exists
    static bool        batteryPlugged  = false; // known external-power status, never inferred from Unknown
    static bool        batteryDefend   = false; // plugged but held at the charge limit (Android's defender)
    static bool        batterySave     = false; // explicit power-profiles-daemon power-saver state
    static bool        batteryUnknown  = false; // Pixel's question attribution; higher priority than every other state
    // the system packs (/sys/class/power_supply/BATx), empty = none: a
    // dual-pack laptop drains one pack first, and the other one is half the
    // charge the user has
    static std::vector<std::string> batteryDirs;
    constexpr size_t                MAX_PACKS = 8;
    // the gauge is read ONCE per event (uevent or minute tick) and both
    // consumers — the widget's display state and the alerts' edge detection —
    // read the result: the raw status string is what the alerts key off, and
    // reading it twice meant four sysfs opens per event for one sample
    static std::string batteryStatus;
    // the alerts' edge state (alerts() below): out here with the rest of the
    // battery's state so exit() leaves nothing latched
    static std::string alertLastStatus;
    static bool        alertWarned = false, alertCritical = false;

    // Power Profiles Daemon is the Linux-wide explicit power-save state. It
    // owns a separate system-bus link because tray's link is session-local.
    static NAwesome::CBusLink         powerProfilesBus;
    static std::unique_ptr<sdbus::IProxy> powerProfilesProxy;
    static std::unique_ptr<sdbus::IProxy> powerProfilesBroker;
    static uint64_t                       powerProfileGeneration = 0;

    static constexpr const char* POWER_PROFILES_SERVICE = "net.hadess.PowerProfiles";
    static constexpr const char* POWER_PROFILES_PATH    = "/net/hadess/PowerProfiles";
    static constexpr const char* POWER_PROFILES_IFACE   = "net.hadess.PowerProfiles";

    static void applyPowerSave(bool save) {
        if (save == batterySave)
            return;
        batterySave = save;
        barChanged();
    }

    static void refreshPowerProfile() {
        if (!powerProfilesBus.conn() || !powerProfilesProxy)
            return;
        const uint64_t GENERATION = ++powerProfileGeneration;
        try {
            powerProfilesProxy->getPropertyAsync("ActiveProfile").onInterface(POWER_PROFILES_IFACE).uponReplyInvoke([GENERATION](std::optional<sdbus::Error> error, sdbus::Variant profile) {
                if (GENERATION != powerProfileGeneration)
                    return;
                bool save = false;
                if (!error)
                    try {
                        save = Battery::isPowerSaverProfile(profile.get<std::string>());
                    } catch (...) {}
                applyPowerSave(save);
            });
            powerProfilesBus.pollSoon();
        } catch (...) {
            applyPowerSave(false);
        }
    }

    static void initPowerProfiles() {
        powerProfilesBus.dropOwned = []() {
            ++powerProfileGeneration;
            powerProfilesProxy.reset();
            powerProfilesBroker.reset();
        };
        powerProfilesBus.afterTeardown = []() { applyPowerSave(false); };
        try {
            powerProfilesBus.open(true);
            powerProfilesProxy.reset(sdbus::createProxy(*powerProfilesBus.conn(), sdbus::ServiceName{POWER_PROFILES_SERVICE}, sdbus::ObjectPath{POWER_PROFILES_PATH}).release());
            powerProfilesBroker.reset(sdbus::createProxy(*powerProfilesBus.conn(), sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"}).release());
            powerProfilesProxy->uponSignal("PropertiesChanged")
                .onInterface("org.freedesktop.DBus.Properties")
                .call([](std::string iface, std::map<std::string, sdbus::Variant> changed, std::vector<std::string> invalidated) {
                    if (iface != POWER_PROFILES_IFACE)
                        return;
                    const auto IT = changed.find("ActiveProfile");
                    if (IT == changed.end()) {
                        if (std::find(invalidated.begin(), invalidated.end(), "ActiveProfile") != invalidated.end())
                            refreshPowerProfile();
                        return;
                    }
                    ++powerProfileGeneration; // invalidate an older async property reply
                    try {
                        applyPowerSave(Battery::isPowerSaverProfile(IT->second.get<std::string>()));
                    } catch (...) {
                        refreshPowerProfile();
                    }
                });
            powerProfilesBroker->uponSignal("NameOwnerChanged").onInterface("org.freedesktop.DBus").call([](std::string name, std::string, std::string newOwner) {
                if (name != POWER_PROFILES_SERVICE)
                    return;
                if (newOwner.empty()) {
                    ++powerProfileGeneration;
                    applyPowerSave(false);
                    return;
                }
                refreshPowerProfile();
            });
            refreshPowerProfile();
            powerProfilesBus.sync();
        } catch (...) {
            powerProfilesBus.close();
        }
    }

    static bool        hasBattery() {
        return !batteryDirs.empty();
    }

    static void findBatteries() {
        batteryDirs.clear();
        std::error_code ec;
        // increment(ec): the range-for's operator++ throws
        for (auto it = std::filesystem::directory_iterator("/sys/class/power_supply", ec); !ec && it != std::filesystem::end(it); it.increment(ec)) {
            const auto&   e = *it;
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
            batteryDirs.push_back(e.path());
            if (batteryDirs.size() >= MAX_PACKS)
                break;
        }
        std::ranges::sort(batteryDirs); // BAT0 first: a stable read order
    }

    static std::optional<long long> readSysInt(const std::string& path) {
        std::ifstream f(path);
        std::string   s;
        if (!f || !std::getline(f, s))
            return std::nullopt;
        try {
            return std::stoll(s);
        } catch (...) { return std::nullopt; }
    }
    static std::string readSysLine(const std::string& path) {
        std::ifstream f(path);
        std::string   s;
        if (f)
            std::getline(f, s);
        return s;
    }

    // ---- gauge refresh + the alerts (the old scripts/battery-watch.sh) ----

    namespace Battery {
        bool refresh() {
            int  pc      = -1;
            bool plugged = false, defend = false, unknown = false;
            batteryStatus.clear();
            if (!batteryDirs.empty()) {
                // the packs as ONE battery, upower's display device: the
                // level weighted by each pack's energy (charge where a pack
                // reports no energy), the status the most telling of theirs
                long long eNow = 0, eFull = 0, cNow = 0, cFull = 0;
                bool      allEnergy = true, allCharge = true;
                int       capSum = 0, capN = 0;
                bool      anyCharging = false, anyDischarging = false, anyHolding = false, allFull = true, anyRead = false;
                for (const auto& D : batteryDirs) {
                    if (const auto C = readSysInt(D + "/capacity")) {
                        capSum += (int)std::clamp(*C, 0LL, 100LL);
                        ++capN;
                    }
                    const auto EN = readSysInt(D + "/energy_now"), EF = readSysInt(D + "/energy_full");
                    const auto CN = readSysInt(D + "/charge_now"), CF = readSysInt(D + "/charge_full");
                    allEnergy = allEnergy && EN && EF && *EF > 0;
                    allCharge = allCharge && CN && CF && *CF > 0;
                    if (allEnergy) {
                        eNow += *EN;
                        eFull += *EF;
                    }
                    if (allCharge) {
                        cNow += *CN;
                        cFull += *CF;
                    }
                    const auto S = readSysLine(D + "/status");
                    if (S.empty() || S == "Unknown")
                        continue;
                    anyRead = true;
                    anyCharging |= S == "Charging";
                    anyDischarging |= S == "Discharging";
                    anyHolding |= S == "Not charging";
                    allFull = allFull && S == "Full";
                    // Android's defender = plugged but deliberately not
                    // charging. A charge_control_end_threshold below 100
                    // turns "Not charging" from a transient (thermal,
                    // top-off) into the limiter's hold state — including the
                    // start/end hysteresis band, where capacity sits below
                    // the threshold.
                    if (S == "Not charging")
                        if (const auto LIM = readSysInt(D + "/charge_control_end_threshold"); LIM && *LIM < 100)
                            defend = true;
                }
                if (batteryDirs.size() == 1)
                    pc = capN ? capSum : -1; // one pack: its own gauge, as every other UI shows it
                else if (allEnergy && eFull > 0)
                    pc = (int)std::clamp(std::llround(100.0 * (double)eNow / (double)eFull), 0LL, 100LL);
                else if (allCharge && cFull > 0)
                    pc = (int)std::clamp(std::llround(100.0 * (double)cNow / (double)cFull), 0LL, 100LL);
                else if (capN)
                    pc = capSum / capN;
                const std::string status = !anyRead ? "Unknown" :
                    anyCharging                     ? "Charging" :
                    anyDischarging                  ? "Discharging" :
                    allFull                         ? "Full" :
                    anyHolding                      ? "Not charging" :
                                                      "Unknown";
                plugged       = statusIsPlugged(status);
                defend        = defend && status == "Not charging";
                batteryStatus = status;
                unknown       = pc < 0 || status == "Unknown";
            }
            if (batteryPercent == pc && batteryPlugged == plugged && batteryDefend == defend && batteryUnknown == unknown)
                return false;
            batteryPercent  = pc;
            batteryPlugged  = plugged;
            batteryDefend   = defend;
            batteryUnknown  = unknown;
            return true;
        }

        // Edge-triggered off the same uevents as the gauge (the minute tick is
        // the failsafe); thresholds are Android's lines (20 low / 5 critical —
        // the pill's error red shares the 20 line). Never called at INIT:
        // the shell module registers before the notify module, so the first
        // minute tick — not init — is the first moment an alert can land.
        void alerts() {
            // refresh() runs first at both call sites (the uevent and the
            // minute tick) and leaves this sample behind
            if (batteryPercent < 0 || batteryStatus.empty())
                return;

            const auto&        status = batteryStatus;
            const int          capN   = batteryPercent;
            const std::string  cap    = std::to_string(capN);

            constexpr int WARN = 20, CRIT = 5;

            // urgency 0/1/2; 9990 = the script's pinned replace-in-place id.
            // The semantic icon name keeps this fixed-id card on the native
            // battery mark instead of falling through to the generic app grid.
            // The plug edges are feedback, a capsule (the glyph); low and
            // critical stay notification cards
            const auto NOTIFY = [](uint8_t urgency, int32_t timeoutMs, const char* summary, const std::string& body, bool capsule = false) {
                NAwesome::Notify::Model::postCard({.id            = 9990,
                                                   .icon          = "battery-symbolic",
                                                   .summary       = summary,
                                                   .body          = body.c_str(),
                                                   .expireTimeout = timeoutMs,
                                                   .urgency       = urgency,
                                                   .osd           = true,
                                                   .glyph         = capsule ? NAwesome::Glyph::eGlyph::BATTERY : NAwesome::Glyph::eGlyph::NONE}); // id 9990: the alerts' replace-in-place slot, critical stays sticky
            };

            // ACPI transitions/resume report transient "Unknown" — it must not
            // count as an edge (spurious AC cards) nor reset the latches (a
            // duplicate low card, or a 3s transient replacing the sticky critical)
            const bool KNOWN = status == "Charging" || status == "Discharging" || status == "Full" || status == "Not charging";

            if (KNOWN && !alertLastStatus.empty() && status != alertLastStatus) {
                if (status == "Charging")
                    NOTIFY(0, 3000, "Charging", cap + "%", true);
                else if (status == "Discharging")
                    NOTIFY(0, 3000, "On Battery", cap + "%", true);
            }
            if (KNOWN)
                alertLastStatus = status;

            if (status == "Discharging") {
                if (capN <= CRIT && !alertCritical) {
                    NOTIFY(2, 0, "Battery critical", cap + "% — plug in now");
                    // warned latches too: landing straight in the critical band
                    // (login, resume) must not let the next tick's "Battery low"
                    // replace the sticky critical card — one pinned id
                    alertWarned = alertCritical = true;
                } else if (capN <= WARN && !alertWarned) {
                    NOTIFY(1, 6000, "Battery low", cap + "%");
                    alertWarned = true;
                }
            } else if (KNOWN)
                alertWarned = alertCritical = false;
        }
    } // namespace Battery

    // ---- the udev watch ----
    //
    // Event-driven off power_supply uevents: plug/unplug and capacity changes
    // arrive as kernel events instead of a poll (the minute tick is the
    // failsafe read). Skipped on desktops (no gauge).
    static udev*            g_udev       = nullptr;
    static udev_monitor*    g_udevMon    = nullptr;
    static wl_event_source* g_batterySrc = nullptr;

    static int              onBatteryUevent(int, uint32_t, void*) {
        // libudev does not coalesce for us. Yield after a small batch so a
        // noisy power-supply driver cannot monopolize the compositor loop.
        constexpr size_t MAX_UDEV_EVENTS_PER_DISPATCH = 16;
        bool rescan = false, relevant = false;
        if (g_udevMon)
            for (size_t i = 0; i < MAX_UDEV_EVENTS_PER_DISPATCH; ++i) {
                auto* DEV = udev_monitor_receive_device(g_udevMon);
                if (!DEV)
                    break;
                // Only the system's packs and the mains supply move the
                // gauge: a USB-C port's or a headset's battery (scope
                // Device) re-read every pack's sysfs for nothing — a read
                // that can evaluate ACPI methods
                const char*            T      = udev_device_get_property_value(DEV, "POWER_SUPPLY_TYPE");
                const char*            S      = udev_device_get_property_value(DEV, "POWER_SUPPLY_SCOPE");
                const char*            A      = udev_device_get_action(DEV);
                const std::string_view TYPE   = T ? T : "", SCOPE = S ? S : "", ACTION = A ? A : "";
                const bool             PACK   = TYPE == "Battery" && SCOPE != "Device";
                const bool             HOTPLUG = ACTION == "add" || ACTION == "remove";
                // a pack inserted or removed (a ThinkPad's swappable one);
                // a removal may arrive without its properties
                if (HOTPLUG && (PACK || TYPE.empty()))
                    rescan = true;
                if (PACK || TYPE == "Mains" || (HOTPLUG && TYPE.empty()))
                    relevant = true;
                udev_device_unref(DEV);
            }
        if (!relevant)
            return 0;
        if (rescan)
            findBatteries();
        if (Battery::refresh())
            barChanged(); // a uevent lands in the event loop: warm now, no deferral needed
        Battery::alerts();
        return 0;
    }

    namespace Battery {
        void init() {
            findBatteries();
            refresh();
            if (!hasBattery() || !g_pCompositor)
                return;
            initPowerProfiles();
            g_udev = udev_new();
            if (!g_udev)
                return;
            g_udevMon = udev_monitor_new_from_netlink(g_udev, "udev");
            if (g_udevMon) {
                udev_monitor_filter_add_match_subsystem_devtype(g_udevMon, "power_supply", nullptr);
                udev_monitor_enable_receiving(g_udevMon);
                const int FD = udev_monitor_get_fd(g_udevMon);
                if (FD >= 0)
                    g_batterySrc = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, FD, WL_EVENT_READABLE, onBatteryUevent, nullptr);
            }
            if (!g_batterySrc) { // setup failed partway: the minute tick still refreshes
                if (g_udevMon)
                    udev_monitor_unref(g_udevMon);
                udev_unref(g_udev);
                g_udevMon = nullptr;
                g_udev    = nullptr;
            }
        }

        void exit() {
            powerProfilesBus.close(); // fd sources out before the proxy and system connection die
            if (g_batterySrc)
                wl_event_source_remove(g_batterySrc); // before the monitor that owns the fd
            if (g_udevMon)
                udev_monitor_unref(g_udevMon);
            if (g_udev)
                udev_unref(g_udev);
            g_batterySrc = nullptr;
            g_udevMon    = nullptr;
            g_udev       = nullptr;
            batteryPercent = -1;
            batteryPlugged = batteryDefend = batterySave = batteryUnknown = false;
            batteryDirs.clear();
            batteryStatus.clear();
            alertLastStatus.clear();
            alertWarned = alertCritical = false;
        }
    } // namespace Battery

    // ---- the widget ----

    namespace {
        class CBatteryWidget : public IWidget {
          public:
            // "62%", 6 apart, then the outline cell with the level inside:
            // red text and level when low, a green level charging (and held
            // at the limit), amber in power save
            static constexpr double GLYPH_W = 22, GLYPH_H = 11, GAP = 6;
            // the cell's inside (the glyph's 18 x 9.6 body under a 1.2 stroke)
            static constexpr double LEVEL_X = 2.4, LEVEL_Y = 2.4, LEVEL_W = 14.6, LEVEL_H = 6.2, LEVEL_MIN = 2.2;

            struct SLook {
                CHyprColor text, level;
            };
            static SLook look(const SFrame& F) {
                const auto STATE = Battery::visualState(batteryPercent, batteryUnknown, batterySave, batteryDefend, batteryPlugged);
                switch (STATE.color) {
                    case Battery::eColor::Error: {
                        const auto LOW = color(cfg().getColor("plugin:awesome:shell:col_low"));
                        return {LOW, LOW};
                    }
                    case Battery::eColor::Active: return {F.fg, color(cfg().getColor("plugin:awesome:shell:col_charging"))};
                    case Battery::eColor::Warning: return {F.fg, color(cfg().getColor("plugin:awesome:shell:col_powersave"))};
                    case Battery::eColor::None: break;
                }
                return {F.fg, F.fg};
            }

            static SP<ITexture> label(const SPaint& P, const SLook& L) {
                if (batteryPercent < 0)
                    return nullptr;
                static std::string TXT; // reused; main thread only
                TXT = std::to_string(batteryPercent);
                TXT += '%';
                return textTex(TXT, L.text, P.pt, 0, "", 500);
            }

            static SP<ITexture> cell(const SPaint& P) {
                return NAwesome::Glyph::tex(NAwesome::Glyph::eGlyph::BATTERY, NAwesome::tSub(), (int)std::lround(GLYPH_W * P.scale));
            }

            double fit(const SPaint& P, const SFrame& F) override {
                if (!hasBattery())
                    return 0; // no system pack: hidden
                cell(P); // the warm builds what the draw paints
                const auto T = label(P, look(F));
                return (T ? T->m_size.x / P.scale + GAP : 0) + GLYPH_W;
            }

            void draw(const SPaint& P, const SFrame& F, const CBox& box) override {
                const auto L = look(F);
                double     x = box.x;
                if (const auto T = label(P, L); T && T->m_texID != 0) {
                    P.texIn(T, CBox{x, box.y, T->m_size.x / P.scale, P.h});
                    x += T->m_size.x / P.scale + GAP;
                }
                const CBox G{x, box.y + (P.h - GLYPH_H) / 2, GLYPH_W, GLYPH_H};
                P.texIn(cell(P), G);
                if (batteryPercent >= 0)
                    P.rect(CBox{G.x + LEVEL_X, G.y + LEVEL_Y, std::max(LEVEL_MIN, LEVEL_W * batteryPercent / 100.0), LEVEL_H}, L.level, (int)std::lround(P.scale), 2.f);
            }
        };
    } // namespace

    IWidget& batteryWidget() {
        static CBatteryWidget W;
        return W;
    }

} // namespace NAwesome::Shell
