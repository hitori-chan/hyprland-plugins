// common/busclient.hpp — one sd-bus connection wired into the compositor's
// event loop, shared by every plugin that talks D-Bus.
//
// Event-driven: sd-bus's fd + eventFd live in the wayland event loop as
// removable sources, its rare internal timeouts ride a normally disarmed
// timer. Steady state is zero armed timers and no wakeups until the fd
// actually fires; every callback runs on the main thread, never inside
// render or input.
//
// Teardown order is the load-bearing part (the historical SEGV class:
// sdbus-c++ exceptions in flight while the connection dies unwind through
// the event loop's C frames): sources out FIRST, then owned proxies/objects
// (they borrow the connection), then the connection itself. A bus that dies
// under sync() never tears down inside the failing dispatch — the drop is
// deferred to the loop, and only the first failing source arms it.
#pragma once

#include "lifecycle.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>

#include <poll.h>
#include <sdbus-c++/sdbus-c++.h>
#include <wayland-server-core.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace NHyprCommon {

    class CBusLink {
      public:
        // immediate on the first sync() failure; the deferred teardown follows
        std::function<void(const std::string&)> onLost;
        // owned proxies/objects borrow the connection: reset them here —
        // runs inside teardown(), before the connection dies
        std::function<void()>                   dropOwned;
        // deferred with a lost-bus teardown (e.g. repaint a strip the dead
        // items just left)
        std::function<void()>                   afterTeardown;

        // Connect and wire the fds. Throws what sdbus throws (the caller
        // owns the failure message) after cleaning up after itself. Does NOT
        // run the first sync(): a server registers its vtable first, so no
        // early message can dispatch against an unregistered object — call
        // sync() once setup is complete.
        void open(bool system, const char* serviceName = nullptr) {
            try {
                m_conn = system         ? sdbus::createSystemBusConnection() :
                    serviceName != nullptr ? sdbus::createSessionBusConnection(sdbus::ServiceName{serviceName}) :
                                             sdbus::createSessionBusConnection();
                m_poll = makeShared<CEventLoopTimer>(std::nullopt, [this](SP<CEventLoopTimer>, void*) { sync(); }, nullptr);
                g_pEventLoopManager->addTimer(m_poll);
                const auto PD = m_conn->getEventLoopPollData();
                m_src         = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, PD.fd, WL_EVENT_READABLE, fdReady, this);
                m_evtSrc      = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, PD.eventFd, WL_EVENT_READABLE, fdReady, this);
            } catch (...) {
                close();
                throw;
            }
        }

        // sd-bus dispatch is not re-entrant: never drain from a send site or
        // a method handler, park a near tick on the timer instead
        void pollSoon() {
            if (m_poll)
                m_poll->updateTimeout(std::chrono::milliseconds(2));
        }

        // Drain, then hand sd-bus's own poll needs to the event loop: fd
        // mask from PollData::events, its timeout on the timer.
        void sync() {
            if (!m_conn)
                return;
            try {
                int n = 0;
                while (n++ < 64 && m_conn->processPendingEvent()) {} // cap: a flooding client must not stall the frame
                const auto PD = m_conn->getEventLoopPollData();
                if (m_src)
                    wl_event_source_fd_update(m_src, ((PD.events & POLLIN) ? WL_EVENT_READABLE : 0) | ((PD.events & POLLOUT) ? WL_EVENT_WRITABLE : 0));
                const auto REL = PD.getRelativeTimeout();
                if (n > 64) // cap hit: more queued, come back next tick
                    m_poll->updateTimeout(std::chrono::milliseconds(2));
                else if (REL == std::chrono::microseconds::max())
                    m_poll->updateTimeout(std::nullopt);
                else
                    m_poll->updateTimeout(std::max(std::chrono::duration_cast<std::chrono::milliseconds>(REL), std::chrono::milliseconds(1)));
            } catch (const std::exception& E) {
                if (m_conn && g_pEventLoopManager && !m_pendingDrop.armed()) {
                    if (onLost)
                        onLost(E.what());
                    m_pendingDrop.arm([this]() {
                        teardown();
                        if (afterTeardown)
                            afterTeardown();
                    });
                }
            }
        }

        // sources out first, then owned objects, then the connection
        void teardown() {
            if (m_src)
                wl_event_source_remove(m_src);
            if (m_evtSrc)
                wl_event_source_remove(m_evtSrc);
            m_src = m_evtSrc = nullptr;
            if (m_poll)
                m_poll->updateTimeout(std::nullopt);
            if (dropOwned)
                dropOwned();
            m_conn.reset();
        }

        // PLUGIN_EXIT: teardown plus the timer's removal from the loop
        void close() {
            m_pendingDrop.reset();
            teardown();
            if (m_poll && g_pEventLoopManager)
                g_pEventLoopManager->removeTimer(m_poll);
            m_poll.reset();
        }

        sdbus::IConnection* conn() {
            return m_conn.get();
        }

      private:
        static int fdReady(int, uint32_t, void* data) {
            ((CBusLink*)data)->sync();
            return 0;
        }

        std::unique_ptr<sdbus::IConnection> m_conn;
        SP<CEventLoopTimer>                 m_poll;
        wl_event_source*                    m_src    = nullptr;
        wl_event_source*                    m_evtSrc = nullptr;
        CHop                                m_pendingDrop;
    };

} // namespace NHyprCommon
