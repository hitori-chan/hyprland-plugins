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
// 4. `hl.plugin.hyprclick.focus_next()/focus_prev()` — awesome's Mod+J/K
//    (focus.byidx): cycle the workspace's windows in arrival order —
//    the native cycle walks the z-order, which rule 2's raises rotate.
//
// Loads after hyprbar (which swallows its strip clicks) and hyprmax
// (which swallows Super-grabs on maximized windows): a cancelled press
// never raises. No config.

#include "common/lifecycle.hpp"
#include "common/queries.hpp"

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
#include <hyprland/src/helpers/memory/Memory.hpp>

#include <linux/input-event-codes.h>

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

static HANDLE                                 PHANDLE = nullptr;

static NHyprCommon::CLifecycle g_lifecycle;
static NHyprCommon::CHop       pendingRaise, pendingFocus;

// arrival order for focus_next/focus_prev — the z-order list is useless as
// a cycle order under click-to-raise (every focus rotates it)
static std::unordered_map<void*, uint64_t> g_seq;
static uint64_t                            g_seqNext = 0;

static PHLWINDOW                           windowUnderCursor() {
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
        // mid-iteration. windows() runs bottom->top and lower() drops each to
        // the very bottom, so lowering in that order would REVERSE the
        // floaters among themselves (the topmost lands lowest) — a second
        // window behind the fullscreen one ends up under the first. Lower
        // top->bottom instead: relative order preserved, as awesome does.
        std::vector<PHLWINDOW> demote;
        for (const auto& OW : Desktop::windowState()->windows()) {
            if (OW != w && OW->m_isMapped && OW->m_workspace == w->m_workspace && OW->m_allowedOverFullscreen)
                demote.push_back(OW);
        }
        for (auto it = demote.rbegin(); it != demote.rend(); ++it)
            Desktop::windowState()->lower(*it);
    } else if (w->m_isFloating)
        Desktop::windowState()->raise(w);
}

static void onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
    if (e.state != WL_POINTER_BUTTON_STATE_PRESSED)
        return;

    // emissions precede the compositor's lock handling: a click on the
    // lockscreen must not reorder the windows beneath it
    if (NHyprCommon::sessionLocked())
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

    // deferred out of the button emission, like the focus raise below — the
    // compositor's own press handling still walks the stack we'd reorder
    if (const auto W = windowUnderCursor()) {
        PHLWINDOWREF WR{W};
        pendingRaise.arm([WR]() {
            if (NHyprCommon::sessionLocked())
                return; // the lock can engage between the emission and this run
            if (const auto W = WR.lock(); W && W->m_isMapped)
                raiseWindow(W);
        });
    }
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
    pendingRaise.arm([WR]() {
        if (NHyprCommon::sessionLocked())
            return; // the lock can engage between the emission and this run
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
        pendingFocus.arm([TARGET]() {
            if (NHyprCommon::sessionLocked())
                return; // the lock can engage between the emission and this run
            if (const auto W = TARGET.lock())
                Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_SWITCH_TO_WINDOW_HARD);
        });
        break;
    }
    return 0;
}

// hl.plugin.hyprclick.focus_next/focus_prev() — awesome's Mod+J/K
// (focus.byidx): cycle the workspace's windows in ARRIVAL order, wrapping.
// The native cycle_next walks the z-order list, which raise-on-focus
// rotates every press: forward still visits everything (the wrap always
// lands on the lowest window), but backward reads the second-from-top and
// bounces between the two newest raises.
static void focusByIdx(bool next) {
    const auto MON = Desktop::focusState()->monitor();
    const auto WS  = MON ? MON->m_activeWorkspace : nullptr;
    if (!WS)
        return;

    static std::vector<std::pair<uint64_t, PHLWINDOW>> wins; // reused; main thread only
    wins.clear();
    for (const auto& W : Desktop::windowState()->windows()) {
        if (!W->m_isMapped || W->isHidden() || !W->m_workspace || W->m_workspace->m_id != WS->m_id)
            continue;
        const auto [IT, NEW] = g_seq.try_emplace(W.get(), g_seqNext);
        if (NEW)
            g_seqNext++;
        wins.emplace_back(IT->second, W);
    }
    if (wins.empty())
        return;
    std::sort(wins.begin(), wins.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    const auto FOCUS = Desktop::focusState()->window();
    const int  N     = (int)wins.size();
    int        idx   = -1;
    for (int i = 0; i < N; i++)
        if (wins[i].second == FOCUS)
            idx = i;
    const int    TO = idx < 0 ? 0 : (idx + (next ? 1 : N - 1)) % N;

    PHLWINDOWREF TARGET{wins[TO].second};
    wins.clear(); // don't keep strong refs across calls
    pendingFocus.arm([TARGET]() {
        if (NHyprCommon::sessionLocked())
            return; // the lock can engage between the emission and this run
        if (const auto W = TARGET.lock(); W && W->m_isMapped)
            Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_SWITCH_TO_WINDOW_HARD);
    });
}

static int luaFocusNext(lua_State*) {
    focusByIdx(true);
    return 0;
}
static int luaFocusPrev(lua_State*) {
    focusByIdx(false);
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

    g_lifecycle.init();
    g_lifecycle.listen(Event::bus()->m_events.input.mouse.button, [](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { onMouseButton(e, info); });
    g_lifecycle.listen(Event::bus()->m_events.window.active, [](PHLWINDOW w, Desktop::eFocusReason reason) { onWindowActive(w, reason); });
    g_lifecycle.listen(Event::bus()->m_events.window.open, [](PHLWINDOW w) {
        if (w && g_seq.try_emplace(w.get(), g_seqNext).second)
            g_seqNext++;
    });
    g_lifecycle.listen(Event::bus()->m_events.window.destroy, [](PHLWINDOWREF wr) { g_seq.erase(wr.get()); });

    HyprlandAPI::addLuaFunction(PHANDLE, "hyprclick", "focus_prev_here", luaFocusPrevHere);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprclick", "focus_next", luaFocusNext);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprclick", "focus_prev", luaFocusPrev);

    return {"hyprclick", "awesome's click/focus policy", "hitori", "1.1.6"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_lifecycle.resetAll(); // listeners first, then every hop
    g_seq.clear();
}
