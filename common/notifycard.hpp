// common/notifycard.hpp — the fire-and-forget org.freedesktop.Notifications
// Notify send, shared by the plugins that RAISE cards (hyprbar's battery
// alerts, hyprosd's OSD, hyprpad's touchpad feedback). Each caller keeps its
// OWN proxy on its OWN bus link — the daemon's API is the bus name, never
// its symbols — this is just the identical send plumbing in one place.
// Never called from a render or input emission: the link's pollSoon flushes
// from the event loop.
#pragma once

#include "busclient.hpp"

#include <sdbus-c++/sdbus-c++.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace NHyprCommon {

    struct SNotifyCard {
        std::string app      = "osd";
        uint32_t    id       = 0;
        std::string icon;
        std::string summary;
        std::string body;
        uint8_t     urgency  = 0;
        int32_t     timeoutMs = -1; // positive = the card's life; else the daemon default
        bool        osd      = false; // x-hyprnotify-osd hint: the OSD band (no sound, no shade row)
        int32_t     value    = -1;    // `value` hint: the progress bar; -1 = none
    };

    inline void notifyCard(CBusLink& link, std::unique_ptr<sdbus::IProxy>& proxy, SNotifyCard c) {
        if (!link.conn())
            return;
        try {
            if (!proxy)
                proxy = sdbus::createProxy(*link.conn(), sdbus::ServiceName{"org.freedesktop.Notifications"}, sdbus::ObjectPath{"/org/freedesktop/Notifications"});
            std::map<std::string, sdbus::Variant> hints{{"urgency", sdbus::Variant{c.urgency}}};
            if (c.osd)
                hints.emplace("x-hyprnotify-osd", sdbus::Variant{true});
            if (c.value >= 0)
                hints.emplace("value", sdbus::Variant{c.value});
            proxy->callMethodAsync("Notify")
                .onInterface("org.freedesktop.Notifications")
                .withArguments(std::move(c.app), c.id, std::move(c.icon), std::move(c.summary), std::move(c.body), std::vector<std::string>{},
                               std::move(hints), c.timeoutMs > 0 ? c.timeoutMs : -1)
                .uponReplyInvoke([](std::optional<sdbus::Error>, uint32_t) {});
            link.pollSoon();
        } catch (...) {} // broker gone: teardown is already pending, drop the card
    }

} // namespace NHyprCommon
