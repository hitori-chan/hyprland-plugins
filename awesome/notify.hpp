// awesome/notify.hpp — the notify module: the freedesktop notification
// daemon, the card model, popups, the shade, the reply field.
#pragma once

#include "core/supervisor.hpp"

namespace NAwesome::Notify {
    class CModule : public IModule {
      public:
        const char* name() const override {
            return "notify";
        }
        void init() override;
        void teardown() override;
    };
    CModule& module();
}
