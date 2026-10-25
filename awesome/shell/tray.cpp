// awesome/shell/tray.cpp — the in-compositor StatusNotifierWatcher/Host and its items

#include "../core/activate.hpp"
#include "../core/icons.hpp" // iconDeferred: a budget-deferred icon is retried
#include "../core/raster.hpp"



#include "shell/shell.hpp"

#include <chrono>
#include <cmath>
#include <limits>
#include <unistd.h>

namespace NAwesome::Shell {

    // ---- tray: StatusNotifierWatcher + Host, in-compositor ----

    namespace Tray {
        static const sdbus::InterfaceName      WIFACE{"org.kde.StatusNotifierWatcher"};
        // SNI pixmaps are untrusted D-Bus payloads. The tray only paints a
        // small status icon, so reject malformed or pathological dimensions
        // before multiplying or allocating anything.
        constexpr int      MAX_PIXMAP_DIM    = 1024;
        constexpr uint64_t MAX_PIXMAP_PIXELS = 4ull << 20;

        // SNI ARGB (network order) -> premultiplied BGRA within cap a side.
        // An app that ships only a large pixmap (some send 512-1024 px) cost
        // a full-size premultiply, compare and upload per icon change for a
        // 16 px cell: box-averaged by an integer K while premultiplying (to
        // within 2x of the cap), then fitted exactly.
        static SPixels sniPixels(const std::vector<uint8_t>& src, int w, int h, int cap) {
            const int K  = std::max(1, std::max(w, h) / std::max(1, cap * 2));
            const int OW = std::max(1, w / K), OH = std::max(1, h / K);
            SPixels   out{.px = std::vector<uint8_t>((size_t)OW * OH * 4), .w = OW, .h = OH};
            for (int oy = 0; oy < OH; oy++)
                for (int ox = 0; ox < OW; ox++) {
                    // the box clamped to the source: a side shorter than K
                    // (a 1024x1 strip) has fewer rows than the box
                    const int Y1 = std::min(h, (oy + 1) * K), X1 = std::min(w, (ox + 1) * K);
                    uint64_t  sb = 0, sg = 0, sr = 0, sa = 0;
                    for (int y = oy * K; y < Y1; y++)
                        for (int x = ox * K; x < X1; x++) {
                            const uint8_t* P = &src[((size_t)y * w + x) * 4];
                            const uint32_t A = P[0];
                            sr += P[1] * A;
                            sg += P[2] * A;
                            sb += P[3] * A;
                            sa += A;
                        }
                    const uint64_t N = (uint64_t)(Y1 - oy * K) * (X1 - ox * K);
                    uint8_t*       D = &out.px[((size_t)oy * OW + ox) * 4];
                    D[0]             = (uint8_t)(sb / (255 * N));
                    D[1]             = (uint8_t)(sg / (255 * N));
                    D[2]             = (uint8_t)(sr / (255 * N));
                    D[3]             = (uint8_t)(sa / N);
                }
            if (out.w > cap || out.h > cap)
                return Raster::fit(Raster::view(out), cap);
            return out;
        }

        NAwesome::CBusLink                  bus; // menu.cpp borrows the connection for its dbusmenu proxies
        static std::unique_ptr<sdbus::IObject> watcher;
        static std::unique_ptr<sdbus::IProxy>  busProxy;
        static sdbus::Slot                     itemMatch, menuMatch; // the connection's item/dbusmenu signal matches (onItemSignal)
        std::vector<SP<SItem>>                 items;
        static std::unordered_map<std::string, std::string> hosts; // service -> unique owner
        static std::string                                  hostService;
        static bool                                          hostNameOwned = false;

        // bumped by every tray click: a deferred menu open answers only the
        // click that asked for it
        static uint64_t clickSerial = 0;

        constexpr size_t MAX_ITEMS            = 256;
        constexpr size_t MAX_ITEMS_PER_SERVICE = 32;

        static int32_t sniCoord(double value) {
            if (!std::isfinite(value))
                return 0;
            value = std::clamp(value, (double)std::numeric_limits<int32_t>::min(), (double)std::numeric_limits<int32_t>::max());
            return (int32_t)std::llround(value);
        }

