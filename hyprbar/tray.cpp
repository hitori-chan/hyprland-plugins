// hyprbar/tray.cpp — the in-compositor StatusNotifierWatcher/Host and its items

#include "common/lifecycle.hpp"

#include "hyprbar.hpp"

namespace NHyprbar {

    // ---- tray: StatusNotifierWatcher + Host, in-compositor ----

    namespace Tray {
        static const sdbus::InterfaceName      WIFACE{"org.kde.StatusNotifierWatcher"};

        NHyprCommon::CBusLink                  bus; // menu.cpp borrows the connection for its dbusmenu proxies
        static std::unique_ptr<sdbus::IObject> watcher;
        static std::unique_ptr<sdbus::IProxy>  busProxy;
        static std::unique_ptr<sdbus::IProxy>  notifyProxy; // the battery alerts' Notify sender
        std::vector<SP<SItem>>                 items;


        // One Notify onto the session bus, over the tray's live connection —
        // no fork, no shell. hyprnotify's API is the bus name, never its
        // symbols (two independently-versioned .so files must not couple);
        // whatever daemon owns the name receives it.
        void notify(const std::string& app, uint32_t replacesId, const std::string& icon, const std::string& summary, const std::string& body, uint8_t urgency, int32_t timeoutMs) {
            if (!bus.conn())
                return;
            try {
                if (!notifyProxy)
                    notifyProxy = sdbus::createProxy(*bus.conn(), sdbus::ServiceName{"org.freedesktop.Notifications"}, sdbus::ObjectPath{"/org/freedesktop/Notifications"});
                notifyProxy->callMethodAsync("Notify")
                    .onInterface("org.freedesktop.Notifications")
                    .withArguments(app, replacesId, icon, summary, body, std::vector<std::string>{}, std::map<std::string, sdbus::Variant>{{"urgency", sdbus::Variant{urgency}}},
                                   timeoutMs > 0 ? timeoutMs : -1)
                    .uponReplyInvoke([](std::optional<sdbus::Error>, uint32_t) {});
                pollSoon();  // flush the send from the event loop, never from here
            } catch (...) {} // broker gone: teardown is already pending, drop the card
        }

        // A drain must never run synchronously from a send site: sends happen
        // inside signal/reply handlers, i.e. inside processPendingEvent, and
        // sd-bus dispatch is not re-entrant — park a near tick on the link's
        // timer instead.
        void pollSoon() {
            bus.pollSoon();
        }

