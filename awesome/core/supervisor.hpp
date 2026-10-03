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
#include "queries.hpp"
#include "state.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>

#include <functional>
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
            m_listenRenderStage = EV.render.stage.listen([](eRenderStage stage) { Canvas::inst().onRenderStage(stage); });
            m_listenButton      = EV.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { Supervisor::inst().dispatchButton(e, info); });
            m_listenMove        = EV.input.mouse.move.listen([](Vector2D pos, Event::SCallbackInfo& info) { Supervisor::inst().dispatchMove(pos, info); });
            m_listenAxis        = EV.input.mouse.axis.listen([](IPointer::SAxisEvent e, Event::SCallbackInfo& info) { Supervisor::inst().dispatchAxis(e, info); });
            m_listenKey         = EV.input.keyboard.key.listen([](IKeyboard::SKeyEvent e, Event::SCallbackInfo& info) { Supervisor::inst().dispatchKey(e, info); });
        }

        // The only safe order: listeners, then hops, then modules — reverse
        // priority.
        void stop() {
            m_listenRenderStage.reset();
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
        }

        // ---- the pipeline ----

        void dispatchButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
            if (info.cancelled)
                return;
            if (sessionLocked() || nativeInputCaptureActive()) {
                for (auto* M : m_modules)
                    M->onInputBlocked();
                return;
            }
            for (auto* M : m_modules) {
                M->onPointerButton(e, info);
                if (info.cancelled)
                    return;
            }
        }
        void dispatchMove(const Vector2D& pos, Event::SCallbackInfo& info) {
            if (info.cancelled)
                return;
            if (sessionLocked() || nativeInputCaptureActive()) {
                for (auto* M : m_modules)
                    M->onInputBlocked();
                return;
            }
            for (auto* M : m_modules) {
                M->onPointerMove(pos, info);
                if (info.cancelled)
                    return;
            }
        }
        void dispatchAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) {
            if (info.cancelled)
                return;
            if (sessionLocked() || nativeInputCaptureActive()) {
                for (auto* M : m_modules)
                    M->onInputBlocked();
                return;
            }
            for (auto* M : m_modules) {
                M->onPointerAxis(e, info);
                if (info.cancelled)
                    return;
            }
        }
        void dispatchKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info) {
            if (info.cancelled)
                return;
            if (sessionLocked() || nativeInputCaptureActive()) {
                for (auto* M : m_modules)
                    M->onInputBlocked();
                return;
            }
            for (auto* M : m_modules) {
                M->onKey(e, info);
                if (info.cancelled)
                    return;
            }
        }

        HANDLE handle() const {
            return m_handle;
        }

      private:
        HANDLE                                        m_handle = nullptr;
        std::vector<IModule*>                         m_modules;
        std::vector<Hyprutils::Signal::CHyprSignalListener> m_listeners;
        Hyprutils::Signal::CHyprSignalListener m_listenRenderStage;
        Hyprutils::Signal::CHyprSignalListener m_listenButton;
        Hyprutils::Signal::CHyprSignalListener m_listenMove;
        Hyprutils::Signal::CHyprSignalListener m_listenAxis;
        Hyprutils::Signal::CHyprSignalListener m_listenKey;
    };

    inline Supervisor& supervisor() {
        return Supervisor::inst();
    }

} // namespace NAwesome
