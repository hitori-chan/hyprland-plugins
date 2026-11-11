// vclick: click, scroll or move inside a window without the user's pointer.
// Load and unload: the hyprctl command, the set_cursor hook that lets
// vclick draw the app's own cursor image, and the listeners. What each unit
// does is in vclick.hpp; how and why in docs/design.md.
#include "vclick.hpp"

#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/protocols/CursorShape.hpp>
#include <hyprland/src/protocols/core/Seat.hpp>

#include <stdexcept>

namespace {
    HANDLE                                 PHANDLE = nullptr;
    SP<IPC::Socket1::SCommand>             ctlCmd;
    CFunctionHook*                         setCursorHook = nullptr;
    Hyprutils::Signal::CHyprSignalListener renderListener, shapeListener, focusListener;

    using FSetCursor = void (*)(CSeatManager*, SP<CWLSeatResource>, uint32_t, SP<CWLSurfaceResource>, const Vector2D&);

    // every set_cursor request reaches the seat manager here: vclick keeps
    // the app's image, then the compositor decides as it always does
    void hkSetCursor(CSeatManager* self, SP<CWLSeatResource> seat, uint32_t serial, SP<CWLSurfaceResource> surf, const Vector2D& hotspot) {
        if (seat)
            VClick::Cursor::remember(seat->client(), surf, hotspot, "");
        rc<FSetCursor>(setCursorHook->m_original)(self, seat, serial, surf, hotspot);
    }

    void hookSetCursor() {
        for (const auto& f : HyprlandAPI::findFunctionsByName(PHANDLE, "onSetCursor")) {
            if (f.demangled.starts_with("CSeatManager::onSetCursor(") && !f.demangled.contains("clone")) {
                setCursorHook = HyprlandAPI::createFunctionHook(PHANDLE, f.address, rc<const void*>(&hkSetCursor));
                break;
            }
        }
        // optional: without it vclick draws the built-in arrow
        VClick::Cursor::appImages = setCursorHook && setCursorHook->hook();
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
    focusListener = g_pSeatManager->m_events.pointerFocusChange.listen([] { VClick::Cursor::userMoved(); });

    // a prefix command: the handler gets the full request, the command word included
    ctlCmd = HyprlandAPI::registerHyprCtlCommand(PHANDLE, IPC::Socket1::SCommand{.name = VClick::NAME, .match = IPC::Socket1::COMMAND_MATCH_PREFIX,
                                                                                  .handler = [](const IPC::Socket1::SRequest& request) {
                                                                                      return IPC::Socket1::SResponse{VClick::Command::handle(request.command)};
                                                                                  }});

    return {VClick::NAME, "click, scroll or move inside a window without the user's pointer", VClick::AUTHOR, VClick::VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // nothing may call into the plugin once it is unmapped: the listeners
    // and the hook first, then every timer (a glide, a scroll, the fade)
    renderListener.reset();
    shapeListener.reset();
    focusListener.reset();
    if (setCursorHook)
        HyprlandAPI::removeFunctionHook(PHANDLE, setCursorHook);
    setCursorHook = nullptr;
    VClick::Glide::stop();
    VClick::Pointer::reset();
    VClick::Cursor::reset();
    if (ctlCmd)
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, ctlCmd);
    ctlCmd.reset();
}
