// awesome/core/controls.hpp — the control center's seam. The system module
// owns the levels (the display's brightness, the keyboard light, the
// volume), the switches (the microphone, the touchpad), the outputs the
// volume can go to and Bluetooth (the adapter, its paired devices): it
// publishes them here as it learns them and registers the doors that move
// them. The
// notify center draws them and sends the user's moves through those doors.
// Neither module reads the other's state: the values live here.
//
// Main thread only. The system module's teardown closes the doors and
// forgets the values; notify's clears its hook and the panel flag.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace NAwesome::Controls {

    enum class eLevel : uint8_t {
        DISPLAY,
        KEYBOARD,
        VOLUME,
        COUNT_,
    };
    enum class eSwitch : uint8_t {
        MIC,
        TOUCHPAD,
        ROOM, // the keyboard light follows the room's light (-1: no light sensor)
        COUNT_,
    };

    // a level as last known: pct -1 while no device answers (its row hides);
    // a volume past 100 % says so
    struct SLevel {
        int  pct   = -1;
        bool muted = false; // the volume's
        int  steps = 0;     // a light of few levels: how many above off (its slider snaps to them); 0 continuous

        bool operator==(const SLevel&) const = default;
    };

    // a sink the volume can play through (the system module bounds the list)
    struct SOutput {
        uint32_t    id = 0;
        std::string name;
        bool        current = false; // the default

        bool operator==(const SOutput&) const = default;
    };

    // a paired Bluetooth device (the system module bounds the list and the
    // name, connected first, then by name)
    struct SDevice {
        enum eKind : uint8_t {
            OTHER,
            HEADPHONES,
            SPEAKER,
            MOUSE,
            KEYBOARD,
        };
        std::string id; // BlueZ's object path: who it is across a rebuild
        std::string name;
        eKind       kind       = OTHER;
        bool        connected  = false;
        bool        connecting = false; // a connect or disconnect in flight
        int8_t      battery    = -1;    // percent, -1 unknown

        bool operator==(const SDevice&) const = default;
    };
    inline constexpr size_t MAX_DEVICES = 16;

    // the first adapter: present 0 while there is none (no BlueZ, no
    // radio), and the panel shows no Bluetooth
    struct SBluetooth {
        bool                 present = false;
        bool                 powered = false;
        std::vector<SDevice> devices;

        bool operator==(const SBluetooth&) const = default;
    };

    struct SState {
        std::array<SLevel, (size_t)eLevel::COUNT_>  levels{};
        std::array<int8_t, (size_t)eSwitch::COUNT_> switches{-1, -1, -1}; // -1 none, 0 off, 1 on
        std::vector<SOutput>                        outputs;
        SBluetooth                                  bluetooth;
    };

    inline SState& state() {
        static SState S;
        return S;
    }

    // the owner's doors, empty while the system module is out; each defers
    // its work out of the caller's emission
    struct SDoors {
        std::function<void(eLevel, int)>   setLevel; // a percent, 0..100
        std::function<void(eLevel, int)>   step;     // a key's steps, signed (the wheel)
        std::function<void(eSwitch, bool)> setSwitch;
        std::function<void(bool)>          setMuted;  // the volume's
        std::function<void(uint32_t)>      setOutput; // the default sink, by its id
        std::function<void()>              refresh;   // read every device again (the panel opened)
        std::function<void(bool)>          setBluetooth; // the adapter's power
        // a device by its id: connect it (powering the adapter first when
        // off), or disconnect it
        std::function<void(const std::string&, bool)> connectDevice;
    };
    inline SDoors& doors() {
        static SDoors D;
        return D;
    }

    // the reader's: runs after a published value changed (the center repaints)
    inline std::function<void()>& changedHook() {
        static std::function<void()> H;
        return H;
    }

    // the center shows the levels: a key that moves one moves its slider,
    // and no capsule covers the panel
    inline bool& panelShowing() {
        static bool B = false;
        return B;
    }

    inline void publish(eLevel l, SLevel v) {
        auto& S = state().levels[(size_t)l];
        if (S == v)
            return;
        S = v;
        if (changedHook())
            changedHook()();
    }

    inline void publish(eSwitch s, int8_t v) {
        auto& S = state().switches[(size_t)s];
        if (S == v)
            return;
        S = v;
        if (changedHook())
            changedHook()();
    }

    inline void publish(SBluetooth v) {
        if (state().bluetooth == v)
            return;
        state().bluetooth = std::move(v);
        if (changedHook())
            changedHook()();
    }

    inline void publish(std::vector<SOutput> v) {
        if (state().outputs == v)
            return;
        state().outputs = std::move(v);
        if (changedHook())
            changedHook()();
    }

} // namespace NAwesome::Controls
