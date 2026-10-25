// awesome/notify/bus.cpp — the org.freedesktop.Notifications connection:
// the object, its vtable, the signals we emit and the name we own. It
// holds no cards; every method here is a thin translation between the wire
// and model.cpp, and every signal is something the model asked to send.
//
// The only D-Bus interface in the plugin: the old private bus shell face
// is gone — the bell lives in the same .so now and calls the model's
// functions directly.
#include "model.hpp"

#include "../core/activate.hpp"
#include "../core/busclient.hpp"

#include <hyprland/src/protocols/XDGActivation.hpp>

namespace NAwesome::Notify::Bus {

    static const sdbus::InterfaceName IFACE{"org.freedesktop.Notifications"};

    static std::unique_ptr<sdbus::IObject> obj;
    static std::unique_ptr<sdbus::IProxy>  busProbe; // org.freedesktop.DBus: sender -> pid
    static NAwesome::CBusLink           g_bus;

    // Model changes can originate in pointer/key drains, expiry timers, or
    // D-Bus method handlers. Keep signal construction off all of those
    // callbacks: every outbound signal joins ONE ordered queue, sent by one
    // post per drain. Ordered: a card's NotificationClosed must not overtake
    // its ActionInvoked (GLib's GNotification forgets an id on close and
    // drops the action after it), which two separate posts allowed. Batched:
    // a Clear all over 400 cards was 400 posts against the link's 256 bound
    // once, and the tail's closes were dropped. Bounded: past MAX_OUT queued
    // signals a push is refused.
    struct SOut {
        enum eKind : uint8_t {
            CLOSED,
            ACTION,
            REPLY,
        } kind;
        uint32_t    id     = 0;
        uint32_t    reason = 0;
        std::string text;   // the action key, or the reply
        std::string sender; // ACTION/REPLY: the one client that hears them
        bool        raise = true; // ACTION: the sender's window comes up (see invokeLive)
    };
    static std::vector<SOut> s_out;
    static bool              s_outPosted = false;
    constexpr size_t         MAX_OUT     = 8192;

    // The action, its activation token and a reply's text concern the
    // sender alone: sent to it, never broadcast (any session-bus listener
    // could otherwise read what the user typed, or spend the token to take
    // focus). A card with no known sender (one posted in-process) has no
    // client to tell.
    template <typename... A>
    static void emitTo(const std::string& sender, const char* name, const A&... args) {
        if (sender.empty())
            return;
        auto signal = obj->createSignal(IFACE, sdbus::SignalName{name});
        (signal << ... << args);
        signal.setDestination(sender);
        obj->emitSignal(signal);
    }

