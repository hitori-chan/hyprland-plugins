// awesome/system/audio.cpp — volume, mic, and the lights (the display's
// brightness, the keyboard's).
//
// The cards post straight into the notify model — ids 9992/9993/9994/9995,
// value hints and replace-in-place semantics are the contract; the OSD
// band lives in the model, so no OSD hint is needed. Every level and the
// mic's state are published to the control center (core/controls.hpp) as
// they are learned; while its panel shows, its sliders are the feedback
// and no capsule covers them.
//
// Each audio device runs one wpctl set at a time, then a readback (set ->
// get -> parse -> card), on the core's Jobs (pidfd-watched, bounded,
// generation-checked); presses meanwhile accumulate into the next set. A
// Bluetooth headset's own volume change (BlueZ, the system bus) reads the
// sink back too, and shows only what moved.
//
// The lights stay fork-free: sysfs for current/max (/sys/class/backlight,
// the keyboard's *kbd_backlight LED), ±5% linear steps or the slider's
// percent (the shown percent IS current/max), the display floored at 2
// raw so the panel never goes black, the write through logind
// Session.SetBrightness on the system bus — no root, no udev rule.

#include "system.hpp"

#include "notify/model.hpp" // NAwesome::Notify::Notify::Model::postCard (the in-process seam)

#include "core/busclient.hpp"
#include "core/controls.hpp"
#include "core/hop.hpp"
#include "core/jobs.hpp"
#include "system/wpctl.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/helpers/time/Time.hpp>

#include <sdbus-c++/sdbus-c++.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <string>

namespace NAwesome::System {

    // The OSD pin ids (the old scripts' contract; the model's OSD band
    // 9990-9999 gives them replace-in-place, no history, no grouping).
    inline constexpr uint32_t ID_BRIGHTNESS = 9992;
    inline constexpr uint32_t ID_VOLUME     = 9993;
    inline constexpr uint32_t ID_KEYBOARD   = 9994;
    inline constexpr uint32_t ID_MIC        = 9995;
    inline constexpr int      OSD_TIMEOUT   = 1200;

    constexpr size_t MAX_ACTION_QUEUE = 128; // a key-repeat storm must back off, not fork

    // ---- the system bus (logind only; the cards need no bus at all) ----

    static CBusLink& systemBus() {
        static CBusLink L;
        return L;
    }
    static std::unique_ptr<sdbus::IProxy> logindProxy;
    static sdbus::Slot                    transportMatch; // the headset volume watch (below)
    static void                           lightsDropped();

