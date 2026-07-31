// hyprnotify/bus.cpp — the org.freedesktop.Notifications connection: the
// object, its two vtables, the signals we emit and the name we own. It holds
// no cards; every method here is a thin translation between the wire and
// model.cpp, and every signal is something the model asked to send.

#include "common/busclient.hpp"
#include "common/lifecycle.hpp"

#include "hyprnotify.hpp"

#include <hyprland/src/protocols/XDGActivation.hpp>

namespace NHyprnotify::Bus {

    static const sdbus::InterfaceName IFACE{"org.freedesktop.Notifications"};
    // the shell's own face on the same object (dunst does the same with
    // org.dunstproject.cmd0): the bar's bell reads State and calls Toggle
    // over the bus — the sanctioned cross-plugin channel, never symbols
    static const sdbus::InterfaceName CIFACE{"org.hitori.hyprnotify"};

    static std::unique_ptr<sdbus::IObject> obj;
    static NHyprCommon::CBusLink           g_bus;
    static NHyprCommon::CHop               pendingState;

    // A drain must never run synchronously from here: emits happen inside
    // method handlers, i.e. inside processPendingEvent, and sd-bus dispatch
    // is not re-entrant. Park it on the link's timer instead.
    void pollSoon() {
        g_bus.pollSoon();
    }

    void emitClosed(uint32_t id, uint32_t reason) {
        if (!obj)
            return;
        // Model changes can originate in pointer/key drains, expiry timers, or
        // D-Bus method handlers. Keep signal construction off all of those
        // callbacks and let the link own the bounded send queue.
        g_bus.post([id, reason]() {
            if (!obj)
                return;
            try {
                obj->emitSignal("NotificationClosed").onInterface(IFACE).withArguments(id, reason);
            } catch (...) {} // a dead bus must not unwind through the idle C frame
            g_bus.pollSoon();
        });
    }

    // the bar's bell: live + kept + dnd + center, coalesced per model change
    void emitStateSoon() {
        pendingState.arm([]() {
            g_bus.post([]() {
                if (!obj)
                    return;
                try {
                    const auto [LIVE, KEPT] = Model::badgeCounts();
                    obj->emitSignal("State").onInterface(CIFACE).withArguments(LIVE, KEPT, Model::suspendedNow(), centerVisible());
                } catch (...) {}
                g_bus.pollSoon();
            });
        });
    }

    void invokeAction(uint32_t id, const std::string& key) {
        if (!obj)
            return;
        g_bus.post([id, key]() {
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
            g_bus.pollSoon();
        });
    }

    // The user typed a reply: hand it back and close the card, the same
    // way an invoked action does — the sender will post its own follow-up
    // if the conversation continues. `resident` holds the card, as ever.
    void sendReply(uint32_t id, const std::string& text) {
        if (text.empty())
            return;
        const auto N = Model::byId(id);
        if (!N || !N->canReply)
            return;
        if (obj)
            g_bus.post([id, text]() {
                if (!obj)
                    return;
                try {
                    if (PROTO::activation)
                        obj->emitSignal("ActivationToken").onInterface(IFACE).withArguments(id, PROTO::activation->mintToken());
                    obj->emitSignal("NotificationReplied").onInterface(IFACE).withArguments(id, text);
                } catch (...) {}
                g_bus.pollSoon();
            });
        if (!N->resident)
            Model::closeOne(id, Model::R_DISMISSED);
        else
            notifChanged();
    }

    void init() {
        g_bus.onLost = [](const std::string& err) {
            HyprlandAPI::addNotification(PHANDLE, "[hyprnotify] bus lost, notifications disabled: " + err, CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
        };
        g_bus.dropOwned = []() { obj.reset(); };
        try {
            g_bus.open(false, "org.freedesktop.Notifications");
            obj = sdbus::createObject(*g_bus.conn(), sdbus::ObjectPath{"/org/freedesktop/Notifications"});

            obj->addVTable(sdbus::registerMethod("Notify")
                               .withInputParamNames("app_name", "replaces_id", "app_icon", "summary", "body", "actions", "hints", "expire_timeout")
                               .withOutputParamNames("id")
                               .implementedAs([](std::string appName, uint32_t replacesId, std::string appIcon, std::string summary, std::string body,
                                                 std::vector<std::string> actions, std::map<std::string, sdbus::Variant> hints,
                                                 int32_t expireTimeout) { return Model::arrive(appName, replacesId, appIcon, summary, body, actions, hints, expireTimeout); }),
                           // ignore_dbusclose (dunst's knob): an app revoking its
                           // own notification (Telegram on read-elsewhere) is
                           // ignored — the card lives out its banner and waits in
                           // the shade. Only the bus path is gated; user
                           // dismissals and expiry are untouched.
                           sdbus::registerMethod("CloseNotification").withInputParamNames("id").implementedAs([](uint32_t id) {
                               if (cfg.ignoreDbusClose->value())
                                   return;
                               Model::closeOne(id, Model::R_CLOSED);
                               emitStateSoon();
                           }),
                           sdbus::registerMethod("GetCapabilities").withOutputParamNames("capabilities").implementedAs([]() {
                               return std::vector<std::string>{"actions", "action-icons", "body", "body-markup", "body-hyperlinks", "body-images", "icon-static", "inline-reply", "persistence", "sound"};
                           }),
                           sdbus::registerMethod("GetServerInformation").withOutputParamNames("name", "vendor", "version", "spec_version").implementedAs([]() {
                               return std::tuple<std::string, std::string, std::string, std::string>{"hyprnotify", "hitori", VERSION, "1.3"};
                           }),
                           sdbus::registerSignal("NotificationClosed").withParameters<uint32_t, uint32_t>("id", "reason"),
                           sdbus::registerSignal("ActionInvoked").withParameters<uint32_t, std::string>("id", "action_key"),
                           sdbus::registerSignal("ActivationToken").withParameters<uint32_t, std::string>("id", "activation_token"),
                           sdbus::registerSignal("NotificationReplied").withParameters<uint32_t, std::string>("id", "text"))
                .forInterface(IFACE);

            // the shell face: the bar's bell toggles the center and reads
            // the badge counts here
            obj->addVTable(sdbus::registerMethod("Toggle").implementedAs([]() { queueCenterToggle(); }),
                           sdbus::registerMethod("Peek").withInputParamNames("on_bell").implementedAs([](bool on) { queueCenterPeek(on); }),
                           sdbus::registerMethod("State").withOutputParamNames("live", "kept", "dnd", "center").implementedAs([]() {
                               const auto [LIVE, KEPT] = Model::badgeCounts();
                               return std::tuple<uint32_t, uint32_t, bool, bool>{LIVE, KEPT, Model::suspendedNow(), centerVisible()};
                           }),
                           sdbus::registerSignal("State").withParameters<uint32_t, uint32_t, bool, bool>("live", "kept", "dnd", "center"))
                .forInterface(CIFACE);

            g_bus.sync(); // drain anything queued during setup — the vtable is registered, nothing dispatches early
        } catch (const std::exception& E) {
            // most likely another daemon owns the name (dunst still installed)
            HyprlandAPI::addNotification(PHANDLE, std::string{"[hyprnotify] disabled: "} + E.what(), CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
            obj.reset();
            g_bus.close();
        }
    }

    void exit() {
        g_bus.close(); // fd sources out BEFORE the connection dies
    }

} // namespace NHyprnotify::Bus
