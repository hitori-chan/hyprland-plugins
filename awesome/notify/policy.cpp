// awesome/notify/policy.cpp — the card menu's persisted switches: an app's
// banners and sounds, a chat's mute. The rows live in the StateStore (the
// documented seam for anything persisted); this unit is their one reader
// and writer.
#include "model.hpp"

#include "../core/state.hpp"

#include <cstdio>
#include <ctime>

namespace NAwesome::Notify::Policy {

    static AppState& state() {
        return StateStore::inst().data();
    }

    static int64_t now() {
        return (int64_t)std::time(nullptr);
    }

    // The store's key rules (bounded; no tab, CR, LF or NUL): a key that
    // breaks them is kept by its hash — a chat id is the sender's free text.
    static std::string storeKey(std::string raw) {
        if (validStoreKey(raw))
            return raw;
        uint64_t h = 1469598103934665603ull; // FNV-1a
        for (const unsigned char C : raw) {
            h ^= C;
            h *= 1099511628211ull;
        }
        char buf[24];
        std::snprintf(buf, sizeof buf, "#%016llx", (unsigned long long)h);
        return buf;
    }

    // a chat is its app and its conversation id — the title for a sender
    // that names no id (one card per chat either way)
    std::string chatKey(const SNotif& n) {
        if (!n.conversation || n.appKey.empty())
            return {};
        const auto& CHAT = n.conversationId.empty() ? n.conversationTitle : n.conversationId;
        return CHAT.empty() ? std::string{} : storeKey(n.appKey + '\x1f' + CHAT);
    }

    std::string appLabel(const SNotif& n) {
        return storeLabel(n.appName.empty() ? n.appKey : n.appName);
    }

    std::string chatLabel(const SNotif& n) {
        return storeLabel(n.conversationTitle.empty() ? n.summary : n.conversationTitle);
    }

    int appFlags(const std::string& appKey) {
        if (appKey.empty())
            return 0;
        const auto* F = state().notifyApp.find(storeKey(appKey));
        return F ? F->value : 0;
    }

    void setAppFlag(const std::string& appKey, int flag, bool on, std::string_view label) {
        if (appKey.empty())
            return;
        const auto  KEY = storeKey(appKey);
        auto&       M   = state().notifyApp;
        const auto* WAS = M.find(KEY);
        const int   OLD = WAS ? WAS->value : 0, NEXT = on ? (OLD | flag) : (OLD & ~flag);
        if (NEXT == OLD)
            return;
        if (NEXT == 0) // an app back to the defaults keeps no row
            M.eraseIf([&](const std::string& k, const SLabeled<int>&) { return k == KEY; });
        else
            M.put(KEY, {NEXT, label.empty() && WAS ? WAS->label : storeLabel(label)});
        StateStore::inst().dirty();
    }

    int64_t mutedUntil(const std::string& chat) {
        if (chat.empty())
            return 0;
        const auto* T = state().notifyMute.find(chat);
        return T && T->value > now() ? T->value : 0;
    }

    void prune() {
        const auto NOW     = now();
        bool       dropped = false;
        state().notifyMute.eraseIf([&](const std::string&, const SLabeled<int64_t>& t) { return t.value <= NOW && (dropped = true); });
        if (dropped)
            StateStore::inst().dirty();
    }

    void mute(const std::string& chat, int64_t until, std::string_view label) {
        if (chat.empty())
            return;
        auto& M = state().notifyMute;
        if (until > now()) {
            const auto* WAS = M.find(chat);
            M.put(chat, {until, label.empty() && WAS ? WAS->label : storeLabel(label)});
        } else
            M.eraseIf([&](const std::string& k, const SLabeled<int64_t>&) { return k == chat; });
        StateStore::inst().dirty();
        prune(); // every write sweeps the mutes that ended
    }

    std::vector<SSilenced> silenced() {
        prune();
        std::vector<SSilenced> out;
        const auto&            APPS  = state().notifyApp;
        const auto&            MUTES = state().notifyMute;
        for (auto it = APPS.end(); it != APPS.begin() && out.size() < MAX_SILENCED;) {
            --it;
            out.push_back({it->first, it->second.label, it->second.value, 0});
        }
        for (auto it = MUTES.end(); it != MUTES.begin() && out.size() < MAX_SILENCED;) {
            --it;
            out.push_back({it->first, it->second.label, 0, it->second.value});
        }
        return out;
    }

    void restore(const SSilenced& s) {
        if (s.until > 0)
            mute(s.key, 0);
        else
            setAppFlag(s.key, BANNERS_OFF | SOUNDS_OFF, false);
    }

    bool bannersOff(const SNotif& n) {
        return n.urgency < 2 && ((appFlags(n.appKey) & BANNERS_OFF) || mutedUntil(chatKey(n)) > 0);
    }

    bool soundsOff(const SNotif& n) {
        return n.urgency < 2 && ((appFlags(n.appKey) & SOUNDS_OFF) || mutedUntil(chatKey(n)) > 0);
    }

} // namespace NAwesome::Notify::Policy
