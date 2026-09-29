// awesome/system/audio.cpp — volume, mic, and brightness.
//
// Ported from the old hyprosd with two changes of shape, both forced by
// the monolith:
// - The cards no longer travel the session bus (the fd.o daemon WAS this
//   plugin's sibling; now it is this module's neighbor). They post
//   straight into the notify model — same ids (9992/9993/9995), same
//   value hints, same replace-in-place semantics; the OSD band lives in
//   the model now, so no x-hyprnotify-osd hint.
// - The wpctl readback chain (set -> get -> parse -> card) rides the
//   core's Jobs (pidfd-watched, bounded, generation-checked) instead of
//   a private event-source fleet. The chain's state is therefore just
//   (mic, generation): the children are Jobs' children.
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
    constexpr size_t MAX_ACTIVE_CHAINS = 16;

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

    static std::string backlightDev; // /sys/class/backlight/<dev>, name only
    static int         backlightMax = 0;
    static std::string logindSessionPath; // explicit over "auto": see findLogindSession

    static void findBacklight() {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator("/sys/class/backlight", ec)) {
            std::ifstream m(e.path() / "max_brightness");
            if (m && (m >> backlightMax) && backlightMax > 0) {
                backlightDev = e.path().filename();
                return;
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
    static uint64_t        volumeGeneration     = 0;
    static uint64_t        micGeneration        = 0;
    static uint64_t        volumeFeedback       = 0;
    static uint64_t        micFeedback          = 0;

    static const char* volumeIcon(int pct) {
        if (pct <= 33)
            return "audio-volume-low";
        if (pct <= 66)
            return "audio-volume-medium";
        return "audio-volume-high";
    }

    static void brightnessStep(int dir) {
        const uint64_t GENERATION = ++brightnessGeneration;
        if (backlightDev.empty() || !systemBus().conn())
            return;

        int raw = -1;
        if (lastSetRaw >= 0 && Time::steadyNow() - lastSetAt < std::chrono::milliseconds(500))
            raw = lastSetRaw;
        else {
            std::ifstream b("/sys/class/backlight/" + backlightDev + "/brightness");
            if (!b || !(b >> raw) || raw < 0)
                return;
        }

        // ±5% of max, linear, floored at 2 raw (the old brightnessctl -n2)
        const int STEP = std::max(1, (int)std::lround(backlightMax * 0.05));
        raw            = std::clamp(raw + dir * STEP, 2, backlightMax);

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
                        return; // a newer press owns the state
                    if (!err)
                        Notify::Model::postCard({.id = ID_BRIGHTNESS, .icon = "display-brightness-symbolic", .summary = "Brightness", .body = (std::to_string(PCT) + "%").c_str(), .expireTimeout = OSD_TIMEOUT, .value = PCT});
                    else
                        lastSetRaw = -1; // logind refused: drop the trust window so the next press re-reads sysfs
                });
            L.pollSoon();
        } catch (...) { return; }

        lastSetRaw = raw;
        lastSetAt  = Time::steadyNow();
    }

    // ---- volume / mic (wpctl, sequenced on the event loop) ----

    // A readback in flight: the set-child (Jobs-tracked) hands to the
    // get-child (Jobs-piped), whose stdout is the authoritative state.
    // Chains overlap freely under key repeat — every set runs (each IS a
    // step), a late get just shows the final state; the feedback
    // generations decide which cards earn their draw.
    static std::vector<uint64_t> liveChains; // the live generations

    static bool canTrackChain() {
        return liveChains.size() < MAX_ACTIVE_CHAINS;
    }

    static void dropChain(uint64_t gen) {
        std::erase_if(liveChains, [gen](uint64_t g) { return g == gen; });
    }

    static void postReadback(bool mic, uint64_t generation, const Wpctl::SReadback* rb) {
        uint64_t& feedback = mic ? micFeedback : volumeFeedback;
        if (!rb || generation <= feedback)
            return; // no parseable readback, or a newer chain already showed
        feedback = generation;
        if (mic) {
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

    static void wpctlAction(eAction a) { // the eAction four — ARGV below indexes them
        static const std::vector<const char*> ARGV[] = {
            {"wpctl", "set-volume", "-l", "1.0", "@DEFAULT_AUDIO_SINK@", "5%+", nullptr},
            {"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "5%-", nullptr},
            {"wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle", nullptr},
            {"wpctl", "set-mute", "@DEFAULT_AUDIO_SOURCE@", "toggle", nullptr},
        };

        if (!canTrackChain())
            return;
        const bool     MIC  = a == MIC_MUTE;
        const uint64_t GEN  = ++(MIC ? micGeneration : volumeGeneration);
        liveChains.push_back(GEN);
        if (!Jobs::inst().spawnChecked(ARGV[a], [MIC, GEN](int) {
                // the set is done; the readback is the authoritative state
                static const std::vector<const char*> GET[] = {
                    {"wpctl", "get-volume", "@DEFAULT_AUDIO_SOURCE@", nullptr},
                    {"wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", nullptr},
                };
                const bool OK = Jobs::inst().spawnPiped(GET[MIC], [MIC, GEN](Jobs::SPipedResult R) {
                    dropChain(GEN);
                    const auto RB = Wpctl::parseReadback(R.out);
                    postReadback(MIC, GEN, RB.has_value() ? &*RB : nullptr);
                });
                if (!OK)
                    dropChain(GEN);
            }))
            dropChain(GEN);
    }

    // ---- the action queue (the Lua face's door) ----

    // Actions queue and drain from the event loop, never inside the bind's
    // input emission; a queue rather than one deferred slot so a key-repeat
    // burst never coalesces two steps into one.
    static std::vector<uint8_t> queued;
    static CHop                 pendingDrain;

    void enqueueAction(eAction a) {
        if (!g_pEventLoopManager)
            return; // an unarmable drain must not let the queue grow
        if (queued.size() >= MAX_ACTION_QUEUE)
            return; // bounded backpressure under a key-repeat storm
        queued.push_back(a);
        pendingDrain.arm([]() {
            for (const auto A : queued)
                if (A == BRI_UP || A == BRI_DOWN)
                    brightnessStep(A == BRI_UP ? 1 : -1);
                else
                    wpctlAction((eAction)A);
            queued.clear();
        });
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
        queued.clear();
        pendingDrain.reset();
        liveChains.clear();
        ++brightnessGeneration;
        ++volumeGeneration;
        ++micGeneration;
        ++volumeFeedback;
        ++micFeedback;
        lastSetRaw   = -1;
        backlightDev.clear();
        backlightMax = 0;
        systemBus().close(); // fd sources out BEFORE the connection dies
        logindProxy.reset();
    }

} // namespace NAwesome::System
