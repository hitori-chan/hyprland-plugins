// awesome/core/supervisor.hpp — the one PLUGIN_INIT/PLUGIN_EXIT, the
// module table, the input pipeline, and the state-listener registry.
//
// Registration order is the input priority order (the old load-order
// contract, now in code, unbreakable by a manifest edit): the shell's
// strip and menus first, the notify surfaces second, the windows policy
// last. Teardown is the exact reverse, and the only order the plugin has
// ever been safe in: state listeners first (nothing firing mid-teardown
// can re-queue a hop), then the hops, then the modules.
#pragma once

#include "canvas.hpp"
#include "config.hpp"
#include "hop.hpp"
#include "jobs.hpp"
#include "queries.hpp"
#include "state.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
#include <string>
#include <vector>

namespace NAwesome::detail {

    // mirror of CSignalT's RefArg (a nested alias there): trivially
    // copyable args by value, the rest by const reference
    template <typename T>
    using RefArgT = std::conditional_t<std::is_trivially_copyable_v<T>, T, const T&>;

    // a listener wrapper that no-ops once the compositor begins shutting
    // down (crash class 8) while keeping the signal's exact argument
    // signature: a variadic/generic wrapper is ambiguous between CSignalT's
    // listen(std::function<void(RefArg<Args>...)>) and listen(std::function<void()>).
    template <typename S>
    struct ListenerGate;
    template <typename... A>
    struct ListenerGate<Hyprutils::Signal::CSignalT<A...>> {
        template <typename F>
        static auto make(F fn) {
            struct Guarded {
                F fn;
                void operator()(RefArgT<A>... a) {
                    if (compositorShuttingDown())
                        return;
                    fn(a...);
                }
            };
            return Guarded{std::move(fn)};
        }
    };

} // namespace NAwesome::detail

namespace NAwesome {

    struct IModule {
        virtual ~IModule() = default;
        virtual const char* name() const = 0;
        // init in priority order; state listeners register here through
        // supervisor().listen()
        virtual void        init() = 0;
        // teardown in reverse order, after listeners and hops are down
        virtual void        teardown() = 0;

        // The pipeline, in registration order. Setting info.cancelled
        // consumes the event for everything after.
        virtual void onPointerButton(const IPointer::SButtonEvent&, Event::SCallbackInfo&) {}
        virtual void onPointerMove(const Vector2D&, Event::SCallbackInfo&) {}
        virtual void onPointerAxis(const IPointer::SAxisEvent&, Event::SCallbackInfo&) {}
        virtual void onKey(const IKeyboard::SKeyEvent&, Event::SCallbackInfo&) {}
        // Blocked input: the session is locked or a native input-capture
        // session owns the stream. Reset every swallow mask, held counter,
        // drag state, and armed zone here (crash class 7) — the pipeline
        // calls this instead of dispatching, so a module cannot forget.
        virtual void onInputBlocked() {}

        // `hyprctl awesome <verb>`: the first module to answer owns it.
        virtual std::optional<std::string> handleCtl(const std::string&) const {
            return std::nullopt;
        }
    };

    class Supervisor {
      public:
        static Supervisor& inst() {
            static Supervisor S;
            return S;
        }

        // priority order = registration order
        void registerModule(IModule* M) {
            m_modules.push_back(M);
        }

        // state listeners: registered during a module's init(), cleared in
        // stop() BEFORE any module teardown (a listener firing mid-teardown
        // could re-queue a hop that outlives the .so — crash class 6).
        // Every listener is wrapped in detail::ListenerGate: once the
        // COMPOSITOR begins shutting down, cleanup() clears the
        // window/workspace/monitor state with the plugin still loaded
        // (unload comes after the renderer), so ~CWindow emissions still
        // reach us into a half-dead state — warming the bar there SEGVs
        // (crash class 8). The gate makes those emissions no-ops.
        template <typename S, typename F>
        void listen(S& signal, F&& fn) {
            m_listeners.emplace_back(signal.listen(detail::ListenerGate<S>::make(std::forward<F>(fn))));
        }

        void start(HANDLE handle) {
            m_handle = handle;
            beginSession();
            // load (and, once, migrate + consume the legacy layouts into)
            // the unified state file — before any module touches its
            // store, or it would seed the old paths
            StateStore::inst().load();
            for (auto* M : m_modules)
                M->init();

            auto& EV = Event::bus()->m_events;
            m_listenRenderStage = EV.render.stage.listen([](const Event::SRenderStageEvent& ev) { Canvas::inst().onRenderStage(ev); });
            m_listenSolitary    = EV.monitor.blockSolitary.listen([](PHLMONITOR mon, bool& block) { Canvas::inst().onBlockSolitary(mon, block); });
            m_listenButton      = EV.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { Supervisor::inst().dispatchButton(e, info); });
            m_listenMove        = EV.input.mouse.move.listen([](Vector2D pos, Event::SCallbackInfo& info) { Supervisor::inst().dispatchMove(pos, info); });
            m_listenAxis        = EV.input.mouse.axis.listen([](IPointer::SAxisEvent e, Event::SCallbackInfo& info) { Supervisor::inst().dispatchAxis(e, info); });
            m_listenKey         = EV.input.keyboard.key.listen([](IKeyboard::SKeyEvent e, Event::SCallbackInfo& info) { Supervisor::inst().dispatchKey(e, info); });
        }

