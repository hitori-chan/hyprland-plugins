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

#include "common/input.hpp"
#include "common/lifecycle.hpp"
#include "common/order.hpp"
#include "common/persist.hpp"
#include "common/queries.hpp"

#include "geometry.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/layout/target/WindowTarget.hpp>
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
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>

#include <linux/input-event-codes.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_map>

static HANDLE                  PHANDLE = nullptr;

static NHyprCommon::CLifecycle g_lifecycle;
static NHyprCommon::CHop       pendingMax;

// plugin-maximized windows and their restore geometry.
static std::unordered_map<PHLWINDOWREF, CBox> g_maximized;

// last windowed box per app class, surviving window closes and relogs: the
// restore target when a window of that app is born maximized again.
static NHyprCommon::SBoxStore g_lastWindowed;

static CBox                   boundedRestore(PHLWINDOW w, const CBox& box, const CBox& workarea) {
    const auto T = w ? w->windowTarget() : nullptr;
    if (!T)
        return box;
    return NHyprmax::Geometry::boundedRestore(box, workarea, T->minSize(), T->maxSize());
}

static std::filesystem::path storePath() {
    return NHyprCommon::statePath("hyprmax", "windowed.tsv");
}

static void loadWindowed() {
    g_lastWindowed = NHyprCommon::readBoxTsv(storePath());
    // only a real windowed size is a restore target (a legacy position-only
    // row carries none)
    std::erase_if(g_lastWindowed, [](const auto& E) { return E.second.w <= 5 || E.second.h <= 5; });
}

static void saveWindowed() {
    NHyprCommon::writeBoxTsv(storePath(), g_lastWindowed);
}

static NHyprCommon::CSaver g_saver{saveWindowed};

static void                rememberWindowed(const std::string& cls, const CBox& box) {
    if (cls.empty() || box.w <= 5 || box.h <= 5)
        return;
    if (NHyprCommon::rememberBox(g_lastWindowed, cls, box))
        g_saver.dirty();
}

static bool pluginMaximized(PHLWINDOW w) {
    return g_maximized.contains(PHLWINDOWREF{w});
}

// Compositor-granted maximize (born-maximized at map, app request) holds
// the workspace's single internal fullscreen slot: a later fullscreen
// window evicts it, and it never comes back. Dissolve the grant into
// plugin maximize instead — slot freed, told-state and workarea box kept.
static void adoptCompositorMax(PHLWINDOW W) {
    if (!W || !W->mapped() || !W->windowTarget() || !W->isFloating() || pluginMaximized(W))
        return;
    if (Fullscreen::controller()->getFullscreenModes(W).internal != Fullscreen::FSMODE_MAXIMIZED)
        return;
    const auto MON = W->m_monitor.lock();
    if (!MON)
        return;

    Fullscreen::controller()->setFullscreenMode(W, Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE);
    // the exit just granted the client the size choice — the box is ours
    W->m_sizeFromClientSerial = 0;
    // the geometry change below re-enters the floating recalc, which would
    // fire the one-shot respawnIfBornFullscreen and re-arm the 0x0 client-
    // size grant on top of the plugin box; a client answering with its
    // normal size would resize the plugin-maximized window out from under us
    W->m_bornFullscreen = false;

    const auto WA = MON->logicalBoxMinusReserved();
    // empty restore box = no windowed geometry ever existed; un-maximizing
    // hands the size choice to the client (see luaToggle)
    CBox restore{};
    if (const auto IT = g_lastWindowed.find(W->metadata().appID()); IT != g_lastWindowed.end())
        restore = boundedRestore(W, IT->second, WA);
    g_maximized[PHLWINDOWREF{W}] = restore;

    if (auto TOP = NHyprCommon::xdgToplevel(W))
        TOP->setMaximized(true);
    // through the layout, never the raw target: the floating algorithm's
    // fullscreen-exit recenter restores ITS tracked lastBox — a raw
    // setPositionGlobal leaves that stale at the pre-maximize box, and the
    // window "un-maximizes" on any later fullscreen roundtrip
    g_layoutManager->setTargetGeom(WA, W->windowTarget());
    W->windowTarget()->warpPositionSize();
    // the exit's 0x0 grant configure already reached the client, but
    // m_pendingReportedSize kept the real size — an unforced send dedups
    // against it and stays silent, leaving the client maximized at 0x0
    // with nothing to lay out: it never commits a frame (invisible
    // window). Force the workarea configure out.
    W->sendWindowSize(true);
}

static std::vector<PHLWINDOWREF> g_adoptQueue;
static bool                      adoptQueued = false;
static NHyprCommon::CHop         pendingAdopt;

static bool                reflowQueued = false;
static NHyprCommon::CHop   pendingReflow;

