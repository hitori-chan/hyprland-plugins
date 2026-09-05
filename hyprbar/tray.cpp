// hyprbar/tray.cpp — the in-compositor StatusNotifierWatcher/Host and its items

#include "common/lifecycle.hpp"

#include "hyprbar.hpp"

#include <cmath>
#include <limits>
#include <unistd.h>

namespace NHyprbar {

    // ---- tray: StatusNotifierWatcher + Host, in-compositor ----

    namespace Tray {
        static const sdbus::InterfaceName      WIFACE{"org.kde.StatusNotifierWatcher"};
        // SNI pixmaps are untrusted D-Bus payloads. The tray only paints a
        // small status icon, so reject malformed or pathological dimensions
        // before multiplying or allocating anything.
        constexpr int      MAX_PIXMAP_DIM    = 1024;
        constexpr uint64_t MAX_PIXMAP_PIXELS = 4ull << 20;

        NHyprCommon::CBusLink                  bus; // menu.cpp borrows the connection for its dbusmenu proxies
        static std::unique_ptr<sdbus::IObject> watcher;
        static std::unique_ptr<sdbus::IProxy>  busProxy;
        static std::unique_ptr<sdbus::IProxy>  notifyProxy; // the battery alerts' Notify sender
        std::vector<SP<SItem>>                 items;
        static std::unordered_map<std::string, std::string> hosts; // service -> unique owner
        static std::string                                  hostService;
        static bool                                          hostNameOwned = false;

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


