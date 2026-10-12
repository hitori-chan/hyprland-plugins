// awesome/system/audio.cpp — volume, mic, and brightness.
//
// The cards post straight into the notify model — ids 9992/9993/9995,
// value hints and replace-in-place semantics are the contract; the OSD
// band lives in the model, so no OSD hint is needed.
//
// Each audio device runs one wpctl set at a time, then a readback (set ->
// get -> parse -> card), on the core's Jobs (pidfd-watched, bounded,
// generation-checked); presses meanwhile accumulate into the next set.
//
// Brightness stays fork-free: /sys/class/backlight for current/max,
// ±5% linear steps (the shown percent IS current/max), floor 2 raw so
// the panel never goes black, the write through logind
// Session.SetBrightness on the system bus — no root, no udev rule.

#include "system.hpp"

#include "notify/model.hpp" // NAwesome::Notify::Notify::Model::postCard (the in-process seam)

#include "core/busclient.hpp"
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
#include <fstream>
#include <optional>
#include <string>

namespace NAwesome::System {

    // The OSD pin ids (the old scripts' contract; the model's OSD band
    // 9990-9999 gives them replace-in-place, no history, no grouping).
    inline constexpr uint32_t ID_BRIGHTNESS = 9992;
    inline constexpr uint32_t ID_VOLUME     = 9993;
    inline constexpr uint32_t ID_MIC        = 9995;
    inline constexpr int      OSD_TIMEOUT   = 1200;

    constexpr size_t MAX_ACTION_QUEUE = 128; // a key-repeat storm must back off, not fork

    // ---- the system bus (logind only; the cards need no bus at all) ----

    static CBusLink& systemBus() {
        static CBusLink L;
        return L;
    }
    static std::unique_ptr<sdbus::IProxy> logindProxy;

