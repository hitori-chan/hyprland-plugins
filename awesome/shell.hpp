// awesome/shell.hpp — the shell module: the strip (workspaces, tasks,
// tray, bell, battery, clock), the menubar, the launcher.
#pragma once

#include "core/supervisor.hpp"

namespace NAwesome::Shell {
    class CModule : public IModule {
      public:
        const char* name() const override {
            return "shell";
        }
        void init() override;
        void teardown() override;

        void onPointerButton(const IPointer::SButtonEvent&, Event::SCallbackInfo&) override;
        void onPointerMove(const Vector2D&, Event::SCallbackInfo&) override;
        void onPointerAxis(const IPointer::SAxisEvent&, Event::SCallbackInfo&) override;
        void onKey(const IKeyboard::SKeyEvent&, Event::SCallbackInfo&) override;
        void onInputBlocked() override;
    };
    CModule& module();
}
