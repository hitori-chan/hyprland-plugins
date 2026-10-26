// awesome/system/audio.cpp — volume and the mic, and the module's action
// queue and panel asks (the lights' go on to light.cpp).
//
// The cards post straight into the notify model — ids 9993/9995, value
// hints and replace-in-place semantics are the contract; the OSD band
// lives in the model, so no OSD hint is needed. The volume and the mic's
// state are published to the control center (core/controls.hpp) as they
// are learned; while its panel shows, its sliders are the feedback and no
// capsule covers them.
//
// Each audio device runs one wpctl set at a time, then a readback (set ->
// get -> parse -> card), on the core's Jobs (pidfd-watched, bounded,
// generation-checked); presses meanwhile accumulate into the next set. A
// Bluetooth headset's own volume change (BlueZ, the system bus) reads the
// sink back too, and shows only what moved.

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
#include <format>
#include <map>
#include <optional>
#include <string>

namespace NAwesome::System {

    // The OSD pin ids (the old scripts' contract; the model's OSD band
    // 9990-9999 gives them replace-in-place, no history, no grouping).
    inline constexpr uint32_t ID_VOLUME = 9993;
    inline constexpr uint32_t ID_MIC    = 9995;

    constexpr size_t MAX_ACTION_QUEUE = 128; // a key-repeat storm must back off, not fork

    static sdbus::Slot transportMatch; // the headset volume watch (below)

    static const char* volumeIcon(int pct) {
        if (pct <= 33)
            return "audio-volume-low";
        if (pct <= 66)
            return "audio-volume-medium";
        return "audio-volume-high";
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

    // ---- the outputs (the volume's menu in the control center) ----

    // `wpctl status` names the sinks and stars the default: read at the
    // load, when the panel opens and after a pick, one read at a time (an
    // ask meanwhile reads again when it lands). Its Audio tree follows a row
    // per client, so the read takes more than a readback's cap.
    static constexpr size_t STATUS_CAP = 32 * 1024;
    static bool             outputsBusy = false, outputsAgain = false;
    static uint64_t         outputsGen = 0, audioEpoch = 0; // the read's; the unit's life (a teardown bumps both)

    static void outputsRead() {
        if (outputsBusy) {
            outputsAgain = true;
            return;
        }
        const uint64_t GEN = ++outputsGen;
        outputsBusy        = Jobs::inst().spawnPiped(
            Jobs::CONTROL, {"wpctl", "status", nullptr},
            [GEN](Jobs::SPipedResult R) {
                if (GEN != outputsGen)
                    return; // a teardown owns the state now
                outputsBusy = false;
                std::vector<Controls::SOutput> outs;
                for (auto& S : Wpctl::parseSinks(R.out))
                    outs.push_back({.id = S.id, .name = std::move(S.name), .current = S.current});
                // a read cut short before the sinks keeps the list it had
                if (!outs.empty() || !R.truncated)
                    Controls::publish(std::move(outs));
                if (std::exchange(outputsAgain, false))
                    outputsRead();
            },
            STATUS_CAP);
    }

    // the pick: the default moves, then the sink reads back as after a
    // headset's own change (the volume is now the new sink's) and the list
    // reads again (its star moved)
    static void setDefaultOutput(uint32_t id) {
        const std::string ID    = std::to_string(id); // outlives the spawn: the argv is copied there
        const uint64_t    EPOCH = audioEpoch;
        Jobs::inst().spawnChecked(Jobs::CONTROL, {"wpctl", "set-default", ID.c_str(), nullptr}, [EPOCH](int) {
            if (EPOCH != audioEpoch)
                return;
            sink.level = -1; // another sink: the next raise reads its level first
            headsetMoved();
            outputsRead();
        });
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
                lightKey(Controls::eLevel::DISPLAY, A == BRI_UP ? 1 : -1);
            else if (A == KBD_UP || A == KBD_DOWN)
                lightKey(Controls::eLevel::KEYBOARD, A == KBD_UP ? 1 : -1);
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
    static std::optional<uint32_t>                                          outputAsk;
    static std::array<int, (size_t)Controls::eLevel::COUNT_>                levelSteps{}; // the wheel's, signed
    static CHop                                                             pendingAsks;
    static bool                                                             asksArmed = false; // one hop per batch, not one per motion

    static void applyAsks() {
        asksArmed = false;
        for (size_t i = 0; i < levelAsks.size(); i++) {
            const auto PCT = std::exchange(levelAsks[i], std::nullopt);
            if (!PCT)
                continue;
            switch ((Controls::eLevel)i) {
                case Controls::eLevel::DISPLAY:
                case Controls::eLevel::KEYBOARD: lightAsk((Controls::eLevel)i, *PCT); break;
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
        for (size_t i = 0; i < levelSteps.size(); i++) {
            const int N = std::exchange(levelSteps[i], 0);
            if (N == 0)
                continue;
            if ((Controls::eLevel)i == Controls::eLevel::VOLUME) {
                sink.steps = std::clamp(sink.steps + N, -20, 20);
                pump(sink);
            } else
                lightKey((Controls::eLevel)i, N);
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
        if (const auto ID = std::exchange(outputAsk, std::nullopt))
            setDefaultOutput(*ID);
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

    void stepLevel(Controls::eLevel l, int steps) {
        if (l >= Controls::eLevel::COUNT_)
            return;
        levelSteps[(size_t)l] = std::clamp(levelSteps[(size_t)l] + steps, -20, 20);
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

    void setOutput(uint32_t id) {
        outputAsk = id;
        armAsks();
    }

    // the load, and the panel opening: the volume, the mic and the outputs
    // as they are now, read back quietly (one already reading answers with
    // that), so the panel opens on what it shows and never grows a row
    void audioRefresh() {
        for (auto* D : {&sink, &source})
            if (!D->busy) {
                D->busy = true;
                readBack(*D, eShow::NEVER);
            }
        outputsRead();
    }

    void audioInit() {
        headsetInit();
        audioRefresh();
    }

    void audioBusDropped() {
        transportMatch = {};
    }

    void audioExit() {
        // the queue is the module's own; Jobs' children are the core's and
        // go with the loop — in-flight callbacks are generation-checked by
        // the teardown bump below.
        queued.reset();
        pendingAsks.reset();
        asksArmed = false;
        levelAsks.fill(std::nullopt);
        levelSteps.fill(0);
        micAsk.reset();
        muteAsk.reset();
        outputAsk.reset();
        ++outputsGen; // a late status or set-default finds itself outdated
        ++audioEpoch;
        outputsBusy = outputsAgain = false;
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
    }

} // namespace NAwesome::System