// A plugin-maximized window is a client-only state, so Hyprland's layout
// reflow does not resize it when a workspace changes monitor or a reserved
// area changes. Coalesce those events and apply the current workarea after
// the compositor finishes its own movement.
static void reflowMaximized() {
    for (const auto& ENTRY : g_maximized) {
        const auto W = ENTRY.first.lock();
        if (!W || !W->mapped() || !W->isFloating() || !W->windowTarget())
            continue;
        const auto MODES = Fullscreen::controller()->getFullscreenModes(W);
        if (MODES.internal != Fullscreen::FSMODE_NONE || MODES.client != Fullscreen::FSMODE_NONE)
            continue; // a native fullscreen/maximize grant owns the geometry
        const auto MON = W->m_monitor.lock();
        if (!MON)
            continue;
        const auto WA = MON->logicalBoxMinusReserved();
        if (W->windowTarget()->position() == WA)
            continue;
        g_layoutManager->setTargetGeom(WA, W->windowTarget());
        W->windowTarget()->warpPositionSize();
    }
}

static void queueReflow() {
    if (reflowQueued)
        return;
    reflowQueued = true;
    pendingReflow.arm([]() {
        reflowQueued = false;
        reflowMaximized();
    });
}

// queue+drain, never a lone doLaterLock: two born-maximized windows can
// map in one dispatch, and overwriting the lock cancels the unfired one
static void queueAdopt(PHLWINDOW w) {
    g_adoptQueue.emplace_back(w);
    if (adoptQueued)
        return;
    adoptQueued = true;
    pendingAdopt.arm([]() {
        adoptQueued  = false;
        const auto Q = std::move(g_adoptQueue);
        g_adoptQueue.clear();
        for (const auto& WR : Q)
            adoptCompositorMax(WR.lock());
    });
}

static bool maximizedAny(PHLWINDOW w) {
    return pluginMaximized(w) || Fullscreen::controller()->isFullscreen(w);
}

using NHyprCommon::superHeld;
using NHyprCommon::windowUnderCursor;

// Buttons whose press we swallowed; their release must be swallowed too so
// nothing downstream sees half a click.
static uint32_t swallowedButtons = 0;

