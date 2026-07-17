// hyprsnap — awful.mouse.snap as a native Hyprland plugin: while a
// floater is move-dragged its edges pull flush to the screen, the
// workarea and the other windows (snap_distance px); the cursor at a
// screen edge arms a half slot — at two edges, that corner's quarter —
// with an outline preview, committed on drop.
//
// Config:
//   plugin:hyprsnap:edge           px that arms an aerosnap zone (16, awesome's hardcoded value)
//   plugin:hyprsnap:snap_distance  px of magnetic pull (8, awesome's default_distance)
//   plugin:hyprsnap:col_frame      the armed zone's outline color

#include "hyprsnap.hpp"

#include <hyprland/src/config/ConfigValue.hpp>

namespace NHyprsnap {
    SConfig                    g_config;

    static SP<Layout::ITarget> floatingDragTarget(bool resize) {
        if (!g_layoutManager)
            return nullptr;

        // the keybind layer wipes the controller's reached-flag at the end of
        // every key/button dispatch — including the one that just began the
        // drag — and with binds:drag_threshold at 0 nothing ever sets it
        // again: a live drag reads "not picked up" forever. No threshold
        // means the window is picked up at press.
        static auto PDRAGTHRESHOLD = CConfigValue<Config::INTEGER>("binds:drag_threshold");

        const auto& DC = g_layoutManager->dragController();
        auto        t  = DC->target();
        if (!t || (resize ? DC->mode() < MBIND_RESIZE : DC->mode() != MBIND_MOVE) || !(DC->dragThresholdReached() || *PDRAGTHRESHOLD <= 0) || DC->draggingTiled() || !t->floating())
            return nullptr;
        return t;
    }

    SP<Layout::ITarget> draggedFloatingTarget() {
        return floatingDragTarget(false);
    }

    SP<Layout::ITarget> resizingFloatingTarget() {
        return floatingDragTarget(true);
    }
}

using namespace NHyprsnap;

static HANDLE                                 PHANDLE = nullptr;

static Hyprutils::Signal::CHyprSignalListener lMove, lButton, lKey;

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprsnap] Version mismatch: rebuild the plugin against the running Hyprland", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hyprsnap] version mismatch");
    }

    g_config.edge     = makeShared<Config::Values::CIntValue>("plugin:hyprsnap:edge", "px from a screen edge that arms an aerosnap zone", 16);
    g_config.snapDist = makeShared<Config::Values::CIntValue>("plugin:hyprsnap:snap_distance", "px of magnetic pull between window and screen/client edges", 8);
    g_config.colFrame = makeShared<Config::Values::CColorValue>("plugin:hyprsnap:col_frame", "armed snap zone outline", 0xff32d6ff);

    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.edge);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.snapDist);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.colFrame);

    auto& EV = Event::bus()->m_events;
    lMove    = EV.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo&) { Snap::onMouseMove(); });
    lButton  = EV.input.mouse.button.listen([](IPointer::SButtonEvent, Event::SCallbackInfo&) { Snap::onInputEndingDrag(); });
    lKey     = EV.input.keyboard.key.listen([](IKeyboard::SKeyEvent, Event::SCallbackInfo&) { Snap::onInputEndingDrag(); });
    // no render.stage listener here: snap.cpp connects one only while a zone
    // is armed — the signal fires per window per frame

    return {"hyprsnap", "awesome's awful.mouse.snap: magnetic edge pull + aerosnap zones while dragging", "hitori", "1.3.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    lMove.reset();
    lButton.reset();
    lKey.reset();
    Snap::reset(); // also drops the zone-armed render listener
}