    static void systemBusInit() {
        auto& L = systemBus();
        L.onLost = [](const std::string& err) {
            HyprlandAPI::addNotification(supervisor().handle(), "[awesome] system bus lost: " + err, CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
        };
        L.dropOwned = []() { logindProxy.reset(); };
        try {
            L.open(true);
            L.sync();
        } catch (const std::exception& E) {
            HyprlandAPI::addNotification(supervisor().handle(), std::string{"[awesome] no system bus, brightness off: "} + E.what(), CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
        }
    }

    // ---- brightness (sysfs + logind, zero forks) ----

    static std::string backlightRoot = "/sys/class/backlight";
    static std::string backlightDev; // <backlightRoot>/<dev>, name only
    static int         backlightMax = 0;
    static std::string logindSessionPath; // explicit over "auto": see findLogindSession

    static void findBacklight() {
#ifdef AWESOME_GATE
        // the gate's fake backlight class dir, driven by a fake logind on the
        // nested's private system bus: its battery never moves the panel
        if (const char* DIR = std::getenv("AW_BACKLIGHT_DIR"); DIR && *DIR)
            backlightRoot = DIR;
#endif
        // Several devices (acpi_video0 beside the native panel, a GPU's,
        // ddcci): the systemd rule — firmware, then platform, then raw;
        // directory order only breaks a tie. The firmware interface is the
        // one that reliably moves the panel.
        const auto RANK = [](const std::string& type) { return type == "firmware" ? 0 : type == "platform" ? 1 : type == "raw" ? 2 : 3; };
        int        best = 4;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(backlightRoot, ec)) {
            int           max = 0;
            std::ifstream m(e.path() / "max_brightness");
            if (!m || !(m >> max) || max <= 0)
                continue;
            std::string   type;
            std::ifstream t(e.path() / "type");
            if (t)
                t >> type;
            if (RANK(type) < best) {
                best         = RANK(type);
                backlightDev = e.path().filename();
                backlightMax = max;
            }
        }
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

    // A keypress bases its step on what the previous one just asked for:
    // logind's write is asynchronous, so a fast repeat would read stale
    // sysfs and re-step from the same value. Half a second of trust, then
    // sysfs is the truth again (external tools, resume).
    static int             lastSetRaw = -1;
    static Time::steady_tp lastSetAt;
    static uint64_t        brightnessGeneration = 0;

    static const char* volumeIcon(int pct) {
        if (pct <= 33)
            return "audio-volume-low";
        if (pct <= 66)
            return "audio-volume-medium";
        return "audio-volume-high";
    }

    // One SetBrightness in flight: presses while it runs add up into the
    // next one (a stalled logind would otherwise collect a call per press
    // until its 25 s timeout)
    static bool brightnessBusy    = false;
    static int  brightnessPending = 0;

    static void brightnessApply();
    static void brightnessStep(int dir) {
        brightnessPending = std::clamp(brightnessPending + dir, -20, 20);
        if (!brightnessBusy)
            brightnessApply();
    }

    static void brightnessApply() {
        const int dir = std::exchange(brightnessPending, 0); // the steps, signed
        if (dir == 0)
            return;
        const uint64_t GENERATION = ++brightnessGeneration;
        if (backlightDev.empty() || !systemBus().conn())
            return;

        int raw = -1;
        if (lastSetRaw >= 0 && Time::steadyNow() - lastSetAt < std::chrono::milliseconds(500))
            raw = lastSetRaw;
        else {
            std::ifstream b(backlightRoot + "/" + backlightDev + "/brightness");
            if (!b || !(b >> raw) || raw < 0)
                return;
        }

        // ±5% of max, linear, floored at 2 raw (the old brightnessctl -n2)
        // — a floor never above the panel's own max (a 0..1 panel), and a
        // step down never raises a panel already under the floor
        const int STEP  = std::max(1, (int)std::lround(backlightMax * 0.05));
        const int FLOOR = std::min({2, backlightMax, dir < 0 ? raw : 2});
        raw             = std::clamp(raw + dir * STEP, FLOOR, backlightMax);

        const int PCT = (int)std::lround(100.0 * raw / backlightMax);
        try {
            auto& L = systemBus();
            if (!logindProxy)
                logindProxy = sdbus::createProxy(*L.conn(), sdbus::ServiceName{"org.freedesktop.login1"}, sdbus::ObjectPath{logindSessionPath});
            // the card waits for logind's ack: a refused write must not
            // flash a percent that never applied (the reply lands on this
            // event loop; sending from a dispatch callback is fine, only
            // draining is not)
            logindProxy->callMethodAsync("SetBrightness")
                .onInterface("org.freedesktop.login1.Session")
                .withArguments(std::string{"backlight"}, backlightDev, (uint32_t)raw)
                .uponReplyInvoke([PCT, GENERATION](std::optional<sdbus::Error> err) {
                    if (GENERATION != brightnessGeneration)
                        return; // a teardown owns the state now
                    brightnessBusy = false;
                    if (!err)
                        Notify::Model::postCard({.id = ID_BRIGHTNESS, .icon = "display-brightness-symbolic", .summary = "Brightness", .body = (std::to_string(PCT) + "%").c_str(), .expireTimeout = OSD_TIMEOUT, .value = PCT});
                    else
                        lastSetRaw = -1; // logind refused: drop the trust window so the next press re-reads sysfs
                    brightnessApply(); // what was pressed meanwhile
                });
            brightnessBusy = true;
            L.pollSoon();
        } catch (...) { return; }

        lastSetRaw = raw;
        lastSetAt  = Time::steadyNow();
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
        uint64_t    generation = 0; // a teardown or a newer readback outdates replies
    };
    static SDevice sink{.node = "@DEFAULT_AUDIO_SINK@", .mic = false}, source{.node = "@DEFAULT_AUDIO_SOURCE@", .mic = true};

    static void showReadback(const SDevice& d, const Wpctl::SReadback* rb) {
        if (!rb)
            return; // no parseable readback: no card beats a wrong one
        if (d.mic) {
            if (rb->muted || rb->value >= 0)
                Notify::Model::postCard({.id = ID_MIC, .icon = rb->muted ? "microphone-sensitivity-muted" : "microphone-sensitivity-high", .summary = "Microphone", .body = rb->muted ? "muted" : "live", .expireTimeout = OSD_TIMEOUT});
            return;
        }
        if (rb->muted) {
            Notify::Model::postCard({.id = ID_VOLUME, .icon = "audio-volume-muted", .summary = "Volume", .body = "muted", .expireTimeout = OSD_TIMEOUT});
            return;
        }
        const int PCT = rb->value >= 1.0 ? 100 : (int)std::lround(rb->value * 100.0);
        if (PCT >= 0)
            Notify::Model::postCard({.id = ID_VOLUME, .icon = volumeIcon(PCT), .summary = "Volume", .body = (std::to_string(PCT) + "%").c_str(), .expireTimeout = OSD_TIMEOUT, .value = std::min(PCT, 100)});
    }

    static void pump(SDevice& d);

    static void readBack(SDevice& d) {
        const uint64_t GEN = ++d.generation;
        const bool     OK  = Jobs::inst().spawnPiped(Jobs::CONTROL, {"wpctl", "get-volume", d.node, nullptr}, [&d, GEN](Jobs::SPipedResult R) {
            if (GEN != d.generation)
                return; // a teardown, or a newer set already reads back
            d.busy        = false;
            const auto RB = Wpctl::parseReadback(R.out);
            showReadback(d, RB.has_value() ? &*RB : nullptr);
            pump(d);
        });
        if (!OK) {
            d.busy = false;
            pump(d);
        }
    }

    static void pump(SDevice& d) {
        if (d.busy)
            return;
        std::string amount; // outlives the spawn: the argv is copied there
        std::vector<const char*> argv;
        if (d.toggles % 2 != 0)
            argv = {"wpctl", "set-mute", d.node, "toggle", nullptr};
        else if (d.steps != 0) {
            amount = std::to_string(std::min(std::abs(d.steps) * 5, 100)) + (d.steps > 0 ? "%+" : "%-");
            if (d.steps > 0)
                argv = {"wpctl", "set-volume", "-l", "1.0", d.node, amount.c_str(), nullptr};
            else
                argv = {"wpctl", "set-volume", d.node, amount.c_str(), nullptr};
        }
        d.toggles = 0; // an even count is no change
        if (argv.empty())
            return;
        // the mute toggle and the steps are separate sets: a toggle runs
        // first, the steps wait for the next pump
        if (argv[1] == std::string_view{"set-volume"})
            d.steps = 0;
        d.busy         = true;
        const uint64_t GEN = d.generation;
        if (!Jobs::inst().spawnChecked(Jobs::CONTROL, argv, [&d, GEN](int) {
                if (GEN == d.generation)
                    readBack(d);
            }))
            d.busy = false; // the class budget is full: the presses stay pending for the next one
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

    // ---- the action queue (the Lua face's door) ----

    // Actions queue and drain from the event loop, never inside the bind's
    // input emission; a queue rather than one deferred slot so a key-repeat
    // burst never coalesces two steps into one.
    // bounded backpressure under a key-repeat storm (a full queue drops the
    // step: a storm must back off, not fork)
    static CHopQueue<uint8_t, MAX_ACTION_QUEUE> queued([](std::vector<uint8_t>& batch) {
        for (const auto A : batch)
            if (A == BRI_UP || A == BRI_DOWN)
                brightnessStep(A == BRI_UP ? 1 : -1);
            else
                wpctlAction((eAction)A);
    });

    void enqueueAction(eAction a) {
        queued.push(a);
    }

    void audioInit() {
        systemBusInit();
        findBacklight();
        findLogindSession();
    }

    void audioExit() {
        // the queue is the module's own; Jobs' children are the core's and
        // go with the loop — in-flight callbacks are generation-checked by
        // the teardown bump below.
        queued.reset();
        for (auto* D : {&sink, &source}) {
            ++D->generation; // a late wpctl callback finds itself outdated
            D->busy    = false;
            D->steps   = 0;
            D->toggles = 0;
        }
        ++brightnessGeneration;
        brightnessBusy    = false;
        brightnessPending = 0;
        lastSetRaw        = -1;
        backlightDev.clear();
        backlightMax = 0;
        systemBus().close(); // fd sources out BEFORE the connection dies
        logindProxy.reset();
    }

} // namespace NAwesome::System