static void     onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
    // emissions precede the compositor's lock handling: locked input belongs
    // to the lockscreen
    if (NHyprCommon::sessionLocked()) {
        swallowedButtons = 0;
        return;
    }
    if (NHyprCommon::nativeInputCaptureActive()) {
        swallowedButtons = 0;
        return;
    }

    const uint32_t BIT = NHyprCommon::trackedPointerButtonBit(e.button) & 3u;

    if (e.state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (!BIT || info.cancelled)
            return;

        // the bar owns clicks on layer surfaces; only a Super-grab can move
        // a window, so only that needs swallowing
        if (NHyprCommon::nativePointerGrabActive() || NHyprCommon::nativeLayerOwnsPointer() || !superHeld())
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
static void applyMaxToggle(const PHLWINDOWREF& WR) {
    const auto W = WR.lock();
    if (!W || !W->mapped() || !W->windowTarget())
        return;
    // the lock can engage between the keypress and this deferred run
    if (NHyprCommon::sessionLocked())
        return;

    std::erase_if(g_maximized, [](const auto& E) { return E.first.expired(); });

    const auto FSMODE = Fullscreen::controller()->getFullscreenModes(W).internal;

    // awesome keeps maximized and fullscreen independent: Mod+M toggles
    // maximized and must never drop a genuinely-fullscreen window (nor
    // plugin-maximize one — its fullscreen box is not a windowed size).
    if (FSMODE == Fullscreen::FSMODE_FULLSCREEN)
        return;

    // Compositor-maximized (born maximized, app request): native unmax.
    if (FSMODE == Fullscreen::FSMODE_MAXIMIZED) {
        Fullscreen::controller()->setFullscreenMode(W, Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE);

        // Born maximized: the compositor just granted the client the size
        // choice (m_sizeFromClientSerial armed). If this app's windowed
        // box is remembered, that beats the client's answer — GTK forgets
        // its normal geometry across restarts.
        const auto MON = W->m_monitor.lock();
        const auto IT  = g_lastWindowed.find(W->metadata().appID());
        if (IT != g_lastWindowed.end() && W->m_sizeFromClientSerial && MON && W->isFloating()) {
            W->m_sizeFromClientSerial = 0;
            g_layoutManager->setTargetGeom(boundedRestore(W, IT->second, MON->logicalBoxMinusReserved()), W->windowTarget());
            W->windowTarget()->warpPositionSize();
            // same disarmed-grant flush as adoptCompositorMax: unforced
            // sends dedup against m_pendingReportedSize and go silent
            // when the remembered box matches it
            W->sendWindowSize(true);
        }
        return;
    }

    const auto MON = W->m_monitor.lock();
    if (!MON || !W->isFloating())
        return;

    // X11 has no maximize hint on this path; geometry alone.
    const auto setClientMaximized = [&W](bool m) {
        if (auto TOP = NHyprCommon::xdgToplevel(W))
            TOP->setMaximized(m);
    };

    const auto WA = MON->logicalBoxMinusReserved();

    if (const auto IT = g_maximized.find(WR); IT != g_maximized.end()) {
        const CBox STORED = IT->second;
        g_maximized.erase(IT);
        setClientMaximized(false);
        if (STORED.w > 5 && STORED.h > 5) {
            const CBox R = boundedRestore(W, STORED, WA);
            rememberWindowed(W->metadata().appID(), R);
            g_layoutManager->setTargetGeom(R, W->windowTarget());
            W->windowTarget()->warpPositionSize();
        } else {
            // adopted with no remembered box: the client picks its size
            // (the 0x0 grant; the commit adoption recenters it)
            W->requestClientSize();
        }
    } else {
        const auto BOX = W->windowTarget()->position();
        g_maximized.emplace(WR, BOX);
        rememberWindowed(W->metadata().appID(), BOX);
        setClientMaximized(true);
        g_layoutManager->setTargetGeom(WA, W->windowTarget());
        W->windowTarget()->warpPositionSize();
        Desktop::windowState()->raise(W);
    }
}

// queue+drain, never a lone doLaterLock: two toggles can arm in one dispatch
// (scripted binds, event backlog) and overwriting the lock cancels the
// unfired one
static std::vector<PHLWINDOWREF> g_maxToggles;
static bool                      maxToggleQueued = false;

static int luaToggle(lua_State*) {
    const auto FOCUS = Desktop::focusState()->window();
    if (!FOCUS || !FOCUS->mapped() || !FOCUS->m_workspace)
        return 0;

    if (g_maxToggles.size() < 16)
        g_maxToggles.emplace_back(PHLWINDOWREF{FOCUS});
    if (maxToggleQueued)
        return 0;
    maxToggleQueued = true;
    pendingMax.arm([]() {
        maxToggleQueued = false;
        const auto Q = std::move(g_maxToggles);
        g_maxToggles.clear();
        for (const auto& WR : Q)
            applyMaxToggle(WR);
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

    // the immovable-maximized swallow must win over click-to-raise
    NHyprCommon::mustLoadBefore(PHANDLE, "hyprmax", {"hyprclick"});

    loadWindowed();

    g_lifecycle.init();
    g_lifecycle.listen(Event::bus()->m_events.input.mouse.button, [](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { onMouseButton(e, info); });

    // A window closed while plugin-maximized: keep its windowed box as the
    // app's remembered size (the window ref itself is about to expire).
    g_lifecycle.listen(Event::bus()->m_events.window.destroy, [](PHLWINDOWREF wr) {
        const auto IT = g_maximized.find(wr);
        if (IT == g_maximized.end())
            return;
        // get(), never lock(): the emission runs inside ~CWindow, where the
        // ref is already marked destroying — lock() can never succeed there,
        // while the pointed-to members are still intact (destructor body).
        if (const auto* W = wr.get())
            rememberWindowed(W->metadata().appID(), IT->second);
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
    g_lifecycle.listen(Event::bus()->m_events.window.fullscreen, [](PHLWINDOW w) {
        if (!w)
            return;
        if (pluginMaximized(w)) {
            if (auto TOP = NHyprCommon::xdgToplevel(w))
                TOP->setMaximized(true);
            queueReflow();
            return;
        }
        if (w->isFloating() && Fullscreen::controller()->getFullscreenModes(w).internal == Fullscreen::FSMODE_MAXIMIZED)
            queueAdopt(w);
        queueReflow();
    });

    auto& EV = Event::bus()->m_events;
    g_lifecycle.listen(EV.window.moveToWorkspace, [](PHLWINDOW, PHLWORKSPACE) { queueReflow(); });
    g_lifecycle.listen(EV.workspace.active, [](PHLWORKSPACE) { queueReflow(); });
    g_lifecycle.listen(EV.workspace.moveToMonitor, [](PHLWORKSPACE, PHLMONITOR) { queueReflow(); });
    g_lifecycle.listen(EV.monitor.layoutChanged, []() { queueReflow(); });
    g_lifecycle.listen(EV.monitor.reservedChanged, [](PHLMONITOR) { queueReflow(); });

    HyprlandAPI::addLuaFunction(PHANDLE, "hyprmax", "toggle", luaToggle);

    return {"hyprmax", "awesome's per-window maximize", "hitori", "1.1.13"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_lifecycle.resetAll(); // listeners first, then every hop
    adoptQueued = false;
    reflowQueued = false;
    g_adoptQueue.clear();
    g_saver.flush(); // the coalesced write must not die with the session
    g_maximized.clear();
    g_lastWindowed.clear();
    swallowedButtons = 0;
}
