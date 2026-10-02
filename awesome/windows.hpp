// awesome/windows.hpp — the windows module: the window-state machine
// (normal / minimized / maximized), the focus policy, spawn placement,
// edge snapping, and the snap indicator.
#pragma once

#include "core/supervisor.hpp"

namespace NAwesome::Windows {
    class CModule : public IModule {
      public:
        const char* name() const override {
            return "windows";
        }
        void init() override;
        void teardown() override;

        void onPointerButton(const IPointer::SButtonEvent&, Event::SCallbackInfo&) override;
        void onPointerMove(const Vector2D&, Event::SCallbackInfo&) override;
        void onKey(const IKeyboard::SKeyEvent&, Event::SCallbackInfo&) override;
        void onInputBlocked() override;
    };
    CModule& module();
}
