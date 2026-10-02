// awesome/notify.hpp — the notify module's entry: the CModule wiring
// (init/teardown order, the pipeline handlers, the Lua API, the hyprctl
// verbs). The unit map lives in notify/model.hpp.
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

        void onPointerButton(const IPointer::SButtonEvent&, Event::SCallbackInfo&) override;
        void onPointerMove(const Vector2D&, Event::SCallbackInfo&) override;
        void onPointerAxis(const IPointer::SAxisEvent&, Event::SCallbackInfo&) override;
        void onKey(const IKeyboard::SKeyEvent&, Event::SCallbackInfo&) override;
        void onInputBlocked() override;

        std::optional<std::string> handleCtl(const std::string&) const override;
    };
    CModule& module();
}
