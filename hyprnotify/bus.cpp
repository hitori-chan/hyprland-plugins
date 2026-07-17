// hyprnotify/bus.cpp — the org.freedesktop.Notifications daemon and the model

#include "hyprnotify.hpp"

namespace NHyprnotify {

    std::vector<SP<SNotif>> notifs;

    namespace Bus {
        static const sdbus::InterfaceName          IFACE{"org.freedesktop.Notifications"};

        static std::unique_ptr<sdbus::IConnection> conn;
        static std::unique_ptr<sdbus::IObject>     obj;
        static SP<CEventLoopTimer>                 poll; // sd-bus timeout carrier + deferred-drain kicker, normally disarmed
        static SP<CEventLoopTimer>                 expiry;
        static wl_event_source*                    busSrc    = nullptr;
        static wl_event_source*                    busEvtSrc = nullptr;
        static UP<SEventLoopDoLaterLock>           pendingTeardown;
        static uint32_t                            nextId = 1;

        // A drain must never run synchronously from here: emits happen inside
        // method handlers, i.e. inside processPendingEvent, and sd-bus dispatch
        // is not re-entrant. Park it on the timer instead.
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
            obj.reset();
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
                if (conn && g_pEventLoopManager) {
                    HyprlandAPI::addNotification(PHANDLE, std::string{"[hyprnotify] bus lost, notifications disabled: "} + E.what(), CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
                    pendingTeardown = g_pEventLoopManager->doLaterLock([]() { teardown(); });
                }
            }
        }

        static int onBusFd(int, uint32_t, void*) {
            syncBus();
            return 0;
        }

        // ---- the model ----

        static SP<SNotif> byId(uint32_t id) {
            for (const auto& N : notifs)
                if (N->id == id)
                    return N;
            return nullptr;
        }

        void rearmExpiry() {
            if (!expiry)
                return;
            const auto NOW  = Time::steadyNow();
            int64_t    next = -1;
            for (const auto& N : notifs) {
                if (N->timeoutMs <= 0)
                    continue;
                const auto MS = std::chrono::duration_cast<std::chrono::milliseconds>(N->deadline - NOW).count();
                if (next < 0 || MS < next)
                    next = MS;
            }
            if (next < 0)
                expiry->updateTimeout(std::nullopt);
            else
                expiry->updateTimeout(std::chrono::milliseconds(std::max<int64_t>(next, 1)));
        }

        static void emitClosed(uint32_t id, uint32_t reason) {
            if (!obj)
                return;
            try {
                obj->emitSignal("NotificationClosed").onInterface(IFACE).withArguments(id, reason);
            } catch (...) {} // a dead bus must not unwind through the timer/doLater C frames
            pollSoon();
        }

        void closeOne(uint32_t id, uint32_t reason) {
            const auto BEFORE = notifs.size();
            std::erase_if(notifs, [&](const auto& N) { return N->id == id; });
            if (notifs.size() == BEFORE)
                return;
            emitClosed(id, reason);
            notifChanged();
            rearmExpiry();
        }

        void closeAll(uint32_t reason) {
            if (notifs.empty())
                return;
            for (const auto& N : notifs)
                emitClosed(N->id, reason);
            notifs.clear();
            notifChanged();
            rearmExpiry();
        }

        void invokeAction(uint32_t id, const std::string& key) {
            if (!obj)
                return;
            try {
                obj->emitSignal("ActionInvoked").onInterface(IFACE).withArguments(id, key);
            } catch (...) {}
            pollSoon();
        }

        // ---- incoming payload massage ----

