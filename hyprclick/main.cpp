// hyprclick — awesome's click and focus-raise policy as a native plugin.
//
// 1. Click-to-raise: a plain left click brings the clicked window to the
//    top — over a maximized window too; clicking a fullscreen/maximized
//    window tucks the floaters back behind it. (Focus itself is native
//    follow_mouse; Hyprland never raises on click by itself.)
// 2. Keyboard focus raises, hover focus doesn't (awesome's rule): focus
//    changes from binds and dispatchers bring the window to the top;
//    sloppy focus never does.
// 3. `hl.plugin.hyprclick.focus_prev_here()` — awesome's Mod+Tab: focus
//    the previously focused window ON THE CURRENT WORKSPACE (the native
//    focus({ last }) follows global history across workspaces).
//
// Loads after hyprbar (which swallows its strip clicks) and hyprmax
// (which swallows Super-grabs on maximized windows): a cancelled press
// never raises. No config.

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/history/WindowHistoryTracker.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>

#include <linux/input-event-codes.h>

#include <vector>

static HANDLE                                 PHANDLE = nullptr;

static Hyprutils::Signal::CHyprSignalListener lButton, lActive;
static UP<SEventLoopDoLaterLock>              pendingRaise, pendingFocus;

static PHLWINDOW                              windowUnderCursor() {
    if (!g_pInputManager)
        return nullptr;
    return Desktop::viewState()->hitTest().windowAt(g_pInputManager->getMouseCoordsInternal(),
                                                    Desktop::View::ALLOW_FLOATING | Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS);
}

static bool superHeld() {
    const auto KB = g_pSeatManager ? g_pSeatManager->m_keyboard.lock() : nullptr;
    return KB && (KB->m_modifiersState.depressed & HL_MODIFIER_META);
}

static void raiseWindow(PHLWINDOW w) {
    if (Fullscreen::controller()->isFullscreen(w)) {
        // "raising" the fullscreen/maximized window = tucking the floaters
        // back behind it (lower() clears their allowed-over flag). Two
        // phases: raise/lower rotate the window list, never mutate
        // mid-iteration.
        std::vector<PHLWINDOW> demote;
        for (const auto& OW : Desktop::windowState()->windows()) {
            if (OW != w && OW->m_isMapped && OW->m_workspace == w->m_workspace && OW->m_allowedOverFullscreen)
                demote.push_back(OW);
        }
        for (const auto& OW : demote)
            Desktop::windowState()->lower(OW);
    } else if (w->m_isFloating)
        Desktop::windowState()->raise(w);
}

static void onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
    if (e.state != WL_POINTER_BUTTON_STATE_PRESSED)
        return;

    // emissions precede the compositor's lock handling: a click on the
    // lockscreen must not reorder the windows beneath it
    if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked())
        return;

    // Already swallowed by an earlier listener — hyprbar cancels clicks on
    // its strip and open tray menus, hyprmax cancels Super-grabs on
    // maximized windows. Those are never raise clicks.
    if (info.cancelled)
        return;

    // The pointer is on a layer surface (a bar): that click is the bar's,
    // never reach through it to the window underneath.
    if (g_pInputManager->m_lastFocusOnLS)
        return;

    // awesome's click-to-raise. A Super+left/right press raises too —
    // grabbing a window raised in awesome as well.
    if (e.button != BTN_LEFT && !(e.button == BTN_RIGHT && superHeld()))
        return;

    if (const auto W = windowUnderCursor())
        raiseWindow(W);
}

// Keyboard focus raises, hover focus doesn't. The raise is deferred out of
// the focus emission (rawWindowFocus is still running when this fires).
static void onWindowActive(PHLWINDOW w, Desktop::eFocusReason reason) {
    using enum Desktop::eFocusReason;
    if (!w ||
        (reason != FOCUS_REASON_KEYBIND && reason != FOCUS_REASON_DISPATCH_FOCUSWINDOW && reason != FOCUS_REASON_SWITCH_TO_WINDOW_SOFT &&
         reason != FOCUS_REASON_SWITCH_TO_WINDOW_HARD))
        return;

    PHLWINDOWREF WR{w};
    pendingRaise = g_pEventLoopManager->doLaterLock([WR]() {
        if (const auto W = WR.lock(); W && W->m_isMapped)
            raiseWindow(W);
    });
}

// hl.plugin.hyprclick.focus_prev_here() — awesome's Mod+Tab: the most
// recently focused OTHER window on the current workspace, focused + raised.
// Each focus rewrites the history head, so repeated presses bounce between
// the two most recent windows, like awful.client.focus.history.previous.
static int luaFocusPrevHere(lua_State*) {
    const auto FOCUS = Desktop::focusState()->window();
    const auto MON   = Desktop::focusState()->monitor();
    const auto WS    = MON ? MON->m_activeWorkspace : nullptr;
    if (!WS)
        return 0;

    // the tracker is ordered old -> new: the previous window is at the BACK
    const auto& HIST = Desktop::History::windowTracker()->fullHistory();
    for (auto it = HIST.rbegin(); it != HIST.rend(); ++it) {
        const auto W = it->lock();
        if (!W || W == FOCUS || !W->m_isMapped || W->isHidden() || W->m_workspace != WS)
            continue;

        PHLWINDOWREF TARGET{W};
        pendingFocus = g_pEventLoopManager->doLaterLock([TARGET]() {
            if (const auto W = TARGET.lock())
                Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_SWITCH_TO_WINDOW_HARD);
        });
        break;
    }
    return 0;
}

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprclick] Version mismatch: rebuild the plugin against the running Hyprland", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hyprclick] version mismatch");
    }

    lButton = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { onMouseButton(e, info); });
    lActive = Event::bus()->m_events.window.active.listen([](PHLWINDOW w, Desktop::eFocusReason reason) { onWindowActive(w, reason); });

    HyprlandAPI::addLuaFunction(PHANDLE, "hyprclick", "focus_prev_here", luaFocusPrevHere);

    return {"hyprclick", "awesome click/focus policy: click-to-raise, keyboard focus raises, hover never does", "hitori", "1.0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    pendingRaise.reset();
    pendingFocus.reset();
    lButton.reset();
    lActive.reset();
}
