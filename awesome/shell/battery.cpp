// The battery: gauge state from sysfs, the plug/low/critical alerts, the
// widget drawing the battery pill (shell/battery_pill.hpp), and the note it
// shows under the pointer
#include "shell/shell.hpp"
#include "shell/battery_pill.hpp"

#include "core/glyph.hpp"
#include "notify/model.hpp"

namespace NAwesome::Shell {

    static int         batteryPercent  = -1;    // -1 = unknown level; batteryDirs decides whether the widget exists
    static bool        batteryPlugged  = false; // known external-power status, never inferred from Unknown
    static bool        batteryDefend   = false; // plugged but held at the charge limit
    static bool        batterySave     = false; // explicit power-profiles-daemon power-saver state
    static bool        batteryUnknown  = false; // the question attribution; higher priority than every other state
    // the system packs (/sys/class/power_supply/BATx), empty = none: a
    // dual-pack laptop drains one pack first, and the other one is half the
    // charge the user has
    static std::vector<std::string> batteryDirs;
    constexpr size_t                MAX_PACKS = 8;
    // the gauge is read ONCE per event (uevent or minute tick) and both
    // consumers — the pill's display state and the alerts' edge detection —
    // read the result: the raw status string is what the alerts key off
    static std::string batteryStatus;
    // the alerts' edge state (alerts() below): out here with the rest of the
    // battery's state so exit() leaves nothing latched
    static std::string alertLastStatus;
    static bool        alertWarned = false, alertCritical = false;

    // The note's rate: the packs' summed draw (power over energy, else
    // current over charge), smoothed across the samples (the uevents and
    // the minute tick: no timer of its own) and started over when the
    // power state flips. A draw under 1 mW (1 mA) is no rate.
    static double      rateEma = 0;
    static std::string rateStatus;
    static int         noteMinutes = -1, noteFullAt = -1, noteLimit = -1;

    static void sampleRate(const std::string& status, double draw, double now, double full, int limit) {
        noteLimit = limit;
        if (status != rateStatus) {
            rateStatus = status;
            rateEma    = 0;
        }
        const bool MOVING = status == "Charging" || status == "Discharging";
        if (MOVING && draw >= 1000)
            rateEma = rateEma > 0 ? rateEma * 0.7 + draw * 0.3 : draw;
        noteMinutes = noteFullAt = -1;
        if (!MOVING || rateEma <= 0 || full <= 0)
            return;
        // charging stops at the limit when one holds
        const double LEFT = status == "Discharging" ? now : std::max(0.0, full * (limit > 0 ? limit : 100) / 100.0 - now);
        noteMinutes       = (int)std::clamp(std::lround(LEFT / rateEma * 60.0), 0L, 99L * 60 + 59);
        if (status == "Charging") {
            const time_t AT = std::time(nullptr) + (time_t)noteMinutes * 60;
            std::tm      at{};
            localtime_r(&AT, &at);
            noteFullAt = at.tm_hour * 60 + at.tm_min;
        }
    }

    // the note (below): its state up here with the rest
    static SP<CEventLoopTimer> noteTimer;
    static bool                noteShown = false;
    static bool                noteDrawn = false; // the last warm laid it out
    static MONITORID           noteMon   = MONITOR_INVALID;
    static CBox                notePill;  // the pill as last drawn on noteMon, global logical
    static std::string         noteLine;  // the words as the last warm composed them
    static std::string         noteFont;  // the cards' face
    constexpr auto             NOTE_DELAY = std::chrono::milliseconds(500);
    constexpr double           NOTE_H = 24, NOTE_PADX = 9, NOTE_GAP = 6, NOTE_OUT = 8, NOTE_MAXW = 360;

    static PHLMONITOR noteMonitor() {
        for (const auto& M : State::monitorState()->monitors())
            if (M && M->m_id == noteMon)
                return M;
        return nullptr;
    }

    // the note `w` wide, under the bar, its right edge past the pill's
    static CBox noteBoxOf(const PHLMONITOR& mon, double w) {
        const auto   MB = mon->logicalBox();
        const double R  = std::min(notePill.x + notePill.w + NOTE_OUT, MB.x + MB.w - 4);
        return CBox{R - w, MB.y + barHeight(mon) + NOTE_GAP, w, NOTE_H};
    }

    static void damageNote() {
        if (const auto M = noteMonitor(); M && g_pHyprRenderer)
            g_pHyprRenderer->damageBox(noteBoxOf(M, NOTE_MAXW).expand(NAwesome::SPaint::noteReach() + 1));
    }

    static void hideNote() {
        if (noteTimer)
            noteTimer->updateTimeout(std::nullopt);
        if (noteShown)
            damageNote();
        noteShown = false;
    }

