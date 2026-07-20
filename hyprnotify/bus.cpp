// hyprnotify/bus.cpp — the org.freedesktop.Notifications daemon and the model

#include "hyprnotify.hpp"

#include <hyprland/src/protocols/XDGActivation.hpp>

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
        static uint32_t                            nextId    = 1;
        static bool                                suspended = false; // DND

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
                // unwind through the event loop's C frames and kill the session.
                // Both fd sources can be ready in one dispatch batch: only the
                // first failure notifies and schedules the teardown.
                if (conn && g_pEventLoopManager && !pendingTeardown) {
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

        // ---- history (persistence): closed notifications kept for recall ----

        static std::vector<SNotif> history; // content snapshots; textures/pixels not carried

        static SNotif              snapshot(const SNotif& n) {
            SNotif s;
            s.appName       = n.appName;
            s.summary       = n.summary; // already sanitized markup: re-displays verbatim
            s.body          = n.body;
            s.image         = n.image;
            s.defaultAction = n.defaultAction;
            s.urgency       = n.urgency;
            s.actionIcons   = n.actionIcons;
            s.resident      = n.resident;
            for (const auto& A : n.actions)
                s.actions.push_back(SAction{.id = A.id, .label = A.label});
            for (const auto& IM : n.bodyImages)
                s.bodyImages.push_back({IM.src});
            return s;
        }

        // A card leaving the model is retained for recall — except progress/OSD
        // cards (a volume blip is not history) and transients (the hint opts out).
        static void retire(const SP<SNotif>& n) {
            if (!n || n->transient || n->progress >= 0)
                return;
            const size_t CAP = std::max<int64_t>(0, cfg.maxHistory->value());
            if (CAP == 0)
                return;
            history.push_back(snapshot(*n));
            while (history.size() > CAP)
                history.erase(history.begin());
        }

        void rearmExpiry() {
            if (!expiry)
                return;
            const auto NOW  = Time::steadyNow();
            int64_t    next = -1;
            for (const auto& N : notifs) {
                if (N->timeoutMs <= 0 || N->waiting)
                    continue;
                // clamp before comparing: -1 is the "none" sentinel, and an
                // overdue card's negative remaining time must still win
                const auto MS = std::max<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(N->deadline - NOW).count(), 1);
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
            if (const auto N = byId(id))
                retire(N);
            const auto BEFORE = notifs.size();
            std::erase_if(notifs, [&](const auto& N) { return N->id == id; });
            if (notifs.size() == BEFORE)
                return;
            emitClosed(id, reason);
            notifChanged();
            rearmExpiry();
        }

        void closeAll(uint32_t reason) {
            // the middle-click sweep clears what is on screen; cards the DND
            // queue holds were never seen and stay for the resume
            const auto BEFORE = notifs.size();
            for (const auto& N : notifs)
                if (!N->waiting) {
                    emitClosed(N->id, reason);
                    retire(N);
                }
            std::erase_if(notifs, [](const auto& N) { return !N->waiting; });
            if (notifs.size() == BEFORE)
                return;
            notifChanged();
            rearmExpiry();
        }

        void invokeAction(uint32_t id, const std::string& key) {
            if (!obj)
                return;
            try {
                // spec 1.3: the token signal precedes the action, so the
                // sender's xdg-activation request can actually raise it —
                // tokenless activates only flag urgent
                if (PROTO::activation)
                    obj->emitSignal("ActivationToken").onInterface(IFACE).withArguments(id, PROTO::activation->mintToken());
                obj->emitSignal("ActionInvoked").onInterface(IFACE).withArguments(id, key);
            } catch (...) {}
            pollSoon();
        }

        void toggleSuspend() {
            suspended = !suspended;
            if (suspended)
                return; // visible cards live out their timeouts; new arrivals queue
            const auto NOW = Time::steadyNow();
            for (const auto& N : notifs) {
                if (!N->waiting)
                    continue;
                N->waiting = false;
                if (N->timeoutMs > 0)
                    N->deadline = NOW + std::chrono::milliseconds((int64_t)N->timeoutMs);
            }
            notifChanged();
            rearmExpiry();
        }

        // Pop the most recently retained card back onto the stack as a fresh
        // notification (naughty/dunst history-pop): new id, fresh timeout.
        void recall() {
            if (history.empty())
                return;
            auto n = makeShared<SNotif>(std::move(history.back()));
            history.pop_back();
            n->waiting = false;
            do {
                n->id = nextId++;
                if (nextId == 0)
                    nextId = 1;
            } while (byId(n->id) || (n->id >= 9990 && n->id <= 9999));
            n->timeoutMs = n->urgency >= 2 ? 0 : (float)(n->urgency == 0 ? cfg.timeoutLow->value() : cfg.timeoutNormal->value());
            if (n->timeoutMs > 0)
                n->deadline = Time::steadyNow() + std::chrono::milliseconds((int64_t)n->timeoutMs);
            notifs.insert(notifs.begin(), n);

            const size_t CAP = std::max((int64_t)1, cfg.maxNotifs->value());
            while (notifs.size() > CAP) {
                auto victim = notifs.end() - 1;
                for (auto it = notifs.end() - 1; it != notifs.begin(); --it)
                    if ((*it)->urgency < 2) {
                        victim = it;
                        break;
                    }
                const auto VID = (*victim)->id;
                retire(*victim); // same as handleNotify: a card pushed off the
                notifs.erase(victim); // bottom stays recallable from history
                emitClosed(VID, R_UNDEFINED);
            }
            notifChanged();
            rearmExpiry();
        }

        size_t historySize() {
            return history.size();
        }

        // ---- incoming payload massage ----

        // We advertise body-markup, so the whitelisted Pango tags pass through
        // live; everything else is neutralized into content. Two client dialects
        // must both come out right: a markup-aware client escapes its reserved
        // chars ("a &amp;amp; b"), a naive one sends them raw ("a & b") — a bare
        // '&'/'<' that forms no entity/tag is escaped so Pango renders it
        // verbatim either way. Disallowed tags are dropped (spec: "filter them
        // out"); ALLOW_LINKS adds <a> (hyperlinks phase). <img> never reaches
        // here — it is extracted before sanitizing (body-images phase).
        static bool allowedTag(const std::string& name, bool allowLinks) {
            return name == "b" || name == "i" || name == "u" || name == "span" || name == "br" || (allowLinks && name == "a");
        }

        static std::string sanitizeMarkup(const std::string& in, bool allowLinks = false) {
            std::string out;
            out.reserve(in.size() + 16);
            for (size_t i = 0; i < in.size();) {
                const char CH = in[i];
                if (CH == '<') {
                    size_t j = i + 1;
                    if (j < in.size() && in[j] == '/')
                        j++;
                    const size_t NS = j;
                    while (j < in.size() && std::isalpha((unsigned char)in[j]))
                        j++;
                    if (j > NS) {
                        const auto END = in.find('>', j);
                        if (END != std::string::npos) {
                            std::string name = in.substr(NS, j - NS);
                            std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });
                            if (name == "br")
                                out += '\n'; // a line break, whatever the card does with it
                            else if (allowedTag(name, allowLinks))
                                out += in.substr(i, END - i + 1); // live tag, verbatim (Pango validates attrs)
                            // else: disallowed tag, dropped
                            i = END + 1;
                            continue;
                        }
                    }
                    out += "&lt;"; // a bare '<' that forms no tag: literal
                    i++;
                    continue;
                }
                if (CH == '&') {
                    const auto END = in.find(';', i);
                    if (END != std::string::npos && END - i <= 10) {
                        const auto E = in.substr(i + 1, END - i - 1);
                        if (E == "amp" || E == "lt" || E == "gt" || E == "quot" || E == "apos" || (E.size() > 1 && E[0] == '#')) {
                            out += in.substr(i, END - i + 1); // a real entity: Pango decodes it
                            i = END + 1;
                            continue;
                        }
                    }
                    out += "&amp;"; // a bare '&': literal
                    i++;
                    continue;
                }
                if (CH != '\r')
                    out += CH;
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

        // A path (file:// or absolute) is taken verbatim; anything else is a
        // freedesktop icon NAME resolved against the theme (dunst/mako do this,
        // and so should a compositor daemon). "" = nothing usable.
        static std::string resolveImage(std::string s, int sizePx) {
            if (s.empty())
                return "";
            if (s.starts_with("file://"))
                s.erase(0, 7);
            if (s.starts_with('/'))
                return s;
            return resolveIconName(s, sizePx);
        }

        // <img src="..."> is not a Pango tag; pull it from the body before the
        // markup sanitizer would drop it, resolve each src (path or themed name),
        // and return the thumbnails — removing the tags from the text. http(s)
        // and data: srcs aren't fetched, so they're skipped.
        static std::vector<std::string> extractImages(std::string& body, int sizePx) {
            std::vector<std::string> out;
            for (size_t i = 0; i < body.size();) {
                if (body[i] != '<') {
                    i++;
                    continue;
                }
                size_t j = i + 1;
                while (j < body.size() && std::isalpha((unsigned char)body[j]))
                    j++;
                std::string name = body.substr(i + 1, j - i - 1);
                std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });
                if (name != "img") {
                    i++;
                    continue;
                }
                const auto END = body.find('>', j);
                if (END == std::string::npos)
                    break;
                const auto  TAG = body.substr(i, END - i + 1);
                std::string tl  = TAG; // case-insensitive attr search, same offsets as TAG
                std::ranges::transform(tl, tl.begin(), [](unsigned char c) { return std::tolower(c); });
                std::string src;
                if (const auto SP = tl.find("src"); SP != std::string::npos)
                    if (const auto Q = TAG.find_first_of("\"'", SP); Q != std::string::npos)
                        if (const auto Q2 = TAG.find(TAG[Q], Q + 1); Q2 != std::string::npos)
                            src = TAG.substr(Q + 1, Q2 - Q - 1);
                if (!src.empty() && !src.starts_with("http") && !src.starts_with("data:"))
                    if (const auto P = resolveImage(src, sizePx); !P.empty())
                        out.push_back(P);
                body.erase(i, END - i + 1); // drop the tag from the text
            }
            return out;
        }

        // the spec's image-data: width, height, rowstride, has_alpha,
        // bits_per_sample, channels, RGB(A) bytes -> premultiplied BGRA
        using ImageData = sdbus::Struct<int32_t, int32_t, int32_t, bool, int32_t, int32_t, std::vector<uint8_t>>;

        static void unpackImageData(SNotif& n, const ImageData& d) {
            const int32_t W = std::get<0>(d), H = std::get<1>(d), STRIDE = std::get<2>(d), BPS = std::get<4>(d), CH = std::get<5>(d);
            const auto&   DATA = std::get<6>(d);
            // STRIDE must cover a row: a lying stride (0) would let a tiny
            // message claim gigapixel W*H and the resize below map it all.
            // The 16 MP cap bounds a genuine ~128 MB image-data (the D-Bus
            // message max) that would otherwise map + premultiply in full.
            if (W <= 0 || H <= 0 || (int64_t)W * H > (16 << 20) || BPS != 8 || (CH != 3 && CH != 4) || (int64_t)STRIDE < (int64_t)W * CH || DATA.size() < (size_t)STRIDE * (H - 1) + (size_t)W * CH)
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
            // keep only what a card can ever paint: warm frees visible cards'
            // buffers after upload, but an off-screen card would hold its
            // full-size pixmap until it scrolls on. 2x card width covers the
            // hero layout at any monitor scale; warm still scales exactly.
            shrinkPixels(n, std::max((int)cfg.width->value() * 2, (int)cfg.maxIcon->value() * 3));
        }

        static uint32_t handleNotify(const std::string& appName, uint32_t replacesId, const std::string& appIcon, const std::string& summary, const std::string& body,
                                     const std::vector<std::string>& actions, const std::map<std::string, sdbus::Variant>& hints, int32_t expireTimeout) {
            uint32_t id = replacesId;
            if (id == 0) {
                // Fresh ids count up from a low counter and skip any that's
                // still live, so they never collide with a displayed
                // notification. Crucially the counter is NOT dragged up to a
                // seen replaces_id (as it once was): the OSD scripts pin ids
                // in the 9990s (osd.sh, touchpad-auto.sh, battery-watch.sh),
                // and bumping past 9991 handed the next fresh notification
                // 9992 — the brightness OSD's id — so a keypress hijacked it.
                // Low fresh ids and the pinned range stay disjoint.
                do {
                    id = nextId++;
                    if (nextId == 0)
                        nextId = 1; // wrap: 0 means "no id"
                } while (byId(id) || (id >= 9990 && id <= 9999)); // never mint into the pinned OSD band
            }

            auto n = byId(id);
            if (!n) {
                n          = makeShared<SNotif>();
                n->id      = id;
                n->waiting = suspended; // DND: collect silently, the resume renders it
                notifs.insert(notifs.begin(), n); // newest on top; a replace keeps its slot

                const size_t CAP = std::max((int64_t)1, cfg.maxNotifs->value());
                while (notifs.size() > CAP) {
                    // oldest non-critical goes first; only an all-critical
                    // stack starts losing its oldest critical
                    auto victim = notifs.end() - 1;
                    for (auto it = notifs.end() - 1; it != notifs.begin(); --it)
                        if ((*it)->urgency < 2) {
                            victim = it;
                            break;
                        }
                    const auto VID = (*victim)->id;
                    retire(*victim);
                    notifs.erase(victim);
                    emitClosed(VID, R_UNDEFINED);
                } // the newcomer always survives: the scan stops short of begin()
            }

            n->appName = appName;
            n->summary = oneLine(sanitizeMarkup(summary));
            std::string bodyText = body;
            n->bodyImages.clear();
            for (const auto& P : extractImages(bodyText, std::max(64, (int)cfg.maxIcon->value() * 2)))
                n->bodyImages.push_back({P});
            n->body = sanitizeMarkup(bodyText, /*allowLinks=*/true);

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
                // precedence: image-path hint, then the app_icon param, then
                // the desktop-entry hint (its .desktop id doubles as an icon
                // name for most apps) — each a path OR a themed name
                const int   ICONPX = std::max(8, (int)cfg.maxIcon->value());
                std::string cand;
                for (const auto* KEY : {"image-path", "image_path"})
                    if (const auto IT = hints.find(KEY); IT != hints.end() && cand.empty())
                        try {
                            cand = IT->second.get<std::string>();
                        } catch (...) {}
                n->image = resolveImage(cand, ICONPX);
                if (n->image.empty())
                    n->image = resolveImage(appIcon, ICONPX);
                if (n->image.empty())
                    if (const auto IT = hints.find("desktop-entry"); IT != hints.end())
                        try {
                            n->image = resolveImage(IT->second.get<std::string>(), ICONPX);
                        } catch (...) {}
            }

            // actions arrive as [id0,label0, id1,label1, ...]. "default" is the
            // body-click target (no button of its own); every other pair becomes
            // a button. A lone named action also doubles as the body-click
            // default, so a one-action card fires by clicking anywhere.
            n->defaultAction.clear();
            n->actions.clear();
            for (size_t i = 0; i + 1 < actions.size(); i += 2) {
                if (actions[i] == "default")
                    n->defaultAction = actions[i];
                else if (!actions[i + 1].empty()) // an empty label has no button to draw
                    n->actions.push_back(SAction{.id = actions[i], .label = actions[i + 1]});
            }
            if (n->defaultAction.empty() && n->actions.size() == 1)
                n->defaultAction = n->actions.front().id;

            n->resident = false;
            if (const auto IT = hints.find("resident"); IT != hints.end())
                try {
                    n->resident = IT->second.get<bool>();
                } catch (...) {}
            n->actionIcons = false;
            if (const auto IT = hints.find("action-icons"); IT != hints.end())
                try {
                    n->actionIcons = IT->second.get<bool>();
                } catch (...) {}
            n->transient = false;
            if (const auto IT = hints.find("transient"); IT != hints.end())
                try {
                    n->transient = IT->second.get<bool>();
                } catch (...) {}

            if (expireTimeout > 0)
                n->timeoutMs = expireTimeout;
            else if (expireTimeout == 0)
                n->timeoutMs = 0;
            else // -1: per-urgency defaults; critical stays until dismissed
                n->timeoutMs = n->urgency >= 2 ? 0 : (float)(n->urgency == 0 ? cfg.timeoutLow->value() : cfg.timeoutNormal->value());
            if (n->timeoutMs > 0 && !n->waiting) // a queued card's clock starts at the resume
                n->deadline = Time::steadyNow() + std::chrono::milliseconds((int64_t)n->timeoutMs);

            if (!n->waiting) // a suspended arrival is invisible: no warm, no damage
                notifChanged();

            // sound: a shown arrival plays sound-file/sound-name through the
            // libcanberra player unless the client suppresses it. DND-queued
            // (waiting) arrivals stay silent; the resume doesn't replay.
            if (!n->waiting) {
                bool        suppress = false;
                std::string soundFile, soundName;
                if (const auto IT = hints.find("suppress-sound"); IT != hints.end())
                    try {
                        suppress = IT->second.get<bool>();
                    } catch (...) {}
                if (const auto IT = hints.find("sound-file"); IT != hints.end())
                    try {
                        soundFile = IT->second.get<std::string>();
                    } catch (...) {}
                if (const auto IT = hints.find("sound-name"); IT != hints.end())
                    try {
                        soundName = IT->second.get<std::string>();
                    } catch (...) {}
                if (soundFile.starts_with("file://"))
                    soundFile.erase(0, 7);
                const std::string CMD = cfg.soundCommand->value();
                if (!suppress && !CMD.empty()) {
                    if (!soundFile.empty())
                        spawnDetached({CMD.c_str(), "-f", soundFile.c_str(), nullptr});
                    else if (!soundName.empty())
                        spawnDetached({CMD.c_str(), "-i", soundName.c_str(), nullptr});
                }
            }

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
                                   return std::vector<std::string>{"actions", "action-icons", "body", "body-markup", "body-hyperlinks", "body-images", "icon-static", "persistence", "sound"};
                               }),
                               sdbus::registerMethod("GetServerInformation").withOutputParamNames("name", "vendor", "version", "spec_version").implementedAs([]() {
                                   return std::tuple<std::string, std::string, std::string, std::string>{"hyprnotify", "hitori", VERSION, "1.3"};
                               }),
                               sdbus::registerSignal("NotificationClosed").withParameters<uint32_t, uint32_t>("id", "reason"),
                               sdbus::registerSignal("ActionInvoked").withParameters<uint32_t, std::string>("id", "action_key"),
                               sdbus::registerSignal("ActivationToken").withParameters<uint32_t, std::string>("id", "activation_token"))
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
                            if (N->timeoutMs > 0 && !N->waiting && N->deadline <= NOW)
                                due.push_back(N->id);
                        for (const auto ID : due) {
                            if (const auto N = byId(ID))
                                retire(N);
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
            history.clear();
            suspended = false;
        }
    }

} // namespace NHyprnotify
