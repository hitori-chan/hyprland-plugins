// The display's backlight.
//
// Fork-free: sysfs for current/max (/sys/class/backlight), ±5% linear
// steps floored at 2 raw so the panel never goes black, the write through
// logind Session.SetBrightness on the module's system bus — no root, no
// udev rule. A key's step answers with a capsule (id 9992) at logind's
// ack.

#include "system.hpp"

#include "notify/model.hpp" // NAwesome::Notify::Model::postCard (the in-process seam)

#include "core/busclient.hpp"

#include <hyprland/src/helpers/time/Time.hpp>

#include <sdbus-c++/sdbus-c++.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace NAwesome::System {

    // The OSD pin id (the model's OSD band 9990-9999 gives it
    // replace-in-place, no history, no grouping).
    inline constexpr uint32_t ID_BRIGHTNESS = 9992;
    // raw: the panel never goes black (brightnessctl's -n2)
    inline constexpr int FLOOR_RAW = 2;

    static std::unique_ptr<sdbus::IProxy> logindProxy;
    static std::string                    logindSessionPath; // explicit over "auto": see findLogindSession

    // The backlight its sysfs class offers, and ONE SetBrightness in flight
    // (presses meanwhile add up into the next one; a stalled logind would
    // otherwise collect a call per press until its 25 s timeout).
    struct SLight {
        std::string root = "/sys/class/backlight";
        std::string dev; // <root>/<dev>, name only
        int         max     = 0;
        bool        busy    = false;
        int         pending = 0; // key steps, signed (±5 % of max each)
        // A keypress bases its step on what the previous one just asked
        // for: logind's write is asynchronous, so a fast repeat would read
        // stale sysfs and re-step from the same value. Half a second of
        // trust, then sysfs is the truth again (external tools, resume).
        int             lastSetRaw = -1;
        Time::steady_tp lastSetAt;
        uint64_t        generation = 0; // a teardown or a bus loss outdates replies
    };
    static SLight light;

    static std::optional<int> readSysfsInt(const std::string& path) {
        int           v = -1;
        std::ifstream f(path);
        if (!f || !(f >> v) || v < 0)
            return std::nullopt;
        return v;
    }

    // at init, and again when the device is missing or stops reading (a
    // driver loaded late, a GPU switch, a ddcci monitor plugged in)
    static void findLight() {
        auto&      L   = light;
        const auto WAS = std::exchange(L.dev, std::string{});
        L.max          = 0;
        L.lastSetRaw   = -1; // the trust window is the old device's: the one found next reads its own sysfs
#ifdef AWESOME_GATE
        // the gate's fake class, driven by a fake logind on the nested's
        // private system bus: its battery never moves the panel
        if (const char* DIR = std::getenv("AW_BACKLIGHT_DIR"); DIR && *DIR)
            L.root = DIR;
#endif
        // Several backlights (acpi_video0 beside the native panel, a GPU's,
        // ddcci): the systemd rule — firmware, then platform, then raw;
        // directory order only breaks a tie. The firmware interface is the
        // one that reliably moves the panel.
        const auto RANK = [](const std::string& type) { return type == "firmware" ? 0 : type == "platform" ? 1 : type == "raw" ? 2 : 3; };
        int        best = 4;
        std::error_code ec;
        // increment(ec): the range-for's operator++ throws
        for (auto it = std::filesystem::directory_iterator(L.root, ec); !ec && it != std::filesystem::end(it); it.increment(ec)) {
            const auto MAX = readSysfsInt(it->path() / "max_brightness");
            if (!MAX || *MAX <= 0)
                continue;
            std::string type;
            std::ifstream(it->path() / "type") >> type;
            if (const int R = RANK(type); R < best) {
                best  = R;
                L.dev = it->path().filename().string();
                L.max = *MAX;
            }
        }
        // another device: a set in flight was the old one's, its percent of
        // the old range (its reply must not announce it)
        if (L.dev != WAS) {
            ++L.generation;
            L.busy = false;
        }
    }

    static std::optional<int> readLight() {
        return light.dev.empty() ? std::nullopt : readSysfsInt(light.root + "/" + light.dev + "/brightness");
    }

    // logind resolves /session/auto from the caller's cgroup, and that is a
    // SILENT NO-OP in some environments (the write acks, sysfs never moves),
    // so resolve it in the open: read our own cgroup's session-N.scope and
    // address the session explicitly. An unparseable cgroup (a compositor
    // outside any session scope) falls back to auto.
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

    static void postLight(const char* body, int value, bool alarm) {
        Notify::Model::postCard({.id            = ID_BRIGHTNESS,
                                 .icon          = "display-brightness-symbolic",
                                 .summary       = "Brightness",
                                 .body          = body,
                                 .expireTimeout = OSD_TIMEOUT,
                                 .value         = value,
                                 .glyph         = Glyph::eGlyph::SUN,
                                 .alarm         = alarm});
    }

    static void lightApply() {
        auto&     L   = light;
        const int DIR = std::exchange(L.pending, 0); // the steps, signed
        if (DIR == 0)
            return;
        if (!systemBus().conn())
            return postLight("Unavailable", -1, true);

        int raw = -1;
        if (L.lastSetRaw >= 0 && Time::steadyNow() - L.lastSetAt < std::chrono::milliseconds(500))
            raw = L.lastSetRaw;
        else {
            auto now = readLight();
            if (!now) {
                // gone or never found: look again, once per press
                findLight();
                now = readLight();
            }
            if (!now)
                return postLight("Unavailable", -1, true);
            raw = *now;
        }

        // ±5% of max, linear, floored — a floor never above the device's
        // own max (a 0..1 panel), and a step down never raises a device
        // already under it
        const int STEP  = std::max(1, (int)std::lround(L.max * 0.05));
        const int FLOOR = std::min({FLOOR_RAW, L.max, DIR < 0 ? raw : FLOOR_RAW});
        raw             = std::clamp(raw + DIR * STEP, FLOOR, L.max);

        const int PCT = (int)std::lround(100.0 * raw / L.max);
        // after findLight: a device found just now bumped the generation
        const uint64_t GENERATION = ++L.generation;
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
                .withArguments(std::string{"backlight"}, L.dev, (uint32_t)raw)
                .uponReplyInvoke([PCT, GENERATION](std::optional<sdbus::Error> err) {
                    if (GENERATION != light.generation)
                        return; // a teardown owns the state now
                    light.busy = false;
                    if (!err)
                        postLight((std::to_string(PCT) + "%").c_str(), PCT, false);
                    else
                        light.lastSetRaw = -1; // logind refused: drop the trust window so the next press re-reads sysfs
                    lightApply();              // what was pressed meanwhile
                });
            L.busy = true;
            B.pollSoon();
        } catch (...) { return; }

        L.lastSetRaw = raw;
        L.lastSetAt  = Time::steadyNow();
    }

    void lightKey(int dir) {
        light.pending = std::clamp(light.pending + dir, -20, 20);
        if (!light.busy)
            lightApply();
    }

    void lightInit() {
        findLogindSession();
        findLight();
    }

    // the bus went (lost, or the module's teardown): its pending
    // SetBrightness dies unanswered
    void lightBusDropped() {
        logindProxy.reset();
        ++light.generation;
        light.busy = false;
    }

    void lightExit() {
        ++light.generation; // a late SetBrightness reply finds itself outdated
        light.busy       = false;
        light.pending    = 0;
        light.lastSetRaw = -1;
        light.dev.clear();
        light.max = 0;
    }

} // namespace NAwesome::System
