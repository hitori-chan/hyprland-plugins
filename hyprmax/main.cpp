// hyprmax — awesome's per-window maximize as a native plugin.
//
// awesome's Mod+M: maximized is a PER-WINDOW flag there, any number at
// once. The compositor's internal maximize enforces one per workspace
// (granting one steals the previous, and a later fullscreen window EVICTS
// a maximized holder), and sync_fullscreen mirrors a client-only mode
// back into that machinery — so this maximize never enters compositor
// fullscreen state at all: the client is told (xdg set_maximized) and the
// window is sized to the workarea, exactly awesome's model. Maximize the
// compositor granted on its own (initial-maximize at map, app requests)
// is ADOPTED into this model on sight, for the same reason.
//
// The last windowed box is remembered per app class across window closes
// AND relogs (persisted to $XDG_STATE_HOME/hyprmax): un-maximizing a
// born-maximized window restores it instead of the client's guess (GTK
// forgets its normal geometry across restarts).
//
// Maximized windows are immovable, like awesome's: Super+left/right-click
// on one is swallowed whole — without this, the drag bind "picks the
// window up" at press time, instantly unmaximizing it and yoinking it to
// the cursor at its restored size. Loads BEFORE hyprclick so the swallow
// wins over click-to-raise.
//
// hl.plugin.hyprmax.toggle() — the Mod+M bind target. No config.

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>

#include <linux/input-event-codes.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

static HANDLE                                 PHANDLE = nullptr;

static Hyprutils::Signal::CHyprSignalListener lButton, lDestroy, lFullscreen;
static UP<SEventLoopDoLaterLock>              pendingMax;

// plugin-maximized windows and their restore geometry.
static std::unordered_map<PHLWINDOWREF, CBox> g_maximized;

// last windowed box per app class, surviving window closes and relogs: the
// restore target when a window of that app is born maximized again.
static std::unordered_map<std::string, CBox> g_lastWindowed;

static CBox                                  clampToWorkarea(CBox box, const CBox& wa) {
    box.x = std::clamp(box.x, wa.x, std::max(wa.x, wa.x + wa.w - box.w));
    box.y = std::clamp(box.y, wa.y, std::max(wa.y, wa.y + wa.h - box.h));
    return box;
}

static std::filesystem::path statePath() {
    const char* XDG  = std::getenv("XDG_STATE_HOME");
    const char* HOME = std::getenv("HOME");
    const auto  BASE = XDG && *XDG ? std::filesystem::path{XDG} : std::filesystem::path{HOME ? HOME : ""} / ".local/state";
    return BASE / "hyprmax" / "windowed.tsv";
}

// x y w h class — class last so any app_id parses.
static void loadWindowed() {
    std::ifstream f(statePath());
    std::string   line;
    while (std::getline(f, line)) {
        std::istringstream is(line);
        CBox               box;
        std::string        cls;
        if (is >> box.x >> box.y >> box.w >> box.h && is.get() == '\t' && std::getline(is, cls) && !cls.empty() && box.w > 5 && box.h > 5)
            g_lastWindowed[cls] = box;
    }
}

static void saveWindowed() {
    const auto      PATH = statePath();
    std::error_code ec;
    std::filesystem::create_directories(PATH.parent_path(), ec);

    // temp + rename: a crash mid-write must not eat the whole store
    const auto TMP = PATH.string() + ".tmp";
    {
        std::ofstream f(TMP, std::ios::trunc);
        if (!f)
            return;
        for (const auto& [CLS, B] : g_lastWindowed)
            f << std::llround(B.x) << '\t' << std::llround(B.y) << '\t' << std::llround(B.w) << '\t' << std::llround(B.h) << '\t' << CLS << '\n';
    }
    std::filesystem::rename(TMP, PATH, ec);
}

// Coalesced: a logout mass-close must not storm the disk with one full
// rewrite per window from inside each destroy emission (hyprplace's pattern).
static bool                      saveQueued = false;
static UP<SEventLoopDoLaterLock> pendingSave;

static void                      queueSave() {
    if (saveQueued || !g_pEventLoopManager)
        return;
    saveQueued  = true;
    pendingSave = g_pEventLoopManager->doLaterLock([]() {
        saveQueued = false;
        saveWindowed();
    });
}

static void rememberWindowed(const std::string& cls, const CBox& box) {
    if (cls.empty() || box.w <= 5 || box.h <= 5)
        return;
    const auto IT = g_lastWindowed.find(cls);
    if (IT != g_lastWindowed.end() && IT->second == box)
        return;
    g_lastWindowed[cls] = box;
    queueSave();
}

static bool pluginMaximized(PHLWINDOW w) {
    return g_maximized.contains(PHLWINDOWREF{w});
}

