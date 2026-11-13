// vclick: click, scroll or move inside a window without the user's pointer.
// Load and unload: the hyprctl command, the set_cursor hook that lets
// vclick draw the app's own cursor image, the initiateDrag hook that keeps
// a held button's drag-and-drop from the user's mouse, and the listeners.
// What each unit does is in vclick.hpp; how and why in docs/design.md.
#include "vclick.hpp"

#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/protocols/CursorShape.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/protocols/core/DataDevice.hpp>
#include <hyprland/src/protocols/core/Seat.hpp>

#include <stdexcept>

namespace {
    HANDLE                                 PHANDLE = nullptr;
    SP<IPC::Socket1::SCommand>             ctlCmd;
    CFunctionHook*                         setCursorHook = nullptr;
    CFunctionHook*                         dragHook      = nullptr;
    Hyprutils::Signal::CHyprSignalListener renderListener, shapeListener, focusListener;

    using FSetCursor = void (*)(CSeatManager*, SP<CWLSeatResource>, uint32_t, SP<CWLSurfaceResource>, const Vector2D&);

    // every set_cursor request reaches the seat manager here: vclick keeps
    // the app's image, then the compositor decides as it always does
    void hkSetCursor(CSeatManager* self, SP<CWLSeatResource> seat, uint32_t serial, SP<CWLSurfaceResource> surf, const Vector2D& hotspot) {
        if (seat)
            VClick::Cursor::remember(seat->client(), surf, hotspot, "");
        rc<FSetCursor>(setCursorHook->m_original)(self, seat, serial, surf, hotspot);
    }

    using FInitiateDrag = void (*)(CWLDataDeviceProtocol*, WP<CWLDataSourceResource>, SP<CWLSurfaceResource>, SP<CWLSurfaceResource>);

    // A drag-and-drop the app begins from vclick's held button (wl_data_device
    // .start_drag checks no serial): the compositor would drive it with the
    // user's real mouse, dropping wherever they next let a button go. It is
    // refused (the app hears its source cancelled) and the hold ends. The
    // user's own drag in that app (their button down in it) goes through.
    void hkInitiateDrag(CWLDataDeviceProtocol* self, WP<CWLDataSourceResource> source, SP<CWLSurfaceResource> icon, SP<CWLSurfaceResource> origin) {
        const auto CLIENT = origin ? origin->client() : nullptr;
        if (CLIENT && VClick::Pointer::holdsIn(CLIENT) && !(VClick::Pointer::userIn(CLIENT) && g_pInputManager->hasHeldButtons())) {
            if (const auto S = source.lock())
                S->cancelled();
            VClick::Pointer::dropHold("the app began a drag-and-drop, which only the user's mouse can drive");
            return;
        }
        rc<FInitiateDrag>(dragHook->m_original)(self, source, icon, origin);
    }

    CFunctionHook* hook(const std::string& name, const std::string& prefix, const void* fn) {
        for (const auto& f : HyprlandAPI::findFunctionsByName(PHANDLE, name)) {
            if (f.demangled.starts_with(prefix) && !f.demangled.contains("clone")) {
                const auto H = HyprlandAPI::createFunctionHook(PHANDLE, f.address, fn);
                if (H && H->hook())
                    return H;
                if (H)
                    HyprlandAPI::removeFunctionHook(PHANDLE, H);
                return nullptr;
            }
        }
        return nullptr;
    }

    void hookSetCursor() {
        // optional: without it no app's own cursor image is drawn
        setCursorHook             = hook("onSetCursor", "CSeatManager::onSetCursor(", rc<const void*>(&hkSetCursor));
        VClick::Cursor::appImages = setCursorHook;
        // without it nothing may hold a button: press answers an error
        dragHook                   = hook("initiateDrag", "CWLDataDeviceProtocol::initiateDrag(", rc<const void*>(&hkInitiateDrag));
        VClick::Pointer::dragGuard = dragHook;
    }
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();
    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[vclick] Version mismatch: rebuild the plugin against the running Hyprland", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[vclick] version mismatch");
    }

    VClick::Glide::loadProfile("");
    hookSetCursor();
    if (PROTO::cursorShape)
        shapeListener = PROTO::cursorShape->m_events.setShape.listen([](const CCursorShapeProtocol::SSetShapeEvent& ev) {
            if (ev.pMgr)
                VClick::Cursor::remember(ev.pMgr->client(), nullptr, {}, ev.shapeName);
        });
    renderListener = Event::bus()->m_events.render.stage.listen([](const Event::SRenderStageEvent& ev) { VClick::Cursor::render(ev); });
    // the cursor hides while the user's pointer is in its app: redraw it as that changes
    focusListener = g_pSeatManager->m_events.pointerFocusChange.listen([] {
        VClick::Pointer::seatMoved(); // the seat left every app's pointer: a held button went up
        VClick::Cursor::userMoved();
    });

    // a prefix command: the handler gets the full request, the command word included
    ctlCmd = HyprlandAPI::registerHyprCtlCommand(PHANDLE, IPC::Socket1::SCommand{.name = VClick::NAME, .match = IPC::Socket1::COMMAND_MATCH_PREFIX,
                                                                                  .handler = [](const IPC::Socket1::SRequest& request) {
                                                                                      return IPC::Socket1::SResponse{VClick::Command::handle(request.command)};
                                                                                  }});

    VClick::Lua::init(PHANDLE); // the compositor removes the functions on unload

    return {VClick::NAME, "click, scroll or move inside a window without the user's pointer", VClick::AUTHOR, VClick::VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // nothing may call into the plugin once it is unmapped: the listeners
    // and the hook first, then every timer (a glide, a scroll, the fade)
    renderListener.reset();
    shapeListener.reset();
    focusListener.reset();
    for (auto** h : {&setCursorHook, &dragHook}) {
        if (*h)
            HyprlandAPI::removeFunctionHook(PHANDLE, *h);
        *h = nullptr;
    }
    VClick::Pointer::dragGuard = false;
    VClick::Glide::stop();
    VClick::Pointer::reset();
    VClick::Cursor::reset();
    if (ctlCmd)
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, ctlCmd);
    ctlCmd.reset();
}
