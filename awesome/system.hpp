// The system module: volume, mic and brightness (their feedback as notify
// cards) and the touchpad policy.
#pragma once

#include "core/supervisor.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace NAwesome {
    class CBusLink;
}

namespace NAwesome::System {
    class CModule : public IModule {
      public:
        const char* name() const override {
            return "system";
        }
        void init() override;
        void teardown() override;

        std::optional<std::string> handleCtl(const std::string& verb) const override;
    };
    CModule& module();

    // a capsule's life (the touchpad's runs longer: its words are read)
    inline constexpr int OSD_TIMEOUT = 1200;

    // the actions the Lua face enqueues (audio.cpp drains them on the event
    // loop; the wpctl four are indexed by it, the brightness steps go to
    // light.cpp)
    enum eAction : uint8_t {
        VOL_UP,
        VOL_DOWN,
        VOL_MUTE,
        MIC_MUTE,
        BRI_UP,
        BRI_DOWN,
    };

    // the module's system bus (system.cpp): logind for the backlight, BlueZ
    // for a headset's own volume
    CBusLink& systemBus();

    // the units (audio.cpp, light.cpp, pad.cpp); a BusDropped resets what
    // the unit holds of the system bus, inside its teardown
    void audioInit();
    void audioExit();
    void audioBusDropped();
    void enqueueAction(eAction a);
    void lightInit();
    void lightExit();
    void lightBusDropped();
    void lightKey(int dir); // a key's ±5 % step, its capsule
    void padInit();
    void padExit();
    void padToggle();
    std::string padState();
    void        padStandIn(const std::string& name); // the gate build's touchpad stand-in; a no-op otherwise
}