// Compositor-granted maximize (born-maximized at map, app request) holds
// the workspace's single internal fullscreen slot: a later fullscreen
// window evicts it, and it never comes back. Dissolve the grant into
// plugin maximize instead — slot freed, told-state and workarea box kept.
static void adoptCompositorMax(PHLWINDOW W) {
    if (!W || !W->m_isMapped || !W->m_target || !W->m_isFloating || pluginMaximized(W))
        return;
    if (Fullscreen::controller()->getFullscreenModes(W).internal != Fullscreen::FSMODE_MAXIMIZED)
        return;
    const auto MON = W->m_monitor.lock();
    if (!MON)
        return;

    Fullscreen::controller()->setFullscreenMode(W, Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE);
    // the exit just granted the client the size choice — the box is ours
    W->m_sizeFromClientSerial = 0;

    const auto WA = MON->logicalBoxMinusReserved();
    // empty restore box = no windowed geometry ever existed; un-maximizing
    // hands the size choice to the client (see luaToggle)
    CBox       restore{};
    if (const auto IT = g_lastWindowed.find(W->m_initialClass); IT != g_lastWindowed.end())
        restore = clampToWorkarea(IT->second, WA);
    g_maximized[PHLWINDOWREF{W}] = restore;

    if (!W->m_isX11 && W->m_xdgSurface && W->m_xdgSurface->m_toplevel)
        W->m_xdgSurface->m_toplevel->setMaximized(true);
    W->m_target->setPositionGlobal(WA);
    W->m_target->warpPositionSize();
}

static std::vector<PHLWINDOWREF> g_adoptQueue;
static bool                      adoptQueued = false;
static UP<SEventLoopDoLaterLock> pendingAdopt;

// queue+drain, never a lone doLaterLock: two born-maximized windows can
// map in one dispatch, and overwriting the lock cancels the unfired one
static void                      queueAdopt(PHLWINDOW w) {
    g_adoptQueue.emplace_back(w);
    if (adoptQueued || !g_pEventLoopManager)
        return;
    adoptQueued  = true;
    pendingAdopt = g_pEventLoopManager->doLaterLock([]() {
        adoptQueued = false;
        const auto Q = std::move(g_adoptQueue);
        g_adoptQueue.clear();
        for (const auto& WR : Q)
            adoptCompositorMax(WR.lock());
    });
}

static bool maximizedAny(PHLWINDOW w) {
    return pluginMaximized(w) || Fullscreen::controller()->isFullscreen(w);
}

static PHLWINDOW windowUnderCursor() {
    if (!g_pInputManager)
        return nullptr;
    return Desktop::viewState()->hitTest().windowAt(g_pInputManager->getMouseCoordsInternal(),
                                                    Desktop::View::ALLOW_FLOATING | Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS);
}

static bool superHeld() {
    const auto KB = g_pSeatManager ? g_pSeatManager->m_keyboard.lock() : nullptr;
    return KB && (KB->m_modifiersState.depressed & HL_MODIFIER_META);
}

// Buttons whose press we swallowed; their release must be swallowed too so
// nothing downstream sees half a click.
static uint32_t swallowedButtons = 0;

static void     onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
    // emissions precede the compositor's lock handling: locked input belongs
    // to the lockscreen
    if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked()) {
        swallowedButtons = 0;
        return;
    }

    const uint32_t BIT = e.button == BTN_LEFT ? 1u : e.button == BTN_RIGHT ? 2u : 0u;

    if (e.state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (!BIT || info.cancelled)
            return;

        // the bar owns clicks on layer surfaces; only a Super-grab can move
        // a window, so only that needs swallowing
        if (g_pInputManager->m_lastFocusOnLS || !superHeld())
            return;

        const auto W = windowUnderCursor();
        if (!W || !maximizedAny(W))
            return;

        // immovable: swallow the press before the keybind layer can start
        // a move/resize drag on it
        info.cancelled = true;
        swallowedButtons |= BIT;
        return;
    }

    if (BIT && (swallowedButtons & BIT)) {
        swallowedButtons &= ~BIT;
        info.cancelled = true;
    }
}

