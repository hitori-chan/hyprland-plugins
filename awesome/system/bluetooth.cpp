// awesome/system/bluetooth.cpp — Bluetooth for the control center: BlueZ's
// first adapter (its power) and its paired devices (connected, a battery),
// published into core/controls.hpp; the doors power the adapter and connect
// or disconnect a device. Asynchronous over the system bus only: the tree
// (GetManagedObjects) is read when the unit starts, when bluetoothd
// (re)appears and when the panel opens; InterfacesAdded/Removed and one
// PropertiesChanged match under /org/bluez keep it current between reads,
// and a change it cannot apply in place (a device newly paired) reads the
// tree again. sdbus's ObjectManager proxy is not used: its handlers
// register synchronously.
//
// A connect takes seconds (a headset brings up its profiles): the device
// shows Connecting… until the reply, an error or Connected settles it.
// Every reply carries the unit's generation: a teardown, a lost bus or a
// new bluetoothd voids the ones in flight. A device's proxy lives with the
// device, dropped (its call cancelled) only from another proxy's callback
// or a signal, never from its own reply.

#include "system.hpp"

#include "core/busclient.hpp"
#include "core/controls.hpp"
#include "core/hop.hpp"
#include "core/queries.hpp"

#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>

#include <sdbus-c++/sdbus-c++.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace NAwesome::System {

    namespace {
        constexpr const char* BLUEZ = "org.bluez";
        constexpr const char* ADAPTER = "org.bluez.Adapter1";
        constexpr const char* DEVICE  = "org.bluez.Device1";
        constexpr const char* BATTERY = "org.bluez.Battery1";
        constexpr const char* PROPS   = "org.freedesktop.DBus.Properties";
        // the paired devices held (bluetoothd lists every device it has
        // seen; only the paired ones are kept, at most this many), a name's
        // bytes
        constexpr size_t MAX_TRACKED = 64, MAX_NAME = 64;
        // a connect brings up a headset's profiles: longer than sd-bus's 25 s
        constexpr auto CONNECT_TIMEOUT = std::chrono::seconds(30);
        // a headset's sink reaches PipeWire after BlueZ says Connected
        constexpr auto SINK_SETTLE = std::chrono::milliseconds(1500);

        using TProps   = std::map<std::string, sdbus::Variant>;
        using TIfaces  = std::map<std::string, TProps>;
        using TObjects = std::map<sdbus::ObjectPath, TIfaces>;
        using eKind    = Controls::SDevice::eKind;

        struct SDev {
            std::string                    name, adapter;
            eKind                          kind      = eKind::OTHER;
            bool                           paired    = false;
            bool                           connected = false;
            bool                           pending   = false; // a Connect/Disconnect in flight
            int8_t                         battery   = -1;
            std::unique_ptr<sdbus::IProxy> proxy; // made at the first call
        };

        std::unique_ptr<sdbus::IProxy>      root;    // org.bluez at "/": the tree
        std::unique_ptr<sdbus::IProxy>      adapterProxy;
        std::string                         adapterPath; // the first adapter's, "" none
        std::map<std::string, bool>         adapters;    // path -> Powered
        std::map<std::string, SDev>         devices;     // path -> a paired device
        std::optional<bool>                 wantPower;   // a power switch in flight shows at once
        sdbus::Slot                         addedMatch, removedMatch, propsMatch, ownerMatch;
        uint64_t                            gen     = 0;
        bool                                reading = false, readAgain = false;
        CHop                                pendingRead, pendingDoor;
        SP<CEventLoopTimer>                 sinkSettle;
    }

    // a name as the panel shows it: control characters out, a bounded
    // UTF-8 prefix
    static std::string cleanName(std::string s) {
        for (auto& c : s)
            if ((unsigned char)c < 0x20 || c == 0x7f)
                c = ' ';
        if (s.size() > MAX_NAME) {
            size_t cut = MAX_NAME;
            while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80)
                cut--;
            s.resize(cut);
            s += "…";
        }
        return s;
    }

    // BlueZ's Icon names the device's class (the freedesktop icon names)
    static eKind kindOf(const std::string& icon) {
        if (icon == "audio-headset" || icon == "audio-headphones")
            return eKind::HEADPHONES;
        if (icon.starts_with("audio-") || icon == "multimedia-player")
            return eKind::SPEAKER;
        if (icon == "input-mouse" || icon == "input-tablet")
            return eKind::MOUSE;
        if (icon == "input-keyboard")
            return eKind::KEYBOARD;
        return eKind::OTHER;
    }

    static bool audioKind(eKind k) {
        return k == eKind::HEADPHONES || k == eKind::SPEAKER;
    }

    static void publishBluetooth() {
        Controls::SBluetooth B;
        B.present = !adapterPath.empty();
        if (B.present) {
            const auto IT = adapters.find(adapterPath);
            B.powered     = wantPower.value_or(IT != adapters.end() && IT->second);
            for (const auto& [PATH, D] : devices)
                if (D.paired && D.adapter == adapterPath)
                    B.devices.push_back({.id = PATH, .name = D.name, .kind = D.kind, .connected = D.connected, .connecting = D.pending, .battery = D.battery});
            // connected first, then by name: what the panel shows of a long list
            std::ranges::sort(B.devices, [](const Controls::SDevice& a, const Controls::SDevice& b) {
                if (a.connected != b.connected)
                    return a.connected;
                return std::ranges::lexicographical_compare(a.name, b.name, [](char x, char y) { return std::tolower((unsigned char)x) < std::tolower((unsigned char)y); });
            });
            if (B.devices.size() > Controls::MAX_DEVICES)
                B.devices.resize(Controls::MAX_DEVICES);
        }
        Controls::publish(std::move(B));
    }

    // the first adapter by path: hci0 before hci1, whichever came first
    static void pickAdapter() {
        const std::string WAS = adapterPath;
        adapterPath           = adapters.empty() ? "" : adapters.begin()->first;
        if (adapterPath != WAS) {
            adapterProxy.reset();
            wantPower.reset();
        }
    }

    // a device's properties as they arrive (GetManagedObjects, a signal):
    // false when the change is one the unit cannot apply in place
    static bool applyDevice(const std::string& path, const TProps& props) {
        auto IT = devices.find(path);
        try {
            if (IT == devices.end()) {
                const auto P = props.find("Paired");
                if (P == props.end() || !P->second.get<bool>())
                    return true; // never paired: not ours
                if (devices.size() >= MAX_TRACKED)
                    return true; // the bound: a further device is not listed
                if (!props.contains("Adapter"))
                    return false; // a partial set (a signal): read the device whole
                IT = devices.emplace(path, SDev{}).first;
            }
            auto& D = IT->second;
            for (const auto& [KEY, V] : props) {
                if (KEY == "Alias")
                    D.name = cleanName(V.get<std::string>());
                else if (KEY == "Name" && D.name.empty())
                    D.name = cleanName(V.get<std::string>());
                else if (KEY == "Icon")
                    D.kind = kindOf(V.get<std::string>());
                else if (KEY == "Paired")
                    D.paired = V.get<bool>();
                else if (KEY == "Connected") {
                    const bool WAS = D.connected;
                    D.connected    = V.get<bool>();
                    if (WAS != D.connected) {
                        D.pending = false;
                        // a headset's sink comes and goes with it: the output
                        // pill reads the sinks again once PipeWire caught up
                        if (audioKind(D.kind) && Controls::panelShowing() && sinkSettle)
                            sinkSettle->updateTimeout(SINK_SETTLE);
                    }
                } else if (KEY == "Adapter")
                    D.adapter = V.get<sdbus::ObjectPath>();
            }
            if (D.name.empty())
                D.name = "Device";
            if (!D.paired)
                devices.erase(IT); // unpaired elsewhere: gone from the list
        } catch (...) {} // a wrong-typed property: the rest stands
        return true;
    }

    static void applyAdapter(const std::string& path, const TProps& props) {
        try {
            bool& P = adapters[path];
            if (const auto IT = props.find("Powered"); IT != props.end()) {
                P = IT->second.get<bool>();
                if (path == adapterPath)
                    wantPower.reset(); // the adapter says what it is now
            }
        } catch (...) {}
    }

    static void applyBattery(const std::string& path, const TProps& props) {
        const auto IT = devices.find(path);
        if (IT == devices.end())
            return;
        try {
            if (const auto P = props.find("Percentage"); P != props.end())
                IT->second.battery = (int8_t)std::min<int>(P->second.get<uint8_t>(), 100);
        } catch (...) {}
    }

    static void readTree();
    static void readSoon() {
        pendingRead.arm([]() { readTree(); });
    }

    // the whole tree: devices still listed keep their proxies (a call in
    // flight on one survives the read)
    static void treeRead(const TObjects& objs) {
        std::map<std::string, bool> seenDevices;
        adapters.clear();
        for (const auto& [PATH, IFACES] : objs) {
            if (const auto IT = IFACES.find(ADAPTER); IT != IFACES.end())
                applyAdapter(PATH, IT->second);
            if (const auto IT = IFACES.find(DEVICE); IT != IFACES.end()) {
                applyDevice(PATH, IT->second);
                if (devices.contains(PATH))
                    seenDevices[PATH] = true;
            }
            if (const auto IT = IFACES.find(BATTERY); IT != IFACES.end())
                applyBattery(PATH, IT->second);
        }
        std::erase_if(devices, [&](const auto& E) { return !seenDevices.contains(E.first); });
        pickAdapter();
        publishBluetooth();
    }

    static void readTree() {
        if (!root || compositorShuttingDown())
            return;
        if (reading) {
            readAgain = true;
            return;
        }
        reading            = true;
        const uint64_t GEN = gen;
        try {
            root->callMethodAsync("GetManagedObjects")
                .onInterface("org.freedesktop.DBus.ObjectManager")
                .uponReplyInvoke([GEN](std::optional<sdbus::Error> err, TObjects objs) {
                    if (GEN != gen || compositorShuttingDown())
                        return;
                    reading = false;
                    if (err) { // no bluetoothd (it activates on this call where installed): no Bluetooth
                        adapters.clear();
                        devices.clear();
                        pickAdapter();
                        publishBluetooth();
                    } else
                        treeRead(objs);
                    if (std::exchange(readAgain, false))
                        readTree();
                });
            systemBus().pollSoon();
        } catch (...) { reading = false; }
    }

    static sdbus::IProxy* proxyOf(const std::string& path) {
        auto& B = systemBus();
        if (!B.conn())
            return nullptr;
        try {
            if (path == adapterPath) {
                if (!adapterProxy)
                    adapterProxy = sdbus::createProxy(*B.conn(), sdbus::ServiceName{BLUEZ}, sdbus::ObjectPath{path});
                return adapterProxy.get();
            }
            const auto IT = devices.find(path);
            if (IT == devices.end())
                return nullptr;
            if (!IT->second.proxy)
                IT->second.proxy = sdbus::createProxy(*B.conn(), sdbus::ServiceName{BLUEZ}, sdbus::ObjectPath{path});
            return IT->second.proxy.get();
        } catch (...) { return nullptr; }
    }

    static void connectNow(const std::string& path, bool connect) {
        const auto IT = devices.find(path);
        auto*      P  = proxyOf(path);
        if (IT == devices.end() || !P)
            return;
        IT->second.pending = true;
        publishBluetooth();
        const uint64_t GEN = gen;
        try {
            P->callMethodAsync(connect ? "Connect" : "Disconnect")
                .onInterface(DEVICE)
                .withTimeout(CONNECT_TIMEOUT)
                .uponReplyInvoke([GEN, path](std::optional<sdbus::Error>) {
                    if (GEN != gen || compositorShuttingDown())
                        return;
                    // the reply settles it either way: Connected (its signal)
                    // says where it landed
                    if (const auto IT = devices.find(path); IT != devices.end() && IT->second.pending) {
                        IT->second.pending = false;
                        publishBluetooth();
                    }
                });
            systemBus().pollSoon();
        } catch (...) {
            IT->second.pending = false;
            publishBluetooth();
        }
    }

    // the adapter's power, then (a device picked while it was off) the connect
    static void powerNow(bool on, const std::string& thenConnect) {
        auto* P = adapterPath.empty() ? nullptr : proxyOf(adapterPath);
        if (!P)
            return;
        wantPower = on;
        if (!thenConnect.empty())
            if (const auto IT = devices.find(thenConnect); IT != devices.end())
                IT->second.pending = true;
        publishBluetooth();
        const uint64_t GEN = gen;
        try {
            P->callMethodAsync("Set")
                .onInterface(PROPS)
                .withArguments(std::string{ADAPTER}, std::string{"Powered"}, sdbus::Variant{on})
                .uponReplyInvoke([GEN, on, thenConnect](std::optional<sdbus::Error> err) {
                    if (GEN != gen || compositorShuttingDown())
                        return;
                    if (err) { // blocked (rfkill), or gone: back to what it is
                        wantPower.reset();
                        if (const auto IT = devices.find(thenConnect); IT != devices.end())
                            IT->second.pending = false;
                        publishBluetooth();
                        return;
                    }
                    adapters[adapterPath] = on;
                    wantPower.reset();
                    if (!thenConnect.empty())
                        connectNow(thenConnect, true);
                    else
                        publishBluetooth();
                });
            systemBus().pollSoon();
        } catch (...) {
            wantPower.reset();
            publishBluetooth();
        }
    }

    // the doors: deferred out of the pick (a hop), one at a time
    void setBluetooth(bool on) {
        pendingDoor.arm([on]() { powerNow(on, ""); });
    }

    void connectDevice(const std::string& id, bool connect) {
        pendingDoor.arm([id, connect]() {
            const auto IT = devices.find(id);
            if (IT == devices.end() || IT->second.pending)
                return;
            const auto A = adapters.find(adapterPath);
            if (connect && (A == adapters.end() || !A->second))
                powerNow(true, id);
            else
                connectNow(id, connect);
        });
    }

    void bluetoothRefresh() {
        readSoon();
    }

    void bluetoothInit() {
        sinkSettle = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { audioRefresh(); }, nullptr);
        g_pEventLoopManager->addTimer(sinkSettle);
        auto& B = systemBus();
        if (!B.conn())
            return publishBluetooth();
        try {
            root = sdbus::createProxy(*B.conn(), sdbus::ServiceName{BLUEZ}, sdbus::ObjectPath{"/"});
            // sender: only bluetoothd may own org.bluez on the system bus
            addedMatch = B.conn()->addMatchAsync(
                "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager',member='InterfacesAdded'",
                [](sdbus::Message m) {
                    if (compositorShuttingDown())
                        return;
                    sdbus::ObjectPath path;
                    TIfaces           ifaces;
                    try {
                        m >> path >> ifaces;
                    } catch (...) { return; }
                    if (const auto IT = ifaces.find(ADAPTER); IT != ifaces.end()) {
                        applyAdapter(path, IT->second);
                        pickAdapter();
                    }
                    if (const auto IT = ifaces.find(DEVICE); IT != ifaces.end() && !applyDevice(path, IT->second))
                        readSoon();
                    if (const auto IT = ifaces.find(BATTERY); IT != ifaces.end())
                        applyBattery(path, IT->second);
                    publishBluetooth();
                },
                [](sdbus::Message) {}, sdbus::return_slot);
            removedMatch = B.conn()->addMatchAsync(
                "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager',member='InterfacesRemoved'",
                [](sdbus::Message m) {
                    if (compositorShuttingDown())
                        return;
                    sdbus::ObjectPath        path;
                    std::vector<std::string> gone;
                    try {
                        m >> path >> gone;
                    } catch (...) { return; }
                    for (const auto& I : gone) {
                        if (I == ADAPTER) {
                            adapters.erase(path);
                            pickAdapter();
                        } else if (I == DEVICE)
                            devices.erase(path);
                        else if (I == BATTERY)
                            if (const auto IT = devices.find(path); IT != devices.end())
                                IT->second.battery = -1;
                    }
                    publishBluetooth();
                },
                [](sdbus::Message) {}, sdbus::return_slot);
            propsMatch = B.conn()->addMatchAsync(
                "type='signal',sender='org.bluez',path_namespace='/org/bluez',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
                [](sdbus::Message m) {
                    if (compositorShuttingDown())
                        return;
                    std::string iface;
                    TProps      changed;
                    try {
                        m >> iface >> changed;
                    } catch (...) { return; }
                    const char* PATH = m.getPath();
                    if (!PATH)
                        return;
                    if (iface == ADAPTER)
                        applyAdapter(PATH, changed);
                    else if (iface == DEVICE) {
                        if (!applyDevice(PATH, changed))
                            readSoon();
                    } else if (iface == BATTERY)
                        applyBattery(PATH, changed);
                    else
                        return; // a transport's volume and the like: audio.cpp's
                    publishBluetooth();
                },
                [](sdbus::Message) {}, sdbus::return_slot);
            // a new bluetoothd holds nothing of the old one's: start over
            ownerMatch = B.conn()->addMatchAsync(
                "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.bluez'",
                [](sdbus::Message) {
                    if (compositorShuttingDown())
                        return;
                    ++gen;
                    reading = readAgain = false;
                    adapters.clear();
                    devices.clear();
                    pickAdapter();
                    publishBluetooth();
                    readSoon();
                },
                [](sdbus::Message) {}, sdbus::return_slot);
            B.pollSoon();
        } catch (...) {}
        readTree();
    }

    static void forget() {
        ++gen; // a late reply finds itself outdated
        reading = readAgain = false;
        pendingRead.reset();
        pendingDoor.reset();
        wantPower.reset();
        devices.clear(); // their proxies with them
        adapters.clear();
        adapterPath.clear();
        adapterProxy.reset();
        root.reset();
        addedMatch   = {};
        removedMatch = {};
        propsMatch   = {};
        ownerMatch   = {};
    }

    void bluetoothBusDropped() {
        forget();
        publishBluetooth();
    }

    void bluetoothExit() {
        forget();
        if (sinkSettle && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(sinkSettle);
        sinkSettle.reset();
    }

#ifdef AWESOME_GATE
    // "none" without an adapter; else "on|off name=state[battery] ..." (state:
    // c connected, p connecting, n neither), as published
    std::string bluetoothState() {
        const auto& B = Controls::state().bluetooth;
        if (!B.present)
            return "none";
        std::string out = B.powered ? "on" : "off";
        for (const auto& D : B.devices)
            out += " " + D.name + "=" + (D.connecting ? "p" : D.connected ? "c" : "n") + (D.battery >= 0 ? std::to_string(D.battery) : "");
        return out;
    }
#endif

} // namespace NAwesome::System
