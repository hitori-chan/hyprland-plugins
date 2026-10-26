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

#include "system.hpp"

#include "notify/model.hpp" // NAwesome::Notify::Model::postCard (the in-process seam)

#include "core/busclient.hpp"
#include "core/controls.hpp"

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

    static SLight* lightOf(Controls::eLevel l) {
        return l == Controls::eLevel::DISPLAY ? &display : l == Controls::eLevel::KEYBOARD ? &keyboard : nullptr;
    }

    void lightKey(Controls::eLevel l, int dir) {
        if (auto* L = lightOf(l))
            lightStep(*L, dir);
    }

    void lightAsk(Controls::eLevel l, int pct) {
        if (auto* L = lightOf(l))
            lightSet(*L, pct);
    }

    // the load, and the panel opening: both lights as sysfs has them
    void lightRefresh() {
        for (auto* L : {&display, &keyboard}) {
            if (L->dev.empty())
                findLight(*L);
            publishLight(*L);
        }
    }

    void lightInit() {
        findLogindSession();
        lightRefresh();
    }

    // the bus went (lost, or the module's teardown): its pending
    // SetBrightness dies unanswered
    void lightBusDropped() {
        logindProxy.reset();
        for (auto* L : {&display, &keyboard}) {
            ++L->generation;
            L->busy = false;
        }
    }

    void lightExit() {
        for (auto* L : {&display, &keyboard}) {
            ++L->generation; // a late SetBrightness reply finds itself outdated
            L->busy    = false;
            L->pending = 0;
            L->target.reset();
            L->lastSetRaw = -1;
            L->dev.clear();
            L->max = 0;
        }
    }

} // namespace NAwesome::System
