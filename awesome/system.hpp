// awesome/system.hpp — the system module: audio and brightness (feedback
// as notify cards) and the touchpad policy.
#pragma once

#include "core/supervisor.hpp"

namespace NAwesome::System {
    class CModule : public IModule {
      public:
        const char* name() const override {
            return "system";
        }
        void init() override;
        void teardown() override;
    };
    CModule& module();
}
