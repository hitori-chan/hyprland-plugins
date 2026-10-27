// awesome/system/light.cpp — the lights: the display's backlight and the
// keyboard's.
//
// Fork-free: sysfs for current/max (/sys/class/backlight, the keyboard's
// *kbd_backlight LED), ±5% linear steps or the slider's percent (the shown
// percent IS current/max), the display floored at 2 raw so the panel never
// goes black, the write through logind Session.SetBrightness on the
// module's system bus — no root, no udev rule. A key's step answers with a
// capsule (ids 9992/9994) unless the control center shows the slider it
// moved; every level is published to the control center as logind acks it.
//
// The keyboard's light goes out while every display is off and comes back
// with the first one, unless a choice was made in the dark; with its option
// on, it follows the room's light (iio-sensor-proxy).

#include "system.hpp"

#include "notify/model.hpp" // NAwesome::Notify::Model::postCard (the in-process seam)

#include "core/busclient.hpp"
#include "core/controls.hpp"
#include "core/hop.hpp"
#include "core/options.hpp"
#include "core/queries.hpp"
#include "core/state.hpp"

#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <sdbus-c++/sdbus-c++.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace NAwesome::System {

    // The OSD pin ids (the old scripts' contract; the model's OSD band
    // 9990-9999 gives them replace-in-place, no history, no grouping).
    inline constexpr uint32_t ID_BRIGHTNESS = 9992;
    inline constexpr uint32_t ID_KEYBOARD   = 9994;

    static std::unique_ptr<sdbus::IProxy> logindProxy;
    static std::string logindSessionPath; // explicit over "auto": see findLogindSession

    // One light: the device its sysfs class offers, and ONE SetBrightness in
    // flight (presses meanwhile add up into the next one; a stalled logind
    // would otherwise collect a call per press until its 25 s timeout). A
    // slider's absolute choice replaces the steps pending.
    struct SLight {
        const char*        subsystem; // logind's: "backlight", "leds"
        std::string        root;      // the sysfs class dir
        Controls::eLevel   level;
        uint32_t           cardId;
        const char*        title;
        const char*        icon;
        Glyph::eGlyph      glyph;
        int                floor; // raw: a panel never goes black, a keyboard light goes off
        std::string        dev;   // <root>/<dev>, name only
        int                max     = 0;
        bool               busy    = false;
        int                pending = 0; // key steps, signed (±5 % of max each)
        std::optional<int> target;      // an absolute raw level (the slider's percent converted, the dark's)
        // A keypress bases its step on what the previous one just asked
        // for: logind's write is asynchronous, so a fast repeat would read
        // stale sysfs and re-step from the same value. Half a second of
        // trust, then sysfs is the truth again (external tools, resume).
        int             lastSetRaw = -1;
        Time::steady_tp lastSetAt;
        uint64_t        generation = 0;     // a teardown or a bus loss outdates replies
        bool            byUser     = false; // the asks pending are the user's (a key, the slider)
    };
    static SLight             display{.subsystem = "backlight",
                                      .root      = "/sys/class/backlight",
                                      .level     = Controls::eLevel::DISPLAY,
                                      .cardId    = ID_BRIGHTNESS,
                                      .title     = "Brightness",
                                      .icon      = "display-brightness-symbolic",
                                      .glyph     = Glyph::eGlyph::SUN,
                                      .floor     = 2};
    static SLight             keyboard{.subsystem = "leds",
                                       .root      = "/sys/class/leds",
                                       .level     = Controls::eLevel::KEYBOARD,
                                       .cardId    = ID_KEYBOARD,
                                       .title     = "Keyboard",
                                       .icon      = "keyboard-brightness-symbolic",
                                       .glyph     = Glyph::eGlyph::KEYBOARD_LIGHT,
                                       .floor     = 0};

    static std::optional<int> readSysfsInt(const std::string& path) {
        int           v = -1;
        std::ifstream f(path);
        if (!f || !(f >> v) || v < 0)
            return std::nullopt;
        return v;
    }

    static void publishRoom();

    // at init, and again when the device is missing or stops reading (a
    // driver loaded late, a GPU switch, a ddcci monitor plugged in, the
    // keyboard's driver bound after the session started); the room's
    // switch follows the keyboard's presence
    static void findLight(SLight& L) {
        L.dev.clear();
        L.max          = 0;
        L.lastSetRaw   = -1; // the trust window is the old device's: the one found next reads its own sysfs
        const bool KBD = &L == &keyboard;
#ifdef AWESOME_GATE
        // the gate's fake classes, driven by a fake logind on the nested's
        // private system bus: its battery never moves the panel or the keys
        if (const char* DIR = std::getenv(KBD ? "AW_LEDS_DIR" : "AW_BACKLIGHT_DIR"); DIR && *DIR)
            L.root = DIR;
#endif
        // Several backlights (acpi_video0 beside the native panel, a GPU's,
        // ddcci): the systemd rule — firmware, then platform, then raw;
        // directory order only breaks a tie. The firmware interface is the
        // one that reliably moves the panel. The keyboard's LED is the one
        // the kernel names *kbd_backlight (asahi's kbd_backlight,
        // tpacpi::kbd_backlight, ...), the first in name order.
        const auto RANK = [](const std::string& type) { return type == "firmware" ? 0 : type == "platform" ? 1 : type == "raw" ? 2 : 3; };
        int        best = 4;
        std::error_code ec;
        // increment(ec): the range-for's operator++ throws
        for (auto it = std::filesystem::directory_iterator(L.root, ec); !ec && it != std::filesystem::end(it); it.increment(ec)) {
            const auto NAME = it->path().filename().string();
            if (KBD && !NAME.ends_with("kbd_backlight"))
                continue;
            const auto MAX = readSysfsInt(it->path() / "max_brightness");
            if (!MAX || *MAX <= 0)
                continue;
            std::string type;
            if (!KBD)
                std::ifstream(it->path() / "type") >> type;
            const int R = KBD ? 0 : RANK(type);
            if (R < best || (R == best && KBD && NAME < L.dev)) {
                best  = R;
                L.dev = NAME;
                L.max = *MAX;
            }
        }
        if (KBD)
            publishRoom();
    }

    static std::optional<int> readLight(const SLight& L) {
        return L.dev.empty() ? std::nullopt : readSysfsInt(L.root + "/" + L.dev + "/brightness");
    }

    // the light as sysfs has it now: the panel's slider starts there
    static void publishLight(SLight& L) {
        const auto RAW = readLight(L);
        Controls::publish(L.level, {.pct = RAW && L.max > 0 ? (int)std::lround(100.0 * *RAW / L.max) : -1});
    }

    // The old plugin wrote through /session/auto — logind resolves that
    // from the caller's cgroup. That resolution is a SILENT NO-OP in some
    // environments (the write acks, sysfs never moves), so do the
    // resolution in the open: read our own cgroup's session-N.scope and
    // address the session explicitly. An unparseable cgroup (a compositor
    // outside any session scope) falls back to auto — the old behavior.
    static void findLogindSession() {
        logindSessionPath = "/org/freedesktop/login1/session/auto";
        std::ifstream cg("/proc/self/cgroup");
        std::string   line;
        while (std::getline(cg, line)) {
            constexpr std::string_view MARK = "session-";
            if (const auto P = line.rfind(MARK); P != std::string::npos && line.ends_with(".scope")) {
                const auto ID = line.substr(P + MARK.size(), line.size() - (P + MARK.size()) - 6);
                if (!ID.empty() && ID.find_first_not_of("0123456789") == std::string::npos) {
                    logindSessionPath = "/org/freedesktop/login1/session/" + ID;
                    return;
                }
            }
        }
    }

    static void lightUnavailable(const SLight& L) {
        Notify::Model::postCard({.id = L.cardId, .icon = L.icon, .summary = L.title, .body = "Unavailable", .expireTimeout = OSD_TIMEOUT, .glyph = L.glyph, .alarm = true});
    }

    // the keyboard's level as the user last chose it: the room turns it on there
    static void rememberKeyboard(int pct) {
        if (StateStore::inst().data().options.set(eOption::KBD_LEVEL, pct))
            StateStore::inst().dirty();
    }

    static void lightApply(SLight& L);
    static void lightStep(SLight& L, int dir) {
        L.pending = std::clamp(L.pending + dir, -20, 20);
        if (!L.busy)
            lightApply(L);
    }

    static void lightSetRaw(SLight& L, int raw) {
        L.target  = std::max(0, raw);
        L.pending = 0; // an absolute choice replaces the steps
        if (!L.busy)
            lightApply(L);
    }

    // the raw level the slider's percent means (a device not found yet is
    // looked for: the slider shows only a light that answered once)
    static void lightSet(SLight& L, int pct) {
        if (L.dev.empty())
            findLight(L);
        if (L.max > 0)
            lightSetRaw(L, (int)std::lround(L.max * std::clamp(pct, 0, 100) / 100.0));
    }

    static void lightApply(SLight& L) {
        const int  DIR    = std::exchange(L.pending, 0); // the steps, signed
        const auto TARGET = std::exchange(L.target, std::nullopt);
        if (DIR == 0 && !TARGET)
            return;
        const uint64_t GENERATION = ++L.generation;
        const bool     BYUSER     = std::exchange(L.byUser, false);
        // a key's step answers with a capsule unless the panel shows the
        // slider it moved; a slider's own set is its own feedback
        const bool ANNOUNCE = DIR != 0 && !Controls::panelShowing();
        if (!systemBus().conn()) {
            if (ANNOUNCE)
                lightUnavailable(L);
            return;
        }

        int raw = -1;
        if (L.lastSetRaw >= 0 && Time::steadyNow() - L.lastSetAt < std::chrono::milliseconds(500))
            raw = L.lastSetRaw;
        else {
            auto now = readLight(L);
            if (!now) {
                // gone or never found: look again, once per press
                findLight(L);
                now = readLight(L);
            }
            if (!now) {
                Controls::publish(L.level, {});
                if (ANNOUNCE)
                    lightUnavailable(L);
                return;
            }
            raw = *now;
        }

        // the absolute level, then the key steps on top: ±5% of max,
        // linear, floored (the display at 2 raw: the old brightnessctl -n2)
        // — a floor never above the device's own max (a 0..1 panel), and a
        // step down never raises a device already under it
        if (TARGET)
            raw = *TARGET;
        const int STEP  = std::max(1, (int)std::lround(L.max * 0.05));
        const int FLOOR = std::min({L.floor, L.max, !TARGET && DIR < 0 ? raw : L.floor});
        raw             = std::clamp(raw + DIR * STEP, FLOOR, L.max);

        const int PCT = (int)std::lround(100.0 * raw / L.max);
        try {
            auto& B = systemBus();
            if (!logindProxy)
                logindProxy = sdbus::createProxy(*B.conn(), sdbus::ServiceName{"org.freedesktop.login1"}, sdbus::ObjectPath{logindSessionPath});
            // the card waits for logind's ack: a refused write must not
            // flash a percent that never applied (the reply lands on this
            // event loop; sending from a dispatch callback is fine, only
            // draining is not)
            logindProxy->callMethodAsync("SetBrightness")
                .onInterface("org.freedesktop.login1.Session")
                .withArguments(std::string{L.subsystem}, L.dev, (uint32_t)raw)
                .uponReplyInvoke([&L, PCT, GENERATION, ANNOUNCE, BYUSER](std::optional<sdbus::Error> err) {
                    if (GENERATION != L.generation)
                        return; // a teardown owns the state now
                    L.busy = false;
                    if (!err) {
                        Controls::publish(L.level, {.pct = PCT});
                        if (BYUSER && &L == &keyboard && PCT > 0)
                            rememberKeyboard(PCT);
                        if (ANNOUNCE)
                            Notify::Model::postCard({.id            = L.cardId,
                                                     .icon          = L.icon,
                                                     .summary       = L.title,
                                                     .body          = (std::to_string(PCT) + "%").c_str(),
                                                     .expireTimeout = OSD_TIMEOUT,
                                                     .value         = PCT,
                                                     .glyph         = L.glyph});
                    } else
                        L.lastSetRaw = -1; // logind refused: drop the trust window so the next press re-reads sysfs
                    lightApply(L);         // what was pressed meanwhile
                });
            L.busy = true;
            B.pollSoon();
        } catch (...) { return; }

        L.lastSetRaw = raw;
        L.lastSetAt  = Time::steadyNow();
    }

    // ---- dark displays put the keyboard out ----

    // While every display is off (DPMS: an idle daemon's blank, the
    // dispatcher), the keyboard light is the last light on the desk: it goes
    // out with the last display and comes back, exactly as it was, with the
    // first. A choice made in the dark (a key, the slider) stands: the wake
    // leaves it. A blank walks every display, one emission each: they
    // coalesce into one check on the next turn, which also rewatches the
    // displays a hotplug changed.
    static std::vector<Hyprutils::Signal::CHyprSignalListener> dpmsWatch;
    static CHop                                                 pendingDark;
    static bool                                                 darkArmed = false, dark = false;
    static int                                                  darkKept  = -1; // the raw level the dark took; -1: nothing to give back

    static bool roomLeads();
    static void roomApply();

    static void darkCheck();
    static void darkSoon() {
        if (compositorShuttingDown() || std::exchange(darkArmed, true))
            return;
        pendingDark.arm(darkCheck);
    }

    static void darkCheck() {
        darkArmed = false;
        if (compositorShuttingDown())
            return;
        const auto& MONS = State::monitorState()->monitors();
        dpmsWatch.clear();
        for (const auto& M : MONS)
            if (M)
                dpmsWatch.push_back(M->m_events.dpmsChanged.listen([]() { darkSoon(); }));
        // no display at all is no verdict (the start before outputs, a
        // closed lid's last panel): the light stays as it is
        if (MONS.empty())
            return;
        const bool DARK = std::ranges::none_of(MONS, [](const PHLMONITOR& M) { return M && M->m_dpmsStatus; });
        if (DARK == dark)
            return;
        dark = DARK;
        if (keyboard.dev.empty())
            return;
        if (dark) {
            // what it shows: an ask still in flight is newer than sysfs
            const int NOW = keyboard.target ? *keyboard.target : keyboard.busy && keyboard.lastSetRaw >= 0 ? keyboard.lastSetRaw : readLight(keyboard).value_or(0);
            darkKept      = NOW > 0 ? NOW : -1;
            if (darkKept > 0)
                lightSetRaw(keyboard, 0);
        } else if (roomLeads()) {
            darkKept = -1;
            roomApply(); // the room says what the wake brings
        } else if (darkKept > 0)
            lightSetRaw(keyboard, std::exchange(darkKept, -1));
    }

    // ---- the keyboard light follows the room (iio-sensor-proxy) ----

    // With its option on (the keyboard slider's menu), the keyboard light is
    // on in a dark room and off in daylight, as the ambient light sensor
    // reads it: iio-sensor-proxy's net.hadess.SensorProxy, claimed while the
    // option is on (its LightLevel moves only for a claim). The zones keep a
    // margin — dark under DARK_LUX, light over LIGHT_LUX, a reading between
    // leaves the zone as it is — and a new zone must hold ROOM_SETTLE before
    // the light follows: a hand over the sensor or a passing car's lights
    // never flicker it. On is the level the user last chose (KBD_LEVEL,
    // remembered at logind's ack; 50 % before any). A choice made by hand
    // (a key, the slider) steps the room aside until its zone changes; dark
    // displays win over both. A sensor whose unit is not lux ("vendor": a
    // scale the margin cannot know) is no sensor.
    constexpr double DARK_LUX = 20, LIGHT_LUX = 60;
