// hyprbar/tray.cpp — the in-compositor StatusNotifierWatcher/Host and its items

#include "hyprbar.hpp"

namespace NHyprbar {

    // ---- tray: StatusNotifierWatcher + Host, in-compositor ----

    namespace Tray {
        static const sdbus::InterfaceName      WIFACE{"org.kde.StatusNotifierWatcher"};

        std::unique_ptr<sdbus::IConnection>    conn;
        static std::unique_ptr<sdbus::IObject> watcher;
        static std::unique_ptr<sdbus::IProxy>  busProxy;
        std::vector<SP<SItem>>                 items;
        static SP<CEventLoopTimer>             poll; // sd-bus timeout carrier + deferred-drain kicker, normally disarmed
        static wl_event_source*                busSrc    = nullptr;
        static wl_event_source*                busEvtSrc = nullptr;
        static UP<SEventLoopDoLaterLock>       pendingTeardown;

        // A drain must never run synchronously from here: sends happen inside
        // signal/reply handlers, i.e. inside processPendingEvent, and sd-bus
        // dispatch is not re-entrant. Park a near tick on the timer instead —
        // it re-syncs the fd mask (a send may want POLLOUT the current mask
        // predates) and drains whatever the send provoked.
        void pollSoon() {
            if (poll)
                poll->updateTimeout(std::chrono::milliseconds(2));
        }

        static void teardown() {
            if (busSrc)
                wl_event_source_remove(busSrc);
            if (busEvtSrc)
                wl_event_source_remove(busEvtSrc);
            busSrc = busEvtSrc = nullptr;
            if (poll)
                poll->updateTimeout(std::nullopt);
            if (!Menu::isLocal)
                try {
                    Menu::close(); // its proxy borrows conn; close it before conn dies
                } catch (...) {}   // close() sends "closed" events — the bus may already be gone
            items.clear();
            busProxy.reset();
            watcher.reset();
            conn.reset();
        }