        static void appendUtf8(std::string& out, uint32_t c) {
            if (c < 0x80)
                out += (char)c;
            else if (c < 0x800) {
                out += (char)(0xC0 | (c >> 6));
                out += (char)(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                out += (char)(0xE0 | (c >> 12));
                out += (char)(0x80 | ((c >> 6) & 0x3F));
                out += (char)(0x80 | (c & 0x3F));
            } else {
                out += (char)(0xF0 | (c >> 18));
                out += (char)(0x80 | ((c >> 12) & 0x3F));
                out += (char)(0x80 | ((c >> 6) & 0x3F));
                out += (char)(0x80 | (c & 0x3F));
            }
        }

        // The server never advertises body-markup, so tags are decoration we
        // must not render literally: strip them, unescape the entities. But
        // clients are entitled to send RAW text — a bare '<' or '&' that
        // doesn't form a tag/entity is content and must survive verbatim.
        static std::string stripMarkup(const std::string& in) {
            std::string out;
            out.reserve(in.size());
            for (size_t i = 0; i < in.size();) {
                if (in[i] == '<') {
                    const auto END = in.find('>', i);
                    if (END != std::string::npos) {
                        i = END + 1;
                        continue;
                    }
                } else if (in[i] == '&') {
                    const auto END = in.find(';', i);
                    if (END != std::string::npos && END - i <= 10) {
                        const auto E  = in.substr(i + 1, END - i - 1);
                        bool       ok = true;
                        if (E == "amp")
                            out += '&';
                        else if (E == "lt")
                            out += '<';
                        else if (E == "gt")
                            out += '>';
                        else if (E == "quot")
                            out += '"';
                        else if (E == "apos")
                            out += '\'';
                        else if (E.size() > 1 && E[0] == '#') {
                            const long C = E[1] == 'x' || E[1] == 'X' ? std::strtol(E.c_str() + 2, nullptr, 16) : std::strtol(E.c_str() + 1, nullptr, 10);
                            if (C > 0 && C < 0x110000)
                                appendUtf8(out, (uint32_t)C);
                            else
                                ok = false;
                        } else
                            ok = false;
                        if (ok) {
                            i = END + 1;
                            continue;
                        }
                    }
                }
                if (in[i] != '\r')
                    out += in[i];
                i++;
            }
            return out;
        }

        static std::string oneLine(std::string s) {
            for (auto& c : s)
                if (c == '\n')
                    c = ' ';
            return s;
        }

        // notify-send sends file paths and file:// URIs; bare theme names have
        // no analog here (naughty took paths) and resolve to no image.
        static std::string imagePath(std::string s) {
            if (s.starts_with("file://"))
                s.erase(0, 7);
            return s.starts_with('/') ? s : "";
        }

        // the spec's image-data: width, height, rowstride, has_alpha,
        // bits_per_sample, channels, RGB(A) bytes -> premultiplied BGRA
        using ImageData = sdbus::Struct<int32_t, int32_t, int32_t, bool, int32_t, int32_t, std::vector<uint8_t>>;

        static void unpackImageData(SNotif& n, const ImageData& d) {
            const int32_t W = std::get<0>(d), H = std::get<1>(d), STRIDE = std::get<2>(d), BPS = std::get<4>(d), CH = std::get<5>(d);
            const auto&   DATA = std::get<6>(d);
            if (W <= 0 || H <= 0 || BPS != 8 || (CH != 3 && CH != 4) || DATA.size() < (size_t)STRIDE * (H - 1) + (size_t)W * CH)
                return;
            n.pixels.resize((size_t)W * H * 4);
            for (int32_t y = 0; y < H; y++) {
                const uint8_t* row = DATA.data() + (size_t)y * STRIDE;
                uint8_t*       out = n.pixels.data() + (size_t)y * W * 4;
                for (int32_t x = 0; x < W; x++) {
                    const uint8_t R = row[x * CH], G = row[x * CH + 1], B = row[x * CH + 2], A = CH == 4 ? row[x * CH + 3] : 255;
                    out[x * 4]     = (uint8_t)(B * A / 255);
                    out[x * 4 + 1] = (uint8_t)(G * A / 255);
                    out[x * 4 + 2] = (uint8_t)(R * A / 255);
                    out[x * 4 + 3] = A;
                }
            }
            n.pw        = W;
            n.ph        = H;
            n.hasPixels = true;
        }

        static uint32_t handleNotify(const std::string& appName, uint32_t replacesId, const std::string& appIcon, const std::string& summary, const std::string& body,
                                     const std::vector<std::string>& actions, const std::map<std::string, sdbus::Variant>& hints, int32_t expireTimeout) {
            uint32_t id = replacesId;
            if (id == 0) {
                id = nextId++;
                if (nextId == 0)
                    nextId = 1;
            } else if (id >= nextId) {
                nextId = id + 1; // the OSD scripts pin high replace ids — never hand them out again
                if (nextId == 0)
                    nextId = 1; // a replaces_id of UINT32_MAX must not make the next fresh id 0 ("no id")
            }

            auto n = byId(id);
            if (!n) {
                n     = makeShared<SNotif>();
                n->id = id;
                notifs.push_back(n);
            }

            n->appName = appName;
            n->summary = oneLine(stripMarkup(summary));
            n->body    = stripMarkup(body);

            n->urgency  = 1;
            n->progress = -1;
            n->image.clear();
            n->pixels.clear();
            n->hasPixels = false;
            n->pw = n->ph = 0;

            if (const auto IT = hints.find("urgency"); IT != hints.end())
                try {
                    n->urgency = IT->second.get<uint8_t>();
                } catch (...) {
                    try {
                        n->urgency = (uint8_t)std::clamp(IT->second.get<int32_t>(), 0, 2);
                    } catch (...) {}
                }
            if (const auto IT = hints.find("value"); IT != hints.end())
                try {
                    n->progress = std::clamp(IT->second.get<int32_t>(), 0, 100);
                } catch (...) {
                    try {
                        n->progress = (int)std::min(IT->second.get<uint32_t>(), 100u);
                    } catch (...) {}
                }

            // image precedence per spec: image-data, then image-path, then app_icon
            for (const auto* KEY : {"image-data", "image_data", "icon_data"})
                if (const auto IT = hints.find(KEY); IT != hints.end() && n->pixels.empty())
                    try {
                        unpackImageData(*n, IT->second.get<ImageData>());
                    } catch (...) {}
            if (n->pixels.empty()) {
                for (const auto* KEY : {"image-path", "image_path"})
                    if (const auto IT = hints.find(KEY); IT != hints.end() && n->image.empty())
                        try {
                            n->image = imagePath(IT->second.get<std::string>());
                        } catch (...) {}
                if (n->image.empty())
                    n->image = imagePath(appIcon);
            }

            // "default" is what a left click invokes; a sole action counts too
            n->defaultAction.clear();
            for (size_t i = 0; i + 1 < actions.size(); i += 2)
                if (actions[i] == "default") {
                    n->defaultAction = actions[i];
                    break;
                }
            if (n->defaultAction.empty() && actions.size() == 2)
                n->defaultAction = actions[0];

            if (expireTimeout > 0)
                n->timeoutMs = expireTimeout;
            else if (expireTimeout == 0)
                n->timeoutMs = 0;
            else // -1: per-urgency defaults; critical stays until dismissed
                n->timeoutMs = n->urgency >= 2 ? 0 : (float)(n->urgency == 0 ? cfg.timeoutLow->value() : cfg.timeoutNormal->value());
            if (n->timeoutMs > 0)
                n->deadline = Time::steadyNow() + std::chrono::milliseconds((int64_t)n->timeoutMs);

            notifChanged();
            rearmExpiry();
            return id;
        }

        // ---- the daemon ----

        void init() {
            try {
                conn = sdbus::createSessionBusConnection(sdbus::ServiceName{"org.freedesktop.Notifications"});
                obj  = sdbus::createObject(*conn, sdbus::ObjectPath{"/org/freedesktop/Notifications"});

                obj->addVTable(sdbus::registerMethod("Notify")
                                   .withInputParamNames("app_name", "replaces_id", "app_icon", "summary", "body", "actions", "hints", "expire_timeout")
                                   .withOutputParamNames("id")
                                   .implementedAs([](std::string appName, uint32_t replacesId, std::string appIcon, std::string summary, std::string body,
                                                     std::vector<std::string> actions, std::map<std::string, sdbus::Variant> hints,
                                                     int32_t expireTimeout) { return handleNotify(appName, replacesId, appIcon, summary, body, actions, hints, expireTimeout); }),
                               sdbus::registerMethod("CloseNotification").withInputParamNames("id").implementedAs([](uint32_t id) { closeOne(id, R_CLOSED); }),
                               sdbus::registerMethod("GetCapabilities").withOutputParamNames("capabilities").implementedAs([]() {
                                   return std::vector<std::string>{"actions", "body", "icon-static"};
                               }),
                               sdbus::registerMethod("GetServerInformation").withOutputParamNames("name", "vendor", "version", "spec_version").implementedAs([]() {
                                   return std::tuple<std::string, std::string, std::string, std::string>{"hyprnotify", "hitori", "1.0.1", "1.2"};
                               }),
                               sdbus::registerSignal("NotificationClosed").withParameters<uint32_t, uint32_t>("id", "reason"),
                               sdbus::registerSignal("ActionInvoked").withParameters<uint32_t, std::string>("id", "action_key"))
                    .forInterface(IFACE);

                // Event-driven bus: sd-bus's fd + eventFd live in the wayland
                // event loop (removable sources — EXIT pulls them before the
                // connection dies), so idle costs zero wakeups and an incoming
                // Notify lands the same loop iteration. The timer only carries
                // sd-bus's own rare timeouts and the deferred pollSoon() drain;
                // every callback still runs on the main thread.
                poll = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { syncBus(); }, nullptr);
                g_pEventLoopManager->addTimer(poll);

                expiry = makeShared<CEventLoopTimer>(
                    std::nullopt,
                    [](SP<CEventLoopTimer>, void*) {
                        const auto            NOW = Time::steadyNow();
                        std::vector<uint32_t> due;
                        for (const auto& N : notifs)
                            if (N->timeoutMs > 0 && N->deadline <= NOW)
                                due.push_back(N->id);
                        for (const auto ID : due) {
                            std::erase_if(notifs, [&](const auto& N) { return N->id == ID; });
                            emitClosed(ID, R_EXPIRED);
                        }
                        if (!due.empty())
                            notifChanged();
                        rearmExpiry();
                    },
                    nullptr);
                g_pEventLoopManager->addTimer(expiry);

                const auto PD = conn->getEventLoopPollData();
                busSrc        = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, PD.fd, WL_EVENT_READABLE, onBusFd, nullptr);
                busEvtSrc     = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, PD.eventFd, WL_EVENT_READABLE, onBusFd, nullptr);
                syncBus(); // drain anything queued during setup, set the initial mask/timeout
            } catch (const std::exception& E) {
                // most likely another daemon owns the name (dunst still installed)
                HyprlandAPI::addNotification(PHANDLE, std::string{"[hyprnotify] disabled: "} + E.what(), CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
                obj.reset();
                conn.reset();
            }
        }

        void exit() {
            pendingTeardown.reset();
            teardown(); // fd sources out BEFORE the connection dies
            if (g_pEventLoopManager) {
                if (poll)
                    g_pEventLoopManager->removeTimer(poll);
                if (expiry)
                    g_pEventLoopManager->removeTimer(expiry);
            }
            poll.reset();
            expiry.reset();
            notifs.clear();
        }
    }

} // namespace NHyprnotify