#ifdef AWESOME_GATE
    constexpr auto ROOM_SETTLE = std::chrono::milliseconds(600); // the gate's: the logic, not the wait
#else
    constexpr auto ROOM_SETTLE = std::chrono::seconds(5);
#endif
    constexpr int         KBD_FALLBACK_PCT = 50;
    constexpr const char* SENSOR           = "net.hadess.SensorProxy";

    enum class eZone : uint8_t {
        NONE,
        DARK,
        LIGHT,
    };
    static std::unique_ptr<sdbus::IProxy> sensorProxy;
    static sdbus::Slot                    sensorMatch, ownerMatch;
    static bool                           sensorHas = false; // HasAmbientLight
    static std::string                    sensorUnit;        // LightLevelUnit
    static bool                           claimed = false, claiming = false;
    static eZone                          zone = eZone::NONE, pendingZone = eZone::NONE;
    static bool                           aside = false; // a choice by hand holds until the zone changes
    static SP<CEventLoopTimer>            roomSettle;
    static CHop                           pendingFollow;
    static uint64_t                       sensorGen = 0; // a bus loss, a new daemon or a teardown outdates replies

    static bool roomOption() {
        return StateStore::inst().data().options.on(eOption::KBD_FOLLOWS_ROOM);
    }
    static bool sensorLux() {
        return sensorHas && sensorUnit == "lux";
    }
    static bool following() {
        return roomOption() && sensorLux() && claimed;
    }
    static bool roomLeads() {
        return following() && !aside && zone != eZone::NONE;
    }

    static void publishRoom() {
        Controls::publish(Controls::eSwitch::ROOM, (int8_t)(!sensorLux() || keyboard.dev.empty() ? -1 : roomOption() ? 1 : 0));
    }

    static void roomApply() {
        if (!roomLeads() || dark || keyboard.max <= 0)
            return;
        const auto PCT = StateStore::inst().data().options.get(eOption::KBD_LEVEL);
        lightSetRaw(keyboard, zone == eZone::LIGHT ? 0 : (int)std::lround(keyboard.max * (PCT > 0 ? PCT : KBD_FALLBACK_PCT) / 100.0));
    }

    static void roomReading(double lux) {
        if (!following() || !std::isfinite(lux))
            return;
        const eZone Z = lux < DARK_LUX ? eZone::DARK : lux > LIGHT_LUX ? eZone::LIGHT : zone;
        if (zone == eZone::NONE) { // the claim's first reading: follow at once
            zone = Z != eZone::NONE ? Z : lux < (DARK_LUX + LIGHT_LUX) / 2 ? eZone::DARK : eZone::LIGHT;
            roomApply();
            return;
        }
        if (Z == zone) { // back before it held: nothing changed
            pendingZone = eZone::NONE;
            roomSettle->updateTimeout(std::nullopt);
            return;
        }
        if (Z != pendingZone) {
            pendingZone = Z;
            roomSettle->updateTimeout(ROOM_SETTLE);
        }
    }

    static void roomSettled() {
        if (compositorShuttingDown() || pendingZone == eZone::NONE || !following())
            return;
        zone  = std::exchange(pendingZone, eZone::NONE);
        aside = false; // the room changed: it leads again
        roomApply();
    }

    static void sensorRelease() {
        if (!sensorProxy || !claimed)
            return;
        claimed = false;
        try {
            sensorProxy->callMethodAsync("ReleaseLight").onInterface(SENSOR).uponReplyInvoke([](std::optional<sdbus::Error>) {});
            systemBus().pollSoon();
        } catch (...) {}
    }

    static void sensorRead();
    static void sensorClaim() {
        if (!sensorProxy || !sensorLux() || claimed || claiming || !roomOption())
            return;
        claiming           = true;
        const uint64_t GEN = sensorGen;
        try {
            sensorProxy->callMethodAsync("ClaimLight").onInterface(SENSOR).uponReplyInvoke([GEN](std::optional<sdbus::Error> err) {
                if (GEN != sensorGen)
                    return;
                claiming = false;
                if (err)
                    return;
                claimed = true;
                if (!roomOption()) // turned off while the claim ran
                    return sensorRelease();
                zone = pendingZone = eZone::NONE;
                sensorRead(); // the first reading, now that it moves
            });
            systemBus().pollSoon();
        } catch (...) { claiming = false; }
    }

    // GetAll's answer, or a PropertiesChanged: the sensor as it is, and the
    // room's light while claimed
    static void sensorChanged(const std::map<std::string, sdbus::Variant>& props) {
        const bool WAS = sensorLux();
        try {
            if (const auto IT = props.find("HasAmbientLight"); IT != props.end())
                sensorHas = IT->second.get<bool>();
            if (const auto IT = props.find("LightLevelUnit"); IT != props.end())
                sensorUnit = IT->second.get<std::string>();
            if (WAS != sensorLux()) {
                if (!sensorLux()) { // the sensor went, its claim with it
                    claimed = false;
                    zone = pendingZone = eZone::NONE;
                }
                publishRoom();
                sensorClaim();
            }
            if (const auto IT = props.find("LightLevel"); IT != props.end() && claimed)
                roomReading(IT->second.get<double>());
        } catch (...) {}
    }

    static void sensorRead() {
        if (!sensorProxy)
            return;
        const uint64_t GEN = sensorGen;
        try {
            sensorProxy->callMethodAsync("GetAll")
                .onInterface("org.freedesktop.DBus.Properties")
                .withArguments(std::string{SENSOR})
                .uponReplyInvoke([GEN](std::optional<sdbus::Error> err, std::map<std::string, sdbus::Variant> props) {
                    if (GEN != sensorGen)
                        return;
                    if (err) { // no daemon (it activates on this call where installed): no sensor
                        sensorChanged({{"HasAmbientLight", sdbus::Variant{false}}});
                        return;
                    }
                    sensorChanged(props);
                });
            systemBus().pollSoon();
        } catch (...) {}
    }

    static void sensorInit() {
        roomSettle = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { roomSettled(); }, nullptr);
        g_pEventLoopManager->addTimer(roomSettle);
        auto& B = systemBus();
        if (!B.conn())
            return publishRoom();
        try {
            sensorProxy = sdbus::createProxy(*B.conn(), sdbus::ServiceName{SENSOR}, sdbus::ObjectPath{"/net/hadess/SensorProxy"});
            // sender: only the daemon owning the name may move the room
            sensorMatch = B.conn()->addMatchAsync(
                "type='signal',sender='net.hadess.SensorProxy',path='/net/hadess/SensorProxy',interface='org.freedesktop.DBus.Properties',"
                "member='PropertiesChanged',arg0='net.hadess.SensorProxy'",
                [](sdbus::Message m) {
                    std::string                           iface;
                    std::map<std::string, sdbus::Variant> changed;
                    try {
                        m >> iface >> changed;
                    } catch (...) { return; }
                    sensorChanged(changed);
                },
                [](sdbus::Message) {}, sdbus::return_slot);
            // a new daemon (a restart) holds no claim of ours: start over
            ownerMatch = B.conn()->addMatchAsync(
                "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='net.hadess.SensorProxy'",
                [](sdbus::Message m) {
                    std::string name, from, to;
                    try {
                        m >> name >> from >> to;
                    } catch (...) { return; }
                    ++sensorGen;
                    claimed = claiming = false;
                    zone = pendingZone = eZone::NONE;
                    if (to.empty())
                        sensorChanged({{"HasAmbientLight", sdbus::Variant{false}}});
                    else
                        sensorRead();
                },
                [](sdbus::Message) {}, sdbus::return_slot);
            B.pollSoon();
        } catch (...) {}
        sensorRead();
    }

    // the menu's choice, deferred out of its pick like every door
    void lightFollowRoom(bool on) {
        pendingFollow.arm([on]() {
            if (StateStore::inst().data().options.set(eOption::KBD_FOLLOWS_ROOM, on ? 1 : 0))
                StateStore::inst().dirty();
            aside = false;
            zone = pendingZone = eZone::NONE;
            if (roomSettle)
                roomSettle->updateTimeout(std::nullopt);
            if (on)
                sensorClaim();
            else
                sensorRelease();
            publishRoom();
        });
    }

    static SLight* lightOf(Controls::eLevel l) {
        return l == Controls::eLevel::DISPLAY ? &display : l == Controls::eLevel::KEYBOARD ? &keyboard : nullptr;
    }

    // the user's own moves: a keyboard choice made in the dark stands, and
    // one made while the room leads steps it aside
    static void byHand(Controls::eLevel l) {
        if (l != Controls::eLevel::KEYBOARD)
            return;
        darkKept = -1;
        if (following())
            aside = true;
    }

    void lightKey(Controls::eLevel l, int dir) {
        byHand(l);
        if (auto* L = lightOf(l)) {
            L->byUser = true;
            lightStep(*L, dir);
        }
    }

    void lightAsk(Controls::eLevel l, int pct) {
        byHand(l);
        if (auto* L = lightOf(l)) {
            L->byUser = true;
            lightSet(*L, pct);
        }
    }

    // the load, and the panel opening: both lights as sysfs has them, each
    // looked for again when it is missing or stopped reading
    void lightRefresh() {
        for (auto* L : {&display, &keyboard}) {
            if (L->dev.empty() || !readLight(*L))
                findLight(*L);
            publishLight(*L);
        }
    }

    void lightInit() {
        findLogindSession();
        lightRefresh();
        // the displays as they are, and as they come and go
        supervisor().listen(Event::bus()->m_events.monitor.added, [](PHLMONITOR) { darkSoon(); });
        supervisor().listen(Event::bus()->m_events.monitor.removed, [](PHLMONITOR) { darkSoon(); });
        darkSoon();
        sensorInit();
    }

    // the bus went (lost, or the module's teardown): its pending
    // SetBrightness dies unanswered, the sensor's claim with the connection
    void lightBusDropped() {
        logindProxy.reset();
        sensorProxy.reset();
        sensorMatch = {};
        ownerMatch  = {};
        ++sensorGen;
        claimed = claiming = false;
        for (auto* L : {&display, &keyboard}) {
            ++L->generation;
            L->busy = false;
        }
    }

    void lightExit() {
        dpmsWatch.clear();
        pendingDark.reset();
        pendingFollow.reset();
        darkArmed = dark = false;
        darkKept         = -1;
        if (roomSettle && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(roomSettle);
        roomSettle.reset();
        ++sensorGen; // a late claim or GetAll finds itself outdated
        sensorHas = claimed = claiming = aside = false;
        sensorUnit.clear();
        zone = pendingZone = eZone::NONE;
        for (auto* L : {&display, &keyboard}) {
            ++L->generation; // a late SetBrightness reply finds itself outdated
            L->busy    = false;
            L->byUser  = false;
            L->pending = 0;
            L->target.reset();
            L->lastSetRaw = -1;
            L->dev.clear();
            L->max = 0;
        }
    }

} // namespace NAwesome::System