        // One Notify onto the session bus, over the tray's live connection —
        // no fork, no shell. hyprnotify's API is the bus name, never its
        // symbols (two independently-versioned .so files must not couple);
        // whatever daemon owns the name receives it.
        void notify(const std::string& app, uint32_t replacesId, const std::string& icon, const std::string& summary, const std::string& body, uint8_t urgency, int32_t timeoutMs, bool osd) {
            if (!bus.conn())
                return;
            try {
                if (!notifyProxy)
                    notifyProxy = sdbus::createProxy(*bus.conn(), sdbus::ServiceName{"org.freedesktop.Notifications"}, sdbus::ObjectPath{"/org/freedesktop/Notifications"});
                std::map<std::string, sdbus::Variant> hints{{"urgency", sdbus::Variant{urgency}}};
                if (osd)
                    hints.emplace("x-hyprnotify-osd", sdbus::Variant{true});
                notifyProxy->callMethodAsync("Notify")
                    .onInterface("org.freedesktop.Notifications")
                    .withArguments(app, replacesId, icon, summary, body, std::vector<std::string>{}, hints,
                                   timeoutMs > 0 ? timeoutMs : -1)
                    .uponReplyInvoke([](std::optional<sdbus::Error>, uint32_t) {});
                pollSoon();  // flush the send from the event loop, never from here
            } catch (...) {} // broker gone: teardown is already pending, drop the card
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

        // owned objects borrow the connection — reset them before it dies
        static void dropOwnedObjects() {
            Bell::exit(); // the bell's proxy borrows this connection — drop it first
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
            notifyProxy.reset();
            busProxy.reset();
            watcher.reset();
        }

        // Name and pixmap are fetched SERIALLY and committed as ONE change.
        // They arrive as separate replies, and applying each on its own gave
        // every REAL icon flip (fcitx idle <-> unikey — one per IM toggle /
        // input-context change) an intermediate frame: the new name rendered
        // from the theme file, then the item's own pixmap — a visible
        // double-blink on the tray per window action.
        static void fetchIcon(SP<SItem> it) {
            if (!it || !it->active || !it->proxy)
                return;
            // a NeedsAttention item shows its attention icon set (SNI spec)
            const bool     ATTN    = it->status == "NeedsAttention";
            const char*    PNAME   = ATTN ? "AttentionIconName" : "IconName";
            const char*    PPIX    = ATTN ? "AttentionIconPixmap" : "IconPixmap";
            const uint64_t REQUEST = ++it->iconRequest;
            it->proxy->getPropertyAsync(PNAME).onInterface(SNI).uponReplyInvoke([it, PPIX, ATTN, REQUEST](std::optional<sdbus::Error> eN, sdbus::Variant vN) {
                if (!it->active || !it->proxy || it->iconRequest != REQUEST || (it->status == "NeedsAttention") != ATTN)
                    return;
                auto name = it->iconName; // an errored reply keeps the current name
                if (!eN)
                    try {
                        name = vN.get<std::string>();
                    } catch (...) {}
                it->proxy->getPropertyAsync(PPIX).onInterface(SNI).uponReplyInvoke([it, name, ATTN, REQUEST](std::optional<sdbus::Error> e, sdbus::Variant v) {
                    if (!it->active || !it->proxy || it->iconRequest != REQUEST || (it->status == "NeedsAttention") != ATTN)
                        return;
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
                                const auto& DATA = std::get<2>(P);
                                const uint64_t AREA = (W > 0 && H > 0) ? (uint64_t)W * (uint64_t)H : 0;
                                if (W <= 0 || H <= 0 || W > MAX_PIXMAP_DIM || H > MAX_PIXMAP_DIM || AREA > MAX_PIXMAP_PIXELS || DATA.size() != AREA * 4)
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
            if (!it || !it->active || !it->proxy)
                return;
            const uint64_t REQUEST = ++it->propertyRequest;
            const uint64_t STATUS_REQUEST = it->statusRequest;
            it->proxy->getPropertyAsync("Id").onInterface(SNI).uponReplyInvoke([it, REQUEST](std::optional<sdbus::Error> e, sdbus::Variant v) {
                if (!it->active || !it->proxy || it->propertyRequest != REQUEST || e)
                    return;
                try {
                    const auto ID = v.get<std::string>();
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
            it->proxy->getPropertyAsync("IconThemePath").onInterface(SNI).uponReplyInvoke([it, REQUEST](std::optional<sdbus::Error> e, sdbus::Variant v) {
                if (!it->active || !it->proxy || it->propertyRequest != REQUEST)
                    return;
                if (!e)
                    try {
                        const auto THEME = v.get<std::string>();
                        if (THEME != it->themePath) {
                            it->themePath = THEME;
                            it->dirty = true;
                            barChanged();
                        }
                    } catch (...) {}
            });
            it->proxy->getPropertyAsync("Menu").onInterface(SNI).uponReplyInvoke([it, REQUEST](std::optional<sdbus::Error> e, sdbus::Variant v) {
                if (!it->active || !it->proxy || it->propertyRequest != REQUEST)
                    return;
                if (!e)
                    try {
                        it->menuPath = v.get<sdbus::ObjectPath>();
                    } catch (...) {}
            });
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
            it->proxy   = sdbus::createProxy(*bus.conn(), sdbus::ServiceName{service}, sdbus::ObjectPath{path});
            it->proxy->uponSignal("NewIcon").onInterface(SNI).call([it]() { fetchIcon(it); }); // NewIcon changes only the icon (SNI); Menu/Status don't
            it->proxy->uponSignal("NewAttentionIcon").onInterface(SNI).call([it]() { fetchIcon(it); });
            it->proxy->uponSignal("NewStatus").onInterface(SNI).call([it](std::string st) {
                if (!it->active || !it->proxy)
                    return;
                ++it->statusRequest; // invalidate only the initial Status reply
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
            std::erase_if(items, [&](const auto& I) {
                if (I->service != service)
                    return false;
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
                    watcher->emitSignal("StatusNotifierItemUnregistered").onInterface(WIFACE).withArguments(service);
                barChanged();
            }
        }

        void init() {
            bus.onLost = [](const std::string& err) {
                HyprlandAPI::addNotification(PHANDLE, "[hyprbar] tray bus lost, tray disabled: " + err, CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
            };
            bus.dropOwned     = []() { dropOwnedObjects(); };
            bus.afterTeardown = []() { barChanged(); }; // the dead items just left the strip
            try {
                bus.open(false, "org.kde.StatusNotifierWatcher");
                watcher = sdbus::createObject(*bus.conn(), sdbus::ObjectPath{"/StatusNotifierWatcher"});

                watcher
                    ->addVTable(sdbus::registerMethod("RegisterStatusNotifierItem").withInputParamNames("service").implementedAs([](std::string arg) {
                        const std::string SENDER = watcher->getCurrentlyProcessedMessage().getSender();
                        if (!arg.empty() && arg.front() == '/')
                            addItem(SENDER, arg);
                        else
                            addItem(arg.empty() ? SENDER : arg, "/StatusNotifierItem");
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
                    if (name == "org.freedesktop.Notifications" && oldOwner.empty() && !newOwner.empty())
                        Bell::daemonUp(); // hyprnotify (re)appeared: re-read the badge counts
                });

                hostService = "org.freedesktop.StatusNotifierHost-" + std::to_string((long long)getpid());
                bus.conn()->requestName(sdbus::ServiceName{hostService});
                hostNameOwned = true;
                registerHost(hostService, bus.conn()->getUniqueName());

                bus.sync(); // drain anything queued during setup — the vtable is registered, nothing dispatches early
            } catch (const std::exception& E) {
                HyprlandAPI::addNotification(PHANDLE, std::string{"[hyprbar] tray disabled: "} + E.what(), CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
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
            double fit(const SPaint& P, const SFrame&) override {
                const double SPACING = std::max(0.0, (double)cfg.traySpacing->value());
                int n = 0;
                for (const auto& IT : Tray::items)
                    if (IT->status != "Passive")
                        n++;
                return n ? n * P.h + (n - 1) * SPACING : 0;
            }

            void draw(const SPaint& P, const SFrame&, const CBox& box) override {
                // laid from the right edge inwards, spaced like awesome's
                // systray_icon_spacing — the first item keeps the edge
                const double SPACING = std::max(0.0, (double)cfg.traySpacing->value());
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
                    if (right - P.h < box.x)
                        break;
                    // The pixmap is a texture too, so the rule applies: rebuild it on
                    // the warm only. A dirty item reaching a draw keeps its old icon
                    // for this frame and asks for a repaint.
                    if (IT->dirty && warmGate.mayBuild()) {
                        IT->dirty = false;
                        IT->tex.reset();
                        if (!IT->pixels.empty())
                            IT->tex = g_pHyprRenderer->createTexture(DRM_FORMAT_ARGB8888, IT->pixels.data(), IT->pw * 4, Vector2D{(double)IT->pw, (double)IT->ph});
                        if ((!IT->tex || IT->tex->m_texID == 0) && (!IT->iconName.empty() || !IT->id.empty()))
                            IT->tex = trayIcon(IT->iconName, IT->themePath, IT->id);
                    }

                    const CBox CELL{right - P.h, box.y, P.h, P.h};
                    if (IT->tex && IT->tex->m_texID != 0)
                        // 3px inset: SNI pixmaps lack the internal padding XEmbed
                        // icons carried, full-bleed reads as cramped. Fit, not
                        // stretch — a non-square pixmap keeps its proportions.
                        P.texFit(IT->tex, CBox{CELL.x + 3, CELL.y + 3, P.h - 6, P.h - 6});
                    SHit h;
                    h.box     = CELL;
                    h.widget  = this;
                    h.tray    = IT;
                    h.anchorX = CELL.x + P.h / 2.0;
                    h.mon     = P.mon;
                    P.hits->push_back(h);
                    right -= P.h;
                }
            }

            void onHit(const SHit& h, uint32_t bit, bool) override {
                const auto IT = h.tray.lock();
                if (!IT || !IT->proxy)
                    return;
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
                const bool HASMENU = !IT->menuPath.empty();
                if (bit == 1u && !(IT->itemIsMenu && HASMENU)) {
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
                    });
                    return;
                }
                if (HASMENU)
                    Menu::openFor(IT, h.anchorX, h.mon);
                else if (bit == 2u) {
                    Tray::post([IT, X, Y]() {
                        if (!IT->active || !IT->proxy)
                            return;
                        try {
                            IT->proxy->callMethodAsync("ContextMenu")
                                .onInterface(Tray::SNI)
                                .withArguments(X, Y)
                                .uponReplyInvoke([](std::optional<sdbus::Error>) {});
                            Tray::pollSoon();
                        } catch (...) {}
                    });
                }
            }

            void onScroll(const SHit& h, int dir) override {
                // the SNI Scroll call (the XEmbed systray let apps see scroll too)
                if (const auto TI = h.tray.lock(); TI && TI->proxy) {
                    Tray::post([TI, dir]() {
                        if (!TI->active || !TI->proxy)
                            return;
                        try {
                            TI->proxy->callMethodAsync("Scroll")
                                .onInterface(Tray::SNI)
                                .withArguments((int32_t)(dir > 0 ? -120 : 120), std::string{"vertical"})
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

} // namespace NHyprbar