        // The only safe order: listeners, then hops, then modules — reverse
        // priority.
        void stop() {
            m_listenRenderStage.reset();
            m_listenSolitary.reset();
            m_listenButton.reset();
            m_listenMove.reset();
            m_listenAxis.reset();
            m_listenKey.reset();
            for (auto& L : m_listeners)
                L.reset();
            m_listeners.clear(); // state listeners before the hops they arm
            resetHops();
            for (auto IT = m_modules.rbegin(); IT != m_modules.rend(); ++IT)
                (*IT)->teardown();
            Jobs::inst().teardown(); // helpers after the modules that spawned them
        }

        // ---- the pipeline ----
        //
        // One shape for every event class: the head gates on the lock and on
        // native input capture (resetting every module's partial state), then
        // the modules run in priority order until one consumes the event.
        // Every button, axis and key event lands in the input trace with who
        // took it (motion stays out: it would flush the ring at once).

        void dispatchButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
            dispatch(info, "button", e.button, e.state, [&](IModule* M) { M->onPointerButton(e, info); });
        }
        void dispatchMove(const Vector2D& pos, Event::SCallbackInfo& info) {
            dispatch(info, nullptr, 0, 0, [&](IModule* M) { M->onPointerMove(pos, info); });
        }
        void dispatchAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) {
            dispatch(info, "axis", (uint32_t)e.axis, (uint32_t)std::lround(e.delta), [&](IModule* M) { M->onPointerAxis(e, info); });
        }
        void dispatchKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info) {
            dispatch(info, "key", e.keycode, (uint32_t)e.state, [&](IModule* M) { M->onKey(e, info); });
        }

        // `hyprctl awesome trace`: the last input events, oldest first, and
        // who took each one — a module, "blocked" (lock/capture), "upstream"
        // (cancelled before the plugin saw it) or "-" (passed through to the
        // compositor). Motion is not traced: it would flush the ring at once.
        std::string traceString() const {
            std::string out;
            for (size_t i = 0; i < TRACE_N; i++) {
                const auto& T = m_trace[(m_traceHead + i) % TRACE_N];
                if (!T.what)
                    continue;
                out += std::format("{} {} {} {} by {}\n", T.ms, T.what, T.a, T.b, T.by);
            }
            return out.empty() ? "no input yet\n" : out;
        }

        HANDLE handle() const {
            return m_handle;
        }

      private:
        template <typename F>
        void dispatch(Event::SCallbackInfo& info, const char* what, uint32_t a, uint32_t b, F&& run) {
            if (info.cancelled) {
                trace(what, a, b, "upstream");
                return;
            }
            if (sessionLocked() || nativeInputCaptureActive()) {
                for (auto* M : m_modules)
                    M->onInputBlocked();
                trace(what, a, b, "blocked");
                return;
            }
            for (auto* M : m_modules) {
                run(M);
                if (info.cancelled) {
                    trace(what, a, b, M->name());
                    return;
                }
            }
            trace(what, a, b, "-"); // passed through to the compositor
        }

        struct STrace {
            uint64_t    ms   = 0;
            const char* what = nullptr; // nullptr = empty slot (and motion: untraced)
            uint32_t    a = 0, b = 0;
            const char* by = "";
        };
        static constexpr size_t TRACE_N = 32;

        void trace(const char* what, uint32_t a, uint32_t b, const char* by) {
            if (!what)
                return;
            const auto NOW = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            m_trace[m_traceHead] = {.ms = (uint64_t)NOW, .what = what, .a = a, .b = b, .by = by};
            m_traceHead          = (m_traceHead + 1) % TRACE_N;
        }

        std::array<STrace, TRACE_N>                   m_trace{};
        size_t                                        m_traceHead = 0;
        HANDLE                                        m_handle = nullptr;
        std::vector<IModule*>                         m_modules;
        std::vector<Hyprutils::Signal::CHyprSignalListener> m_listeners;
        Hyprutils::Signal::CHyprSignalListener m_listenRenderStage;
        Hyprutils::Signal::CHyprSignalListener m_listenSolitary;
        Hyprutils::Signal::CHyprSignalListener m_listenButton;
        Hyprutils::Signal::CHyprSignalListener m_listenMove;
        Hyprutils::Signal::CHyprSignalListener m_listenAxis;
        Hyprutils::Signal::CHyprSignalListener m_listenKey;
    };

    inline Supervisor& supervisor() {
        return Supervisor::inst();
    }

} // namespace NAwesome