        // owned objects borrow the connection — reset them before it dies
        static void dropOwnedObjects() {
            try {
                Menu::close(); // its proxy borrows the connection; close it first
            } catch (...) {}   // close() sends "closed" events — the bus may already be gone
            Bell::exit();          // the bell's proxy borrows this connection too
            for (auto& I : items)
                I->proxy.reset(); // break the item<->handler cycle before dropping the vector's ref
            items.clear();
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
            it->proxy   = sdbus::createProxy(*bus.conn(), sdbus::ServiceName{service}, sdbus::ObjectPath{path});
            it->proxy->uponSignal("NewIcon").onInterface(SNI).call([it]() { fetchIcon(it); }); // NewIcon changes only the icon (SNI); Menu/Status don't
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
            std::erase_if(items, [&](const auto& I) {
                if (I->service != service)
                    return false;
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

                busProxy = sdbus::createProxy(*bus.conn(), sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"});
                busProxy->uponSignal("NameOwnerChanged").onInterface("org.freedesktop.DBus").call([](std::string name, std::string oldOwner, std::string newOwner) {
                    if (!oldOwner.empty() && newOwner.empty())
                        dropService(name);
                    // hyprnotify loads AFTER the bar: refresh the bell's badge
                    // snapshot whenever the daemon (re)appears
                    if (name == "org.freedesktop.Notifications" && !newOwner.empty())
                        Bell::daemonUp();
                });

                watcher->emitSignal("StatusNotifierHostRegistered").onInterface(WIFACE);

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
            // the spec's cells: 24 x 24 hit targets, 15px icons, gap 3
            static constexpr double CELLSZ = 24, ICONSZ = 15;

            double fit(const SPaint&, const SFrame&) override {
                int n = 0;
                for (const auto& IT : Tray::items)
                    if (IT->status != "Passive")
                        n++;
                return n ? n * CELLSZ + (n - 1) * (double)cfg.traySpacing->value() : 0;
            }

            void draw(const SPaint& P, const SFrame&, const CBox& box) override {
                double x = box.x;
                for (const auto& IT : Tray::items) {
                    if (IT->status == "Passive")
                        continue; // SNI: Passive means don't show the item
                    // The pixmap is a texture too, so the rule applies: rebuild it on
                    // the warm only. A dirty item reaching a draw keeps its old icon
                    // for this frame and asks for a repaint.
                    if (IT->dirty && warmGate.mayBuild()) {
                        IT->dirty = false;
                        IT->tex.reset();
                        if (!IT->pixels.empty())
                            IT->tex = g_pHyprRenderer->createTexture(DRM_FORMAT_ARGB8888, IT->pixels.data(), IT->pw * 4, Vector2D{(double)IT->pw, (double)IT->ph});
                        if ((!IT->tex || IT->tex->m_texID == 0) && !IT->iconName.empty())
                            IT->tex = trayIcon(IT->iconName, IT->themePath);
                    }

                    const bool STRIP = stripMode();
                    const CBox CELL{x, box.y + (box.h - CELLSZ) / 2, CELLSZ, CELLSZ};
                    // strip: the wash and the hit run the full band height
                    const CBox HITCELL = STRIP ? CBox{x, box.y, CELLSZ, box.h} : CELL;
                    if (barHover.widget == this && barHover.tray == IT.get()) {
                        if (STRIP)
                            P.rect(HITCELL, tFill2());
                        else
                            P.rect(CELL, tFill2(), (int)std::lround(6 * P.scale));
                    }

                    if (IT->tex && IT->tex->m_texID != 0) {
                        const double S = std::round(ICONSZ * P.scale);
                        const auto   B = P.toPhys(CELL);
                        CBox         b{B.x + (B.w - S) / 2.0, B.y + (B.h - S) / 2.0, S, S};
                        if (!P.warm)
                            g_pHyprOpenGL->renderTexture(IT->tex, b.round(), {.round = (int)std::lround(4 * P.scale)});
                    } else
                        P.texIn(textTex(letterOf(IT->iconName), color(cfg.colMuted), P.pt), CELL);

                    SHit h;
                    h.box     = HITCELL;
                    h.widget  = this;
                    h.tray    = IT;
                    h.anchorX = CELL.x + CELLSZ / 2.0;
                    h.mon     = P.mon;
                    P.hits->push_back(h);
                    x += CELLSZ + (double)cfg.traySpacing->value();
                }
            }

            void onHit(const SHit& h, uint32_t bit, bool) override {
                const auto IT = h.tray.lock();
                if (!IT || !IT->proxy)
                    return;
                if (bit == 4u) { // middle: the SNI SecondaryActivate call
                    try {
                        IT->proxy->callMethodAsync("SecondaryActivate")
                            .onInterface(Tray::SNI)
                            .withArguments((int32_t)0, (int32_t)0)
                            .uponReplyInvoke([](std::optional<sdbus::Error>) {});
                    } catch (...) {} // dying bus: teardown is already pending
                    Tray::pollSoon();
                    return;
                }
                const bool HASMENU = !IT->menuPath.empty();
                if (bit == 1u && !(IT->itemIsMenu && HASMENU)) {
                    try {
                        IT->proxy->callMethodAsync("Activate").onInterface(Tray::SNI).withArguments((int32_t)0, (int32_t)0).uponReplyInvoke([](std::optional<sdbus::Error>) {});
                    } catch (...) {}  // dying bus: teardown is already pending
                    Tray::pollSoon(); // the activation usually flips the icon right back
                    return;
                }
                if (HASMENU)
                    Menu::openFor(IT, h.anchorX, h.mon);
                else if (bit == 2u) {
                    try {
                        IT->proxy->callMethodAsync("ContextMenu").onInterface(Tray::SNI).withArguments((int32_t)0, (int32_t)0).uponReplyInvoke([](std::optional<sdbus::Error>) {});
                    } catch (...) {}
                    Tray::pollSoon();
                }
            }

            void onScroll(const SHit& h, int dir) override {
                // the SNI Scroll call (the XEmbed systray let apps see scroll too)
                if (const auto TI = h.tray.lock(); TI && TI->proxy) {
                    try {
                        TI->proxy->callMethodAsync("Scroll")
                            .onInterface(Tray::SNI)
                            .withArguments((int32_t)(dir > 0 ? -120 : 120), std::string{"vertical"})
                            .uponReplyInvoke([](std::optional<sdbus::Error>) {});
                    } catch (...) {} // dying bus: teardown is already pending
                    Tray::pollSoon();
                }
            }
        };
    } // namespace

    IWidget& trayWidget() {
        static CTrayWidget W;
        return W;
    }

} // namespace NHyprbar
