#pragma once
// hyprsnap — awful.mouse.snap as a native Hyprland plugin.
// Shared declarations between the translation units; everything lives in
// NHyprsnap so no symbol can collide with another plugin's at dlopen time.
//
//   snap.cpp   magnetic edge pull while a floater is dragged + the
//              aerosnap zones with an outline preview
//   main.cpp   plugin glue: config, listeners, init/exit

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>

#include <algorithm>
#include <optional>
#include <vector>

namespace NHyprsnap {
    struct SConfig {
        SP<Config::Values::CIntValue>   edge;     // px from a screen edge that arms an aerosnap zone
        SP<Config::Values::CIntValue>   snapDist; // px of magnetic pull between window and screen/client edges
        SP<Config::Values::CColorValue> colFrame; // the armed zone's outline
    };
    extern SConfig g_config;

    // The window being move-dragged right now, if it's a plain floating
    // drag. draggingTiled is excluded: those re-tile on drop, a snap would
    // lie.
    SP<Layout::ITarget> draggedFloatingTarget();

    namespace Snap {
        void onMouseMove();       // magnetism + aerosnap arming/preview, per motion
        void onInputEndingDrag(); // commits the armed zone; runs before the keybind layer tears the drag down
        void onRenderStage(eRenderStage stage);
        void reset();
    }
}