        static void registerHost(const std::string& requested, const std::string& owner) {
            if (owner.empty())
                return;
            const std::string service = requested.empty() ? owner : requested;
            if (service.empty())
                return;
            const bool inserted = hosts.insert_or_assign(service, owner).second;
            if (inserted && watcher)
                watcher->emitSignal("StatusNotifierHostRegistered").onInterface(WIFACE);
        }

        static void dropHostName(const std::string& name, const std::string& oldOwner, const std::string& newOwner) {
            if (name.empty() || oldOwner == newOwner)
                return;
            for (auto it = hosts.begin(); it != hosts.end();) {
                const bool SERVICE_GONE = it->first == name && !oldOwner.empty() && it->second == oldOwner;
                const bool OWNER_GONE   = it->second == name && newOwner.empty();
                if (SERVICE_GONE || OWNER_GONE)
                    it = hosts.erase(it);
                else
                    ++it;
            }
        }


        // A drain must never run synchronously from a send site. Input/render
        // callers enqueue message construction on the link's idle queue;
        // signal/reply handlers can still use the non-blocking API directly,
        // and the link parks sd-bus dispatch on its normal poll timer.
        void pollSoon() {
            bus.pollSoon();
        }

        void post(std::function<void()> fn) {
            bus.post(std::move(fn));
        }

        // the click IS the activation: resolve the item's bus pid and focus
        // its own X11 window (see core/activate.hpp). Wayland apps answer
        // the Activate call with a token-validated xdg-activation of their
        // own, so only the X11 side needs the focus from our side.
        static void activateApp(SP<SItem> it) {
            if (!it || !it->active || !busProxy)
                return;
            try {
                busProxy->callMethodAsync("GetConnectionUnixProcessID")
                    .onInterface("org.freedesktop.DBus")
                    .withArguments(it->service)
                    .uponReplyInvoke([it](std::optional<sdbus::Error> e, uint32_t pid) {
                        if (e || !it->active)
                            return; // the item may have vanished over the round trip
                        NAwesome::activateAppWindow(pid);
                    });
                pollSoon();
            } catch (...) {} // dying bus: teardown is already pending
        }

        // owned objects borrow the connection — reset them before it dies
        static void dropOwnedObjects() {
            if (!Menu::isLocal)
                try {
                    Menu::close(); // its proxy borrows the connection; close it first
                } catch (...) {}   // close() sends "closed" events — the bus may already be gone
            for (auto& I : items) {
                I->active = false;
                ++I->propertyRequest;
                ++I->statusRequest;
                ++I->iconRequest;
                I->proxy.reset(); // break the item<->handler cycle before dropping the vector's ref
            }
            items.clear();
            hosts.clear();
            if (hostNameOwned && bus.conn()) {
                try {
                    bus.conn()->releaseName(sdbus::ServiceName{hostService});
                } catch (...) {}
            }
            hostService.clear();
            hostNameOwned = false;
            itemMatch     = {};
            menuMatch     = {};
            busProxy.reset();
            watcher.reset();
        }

        // An item's string as stored: a hostile item cannot make one
        // unbounded (a 10 MB IconName was copied per directory per lookup)
        static std::string boundedProp(std::string s) {
            constexpr size_t MAX_PROP_BYTES = 1024;
            if (s.size() > MAX_PROP_BYTES)
                s.clear();
            return s;
        }

        // Name and pixmap are fetched SERIALLY and committed as ONE change.
        // They arrive as separate replies, and applying each on its own gave
        // every REAL icon flip (fcitx idle <-> unikey — one per IM toggle /
        // input-context change) an intermediate frame: the new name rendered
        // from the theme file, then the item's own pixmap — a visible
        // double-blink on the tray per window action. One fetch is in flight
        // per item: a NewIcon storm (fcitx fires one per input-context
        // change) only marks the item for one more fetch when the current
        // one lands, instead of a request chain per signal.
        static void fetchIcon(SP<SItem> it);

