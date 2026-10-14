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
            if (O.kind == SOut::ACTION)
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

    void invokeAction(uint32_t id, const std::string& key, const std::string& sender) {
        queueOut({.kind = SOut::ACTION, .id = id, .text = key, .sender = sender});
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

            obj->addVTable(sdbus::registerMethod("Notify")
                               .withInputParamNames("app_name", "replaces_id", "app_icon", "summary", "body", "actions", "hints", "expire_timeout")
                               .withOutputParamNames("id")
                               .implementedAs([](std::string appName, uint32_t replacesId, std::string appIcon, std::string summary, std::string body,
                                                 std::vector<std::string> actions, std::map<std::string, sdbus::Variant> hints,
                                                 int32_t expireTimeout) {
                                   std::string sender;
                                   if (g_bus.conn())
                                       try {
                                           sender = g_bus.conn()->getCurrentlyProcessedMessage().getSender();
                                       } catch (...) {}
                                   return Model::arrive(appName, replacesId, appIcon, summary, body, sender, actions, hints, expireTimeout);
                               }),
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
                               return std::vector<std::string>{"actions", "action-icons", "body", "body-markup", "body-hyperlinks", "body-images", "icon-static", "inline-reply", "persistence", "sound"};
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
