// awesome/notify.cpp — the notify module: the freedesktop notification
// daemon, the card model, the surfaces (popups, the shade, the
// inline-reply field), and the input they own.
//
// The unit map lives in notify/model.hpp (bus, parse, model, icons,
// text, paint, popups, row, center, surface, input, reply).
//
// The pipeline order puts the shell first (its strip and menus claim
// their pixels); notify's surfaces sit under nothing in the windows
// module, so the click routing order between them is settled by the
// hit queue inside the module, not by the pipeline.
#include "notify.hpp"

#include "notify/model.hpp"
#ifdef AWESOME_GATE
#include "notify/ui.hpp" // the gate's menu probe
#endif

#include <hyprland/src/plugins/PluginAPI.hpp>

#include <lua.hpp>

#include <cmath>
#include <ctime>

namespace NAwesome::Notify {

    void queueCenterToggle(PHLMONITOR on); // defined below; the Lua door calls it first

    CModule& module() {
        static CModule M;
        return M;
    }

    namespace {
        // hl.plugin.awesome.suspend([minutes]) — the DND chord. Bare, it
        // toggles (off, or on until turned off); with minutes, DND holds
        // that long from now (0 ends it, math.huge never does). Deferred out
        // of the bind's input emission (the resume reflows and repaints).
        // Presses ACCUMULATE — two in one dispatch are two toggles, not one
        // — and a timed call replaces whatever was pending before it.
        static NAwesome::CHop         pendingSuspend;
        static int                    suspendPresses = 0;
        static std::optional<int64_t> suspendSet; // a timed call's setSuspend value, before the presses
        int                           luaSuspend(lua_State* L) {
            if (!g_pEventLoopManager)
                return 0; // presses must not accumulate with no drain to run them
            if (lua_gettop(L) >= 1 && lua_type(L, 1) == LUA_TNUMBER) {
                const double MIN = lua_tonumber(L, 1);
                if (std::isnan(MIN))
                    return 0;
                suspendSet     = MIN <= 0 ? 0 : MIN >= 1e7 ? -1 : (int64_t)std::time(nullptr) + std::max<int64_t>(1, std::llround(MIN * 60));
                suspendPresses = 0;
            } else
                suspendPresses++;
            pendingSuspend.arm([]() {
                if (const auto SET = std::exchange(suspendSet, std::nullopt))
                    Model::setSuspend(*SET);
                if (std::exchange(suspendPresses, 0) & 1)
                    Model::toggleSuspend();
            });
            return 0;
        }

        // The shade toggle: F12's user bind (hl.plugin.awesome.center()),
        // the shell's bell, and `hyprctl awesome center` all funnel here —
        // deferred and accumulating like suspend.
        static int               centerPresses = 0;
        static PHLMONITORREF     centerOn; // the latest press's monitor (the bell's)
        static NAwesome::CHop    pendingCenter;
        int                      luaCenter(lua_State*) {
            queueCenterToggle(nullptr);
            return 0;
        }
        int luaClearAll(lua_State*) {
            static NAwesome::CHop pendingClear;
            pendingClear.arm([]() { Model::dismissAllLive(); });
            return 0;
        }
    }

    static NAwesome::CHop pendingQuiet;

    // Where the last pointer MOTION landed. A pointer crossing onto another
    // monitor moves first and focuses the monitor after; a keyboard focus
    // move warps the pointer (no motion) and then focuses: only the motion
    // tells the two apart — the live pointer is on the new monitor either
    // way.
    static Vector2D lastMovePos{-1, -1};

    void queueCenterToggle(PHLMONITOR on) {
        if (!g_pEventLoopManager)
            return;
        centerOn = on;
        if (++centerPresses > 1)
            return;
        pendingCenter.arm([]() {
            if (!(std::exchange(centerPresses, 0) & 1))
                return;
            if (centerVisible())
                setCenter(false, /*repop=*/true); // an explicit close returns the parked stack
            else {
                placeCardsOn(centerOn.lock()); // the bell's monitor, not the focus's
                setCenter(true);
            }
        });
    }