    // Power Profiles Daemon is the Linux-wide explicit power-save state. It
    // owns a separate system-bus link because tray's link is session-local.
    static NAwesome::CBusLink         powerProfilesBus;
    static std::unique_ptr<sdbus::IProxy> powerProfilesProxy;
    static sdbus::Slot                    powerProfilesMatch, powerProfilesOwner; // async matches: no round trip at load
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
            powerProfilesMatch.reset();
            powerProfilesOwner.reset();
            powerProfilesProxy.reset();
        };
        powerProfilesBus.afterTeardown = []() { applyPowerSave(false); };
        try {
            powerProfilesBus.open(true);
            powerProfilesProxy.reset(sdbus::createProxy(*powerProfilesBus.conn(), sdbus::ServiceName{POWER_PROFILES_SERVICE}, sdbus::ObjectPath{POWER_PROFILES_PATH}).release());
            powerProfilesMatch = powerProfilesBus.conn()->addMatchAsync(
                "type='signal',sender='net.hadess.PowerProfiles',path='/net/hadess/PowerProfiles',interface='org.freedesktop.DBus.Properties',"
                "member='PropertiesChanged',arg0='net.hadess.PowerProfiles'",
                [](sdbus::Message m) {
                    std::string                           iface;
                    std::map<std::string, sdbus::Variant> changed;
                    std::vector<std::string>              invalidated;
                    try {
                        m >> iface >> changed >> invalidated;
                    } catch (...) { return; }
                    const auto IT = changed.find("ActiveProfile");
                    if (IT == changed.end()) {
                        if (std::ranges::contains(invalidated, std::string_view{"ActiveProfile"}))
                            refreshPowerProfile();
                        return;
                    }
                    ++powerProfileGeneration; // invalidate an older async property reply
                    try {
                        applyPowerSave(Battery::isPowerSaverProfile(IT->second.get<std::string>()));
                    } catch (...) {
                        refreshPowerProfile();
                    }
                },
                [](sdbus::Message) {}, sdbus::return_slot);
            powerProfilesOwner = powerProfilesBus.conn()->addMatchAsync(
                "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='net.hadess.PowerProfiles'",
                [](sdbus::Message m) {
                    std::string name, from, to;
                    try {
                        m >> name >> from >> to;
                    } catch (...) { return; }
                    if (to.empty()) {
                        ++powerProfileGeneration;
                        applyPowerSave(false);
                        return;
                    }
                    refreshPowerProfile();
                },
                [](sdbus::Message) {}, sdbus::return_slot);
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

    // the pill as a texture: battery_pill.hpp rasters it, the renderer
    // takes it from the warm (the gate's caller)
    static SP<ITexture> batteryPill(const Battery::SPillSpec& spec) {
        auto* SURF = Battery::rasterPill(spec);
        if (!SURF)
            return nullptr;
        auto tex = g_pHyprRenderer->createTexture(SURF);
        cairo_surface_destroy(SURF);
        return tex;
    }

    // per physical height, so mixed-scale monitors don't evict each other
    struct SPill {
        SP<ITexture> tex;
        uint64_t     key = 0;
    };
    static std::unordered_map<int, SPill> pillCache;

    // ---- gauge refresh + the alerts ----

    namespace Battery {
        bool refresh() {
            int  pc      = -1;
            bool plugged = false, defend = false, unknown = false;
            batteryStatus.clear();
            if (!batteryDirs.empty()) {
                // the packs as ONE battery, upower's display device: the
                // level weighted by each pack's energy (charge where a pack
                // reports no energy), the status the most telling of theirs
                long long eNow = 0, eFull = 0, cNow = 0, cFull = 0, pNow = 0, iNow = 0;
                bool      allEnergy = true, allCharge = true, allPower = true, allCurrent = true;
                int       limit = -1; // the lowest charge limit a pack holds to
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
                    // a driver may sign the draw by its direction
                    const auto PN = readSysInt(D + "/power_now"), IN = readSysInt(D + "/current_now");
                    allPower   = allPower && PN;
                    allCurrent = allCurrent && IN;
                    if (allPower)
                        pNow += std::llabs(*PN);
                    if (allCurrent)
                        iNow += std::llabs(*IN);
                    const auto LIM = readSysInt(D + "/charge_control_end_threshold");
                    if (LIM && *LIM > 0 && *LIM < 100)
                        limit = limit < 0 ? (int)*LIM : std::min(limit, (int)*LIM);
                    const auto S = readSysLine(D + "/status");
                    if (S.empty() || S == "Unknown")
                        continue;
                    anyRead = true;
                    anyCharging |= S == "Charging";
                    anyDischarging |= S == "Discharging";
                    anyHolding |= S == "Not charging";
                    allFull = allFull && S == "Full";
                    // Defended = plugged but deliberately not charging. A
                    // charge_control_end_threshold below 100 turns "Not
                    // charging" from a transient (thermal, top-off) into the
                    // limiter's hold state — including the start/end
                    // hysteresis band, where capacity sits below the
                    // threshold.
                    if (S == "Not charging" && LIM && *LIM < 100)
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
                const bool ENERGY = allEnergy && allPower && eFull > 0, CHARGE = !ENERGY && allCharge && allCurrent && cFull > 0;
                sampleRate(status, ENERGY ? (double)pNow : CHARGE ? (double)iNow : 0, ENERGY ? (double)eNow : (double)cNow, ENERGY ? (double)eFull : (double)cFull, limit);
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
        // the failsafe); thresholds 20 low / 5 critical (the pill's error red
        // shares the 20 line). Never called at INIT: the shell module
        // registers before the notify module, so the first minute tick — not
        // init — is the first moment an alert can land.
        void alerts() {
            // refresh() runs first at both call sites (the uevent and the
            // minute tick) and leaves this sample behind
            if (batteryPercent < 0 || batteryStatus.empty())
                return;

            const auto&        status = batteryStatus;
            const int          capN   = batteryPercent;
            const std::string  cap    = std::to_string(capN);

            constexpr int WARN = 20, CRIT = 5;

            // urgency 0/1/2.
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
                // Device) must not re-read every pack's sysfs, a read that
                // can evaluate ACPI methods
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
        Battery::noteChanged();
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
            if (g_pEventLoopManager) {
                noteTimer = makeShared<CEventLoopTimer>(
                    std::nullopt,
                    [](SP<CEventLoopTimer>, void*) {
                        if (NAwesome::compositorShuttingDown() || NAwesome::sessionLocked() || noteMon == MONITOR_INVALID)
                            return;
                        noteShown = true;
                        warmPopups(); // the popup layer draws the note
                        damageNote();
                    },
                    nullptr);
                g_pEventLoopManager->addTimer(noteTimer);
            }
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
            if (noteTimer && g_pEventLoopManager)
                g_pEventLoopManager->removeTimer(noteTimer);
            noteTimer.reset();
            noteShown = noteDrawn = false;
            noteMon   = MONITOR_INVALID;
            noteLine.clear();
            noteFont.clear();
            rateEma = 0;
            rateStatus.clear();
            noteMinutes = noteFullAt = noteLimit = -1;
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
            pillCache.clear();
            batteryPercent = -1;
            batteryPlugged = batteryDefend = batterySave = batteryUnknown = false;
            batteryDirs.clear();
            batteryStatus.clear();
            alertLastStatus.clear();
            alertWarned = alertCritical = false;
        }
    } // namespace Battery

    // ---- the note: the pill's state in words after a rest ----
    //
    // A dark note under the bar, its right edge 8 px past the pill's; gone
    // when the pointer leaves, on the lock (the hover resets), under a real
    // fullscreen and while a menu is open (strip.cpp's popupsOn). Its words
    // are composed by the popup layer's warm; a click still does nothing.

    namespace Battery {
        void hover(const SBarHover& h) {
            const bool ON = h.widget == &batteryWidget();
            if (ON && h.mon == noteMon && noteMon != MONITOR_INVALID)
                return; // still resting on it (a move inside the cell)
            hideNote();
            noteMon = ON ? h.mon : MONITOR_INVALID;
            if (ON && noteTimer)
                noteTimer->updateTimeout(NOTE_DELAY);
        }

        bool noteWanted(const PHLMONITOR& mon) {
            return noteShown && mon && mon->m_id == noteMon && hasBattery();
        }

        void noteLaidOut(const PHLMONITOR& mon, bool on) {
            if (!mon || mon->m_id != noteMon)
                return;
            // gone under a menu, the lock or a fullscreen window: its box
            // hangs below what their own damage covers
            if (noteDrawn && !on)
                damageNote();
            noteDrawn = on;
        }

        std::optional<CBox> noteReach(const PHLMONITOR& mon) {
            if (!noteWanted(mon))
                return std::nullopt;
            return noteBoxOf(mon, NOTE_MAXW).expand(NAwesome::SPaint::noteReach());
        }

        void noteChanged() {
            if (!noteShown)
                return;
            warmPopups();
            damageNote();
        }

        void renderNote(const SPaint& P) {
            if (P.warm) {
                const auto STATE = visualState(batteryPercent, batteryUnknown, batterySave, batteryDefend, batteryPlugged);
                const auto POWER = batteryStatus == "Charging" ? ePower::CHARGING :
                    batteryStatus == "Full" || (batteryPlugged && batteryPercent >= 100) ? ePower::FULL :
                    batteryPlugged                                                        ? ePower::HOLDING :
                                                                                            ePower::BATTERY;
                auto LINE = noteText({.level     = batteryPercent,
                                      .unknown   = STATE.attribution == eAttribution::Unknown,
                                      .powerSave = batterySave,
                                      .defender  = batteryDefend,
                                      .power     = POWER,
                                      .minutes   = noteMinutes,
                                      .fullAt    = noteFullAt,
                                      .limit     = noteLimit});
                // new words under a resting pointer (a power profile, a
                // uevent): the strip's own damage stops above the note
                if (LINE != noteLine) {
                    noteLine = std::move(LINE);
                    damageNote();
                }
                noteFont = std::string{cfg().getS("plugin:awesome:notify:font")};
            }
            const auto   TEX = textTex(noteLine, NAwesome::tTitle(), P.pt, (int)std::lround((NOTE_MAXW - 2 * NOTE_PADX) * P.scale), noteFont, 500);
            const double W   = textInkW(TEX, P.scale) + 2 * NOTE_PADX;
            const CBox   B   = noteBoxOf(P.mon, W);
            P.note(B);
            if (TEX)
                P.tex(TEX, B.x + (B.w - TEX->m_size.x / P.scale) / 2, B.y + (B.h - TEX->m_size.y / P.scale) / 2);
        }

#ifdef AWESOME_GATE
        std::string noteDescribe() {
            return noteShown ? noteLine : std::string{};
        }
#endif
    } // namespace Battery

    namespace {
        class CBatteryWidget : public IWidget {
          public:
            double fit(const SPaint& P, const SFrame& F) override {
                if (!hasBattery())
                    return 0; // no system pack: hidden
                const int  H     = bodyPx(P);
                const auto STATE = Battery::visualState(batteryPercent, batteryUnknown, batterySave, batteryDefend, batteryPlugged);
                const CHyprColor FILL = STATE.color == Battery::eColor::Active ? color(cfg().getColor("plugin:awesome:shell:col_charging")) :
                    STATE.color == Battery::eColor::Warning                    ? color(cfg().getColor("plugin:awesome:shell:col_powersave")) :
                    STATE.color == Battery::eColor::Error                      ? color(cfg().getColor("plugin:awesome:shell:col_low")) :
                                                                                 CHyprColor{1.f, 1.f, 1.f, 1.f};
                // everything the raster reads: a theme change re-keys it
                const uint64_t KEY = ((uint64_t)(batteryPercent + 1) << 1) ^ ((uint64_t)STATE.attribution << 9) ^ (FILL.getAsHex() * 0x9E3779B97F4A7C15ull) ^
                    (F.fg.getAsHex() * 0xC2B2AE3D27D4EB4Full);
                auto& PILL = pillCache[H];
                // the gate, not P.warm: "this pass paints nothing" and "this
                // pass may create a texture" are different questions, and only
                // the gate answers the second one (mayBuild flags texStale when
                // it refuses — a level that moved under a scissored repaint
                // gets its warm and its repaint from that).
                if (PILL.key != KEY && warmGate().mayBuild()) {
                    PILL.tex = batteryPill({.level       = batteryPercent,
                                            .attribution = STATE.attribution,
                                            .heightPx    = H,
                                            .fill        = {FILL.r, FILL.g, FILL.b, FILL.a},
                                            .glyph       = {F.fg.r, F.fg.g, F.fg.b, F.fg.a}});
                    PILL.key = KEY; // stamp even if the build returns null, so a persistent null can't rewarm-storm
                }
                // logical px: the strip's gaps do the spacing
                return (PILL.tex ? PILL.tex->m_size.x : Battery::geometry(H, STATE.attribution).width) / P.scale;
            }

            void draw(const SPaint& P, const SFrame&, const CBox& box) override {
                const auto& PILL = pillCache[bodyPx(P)];
                P.texIn(PILL.tex, box);
                if (P.mon && P.mon->m_id == noteMon)
                    notePill = box; // the note's anchor
            }

          private:
            // 13 px beside the bar's 12 px text, one device px a viewport
            // unit (fractional units blur the digits); it scales with the text
            static int bodyPx(const SPaint& P) {
                return std::max(8, Battery::pixelRound(P.pt * Battery::VIEW_H / 12.0));
            }
        };
    } // namespace

    IWidget& batteryWidget() {
        static CBatteryWidget W;
        return W;
    }

} // namespace NAwesome::Shell