// hl.plugin.hyprmax.toggle() — awesome's Mod+M (see header).
static int luaToggle(lua_State*) {
    const auto FOCUS = Desktop::focusState()->window();
    if (!FOCUS || !FOCUS->m_isMapped || !FOCUS->m_workspace)
        return 0;

    PHLWINDOWREF WR{FOCUS};
    pendingMax = g_pEventLoopManager->doLaterLock([WR]() {
        const auto W = WR.lock();
        if (!W || !W->m_isMapped || !W->m_target)
            return;

        std::erase_if(g_maximized, [](const auto& E) { return E.first.expired(); });

        // Compositor-maximized (born maximized, app request): native unmax.
        if (Fullscreen::controller()->isFullscreen(W)) {
            Fullscreen::controller()->setFullscreenMode(W, Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE);

            // Born maximized: the compositor just granted the client the size
            // choice (m_sizeFromClientSerial armed). If this app's windowed
            // box is remembered, that beats the client's answer — GTK forgets
            // its normal geometry across restarts.
            const auto MON = W->m_monitor.lock();
            const auto IT  = g_lastWindowed.find(W->m_initialClass);
            if (IT != g_lastWindowed.end() && W->m_sizeFromClientSerial && MON && W->m_isFloating) {
                W->m_sizeFromClientSerial = 0;
                g_layoutManager->setTargetGeom(clampToWorkarea(IT->second, MON->logicalBoxMinusReserved()), W->m_target);
                W->m_target->warpPositionSize();
            }
            return;
        }

        const auto MON = W->m_monitor.lock();
        if (!MON || !W->m_isFloating)
            return;

        // X11 has no maximize hint on this path; geometry alone.
        const auto setClientMaximized = [&W](bool m) {
            if (!W->m_isX11 && W->m_xdgSurface && W->m_xdgSurface->m_toplevel)
                W->m_xdgSurface->m_toplevel->setMaximized(m);
        };

        const auto WA = MON->logicalBoxMinusReserved();

        if (const auto IT = g_maximized.find(WR); IT != g_maximized.end()) {
            const CBox STORED = IT->second;
            g_maximized.erase(IT);
            setClientMaximized(false);
            if (STORED.w > 5 && STORED.h > 5) {
                const CBox R = clampToWorkarea(STORED, WA);
                rememberWindowed(W->m_initialClass, R);
                W->m_target->setPositionGlobal(R);
                W->m_target->warpPositionSize();
            } else {
                // adopted with no remembered box: the client picks its size
                // (the 0x0 grant; the commit adoption recenters it)
                W->requestClientSize();
            }
        } else {
            const auto BOX = W->m_target->position();
            g_maximized.emplace(WR, BOX);
            rememberWindowed(W->m_initialClass, BOX);
            setClientMaximized(true);
            W->m_target->setPositionGlobal(WA);
            W->m_target->warpPositionSize();
            Desktop::windowState()->raise(W);
        }
    });
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
        HyprlandAPI::addNotification(PHANDLE, "[hyprmax] Version mismatch: rebuild the plugin against the running Hyprland", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hyprmax] version mismatch");
    }

    loadWindowed();

    lButton = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { onMouseButton(e, info); });

    // A window closed while plugin-maximized: keep its windowed box as the
    // app's remembered size (the window ref itself is about to expire).
    lDestroy = Event::bus()->m_events.window.destroy.listen([](PHLWINDOWREF wr) {
        const auto IT = g_maximized.find(wr);
        if (IT == g_maximized.end())
            return;
        // get(), never lock(): the emission runs inside ~CWindow, where the
        // ref is already marked destroying — lock() can never succeed there,
        // while the pointed-to members are still intact (destructor body).
        if (const auto* W = wr.get())
            rememberWindowed(W->m_initialClass, IT->second);
        g_maximized.erase(IT);
    });

    // The compositor recomputes the client-facing maximized bit from ITS
    // OWN fullscreen mode on every client-mode change
    // (updateClientMaximizedState) — and this plugin's maximize lives
    // outside that machinery, so a video entering/leaving fullscreen
    // stripped the told-maximized state and Firefox came back
    // unmaximized. The controller emits this event AFTER its sync, so
    // reasserting here wins, and both changes flush in one configure —
    // the client's belief never flickers.
    //
    // The same event announces a compositor-granted maximize (internal
    // FSMODE_MAXIMIZED): adopt it, deferred — we are inside the
    // controller's emission.
    lFullscreen = Event::bus()->m_events.window.fullscreen.listen([](PHLWINDOW w) {
        if (!w)
            return;
        if (pluginMaximized(w)) {
            if (!w->m_isX11 && w->m_xdgSurface && w->m_xdgSurface->m_toplevel)
                w->m_xdgSurface->m_toplevel->setMaximized(true);
            return;
        }
        if (w->m_isFloating && Fullscreen::controller()->getFullscreenModes(w).internal == Fullscreen::FSMODE_MAXIMIZED)
            queueAdopt(w);
    });

    HyprlandAPI::addLuaFunction(PHANDLE, "hyprmax", "toggle", luaToggle);

    return {"hyprmax", "awesome's per-window maximize", "hitori",
            "1.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    pendingMax.reset();
    pendingSave.reset();
    pendingAdopt.reset();
    adoptQueued = false;
    g_adoptQueue.clear();
    if (saveQueued) { // the coalesced write must not die with the session
        saveQueued = false;
        saveWindowed();
    }
    lButton.reset();
    lDestroy.reset();
    lFullscreen.reset();
    g_maximized.clear();
    g_lastWindowed.clear();
    swallowedButtons = 0;
}