    void CModule::init() {
        Model::init(); // the expiry timer stands before anything can arrive
        Bus::init();
        surfaceInit(); // the tick timers, the canvas layer
        iconsInit(); // the async decode poll and the .desktop index scan start now
        replyInit();
        inputInit(); // the hold timer
        controlsInit(); // the levels' hook: a published change repaints the open shade

        auto& EV = Event::bus()->m_events;
        // The cards arrive on the FOCUSED monitor and stay there
        // (surface.cpp: layoutMonitor) until a deliberate focus move carries
        // them. The follow is SOURCE-SELECTIVE: a sloppy (follow_mouse)
        // pointer that crosses onto the new monitor IS the focus change, and
        // lingering in the corner next to a notification must not pull every
        // card across the screens and back.
        supervisor().listen(EV.monitor.focused, [](PHLMONITOR mon) {
            if (notifs.empty() && !centerVisible())
                return;
            if (mon && NAwesome::monitorContaining(lastMovePos) == mon)
                return;
            placeCardsOn(mon);
        });
        supervisor().listen(EV.monitor.layoutChanged, []() {
            if (!notifs.empty() || centerVisible())
                notifChanged();
        });
        // quiet automatically: a share starting or ending, a window going
        // fullscreen, the focus landing on one — rechecked out of the
        // emission (a share's comes from inside a frame copy)
        supervisor().listen(EV.screenshare.state, [](bool, uint8_t, const std::string&) { pendingQuiet.arm(Quiet::refresh); });
        supervisor().listen(EV.window.fullscreen, [](PHLWINDOW) { pendingQuiet.arm(Quiet::refresh); });
        supervisor().listen(EV.window.active, [](PHLWINDOW, Desktop::eFocusReason) { pendingQuiet.arm(Quiet::refresh); });
        supervisor().listen(EV.config.reloaded, []() {
            NAwesome::refreshIconTheme();
            resetFallbackCache();
            if (!notifs.empty() || centerVisible())
                notifChanged(); // a live theme reload re-keys the texture caches
        });

        HANDLE H = supervisor().handle();
        HyprlandAPI::addLuaFunction(H, "awesome", "center", luaCenter); // F12 is the reserved bind
        HyprlandAPI::addLuaFunction(H, "awesome", "suspend", luaSuspend);
        HyprlandAPI::addLuaFunction(H, "awesome", "clear_all", luaClearAll);
    }

    void CModule::teardown() {
        lastMovePos = {-1, -1};
        pendingSuspend.reset();
        pendingQuiet.reset();
        suspendPresses = 0;
        suspendSet.reset();
        pendingCenter.reset();
        Bus::exit(); // the connection first: nothing may arrive mid-teardown
        Model::exit(); // then the cards, and their textures with them
        inputExit();
        controlsExit();
        replyTeardown();
        centerExit();
        iconsExit(); // the decode poll and the index helper out with the plugin
        surfaceExit();
    }

    // ---- the pipeline ----

    // The units' free functions share two names with the pipeline members
    // (onKey, onInputBlocked): the qualified calls reach the free ones.
    void CModule::onPointerButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
        NAwesome::Notify::onMouseButton(e, info);
    }
    void CModule::onPointerMove(const Vector2D& pos, Event::SCallbackInfo& info) {
        lastMovePos = pos;
        NAwesome::Notify::onMouseMove(pos, info);
    }
    void CModule::onPointerAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) {
        NAwesome::Notify::onMouseAxis(e, info);
    }
    void CModule::onKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info) {
        NAwesome::Notify::onKey(e, info);
    }
    void CModule::onInputBlocked() {
        NAwesome::Notify::onInputBlocked();
    }

    // `hyprctl awesome <verb>`
    std::optional<std::string> CModule::handleCtl(const std::string& verb) const {
        if (verb == "count")
            return std::to_string(notifs.size());
        if (verb == "center") {
            queueCenterToggle(nullptr);
            return "ok";
        }
        if (verb == "state")
            return Model::stateString();
        if (verb == "badge")
            return Model::badgeString();
        if (verb == "topline") // the gate's text probe: the newest card's collapsed one-liner
            return Model::toplineString();
#ifdef AWESOME_GATE
        if (verb == "shown") { // the newest card's body as drawn: a chat's fitted window, "" until painted
            for (const auto& N : notifs)
                if (!N->waiting && !Model::offShade(N))
                    return N->messages.empty() ? N->body : N->fitMemo.rev == N->rev ? N->fitMemo.body : std::string{};
            return std::string{};
        }
        if (verb == "menu") // the open card menu's rows ("label;label*;(label)": checked, dim)
            return CardMenu::describe();
        if (verb == "quiet") // "now:N hushed:N" — an arrival would hold; a critical banner shows without its words
            return std::format("now:{} hushed:{}", Quiet::now() ? 1 : 0, Quiet::hushed() ? 1 : 0);
#endif
        if (verb == "clear") { // the scripted reset
            static NAwesome::CHop pendingClear;
            pendingClear.arm([]() { Model::dismissAllLive(/*offShadeToo=*/true); });
            return "ok";
        }
        return std::nullopt;
    }

} // namespace NAwesome::Notify
