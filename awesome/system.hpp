// awesome/system.hpp — the system module: audio and brightness (feedback
// as notify cards) and the touchpad policy.
#pragma once

#include "core/supervisor.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace NAwesome::System {
    class CModule : public IModule {
      public:
        const char* name() const override {
            return "system";
        }
        void init() override;
        void teardown() override;

        // `hyprctl awesome <verb>`
        std::optional<std::string> handleCtl(const std::string& verb) const override;
    };
    CModule& module();

    // the actions the Lua face enqueues (audio.cpp executes them on the
    // event loop; the wpctl four are indexed by it, the two brightness
    // steps named)
    enum eAction : uint8_t {
        VOL_UP,
        VOL_DOWN,
        VOL_MUTE,
        MIC_MUTE,
        BRI_UP,
        BRI_DOWN
    };

    // the units (audio.cpp, pad.cpp)
    void audioInit();
    void audioExit();
    void enqueueAction(eAction a);
    void padInit();
    void padExit();
    void padToggle();
    std::string padState();
}