    // The click is a user action: the sender's window comes to the
    // foreground. The token lets a Wayland sender spend it through
    // xdg-activation (vanilla: the app activates itself), but a sender may
    // never spend it, and an X11 sender's own activation is unauthenticated
    // (urgency only) — so the side that SAW the click focuses the sender's
    // own window itself, for every backend (core/activate.hpp; a sender
    // that DID spend the token just lands on its already-focused window).
    static void focusSender(const std::string& sender) {
        if (sender.empty() || !g_bus.conn())
            return;
        if (!busProbe)
            busProbe = sdbus::createProxy(*g_bus.conn(), sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"});
        try {
            busProbe->callMethodAsync("GetConnectionUnixProcessID")
                .onInterface("org.freedesktop.DBus")
                .withArguments(sender)
                .uponReplyInvoke([](std::optional<sdbus::Error> e, uint32_t pid) {
                    if (e)
                        return; // a unique name that vanished: the app is gone, nothing to focus
                    NAwesome::activateAppWindow(pid);
                });
        } catch (...) {}
    }

    static void flushOut() {
        s_outPosted      = false;
        const auto BATCH = std::exchange(s_out, {});
        if (!obj)
            return;
        for (const auto& O : BATCH) {
            try { // a dead bus must not unwind through the idle C frame
                if (O.kind == SOut::CLOSED)
                    obj->emitSignal("NotificationClosed").onInterface(IFACE).withArguments(O.id, O.reason);
                else {
                    // spec 1.3: the token precedes the action, so the
                    // sender's xdg-activation request can actually raise
                    // it — tokenless activates only flag urgent
                    if (PROTO::activation && !O.sender.empty())
                        emitTo(O.sender, "ActivationToken", O.id, PROTO::activation->mintToken());
                    emitTo(O.sender, O.kind == SOut::ACTION ? "ActionInvoked" : "NotificationReplied", O.id, O.text);
                }
            } catch (...) {}
            if (O.kind == SOut::ACTION && O.raise)
                focusSender(O.sender);
        }
        g_bus.pollSoon();
    }

    static bool queueOut(SOut&& o) {
        if (!obj || s_out.size() >= MAX_OUT)
            return false;
        s_out.push_back(std::move(o));
        if (!s_outPosted) // a refused post retries with the next push
            s_outPosted = g_bus.post(flushOut);
        return true;
    }

    void emitClosed(uint32_t id, uint32_t reason) {
        queueOut({.kind = SOut::CLOSED, .id = id, .reason = reason});
    }

    void invokeAction(uint32_t id, const std::string& key, const std::string& sender, bool raise) {
        queueOut({.kind = SOut::ACTION, .id = id, .text = key, .sender = sender, .raise = raise});
    }

    // The user typed a reply: hand it back and close the card, the same
    // way an invoked action does — the sender will post its own follow-up
    // if the conversation continues. `resident` holds the card, as ever.
    // The card must still be the conversation the reply was typed for (a
    // sender may reuse the id for another chat while the user types), and
    // a reply that could not be queued leaves the card — and the user's
    // chance to retry — in place.
    bool sendReply(uint32_t id, const std::string& text, const std::string& target) {
        if (text.empty())
            return false;
        const auto N = Model::byId(id);
        if (!N || !N->canReply || replyTarget(*N) != target)
            return false;
        if (!queueOut({.kind = SOut::REPLY, .id = id, .text = text, .sender = N->sender}))
            return false;
        if (!N->resident)
            Model::closeOne(id, Model::R_DISMISSED);
        else
            notifChanged();
        return true;
    }

    // ---- Notify, read by hand ----

    // sdbus-c++'s typed handler reads the hints into Variants, and a Variant
    // copies its value element by element into a message of its own: a
    // screenshot's image-data held the dispatch 0.2 s (1280x720 RGB) to
    // 0.65 s (1080p RGBA) before the model saw a byte of it. Read here, the
    // image header is checked first and its pixels leave the message in one
    // block; the model's other hints are basic values, cheap as Variants;
    // every other value is walked past without a copy (skipOne), and so is
    // every hint past MAX_HINTS kept and every action string past
    // MAX_ACTION_STRINGS (the model keeps 8 menu rows, the default and the
    // reply). A walk is linear in elements (~0.2-1 us each): past
    // MAX_WALKED of them a call is refused, the defined behavior at a bound
    // only a hostile sender reaches.
    constexpr size_t MAX_HINTS          = 64;
    constexpr size_t MAX_ACTION_STRINGS = 64;
    constexpr size_t MAX_WALKED         = 1 << 16;
    // A big-endian sender's image-data is read byte by byte (~0.1 us
    // each, ~26 ms at the bound): a header declaring more is refused
    // before the walk, a walk reaching it is refused at it (a 256x256
    // RGBA icon fits; 1 MiB walked ~0.1 s against the 150 ms RT budget)
    constexpr size_t         MAX_SWAPPED_IMAGE_BYTES = 256 << 10;

    [[noreturn]] static void refuse(const char* why) {
        throw sdbus::Error{sdbus::Error::Name{"org.freedesktop.DBus.Error.LimitsExceeded"}, why};
    }

    // the spec's three names for it, newest first
    static int imageRank(std::string_view key) {
        return key == "image-data" ? 0 : key == "image_data" ? 1 : key == "icon_data" ? 2 : -1;
    }

    static bool basicType(std::string_view sig) {
        return sig.size() == 1 && std::string_view{"ybnqiuxtdsog"}.find(sig[0]) != std::string_view::npos;
    }

    // the element types sd-bus reads as one block; it serves none from a
    // message that needs byte-swapping (a big-endian sender), and an fd
    // array is not one
    static bool trivialType(char t) {
        return std::string_view{"ybnqiuxtd"}.find(t) != std::string_view::npos;
    }

    // Past the next complete value, copying nothing: a trivial array is one
    // block (element by element from a big-endian sender), a string a
    // pointer into the message. sd-bus can leave no container unread, so
    // this is a walk, linear in the value's elements, never a copy; the
    // nesting is the signature's, which sd-bus bounds. `budget` counts the
    // elements left to walk this call.
    static void skipOne(sdbus::MethodCall& m, size_t& budget) {
        if (budget-- == 0)
            refuse("a Notify value past the size the daemon walks");
        const auto [TYPE, CONTENTS] = m.peekType();
        const auto READ             = [&]<class T>() {
            T v{};
            m >> v;
        };
        switch (TYPE) {
            case 'y': return READ.operator()<uint8_t>();
            case 'b': return READ.operator()<bool>();
            case 'n': return READ.operator()<int16_t>();
            case 'q': return READ.operator()<uint16_t>();
            case 'i': return READ.operator()<int32_t>();
            case 'u': return READ.operator()<uint32_t>();
            case 'x': return READ.operator()<int64_t>();
            case 't': return READ.operator()<uint64_t>();
            case 'd': return READ.operator()<double>();
            case 's': {
                char* v = nullptr;
                m >> v;
                return;
            }
            case 'o': return READ.operator()<sdbus::ObjectPath>();
            case 'g': return READ.operator()<sdbus::Signature>();
            case 'h': return READ.operator()<sdbus::UnixFd>();
            case 'v':
                m.enterVariant(CONTENTS);
                skipOne(m, budget);
                m.exitVariant();
                return;
            case 'a': {
                const std::string_view C{CONTENTS ? CONTENTS : ""};
                if (C.size() == 1 && trivialType(C[0])) {
                    try {
                        const void* p = nullptr;
                        size_t      n = 0;
                        m.readArray(C[0], &p, &n);
                        return;
                    } catch (const sdbus::Error&) {} // a big-endian sender: walked below
                }
                m.enterContainer(CONTENTS);
                while (!m.isAtEnd(false))
                    skipOne(m, budget);
                m.exitContainer();
                return;
            }
            case 'r':
                m.enterStruct(CONTENTS);
                while (!m.isAtEnd(false))
                    skipOne(m, budget);
                m.exitStruct();
                return;
            case 'e':
                m.enterDictEntry(CONTENTS);
                skipOne(m, budget);
                skipOne(m, budget);
                m.exitDictEntry();
                return;
            default: throw sdbus::Error{sdbus::Error::Name{"org.freedesktop.DBus.Error.InvalidArgs"}, "unreadable hint value"};
        }
    }

    // at an image entry's variant; nullopt when the header would not unpack
    static std::optional<Parse::ImageData> readImage(sdbus::MethodCall& m, bool keep) {
        int32_t              w = 0, h = 0, stride = 0, bps = 0, ch = 0;
        bool                 alpha = false;
        const void*          px    = nullptr;
        size_t               bytes = 0;
        std::vector<uint8_t> swapped; // a big-endian sender's bytes, read one by one
        m.enterVariant("(iiibiiay)");
        m.enterStruct("iiibiiay");
        m >> w >> h >> stride >> alpha >> bps >> ch;
        try {
            m.readArray('y', &px, &bytes);
        } catch (const sdbus::Error&) {
            const size_t DECLARED = w > 0 && h > 0 && stride > 0 && ch > 0 ? (size_t)stride * (h - 1) + (size_t)w * ch : 0;
            if (DECLARED > MAX_SWAPPED_IMAGE_BYTES)
                refuse("a big-endian image-data past 256 KiB");
            swapped.reserve(DECLARED);
            m.enterContainer("y");
            while (!m.isAtEnd(false)) {
                if (swapped.size() >= MAX_SWAPPED_IMAGE_BYTES)
                    refuse("a big-endian image-data past 256 KiB");
                uint8_t b = 0;
                m >> b;
                swapped.push_back(b);
            }
            m.exitContainer();
            px    = swapped.data();
            bytes = swapped.size();
        }
        m.exitStruct();
        m.exitVariant();
        if (!keep || Parse::imageDataSize(w, h, stride, bps, ch, bytes).first <= 0)
            return std::nullopt;
        Parse::ImageData d;
        std::get<0>(d)    = w;
        std::get<1>(d)    = h;
        std::get<2>(d)    = stride;
        std::get<3>(d)    = alpha;
        std::get<4>(d)    = bps;
        std::get<5>(d)    = ch;
        const size_t NEED = (size_t)stride * (h - 1) + (size_t)w * ch; // what the unpack reads (checked above)
        const auto*  FROM = static_cast<const uint8_t*>(px);
        std::get<6>(d).assign(FROM, FROM + NEED);
        return d;
    }

    static void onNotify(sdbus::MethodCall call) {
        std::string                           appName, appIcon, summary, body;
        uint32_t                              replacesId    = 0;
        int32_t                               expireTimeout = -1;
        std::vector<std::string>              actions;
        std::map<std::string, sdbus::Variant> hints;
        std::optional<Parse::ImageData>       image;
        int                                   imageAt = 3; // the best name read so far
        size_t                                budget  = MAX_WALKED;
        call >> appName >> replacesId >> appIcon >> summary >> body;
        call.enterContainer("s");
        while (!call.isAtEnd(false))
            if (actions.size() < MAX_ACTION_STRINGS) {
                std::string a;
                call >> a;
                actions.push_back(std::move(a));
            } else
                skipOne(call, budget);
        call.exitContainer();
        call.enterContainer("{sv}");
        while (call.enterDictEntry("sv")) {
            std::string key;
            call >> key;
            const auto [TYPE, CONTENTS] = call.peekType();
            const std::string_view SIG{CONTENTS ? CONTENTS : ""};
            if (const int RANK = imageRank(key); RANK >= 0 && SIG == "(iiibiiay)") {
                if (auto d = readImage(call, RANK < imageAt)) {
                    image   = std::move(d);
                    imageAt = RANK;
                }
            } else if (hints.size() < MAX_HINTS && basicType(SIG)) {
                sdbus::Variant v;
                call >> v;
                hints.emplace(std::move(key), std::move(v)); // a repeated key keeps its first, as the typed read did
            } else
                skipOne(call, budget);
            call.exitDictEntry();
        }
        call.clearFlags();
        call.exitContainer();
        call >> expireTimeout;

        const char* SENDER = call.getSender();
        const auto  ID     = Model::arrive(appName, replacesId, appIcon, summary, body, SENDER ? SENDER : "", actions, hints, expireTimeout, std::move(image));
        auto        reply  = call.createReply();
        reply << ID;
        reply.send();
    }

    void init() {
        g_bus.onLost = [](const std::string& err) {
            HyprlandAPI::addNotification(NAwesome::supervisor().handle(), "[awesome] bus lost, notifications disabled: " + err, CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
        };
        // everything bound to the connection goes before it: an in-flight
        // GetConnectionUnixProcessID would otherwise run its slot deleter
        // against a destroyed connection when the static proxy dies at unload
        g_bus.dropOwned = []() {
            busProbe.reset();
            obj.reset();
        };
        try {
            g_bus.open(false, "org.freedesktop.Notifications");
            obj = sdbus::createObject(*g_bus.conn(), sdbus::ObjectPath{"/org/freedesktop/Notifications"});

            obj->addVTable(sdbus::MethodVTableItem{.name             = sdbus::MethodName{"Notify"},
                                                   .inputSignature   = sdbus::Signature{"susssasa{sv}i"},
                                                   .inputParamNames  = {"app_name", "replaces_id", "app_icon", "summary", "body", "actions", "hints", "expire_timeout"},
                                                   .outputSignature  = sdbus::Signature{"u"},
                                                   .outputParamNames = {"id"},
                                                   .callbackHandler =
                                                       [](sdbus::MethodCall call) {
                                                           // a malformed body is the caller's error, never an
                                                           // exception through sd-bus's C frames
                                                           try {
                                                               onNotify(std::move(call));
                                                           } catch (const sdbus::Error&) { throw; } catch (const std::exception& E) {
                                                               throw sdbus::Error{sdbus::Error::Name{"org.freedesktop.DBus.Error.InvalidArgs"}, E.what()};
                                                           }
                                                       },
                                                   .flags = {}},
                           // ignore_dbusclose (dunst's knob): an app revoking
                           // its own notification (Telegram on read-elsewhere)
                           // is ignored — the card lives out its banner and
                           // waits in the shade. Only the bus path is gated;
                           // user dismissals and expiry are untouched.
                           sdbus::registerMethod("CloseNotification").withInputParamNames("id").implementedAs([](uint32_t id) {
                               if (NAwesome::cfg().getI("plugin:awesome:notify:ignore_dbusclose"))
                                   return;
                               // spec: an unknown ID is an error, not a silent no-op
                               if (!Model::closeOne(id, Model::R_CLOSED))
                                   throw sdbus::Error{sdbus::Error::Name{"org.freedesktop.Notifications.Error"}, "Unknown notification ID"};
                           }),
                           sdbus::registerMethod("GetCapabilities").withOutputParamNames("capabilities").implementedAs([]() {
                               return std::vector<std::string>{"actions", "body", "body-markup", "body-hyperlinks", "body-images", "icon-static", "inline-reply", "persistence", "sound"};
                           }),
                           sdbus::registerMethod("GetServerInformation").withOutputParamNames("name", "vendor", "version", "spec_version").implementedAs([]() {
                               return std::tuple<std::string, std::string, std::string, std::string>{"awesome", "hitori", NAwesome::VERSION, "1.3"};
                           }),
                           sdbus::registerSignal("NotificationClosed").withParameters<uint32_t, uint32_t>("id", "reason"),
                           sdbus::registerSignal("ActionInvoked").withParameters<uint32_t, std::string>("id", "action_key"),
                           sdbus::registerSignal("ActivationToken").withParameters<uint32_t, std::string>("id", "activation_token"),
                           sdbus::registerSignal("NotificationReplied").withParameters<uint32_t, std::string>("id", "text"))
                .forInterface(IFACE);

            // a card belongs to its sender's unique name (Model::arrive): one
            // leaving the bus orphans its cards, so the app's next instance
            // may continue them
            busProbe = sdbus::createProxy(*g_bus.conn(), sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"});
            busProbe->uponSignal("NameOwnerChanged").onInterface("org.freedesktop.DBus").call([](std::string name, std::string, std::string newOwner) {
                if (!name.empty() && name.front() == ':' && newOwner.empty())
                    Model::senderGone(name);
            });

            g_bus.sync(); // drain anything queued during setup — the vtable is registered, nothing dispatches early
        } catch (const std::exception& E) {
            // most likely another daemon owns the name (dunst still
            // installed)
            HyprlandAPI::addNotification(NAwesome::supervisor().handle(), std::string{"[awesome] notifications disabled: "} + E.what(), CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
            obj.reset();
            g_bus.close();
        }
    }

    void exit() {
        g_bus.close(); // fd sources out BEFORE the connection dies
        s_out.clear();
        s_outPosted = false;
    }

} // namespace NAwesome::Notify::Bus