        // One icon set: its name, then its pixmap (SET: the attention set,
        // else the plain one). ATTN is the status the chain started under.
        // An attention set with neither (most apps implement no attention
        // icon) shows the plain set: the cell went blank on NeedsAttention.
        static void fetchIconSet(SP<SItem> it, bool ATTN, bool SET, uint64_t REQUEST) {
            // the chain's end, whatever the outcome: run the one queued
            const auto done = [](const SP<SItem>& it) {
                it->iconFetching = false;
                if (std::exchange(it->iconAgain, false))
                    fetchIcon(it);
            };
            const auto stale = [ATTN, REQUEST](const SP<SItem>& it) {
                return !it->active || !it->proxy || it->iconRequest != REQUEST || (it->status == "NeedsAttention") != ATTN;
            };
            it->proxy->getPropertyAsync(SET ? "AttentionIconName" : "IconName").onInterface(SNI).uponReplyInvoke([it, ATTN, SET, REQUEST, done, stale](std::optional<sdbus::Error> eN, sdbus::Variant vN) {
                if (stale(it)) {
                    done(it);
                    return;
                }
                // an errored reply keeps the current name; the attention set
                // has none of its own to keep
                auto name = SET ? std::string{} : it->iconName;
                if (!eN)
                    try {
                        name = boundedProp(vN.get<std::string>());
                    } catch (...) {}
                it->proxy->getPropertyAsync(SET ? "AttentionIconPixmap" : "IconPixmap").onInterface(SNI).uponReplyInvoke([it, name, ATTN, SET, REQUEST, done, stale](std::optional<sdbus::Error> e, sdbus::Variant v) {
                    if (stale(it)) {
                        done(it);
                        return;
                    }
                    std::vector<uint8_t> px;
                    int                  pw = 0, ph = 0;
                    if (!e) {
                        try {
                            auto pixmaps = v.get<std::vector<sdbus::Struct<int32_t, int32_t, std::vector<uint8_t>>>>();

                            // smallest one still >= the cell's PHYSICAL size (at the
                            // densest monitor's scale: a 16 px pixmap stretched to
                            // 32 on a scale-2 panel is a blur when the app offers
                            // 32), else the biggest available
                            const int                   WANT = iconDevicePx();
                            const std::vector<uint8_t>* best = nullptr;
                            int                         bw = 0, bh = 0;
                            for (const auto& P : pixmaps) {
                                const int W = std::get<0>(P), H = std::get<1>(P);
                                const auto& DATA = std::get<2>(P);
                                const uint64_t AREA = (W > 0 && H > 0) ? (uint64_t)W * (uint64_t)H : 0;
                                if (W <= 0 || H <= 0 || W > MAX_PIXMAP_DIM || H > MAX_PIXMAP_DIM || AREA > MAX_PIXMAP_PIXELS || DATA.size() != AREA * 4)
                                    continue;
                                if (!best || (bw < WANT && W > bw) || (W >= WANT && bw >= WANT && W < bw) || (W >= WANT && bw < WANT)) {
                                    best = &std::get<2>(P);
                                    bw   = W;
                                    bh   = H;
                                }
                            }

                            if (best) {
                                // SNI pixmaps are ARGB32 in network byte order (A,R,G,B);
                                // DRM_FORMAT_ARGB8888 wants B,G,R,A, premultiplied
                                auto P = sniPixels(*best, bw, bh, WANT); // box-filtered to the cell: drawn 1:1, never GL-shrunk
                                px     = std::move(P.px);
                                pw     = P.w;
                                ph     = P.h;
                            }
                        } catch (...) {}
                    }
                    if (SET && name.empty() && px.empty()) {
                        fetchIconSet(it, ATTN, false, REQUEST); // the chain goes on: its end runs done
                        return;
                    }
                    done(it);
                    if (it->iconRequest != REQUEST)
                        return; // done started the queued fetch: it commits instead
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

        static void fetchIcon(SP<SItem> it) {
            if (!it || !it->active || !it->proxy)
                return;
            if (it->iconFetching) {
                it->iconAgain = true;
                return;
            }
            it->iconFetching = true;
            // a NeedsAttention item shows its attention icon set (SNI spec)
            const bool ATTN = it->status == "NeedsAttention";
            fetchIconSet(it, ATTN, ATTN, ++it->iconRequest);
        }

        // The theme path the item's icon names resolve in. NewIconThemePath
        // re-reads it, and the icon with it (an app moving its theme dir
        // renames into it too). One read in flight, like fetchIcon: a
        // signal storm queues one more, not a request per signal.
        static void fetchThemePath(SP<SItem> it, bool thenIcon) {
            if (it->themeFetching) {
                it->themeAgain = true;
                return;
            }
            it->themeFetching      = true;
            const uint64_t REQUEST = it->propertyRequest;
            it->proxy->getPropertyAsync("IconThemePath").onInterface(SNI).uponReplyInvoke([it, REQUEST, thenIcon](std::optional<sdbus::Error> e, sdbus::Variant v) {
                it->themeFetching = false;
                const bool AGAIN  = std::exchange(it->themeAgain, false);
                if (!it->active || !it->proxy)
                    return;
                if (AGAIN) { // the newer read answers, the icon after it
                    fetchThemePath(it, true);
                    return;
                }
                if (it->propertyRequest != REQUEST)
                    return;
                if (!e)
                    try {
                        const auto THEME = boundedProp(v.get<std::string>());
                        if (THEME != it->themePath) {
                            it->themePath = THEME;
                            it->dirty     = true;
                            barChanged();
                        }
                    } catch (...) {}
                if (thenIcon)
                    fetchIcon(it);
            });
            pollSoon();
        }

        // The dbusmenu path. An app may export its menu after registering
        // (the property has no change signal but KDE's NewMenu), so a click
        // that finds none reads it again before giving up on a menu: `then`
        // runs on the reply, whatever it held. Unprompted reads (NewMenu,
        // the first props) fold like the theme path; a click's goes out.
        static void fetchMenuPath(SP<SItem> it, std::function<void()> then) {
            const bool FOLDS = !then;
            if (FOLDS) {
                if (it->menuFetching) {
                    it->menuAgain = true;
                    return;
                }
                it->menuFetching = true;
            }
            const uint64_t REQUEST = it->propertyRequest;
            it->proxy->getPropertyAsync("Menu").onInterface(SNI).uponReplyInvoke([it, REQUEST, FOLDS, then = std::move(then)](std::optional<sdbus::Error> e, sdbus::Variant v) {
                bool again = false;
                if (FOLDS) {
                    it->menuFetching = false;
                    again            = std::exchange(it->menuAgain, false);
                }
                if (!it->active || !it->proxy)
                    return;
                if (again) {
                    fetchMenuPath(it, nullptr);
                    return;
                }
                if (it->propertyRequest != REQUEST)
                    return;
                if (!e)
                    try {
                        // "/" and KDE's "/NO_DBUSMENU" mean "no menu": the item
                        // then gets ContextMenu on a right click, not a panel
                        // stuck on its first layout call
                        const std::string PATH = v.get<sdbus::ObjectPath>();
                        it->menuPath           = PATH == "/" || PATH == "/NO_DBUSMENU" ? "" : PATH;
                    } catch (...) {}
                if (then)
                    then();
            });
            pollSoon();
        }

        static void fetchProps(SP<SItem> it) {
            if (!it || !it->active || !it->proxy)
                return;
            const uint64_t REQUEST = ++it->propertyRequest;
            const uint64_t STATUS_REQUEST = it->statusRequest;
            it->proxy->getPropertyAsync("Id").onInterface(SNI).uponReplyInvoke([it, REQUEST](std::optional<sdbus::Error> e, sdbus::Variant v) {
                if (!it->active || !it->proxy || it->propertyRequest != REQUEST || e)
                    return;
                try {
                    const auto ID = boundedProp(v.get<std::string>());
                    if (ID != it->id) {
                        it->id    = ID;
                        it->dirty = true;
                        barChanged();
                    }
                } catch (...) {}
            });
            // Every reply is change-detected: fcitx fires NewIcon on every input
            // context change (= every window focus), and rebuilding textures +
            // redrawing the bar for identical content made the bar flicker like
            // it was reloading during any window action.
            fetchThemePath(it, false);
            fetchMenuPath(it, nullptr);
            it->proxy->getPropertyAsync("ItemIsMenu").onInterface(SNI).uponReplyInvoke([it, REQUEST](std::optional<sdbus::Error> e, sdbus::Variant v) {
                if (!it->active || !it->proxy || it->propertyRequest != REQUEST)
                    return;
                if (!e)
                    try {
                        it->itemIsMenu = v.get<bool>();
                    } catch (...) {}
            });
            // Status decides which icon set to read, so the icon chain hangs
            // off its reply.
            it->proxy->getPropertyAsync("Status").onInterface(SNI).uponReplyInvoke([it, REQUEST, STATUS_REQUEST](std::optional<sdbus::Error> e, sdbus::Variant v) {
                if (!it->active || !it->proxy || it->propertyRequest != REQUEST || it->statusRequest != STATUS_REQUEST)
                    return;
                std::string st = it->status; // an errored reply keeps the current status
                if (!e)
                    try {
                        st = boundedProp(v.get<std::string>());
                    } catch (...) {}
                if (st != it->status) {
                    it->status = st;
                    barChanged(); // Passive items vanish from the strip
                }
                fetchIcon(it);
            });
            pollSoon();
        }

        // The item signals, from every client, through ONE match installed
        // asynchronously at init: a per-item uponSignal is a synchronous
        // AddMatch round trip on the compositor thread (three per item, in
        // the RegisterStatusNotifierItem handler), and a stalled broker
        // froze the compositor for sd-bus's timeout. Routed by the sender's
        // unique name and the item's path.
        static void onItemSignal(sdbus::Message& m) {
            const char* SENDER = m.getSender();
            const char* PATH   = m.getPath();
            const char* MEMBER = m.getMemberName();
            if (!SENDER || !PATH || !MEMBER)
                return;
            for (const auto& I : items) {
                if (!I->active || I->owner != SENDER || I->path != PATH)
                    continue;
                const std::string_view NAME{MEMBER};
                if (NAME == "NewIcon" || NAME == "NewAttentionIcon") // NewIcon changes only the icon (SNI); Menu/Status don't
                    fetchIcon(I);
                else if (NAME == "NewIconThemePath")
                    fetchThemePath(I, true);
                else if (NAME == "NewMenu")
                    fetchMenuPath(I, nullptr);
                else if (NAME == "NewStatus") {
                    std::string st;
                    try {
                        m >> st;
                    } catch (...) { return; }
                    st = boundedProp(std::move(st));
                    ++I->statusRequest; // invalidate only the initial Status reply
                    if (st != I->status) {
                        I->status = st;
                        barChanged(); // Passive <-> shown, NeedsAttention swaps the icon set
                    }
                    fetchIcon(I);
                }
                return;
            }
        }

        static void addItem(const std::string& service, const std::string& path, const std::string& owner) {
            if (service.empty() || path.empty() || items.size() >= MAX_ITEMS)
                return;
            for (const auto& I : items)
                if (I->service == service && I->path == path)
                    return;
            if (std::ranges::count_if(items, [&](const auto& I) { return I->service == service; }) >= (int)MAX_ITEMS_PER_SERVICE)
                return;

            auto it     = makeShared<SItem>();
            it->service = service;
            it->path    = path;
            it->owner   = owner;
            // no per-proxy signal handlers: the item's signals ride the
            // connection's one match (onItemSignal)
            it->proxy = sdbus::createProxy(*bus.conn(), sdbus::ServiceName{service}, sdbus::ObjectPath{path});
            fetchProps(it);
            items.push_back(it);
            watcher->emitSignal("StatusNotifierItemRegistered").onInterface(WIFACE).withArguments(service + path);
            barChanged();
        }

        static void dropService(const std::string& service) {
            const auto               BEFORE = items.size();
            std::vector<std::string> gone; // service+path, as Registered announced them
            std::erase_if(items, [&](const auto& I) {
                if (I->service != service)
                    return false;
                gone.push_back(I->service + I->path);
                I->active = false;
                ++I->propertyRequest;
                ++I->statusRequest;
                ++I->iconRequest;
                I->proxy.reset(); // the signal handlers hold a strong ref back to the item; drop them or it (and its texture) never frees
                return true;
            });
            if (items.size() != BEFORE) {
                onServiceDropped(service);
                if (watcher)
                    for (const auto& G : gone)
                        try {
                            watcher->emitSignal("StatusNotifierItemUnregistered").onInterface(WIFACE).withArguments(G);
                        } catch (...) {}
                barChanged();
            }
        }

        void init() {
            bus.onLost = [](const std::string& err) {
                HyprlandAPI::addNotification(NAwesome::supervisor().handle(), "[awesome] tray bus lost, tray disabled: " + err, CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
            };
            bus.dropOwned     = []() { dropOwnedObjects(); };
            bus.afterTeardown = []() { barChanged(); }; // the dead items just left the strip
            try {
                bus.open(false, "org.kde.StatusNotifierWatcher");
                watcher = sdbus::createObject(*bus.conn(), sdbus::ObjectPath{"/StatusNotifierWatcher"});

                watcher
                    ->addVTable(sdbus::registerMethod("RegisterStatusNotifierItem").withInputParamNames("service").implementedAs([](std::string arg) {
                        const std::string SENDER = watcher->getCurrentlyProcessedMessage().getSender();
                        if (!arg.empty() && arg.front() == '/') {
                            addItem(SENDER, arg, SENDER);
                            return;
                        }
                        if (arg.empty() || arg == SENDER) {
                            addItem(SENDER, "/StatusNotifierItem", SENDER);
                            return;
                        }
                        // a well-known name nobody owns would never leave:
                        // its NameOwnerChanged never comes, and the cell
                        // stays a blank click target for the session
                        if (!busProxy)
                            return;
                        try {
                            busProxy->callMethodAsync("GetNameOwner")
                                .onInterface("org.freedesktop.DBus")
                                .withArguments(arg)
                                .uponReplyInvoke([arg](std::optional<sdbus::Error> e, std::string owner) {
                                    if (!e && !owner.empty())
                                        addItem(arg, "/StatusNotifierItem", owner);
                                });
                            pollSoon();
                        } catch (...) {}
                    }),
                                sdbus::registerMethod("RegisterStatusNotifierHost").withInputParamNames("service").implementedAs([](std::string service) {
                                    if (!watcher)
                                        return;
                                    const std::string SENDER = watcher->getCurrentlyProcessedMessage().getSender();
                                    if (service.empty() || service == SENDER) {
                                        registerHost(service, SENDER);
                                        return;
                                    }
                                    // The argument is a well-known host name.
                                    // Confirm that the caller owns it before it
                                    // affects the host-presence property.
                                    if (!busProxy)
                                        return;
                                    try {
                                        busProxy->callMethodAsync("GetNameOwner")
                                            .onInterface("org.freedesktop.DBus")
                                            .withArguments(service)
                                            .uponReplyInvoke([service, SENDER](std::optional<sdbus::Error> e, std::string owner) {
                                                if (!e && owner == SENDER)
                                                    registerHost(service, SENDER);
                                            });
                                        pollSoon();
                                    } catch (...) {}
                                }),
                                sdbus::registerProperty("RegisteredStatusNotifierItems").withGetter([]() {
                                    std::vector<std::string> v;
                                    for (const auto& I : items)
                                        v.push_back(I->service + I->path);
                                    return v;
                                }),
                                sdbus::registerProperty("IsStatusNotifierHostRegistered").withGetter([]() { return !hosts.empty(); }),
                                sdbus::registerProperty("ProtocolVersion").withGetter([]() { return (int32_t)0; }),
                                sdbus::registerSignal("StatusNotifierItemRegistered").withParameters<std::string>("service"),
                                sdbus::registerSignal("StatusNotifierItemUnregistered").withParameters<std::string>("service"),
                                sdbus::registerSignal("StatusNotifierHostRegistered"))
                    .forInterface(WIFACE);

                busProxy = sdbus::createProxy(*bus.conn(), sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"});
                busProxy->uponSignal("NameOwnerChanged").onInterface("org.freedesktop.DBus").call([](std::string name, std::string oldOwner, std::string newOwner) {
                    // A service restart may replace its unique owner without
                    // first losing the well-known name. Treat both that case
                    // and a disappearance as a new item owner.
                    if (!oldOwner.empty() && oldOwner != newOwner)
                        dropService(name);
                    dropHostName(name, oldOwner, newOwner);
                });

                itemMatch = bus.conn()->addMatchAsync("type='signal',interface='org.kde.StatusNotifierItem'", [](sdbus::Message m) { onItemSignal(m); },
                                                      [](sdbus::Message) {}, sdbus::return_slot);
                menuMatch = bus.conn()->addMatchAsync("type='signal',interface='com.canonical.dbusmenu'", [](sdbus::Message m) { onMenuSignal(m); },
                                                      [](sdbus::Message) {}, sdbus::return_slot);

                hostService = "org.freedesktop.StatusNotifierHost-" + std::to_string((long long)getpid());
                bus.conn()->requestName(sdbus::ServiceName{hostService});
                hostNameOwned = true;
                registerHost(hostService, bus.conn()->getUniqueName());

                bus.sync(); // drain anything queued during setup — the vtable is registered, nothing dispatches early
            } catch (const std::exception& E) {
                HyprlandAPI::addNotification(NAwesome::supervisor().handle(), std::string{"[awesome] tray disabled: "} + E.what(), CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
                dropOwnedObjects();
                bus.close();
            }
        }

        void exit() {
            bus.close(); // fd sources out BEFORE the connection dies
        }
    }

    // ---- the widget: the icon strip cells ----

    namespace {
        class CTrayWidget : public IWidget {
          public:
            static constexpr double ICON = ICON_CELL;

            double fit(const SPaint& P, const SFrame&) override {
                const double SPACING = std::max(0.0, (double)cfg().getI("plugin:awesome:shell:tray_spacing"));
                int n = 0;
                for (const auto& IT : Tray::items)
                    if (IT->status != "Passive")
                        n++;
                return n ? n * ICON + (n - 1) * SPACING : 0;
            }

            void draw(const SPaint& P, const SFrame&, const CBox& box) override {
                // laid from the right edge inwards, spaced like awesome's
                // systray_icon_spacing — the first item keeps the edge
                const double SPACING = std::max(0.0, (double)cfg().getI("plugin:awesome:shell:tray_spacing"));
                double       right   = box.x + box.w;
                bool   first = true;
                for (const auto& IT : Tray::items) {
                    if (IT->status == "Passive")
                        continue; // SNI: Passive means don't show the item
                    if (!first)
                        right -= SPACING;
                    first = false;
                    // The right-side slot can be clipped on a narrow output.
                    // Keep the cell and hitbox wholly inside the width the
                    // layout assigned to this widget; an SNI item must never
                    // paint over the tasklist or another right-side widget.
                    if (right - ICON < box.x)
                        break;
                    // The pixmap is a texture too, so the rule applies: rebuild it on
                    // the warm only. A dirty item reaching a draw keeps its old icon
                    // for this frame and asks for a repaint.
                    if (IT->dirty && warmGate().mayBuild()) {
                        IT->dirty = false;
                        IT->tex.reset();
                        if (!IT->pixels.empty())
                            IT->tex = g_pHyprRenderer->createTexture(DRM_FORMAT_ARGB8888, IT->pixels.data(), IT->pw * 4, Vector2D{(double)IT->pw, (double)IT->ph});
                        if ((!IT->tex || IT->tex->m_texID == 0) && (!IT->iconName.empty() || !IT->id.empty())) {
                            IT->tex   = trayIcon(IT->iconName, IT->themePath, IT->id);
                            IT->dirty = !IT->tex && NAwesome::iconDeferred(); // budget-deferred: the retry warm tries again
                        }
                    }

                    const CBox ICONBOX{right - ICON, box.y + (P.h - ICON) / 2, ICON, ICON};
                    if (IT->tex && IT->tex->m_texID != 0)
                        // fit, not stretch: a non-square pixmap keeps its proportions
                        P.texFit(IT->tex, ICONBOX);
                    // the hit spans the bar's height and half of each gap
                    SHit h;
                    h.box     = CBox{ICONBOX.x - SPACING / 2, box.y, ICON + SPACING, P.h};
                    h.widget  = this;
                    h.tray    = IT;
                    h.anchorX = ICONBOX.x + ICON / 2.0;
                    h.mon     = P.mon;
                    P.hits->push_back(h);
                    right -= ICON;
                }
            }

            void onHit(const SHit& h, uint32_t bit, bool) override {
                const auto IT = h.tray.lock();
                if (!IT || !IT->proxy)
                    return;
                const uint64_t CLICK = ++Tray::clickSerial;
                const int32_t X = Tray::sniCoord(h.clickX), Y = Tray::sniCoord(h.clickY);
                if (bit == 4u) { // middle: the SNI SecondaryActivate call
                    Tray::post([IT, X, Y]() {
                        if (!IT->active || !IT->proxy)
                            return;
                        try {
                            IT->proxy->callMethodAsync("SecondaryActivate")
                                .onInterface(Tray::SNI)
                                .withArguments(X, Y)
                                .uponReplyInvoke([](std::optional<sdbus::Error>) {});
                            Tray::pollSoon();
                        } catch (...) {} // dying bus: teardown is already pending
                    });
                    return;
                }
                // ItemIsMenu: the item has no activation of its own, a left
                // click shows its menu too (SNI spec)
                if (bit == 1u && !IT->itemIsMenu) {
                    Tray::post([IT, X, Y]() {
                        if (!IT->active || !IT->proxy)
                            return;
                        try {
                            IT->proxy->callMethodAsync("Activate")
                                .onInterface(Tray::SNI)
                                .withArguments(X, Y)
                                .uponReplyInvoke([](std::optional<sdbus::Error>) {});
                            Tray::pollSoon(); // the activation usually flips the icon right back
                        } catch (...) {} // dying bus: teardown is already pending
                        // X11 apps cannot authenticate this gesture (their
                        // SetForegroundWindow arrives as an urgency ping), so
                        // focus the app's own window here; Wayland apps
                        // self-activate through xdg-activation.
                        activateApp(IT);
                    });
                    return;
                }
                if (!IT->menuPath.empty()) {
                    Menu::openFor(IT, h.anchorX, h.mon);
                    return;
                }
                if (bit != 1u && bit != 2u)
                    return;
                // no menu path yet: read it again (an app may export its menu
                // after registering), then the menu or the item's own
                // ContextMenu — for this click only: not after another click
                // or with a menu open since (a second click opened twice, a
                // slow reply closed the menu the user had moved on to), and
                // not a second after it (sd-bus waits up to 25 s)
                Tray::post([IT, X, Y, AX = h.anchorX, MON = h.mon, CLICK, AT = std::chrono::steady_clock::now()]() {
                    if (!IT->active || !IT->proxy)
                        return;
                    try {
                        Tray::fetchMenuPath(IT, [IT, X, Y, AX, MON, CLICK, AT]() {
                            if (NAwesome::sessionLocked() || CLICK != Tray::clickSerial || Menu::isOpen ||
                                std::chrono::steady_clock::now() - AT > std::chrono::seconds(1))
                                return;
                            if (!IT->menuPath.empty()) {
                                Menu::openFor(IT, AX, MON);
                                return;
                            }
                            try {
                                IT->proxy->callMethodAsync("ContextMenu")
                                    .onInterface(Tray::SNI)
                                    .withArguments(X, Y)
                                    .uponReplyInvoke([](std::optional<sdbus::Error>) {});
                                Tray::pollSoon();
                            } catch (...) {}
                        });
                    } catch (...) {} // dying bus: teardown is already pending
                });
            }

            void onScroll(const SHit& h, int steps) override {
                // the SNI Scroll call (the XEmbed systray let apps see scroll too),
                // one wheel detent = 120 as KDE's host sends it
                if (const auto TI = h.tray.lock(); TI && TI->proxy) {
                    Tray::post([TI, steps]() {
                        if (!TI->active || !TI->proxy)
                            return;
                        try {
                            TI->proxy->callMethodAsync("Scroll")
                                .onInterface(Tray::SNI)
                                .withArguments((int32_t)(-120 * steps), std::string{"vertical"})
                                .uponReplyInvoke([](std::optional<sdbus::Error>) {});
                            Tray::pollSoon();
                        } catch (...) {} // dying bus: teardown is already pending
                    });
                }
            }
        };
    } // namespace

    IWidget& trayWidget() {
        static CTrayWidget W;
        return W;
    }

} // namespace NAwesome::Shell