    static void systemBusInit() {
        auto& L = systemBus();
        L.onLost = [](const std::string& err) {
            HyprlandAPI::addNotification(supervisor().handle(), "[awesome] system bus lost: " + err, CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
        };
        L.dropOwned = []() {
            logindProxy.reset(); // its pending SetBrightness dies unanswered
            transportMatch = {};
            lightsDropped();
        };
        try {
            L.open(true);
            L.sync();
        } catch (const std::exception& E) {
            HyprlandAPI::addNotification(supervisor().handle(), std::string{"[awesome] no system bus, brightness off: "} + E.what(), CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
        }
    }

    // ---- the lights: the display's backlight and the keyboard's (sysfs + logind, zero forks) ----

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
        std::optional<int> target;      // a percent the slider asked for
        // A keypress bases its step on what the previous one just asked
        // for: logind's write is asynchronous, so a fast repeat would read
        // stale sysfs and re-step from the same value. Half a second of
        // trust, then sysfs is the truth again (external tools, resume).
        int             lastSetRaw = -1;
        Time::steady_tp lastSetAt;
        uint64_t        generation = 0; // a teardown or a bus loss outdates replies
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

    // at init, and again when the device is missing or stops reading (a
    // driver loaded late, a GPU switch, a ddcci monitor plugged in, the
    // keyboard's driver bound after the session started)
    static void findLight(SLight& L) {
        L.dev.clear();
        L.max          = 0;
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

    static const char* volumeIcon(int pct) {
        if (pct <= 33)
            return "audio-volume-low";
        if (pct <= 66)
            return "audio-volume-medium";
        return "audio-volume-high";
    }

    static void lightsDropped() {
        for (auto* L : {&display, &keyboard}) {
            ++L->generation;
            L->busy = false;
        }
    }

    static void lightUnavailable(const SLight& L) {
        Notify::Model::postCard({.id = L.cardId, .icon = L.icon, .summary = L.title, .body = "Unavailable", .expireTimeout = OSD_TIMEOUT, .glyph = L.glyph, .alarm = true});
    }

    static void lightApply(SLight& L);
    static void lightStep(SLight& L, int dir) {
        L.pending = std::clamp(L.pending + dir, -20, 20);
        if (!L.busy)
            lightApply(L);
    }

    static void lightSet(SLight& L, int pct) {
        L.target  = std::clamp(pct, 0, 100);
        L.pending = 0; // the slider's choice is absolute
        if (!L.busy)
            lightApply(L);
    }

    static void lightApply(SLight& L) {
        const int  DIR    = std::exchange(L.pending, 0); // the steps, signed
        const auto TARGET = std::exchange(L.target, std::nullopt);
        if (DIR == 0 && !TARGET)
            return;
        const uint64_t GENERATION = ++L.generation;
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
                L.lastSetRaw = -1;
                now          = readLight(L);
            }
            if (!now) {
                Controls::publish(L.level, {});
                if (ANNOUNCE)
                    lightUnavailable(L);
                return;
            }
            raw = *now;
        }

        // the slider's percent, then the key steps on top: ±5% of max,
        // linear, floored (the display at 2 raw: the old brightnessctl -n2)
        // — a floor never above the device's own max (a 0..1 panel), and a
        // step down never raises a device already under it
        if (TARGET)
            raw = (int)std::lround(L.max * *TARGET / 100.0);
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
                .uponReplyInvoke([&L, PCT, GENERATION, ANNOUNCE](std::optional<sdbus::Error> err) {
                    if (GENERATION != L.generation)
                        return; // a teardown owns the state now
                    L.busy = false;
                    if (!err) {
                        Controls::publish(L.level, {.pct = PCT});
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

    // ---- volume / mic (wpctl, one set in flight per device) ----

    // wpctl reads the current value and writes the new one inside its own
    // process: two sets at once read the same value and write the same
    // result (a held key lost steps, two mute toggles collapsed into one,
    // up/down raced). So each device runs ONE set at a time; presses while
    // it runs ACCUMULATE — volume steps sum into the next set ("15%+"),
    // toggles fold to their parity — and every finished set is read back,
    // the readback being the card's authoritative state.
    struct SDevice {
        const char* node;
        bool        mic;
        bool        busy    = false;
        int         steps   = 0; // ±1 per press, applied as ±5% each
        int         toggles = 0;
        double      level   = -1; // the last readback's volume (1.0 = 100%); < 0: never read
        bool        muted   = false; // the last readback's mute
        // the panel's absolute asks, run before the keys' (a slider's level
        // replaces the steps pending)
        std::optional<double> target; // 1.0 = 100%
        std::optional<bool>   muteTo;
        Time::steady_tp levelAt; // when it was read: the default device or another mixer may have moved it since
        uint64_t    generation = 0; // a teardown or a newer readback outdates replies
    };
    // what a readback's card does: follow a set, stay quiet (the level a
    // raise needs), or show only a change (a headset moved it, or our own
    // set's echo, which then shows nothing)
    enum class eShow : uint8_t {
        ALWAYS,
        NEVER,
        CHANGED,
    };
    // A raise caps at 100% unless the device is already past it, so it needs
    // the level as it is now: a readback older than this is re-read first. A
    // held key's sets read back one after another and stay within it.
    static constexpr auto LEVEL_TRUST = std::chrono::seconds(1);
    static SDevice sink{.node = "@DEFAULT_AUDIO_SINK@", .mic = false}, source{.node = "@DEFAULT_AUDIO_SOURCE@", .mic = true};

    static void showReadback(const SDevice& d, const Wpctl::SReadback* rb) {
        if (!rb)
            return; // no parseable readback: no card beats a wrong one
        if (d.mic) {
            if (rb->muted || rb->value >= 0)
                Notify::Model::postCard({.id = ID_MIC, .icon = rb->muted ? "microphone-sensitivity-muted" : "microphone-sensitivity-high", .summary = "Microphone", .body = rb->muted ? "Off" : "On",
                                         .expireTimeout = OSD_TIMEOUT, .glyph = rb->muted ? Glyph::eGlyph::MIC_OFF : Glyph::eGlyph::MIC, .alarm = rb->muted});
            return;
        }
        if (rb->muted) {
            // the level stays on the dimmed track: unmuting returns to it
            const int LEVEL = rb->value >= 0 ? std::min((int)std::lround(rb->value * 100.0), 100) : -1;
            Notify::Model::postCard({.id = ID_VOLUME, .icon = "audio-volume-muted", .summary = "Volume", .body = "Muted", .expireTimeout = OSD_TIMEOUT, .value = LEVEL,
                                     .glyph = Glyph::eGlyph::SPEAKER_MUTED, .alarm = true});
            return;
        }
        const int PCT = (int)std::lround(rb->value * 100.0); // past 100% says so; the bar stops full
        if (PCT >= 0)
            Notify::Model::postCard({.id = ID_VOLUME, .icon = volumeIcon(PCT), .summary = "Volume", .body = (std::to_string(PCT) + "%").c_str(), .expireTimeout = OSD_TIMEOUT,
                                     .value = std::min(PCT, 100), .glyph = Glyph::eGlyph::SPEAKER});
    }

    static void pump(SDevice& d);
    static void headsetReadDone();

    // the card follows a set; a raise's read of the level ahead of its set
    // shows nothing (the set's own readback is the card)
    static void readBack(SDevice& d, eShow show = eShow::ALWAYS) {
        const uint64_t GEN = ++d.generation;
        const bool     OK  = Jobs::inst().spawnPiped(Jobs::CONTROL, {"wpctl", "get-volume", d.node, nullptr}, [&d, GEN, show](Jobs::SPipedResult R) {
            if (GEN != d.generation)
                return; // a teardown, or a newer set already reads back
            d.busy        = false;
            const auto RB = Wpctl::parseReadback(R.out);
            // a change is 0.005 or more: the readback prints two decimals
            const bool MOVED = RB && (d.level < 0 || std::abs(RB->value - d.level) >= 0.005 || RB->muted != d.muted);
            d.level          = RB ? RB->value : std::max(d.level, 0.0);
            d.muted          = RB ? RB->muted : d.muted;
            d.levelAt        = Time::steadyNow();
            if (RB && d.mic)
                Controls::publish(Controls::eSwitch::MIC, RB->muted ? 0 : 1);
            else if (RB)
                Controls::publish(Controls::eLevel::VOLUME, {.pct = (int)std::lround(RB->value * 100.0), .muted = RB->muted});
            // the open panel's slider (or switch) is the feedback
            if ((show == eShow::ALWAYS || (show == eShow::CHANGED && MOVED)) && !Controls::panelShowing())
                showReadback(d, RB.has_value() ? &*RB : nullptr);
            pump(d);
            if (&d == &sink)
                headsetReadDone();
        });
        if (!OK) {
            d.busy    = false;
            d.level   = std::max(d.level, 0.0); // unreadable: the plain limit
            d.levelAt = Time::steadyNow();
            pump(d);
            if (&d == &sink)
                headsetReadDone();
        }
    }

    static void pump(SDevice& d) {
        if (d.busy)
            return;
        if (d.toggles % 2 == 0)
            d.toggles = 0; // an even count is no change
        // every kind of ask is a set of its own, in this order: the panel's
        // mute, its level, a key's mute toggle, the key steps; the rest
        // wait for the next pump
        const bool MUTETO = d.muteTo.has_value(), TARGET = !MUTETO && d.target.has_value(), TOGGLE = !MUTETO && !TARGET && d.toggles != 0;
        if (!MUTETO && !TARGET && !TOGGLE && d.steps > 0 && (d.level < 0 || Time::steadyNow() - d.levelAt > LEVEL_TRUST)) {
            d.busy = true; // the limit below needs the level as it is now: read it first
            return readBack(d, eShow::NEVER);
        }
        std::string amount, limit; // outlive the spawn: the argv is copied there
        std::vector<const char*> argv;
        if (MUTETO)
            argv = {"wpctl", "set-mute", d.node, *d.muteTo ? "1" : "0", nullptr};
        else if (TARGET) {
            amount = std::format("{:.2f}", std::clamp(*d.target, 0.0, 1.0));
            argv   = {"wpctl", "set-volume", d.node, amount.c_str(), nullptr};
        } else if (TOGGLE)
            argv = {"wpctl", "set-mute", d.node, "toggle", nullptr};
        else if (d.steps != 0) {
            amount = std::to_string(std::min(std::abs(d.steps) * 5, 100)) + (d.steps > 0 ? "%+" : "%-");
            if (d.steps > 0) {
                // the keys stop at 100%, but a volume already past it (set
                // elsewhere) is never pulled down to it: the limit is then
                // the current level (+0.005: the readback rounds to 0.01)
                limit = d.level > 1.0 ? std::format("{:.3f}", d.level + 0.005) : "1.0";
                argv  = {"wpctl", "set-volume", "-l", limit.c_str(), d.node, amount.c_str(), nullptr};
            } else
                argv = {"wpctl", "set-volume", d.node, amount.c_str(), nullptr};
        }
        if (argv.empty())
            return;
        d.busy         = true;
        const uint64_t GEN = d.generation;
        if (!Jobs::inst().spawnChecked(Jobs::CONTROL, argv, [&d, GEN](int) {
                if (GEN == d.generation)
                    readBack(d);
            })) {
            d.busy = false; // the class budget is full: the presses stay pending for the next one
            return;
        }
        // in flight now; a refused spawn kept them
        if (MUTETO)
            d.muteTo.reset();
        else if (TARGET)
            d.target.reset();
        else
            (TOGGLE ? d.toggles : d.steps) = 0;
    }

    static void wpctlAction(eAction a) {
        SDevice& d = a == MIC_MUTE ? source : sink;
        switch (a) {
            case VOL_UP: d.steps = std::min(d.steps + 1, 20); break;
            case VOL_DOWN: d.steps = std::max(d.steps - 1, -20); break;
            default: d.toggles++; break;
        }
        pump(d);
    }

    // ---- a headset's own volume (Bluetooth absolute volume) ----

    // A headset that sets its own volume (its buttons, a touch strip) does
    // it over AVRCP: no key reaches the binds, so no action runs. BlueZ
    // announces the transport's new Volume, which schedules a readback of
    // the default sink. The readback stays the truth — the transport may
    // feed another sink, and PipeWire applies the change only after BlueZ
    // announces it — and the capsule shows only a level or a mute that
    // moved: our own sets echo here and show nothing twice. A headset
    // without absolute volume sends volume keys instead (BlueZ's AVRCP
    // input device), and those reach the binds.
    static SP<CEventLoopTimer> headsetSettle;
    static bool                headsetArmed = false, headsetAgain = false;
    // PipeWire reacts to the same announcement within this; a held button's
    // burst reads back at most this often
    static constexpr auto HEADSET_SETTLE = std::chrono::milliseconds(120);

    static void headsetMoved() {
        if (compositorShuttingDown() || !headsetSettle || headsetArmed)
            return;
        headsetArmed = true;
        headsetSettle->updateTimeout(HEADSET_SETTLE);
    }

    static void headsetRead() {
        headsetArmed = false;
        if (sink.busy) { // one wpctl at a time: the change reads back after it
            headsetAgain = true;
            return;
        }
        sink.busy = true;
        readBack(sink, eShow::CHANGED);
    }

    // after every sink readback: a change announced while it ran may be
    // newer than what it read
    static void headsetReadDone() {
        if (std::exchange(headsetAgain, false))
            headsetMoved();
    }

    static void headsetInit() {
        headsetSettle = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { headsetRead(); }, nullptr);
        g_pEventLoopManager->addTimer(headsetSettle);
        auto& L = systemBus();
        if (!L.conn())
            return;
        try {
            // sender: only bluetoothd may own org.bluez on the system bus
            transportMatch = L.conn()->addMatchAsync(
                "type='signal',sender='org.bluez',path_namespace='/org/bluez',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
                "arg0='org.bluez.MediaTransport1'",
                [](sdbus::Message m) {
                    std::string                           iface;
                    std::map<std::string, sdbus::Variant> changed;
                    try {
                        m >> iface >> changed;
                    } catch (...) { return; }
                    if (iface == "org.bluez.MediaTransport1" && changed.contains("Volume"))
                        headsetMoved();
                },
                [](sdbus::Message) {}, sdbus::return_slot);
            L.pollSoon();
        } catch (...) {}
    }

    // ---- the action queue (the Lua face's door) ----

    // Actions queue and drain from the event loop, never inside the bind's
    // input emission; a queue rather than one deferred slot so a key-repeat
    // burst never coalesces two steps into one.
    // bounded backpressure under a key-repeat storm (a full queue drops the
    // step: a storm must back off, not fork)
    static CHopQueue<uint8_t, MAX_ACTION_QUEUE> queued([](std::vector<uint8_t>& batch) {
        for (const auto A : batch)
            if (A == BRI_UP || A == BRI_DOWN)
                lightStep(display, A == BRI_UP ? 1 : -1);
            else if (A == KBD_UP || A == KBD_DOWN)
                lightStep(keyboard, A == KBD_UP ? 1 : -1);
            else
                wpctlAction((eAction)A);
    });

    void enqueueAction(eAction a) {
        queued.push(a);
    }

    // ---- the panel's asks (the control center's doors) ----

    // A slider drag asks per motion: only the newest ask per level stands,
    // applied on the next event-loop turn (out of the input emission, the
    // D-Bus call and the spawn with it); each device then runs one set at
    // a time and takes the newest ask when it finishes.
    static std::array<std::optional<int>, (size_t)Controls::eLevel::COUNT_> levelAsks;
    static std::optional<bool>                                              micAsk, muteAsk;
    static CHop                                                             pendingAsks;
    static bool                                                             asksArmed = false; // one hop per batch, not one per motion

    static void applyAsks() {
        asksArmed = false;
        for (size_t i = 0; i < levelAsks.size(); i++) {
            const auto PCT = std::exchange(levelAsks[i], std::nullopt);
            if (!PCT)
                continue;
            switch ((Controls::eLevel)i) {
                case Controls::eLevel::DISPLAY: lightSet(display, *PCT); break;
                case Controls::eLevel::KEYBOARD: lightSet(keyboard, *PCT); break;
                case Controls::eLevel::VOLUME:
                    sink.target = *PCT / 100.0;
                    sink.steps  = 0;
                    if (sink.muted && *PCT > 0)
                        sink.muteTo = false; // a slider moved off zero is heard, as macOS's
                    pump(sink);
                    break;
                case Controls::eLevel::COUNT_: break;
            }
        }
        if (const auto MUTED = std::exchange(muteAsk, std::nullopt)) {
            sink.muteTo  = *MUTED;
            sink.toggles = 0;
            pump(sink);
        }
        if (const auto ON = std::exchange(micAsk, std::nullopt)) {
            source.muteTo  = !*ON;
            source.toggles = 0;
            pump(source);
        }
    }

    static void armAsks() {
        if (std::exchange(asksArmed, true))
            return;
        pendingAsks.arm(applyAsks);
    }

    void setLevel(Controls::eLevel l, int pct) {
        if (l >= Controls::eLevel::COUNT_)
            return;
        levelAsks[(size_t)l] = std::clamp(pct, 0, 100);
        armAsks();
    }

    void setMic(bool on) {
        micAsk = on;
        armAsks();
    }

    void setMuted(bool muted) {
        muteAsk = muted;
        armAsks();
    }

    // the load, and the panel opening: every level as it is now (the volume
    // and the mic read back quietly; one already reading answers with
    // that), so the panel opens on what it shows and never grows a row
    void audioRefresh() {
        for (auto* L : {&display, &keyboard}) {
            if (L->dev.empty())
                findLight(*L);
            publishLight(*L);
        }
        for (auto* D : {&sink, &source})
            if (!D->busy) {
                D->busy = true;
                readBack(*D, eShow::NEVER);
            }
    }

    void audioInit() {
        systemBusInit();
        headsetInit();
        findLogindSession();
        audioRefresh();
    }

    void audioExit() {
        // the queue is the module's own; Jobs' children are the core's and
        // go with the loop — in-flight callbacks are generation-checked by
        // the teardown bump below.
        queued.reset();
        pendingAsks.reset();
        asksArmed = false;
        levelAsks.fill(std::nullopt);
        micAsk.reset();
        muteAsk.reset();
        if (headsetSettle && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(headsetSettle);
        headsetSettle.reset();
        headsetArmed = headsetAgain = false;
        for (auto* D : {&sink, &source}) {
            ++D->generation; // a late wpctl callback finds itself outdated
            D->busy    = false;
            D->steps   = 0;
            D->toggles = 0;
            D->level   = -1;
            D->muted   = false;
            D->target.reset();
            D->muteTo.reset();
        }
        for (auto* L : {&display, &keyboard}) {
            ++L->generation; // a late SetBrightness reply finds itself outdated
            L->busy    = false;
            L->pending = 0;
            L->target.reset();
            L->lastSetRaw = -1;
            L->dev.clear();
            L->max = 0;
        }
        systemBus().close(); // fd sources out BEFORE the connection dies
        logindProxy.reset();
    }

} // namespace NAwesome::System