        // Drain the bus, then hand sd-bus's own poll needs to the event loop:
        // fd mask from PollData::events, its (rare) internal timeout on the
        // timer. Steady state: zero timers armed, wakeups only when the fd
        // actually fires.
        static void syncBus() {
            if (!conn)
                return;
            try {
                int n = 0;
                while (n++ < 64 && conn->processPendingEvent()) {} // cap: a flooding client must not stall the frame
                const auto PD = conn->getEventLoopPollData();
                if (busSrc)
                    wl_event_source_fd_update(busSrc, ((PD.events & POLLIN) ? WL_EVENT_READABLE : 0) | ((PD.events & POLLOUT) ? WL_EVENT_WRITABLE : 0));
                const auto REL = PD.getRelativeTimeout();
                if (n > 64) // cap hit: more queued, come back next tick
                    poll->updateTimeout(std::chrono::milliseconds(2));
                else if (REL == std::chrono::microseconds::max())
                    poll->updateTimeout(std::nullopt);
                else
                    poll->updateTimeout(std::max(std::chrono::duration_cast<std::chrono::milliseconds>(REL), std::chrono::milliseconds(1)));
            } catch (const std::exception& E) {
                // the bus died under us (broker restart); an escape here would
                // unwind through the event loop's C frames and kill the session
                if (conn && g_pEventLoopManager && !pendingTeardown) {
                    HyprlandAPI::addNotification(PHANDLE, std::string{"[hyprbar] tray bus lost, tray disabled: "} + E.what(), CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
                    pendingTeardown = g_pEventLoopManager->doLaterLock([]() {
                        teardown();
                        barChanged(); // the dead items just left the strip
                    });
                }
            }
        }

        static int onBusFd(int, uint32_t, void*) {
            syncBus();
            return 0;
        }

        // Name and pixmap are fetched SERIALLY and committed as ONE change.
        // They arrive as separate replies, and applying each on its own gave
        // every REAL icon flip (fcitx idle <-> unikey — one per IM toggle /
        // input-context change) an intermediate frame: the new name rendered
        // from the theme file, then the item's own pixmap — a visible
        // double-blink on the tray per window action.
        static void fetchIcon(SP<SItem> it) {
            // a NeedsAttention item shows its attention icon set (SNI spec)
            const bool  ATTN  = it->status == "NeedsAttention";
            const char* PNAME = ATTN ? "AttentionIconName" : "IconName";
            const char* PPIX  = ATTN ? "AttentionIconPixmap" : "IconPixmap";
            it->proxy->getPropertyAsync(PNAME).onInterface(SNI).uponReplyInvoke([it, PPIX](std::optional<sdbus::Error> eN, sdbus::Variant vN) {
                auto name = it->iconName; // an errored reply keeps the current name
                if (!eN)
                    try {
                        name = vN.get<std::string>();
                    } catch (...) {}
                it->proxy->getPropertyAsync(PPIX).onInterface(SNI).uponReplyInvoke([it, name](std::optional<sdbus::Error> e, sdbus::Variant v) {
                    std::vector<uint8_t> px;
                    int                  pw = 0, ph = 0;
                    if (!e) {
                        try {
                            auto pixmaps = v.get<std::vector<sdbus::Struct<int32_t, int32_t, std::vector<uint8_t>>>>();

                            // smallest one still >= 22 px, else the biggest available
                            const std::vector<uint8_t>* best = nullptr;
                            int                         bw = 0, bh = 0;
                            for (const auto& P : pixmaps) {
                                const int W = std::get<0>(P), H = std::get<1>(P);
                                if (W <= 0 || H <= 0 || std::get<2>(P).size() < (size_t)W * H * 4)
                                    continue;
                                if (!best || (bw < 22 && W > bw) || (W >= 22 && bw >= 22 && W < bw) || (W >= 22 && bw < 22)) {
                                    best = &std::get<2>(P);
                                    bw   = W;
                                    bh   = H;
                                }
                            }

                            if (best) {
                                // SNI pixmaps are ARGB32 in network byte order (A,R,G,B);
                                // DRM_FORMAT_ARGB8888 wants B,G,R,A. Premultiply on the way.
                                px.resize((size_t)bw * bh * 4);
                                for (size_t i = 0; i < (size_t)bw * bh; i++) {
                                    const uint8_t A = (*best)[i * 4], R = (*best)[i * 4 + 1], G = (*best)[i * 4 + 2], B = (*best)[i * 4 + 3];
                                    px[i * 4]     = (uint8_t)(B * A / 255);
                                    px[i * 4 + 1] = (uint8_t)(G * A / 255);
                                    px[i * 4 + 2] = (uint8_t)(R * A / 255);
                                    px[i * 4 + 3] = A;
                                }
                                pw = bw;
                                ph = bh;
                            }
                        } catch (...) {}
                    }
                    // An empty/absent reply must CLEAR old pixels (fcitx's idle
                    // state has no pixmap) — stale ones shadow the new IconName
                    // in the rebuild and the icon sticks on the previous state.
                    if (name == it->iconName && px == it->pixels && pw == it->pw && ph == it->ph)
                        return;
                    it->iconName = name;
                    it->pixels   = std::move(px);
                    it->pw       = pw;
                    it->ph       = ph;
                    it->dirty    = true;
                    barChanged();
                });
                pollSoon(); // the pixmap fetch was just sent
            });
            pollSoon();
        }

        static void fetchProps(SP<SItem> it) {
            // Every reply is change-detected: fcitx fires NewIcon on every input
            // context change (= every window focus), and rebuilding textures +
            // redrawing the bar for identical content made the bar flicker like
            // it was reloading during any window action.
            it->proxy->getPropertyAsync("IconThemePath").onInterface(SNI).uponReplyInvoke([it](std::optional<sdbus::Error> e, sdbus::Variant v) {
                if (!e)
                    try {
                        it->themePath = v.get<std::string>();
                    } catch (...) {}
            });
            it->proxy->getPropertyAsync("Menu").onInterface(SNI).uponReplyInvoke([it](std::optional<sdbus::Error> e, sdbus::Variant v) {
                if (!e)
                    try {
                        it->menuPath = v.get<sdbus::ObjectPath>();
                    } catch (...) {}
            });
            it->proxy->getPropertyAsync("ItemIsMenu").onInterface(SNI).uponReplyInvoke([it](std::optional<sdbus::Error> e, sdbus::Variant v) {
                if (!e)
                    try {
                        it->itemIsMenu = v.get<bool>();
                    } catch (...) {}
            });
            // Status decides which icon set to read, so the icon chain hangs
            // off its reply.
            it->proxy->getPropertyAsync("Status").onInterface(SNI).uponReplyInvoke([it](std::optional<sdbus::Error> e, sdbus::Variant v) {
                std::string st = it->status; // an errored reply keeps the current status
                if (!e)
                    try {
                        st = v.get<std::string>();
                    } catch (...) {}
                if (st != it->status) {
                    it->status = st;
                    barChanged(); // Passive items vanish from the strip
                }
                fetchIcon(it);
            });
            pollSoon();
        }

        static void addItem(const std::string& service, const std::string& path) {
            for (const auto& I : items)
                if (I->service == service && I->path == path)
                    return;

            auto it     = makeShared<SItem>();
            it->service = service;
            it->path    = path;
            it->proxy   = sdbus::createProxy(*conn, sdbus::ServiceName{service}, sdbus::ObjectPath{path});
            it->proxy->uponSignal("NewIcon").onInterface(SNI).call([it]() { fetchProps(it); });
            it->proxy->uponSignal("NewAttentionIcon").onInterface(SNI).call([it]() { fetchIcon(it); });
            it->proxy->uponSignal("NewStatus").onInterface(SNI).call([it](std::string st) {
                if (st != it->status) {
                    it->status = st;
                    barChanged(); // Passive <-> shown, NeedsAttention swaps the icon set
                }
                fetchIcon(it);
            });
            fetchProps(it);
            items.push_back(it);
            watcher->emitSignal("StatusNotifierItemRegistered").onInterface(WIFACE).withArguments(service + path);
            barChanged();
        }

        static void dropService(const std::string& service) {
            const auto BEFORE = items.size();
            std::erase_if(items, [&](const auto& I) { return I->service == service; });
            if (items.size() != BEFORE) {
                onServiceDropped(service);
                if (watcher)
                    watcher->emitSignal("StatusNotifierItemUnregistered").onInterface(WIFACE).withArguments(service);
                barChanged();
            }
        }

        void init() {
            try {
                conn    = sdbus::createSessionBusConnection(sdbus::ServiceName{"org.kde.StatusNotifierWatcher"});
                watcher = sdbus::createObject(*conn, sdbus::ObjectPath{"/StatusNotifierWatcher"});

                watcher
                    ->addVTable(sdbus::registerMethod("RegisterStatusNotifierItem").withInputParamNames("service").implementedAs([](std::string arg) {
                        const std::string SENDER = watcher->getCurrentlyProcessedMessage().getSender();
                        if (!arg.empty() && arg.front() == '/')
                            addItem(SENDER, arg);
                        else
                            addItem(arg.empty() ? SENDER : arg, "/StatusNotifierItem");
                    }),
                                sdbus::registerMethod("RegisterStatusNotifierHost").withInputParamNames("service").implementedAs([](std::string) {}),
                                sdbus::registerProperty("RegisteredStatusNotifierItems").withGetter([]() {
                                    std::vector<std::string> v;
                                    for (const auto& I : items)
                                        v.push_back(I->service + I->path);
                                    return v;
                                }),
                                sdbus::registerProperty("IsStatusNotifierHostRegistered").withGetter([]() { return true; }),
                                sdbus::registerProperty("ProtocolVersion").withGetter([]() { return (int32_t)0; }),
                                sdbus::registerSignal("StatusNotifierItemRegistered").withParameters<std::string>("service"),
                                sdbus::registerSignal("StatusNotifierItemUnregistered").withParameters<std::string>("service"),
                                sdbus::registerSignal("StatusNotifierHostRegistered"))
                    .forInterface(WIFACE);

                busProxy = sdbus::createProxy(*conn, sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"});
                busProxy->uponSignal("NameOwnerChanged").onInterface("org.freedesktop.DBus").call([](std::string name, std::string oldOwner, std::string newOwner) {
                    if (!oldOwner.empty() && newOwner.empty())
                        dropService(name);
                });

                watcher->emitSignal("StatusNotifierHostRegistered").onInterface(WIFACE);

                // Event-driven bus: sd-bus's fd + eventFd live in the wayland
                // event loop (removable sources — exit pulls them before the
                // connection dies), so idle costs zero wakeups and an incoming
                // signal lands the same loop iteration. The timer only carries
                // sd-bus's own rare timeouts and the deferred pollSoon() drain;
                // every callback still runs on the main thread.
                poll = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { syncBus(); }, nullptr);
                g_pEventLoopManager->addTimer(poll);

                const auto PD = conn->getEventLoopPollData();
                busSrc        = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, PD.fd, WL_EVENT_READABLE, onBusFd, nullptr);
                busEvtSrc     = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, PD.eventFd, WL_EVENT_READABLE, onBusFd, nullptr);
                syncBus(); // drain anything queued during setup, set the initial mask/timeout
            } catch (const std::exception& E) {
                HyprlandAPI::addNotification(PHANDLE, std::string{"[hyprbar] tray disabled: "} + E.what(), CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
                teardown();
            }
        }

        void exit() {
            pendingTeardown.reset();
            teardown(); // fd sources out BEFORE the connection dies
            if (poll && g_pEventLoopManager)
                g_pEventLoopManager->removeTimer(poll);
            poll.reset();
        }
    }

} // namespace NHyprbar
