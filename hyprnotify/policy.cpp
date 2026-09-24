// hyprnotify/policy.cpp — the rules the USER sets, and the only state in the
// plugin that outlives the session.
//
// One notch: PRIORITY (per conversation), Android's priority conversations:
// this chat sorts above every other card but a critical one, and its badge
// wears the ring AOSP keeps hidden behind visibility=gone until you mark
// someone. Keyed on the app plus the sender, because one chat app carries
// many people — so nothing here is a per-app branch in code; it is per-chat
// state the user typed with a click.

#include "common/persist.hpp"

#include "hyprnotify.hpp"

namespace NHyprnotify::Policy {

    static std::set<std::string> s_priority; // app key + US + sender

    static std::filesystem::path storePath() {
        return NHyprCommon::statePath("hyprnotify", "policy.tsv");
    }

    // One rule per line: a verb, a tab, the key. Keys are user-facing strings
    // (app names, chat titles) so they may hold anything but a tab or a
    // newline — the two characters the format spends. A line that is neither
    // verb is skipped rather than fatal: this file is editable state, and a
    // hostile one must not take the session down with it. Lines the old
    // format wrote for per-app silences ('s') are skipped too — the feature
    // is gone, and a stored rule nobody can act on is just noise.
    static void load() {
        s_priority.clear();
        std::ifstream f(storePath());
        std::string   line;
        size_t        lines = 0;
        // the hostile-file promise, in numbers: a kilobyre a line, a few
        // thousand lines (the same shape as persist's box stores)
        while (lines < 4096 && std::getline(f, line)) {
            lines++;
            if (line.size() < 3 || line.size() > 1024 || line[1] != '\t' || line[0] != 'p')
                continue;
            const auto rest = line.substr(2);
            if (rest.empty())
                continue;
            s_priority.insert(rest);
        }
    }

    static void save() {
        std::string out;
        for (const auto& K : s_priority)
            out += "p\t" + K + "\n";
        NHyprCommon::writeAtomic(storePath(), out);
    }

    static NHyprCommon::CSaver s_saver{save};

    // The format spends exactly two characters: a tab between the verb and
    // its key, a newline between rows. EVERY key here arrives from the wire
    // — an app name, the desktop-entry hint, a chat's title — so one holding
    // either would write rows load() reads back as rules the user never set
    // (an app named "x\np\tsomeone" marks a conversation on its own). Scrub
    // on the way in and on the way out alike, or a stored rule would stop
    // matching the app it was set on.
    static std::string storeKey(std::string k) {
        for (auto& c : k)
            if (c == '\t' || c == '\n' || c == '\r')
                c = ' ';
        return k;
    }

    // the sender's own line in the store; US separates the two halves
    static std::string convKey(const std::string& appKey, const std::string& sender) {
        return storeKey(appKey + "\x1f" + sender);
    }

    bool priority(const std::string& appKey, const std::string& sender) {
        return !appKey.empty() && s_priority.contains(convKey(appKey, sender));
    }

    void togglePriority(const std::string& appKey, const std::string& sender) {
        if (appKey.empty() || sender.empty())
            return;
        const auto KEY = convKey(appKey, sender);
        const auto IT  = s_priority.find(KEY);
        const bool ON  = IT == s_priority.end();
        if (ON)
            s_priority.insert(KEY);
        else
            s_priority.erase(IT);
        // The paint reads the flag, not the store — RE-DERIVE it for the
        // app's cards, mirroring arrive's key choice: a conversation card
        // keys on its conversation-id (the summary as fallback), a plain
        // card on the summary, and a mark only ever exists on a
        // conversation. A summary-only match would leave a visible
        // conversation-id card unranked and unringed until its next replace.
        for (const auto& N : notifs)
            if (N->appKey == appKey)
                N->priority = (N->conversation || !N->conversationId.empty()) && (!N->conversationId.empty() ? (s_priority.contains(convKey(appKey, N->conversationId)) || s_priority.contains(convKey(appKey, N->summary)))
                                                                                                              : s_priority.contains(convKey(appKey, N->summary)));
        s_saver.dirty();
        notifChanged();
        Bus::emitStateSoon();
    }

    // how many marks are in force — the debug line, so a mark you set once
    // is never invisible again
    size_t priorityCount() {
        return s_priority.size();
    }

    // "priority:N p=app/sender" — the debug line, and what the gate reads.
    std::string stateString() {
        std::string out = "priority:" + std::to_string(priorityCount());
        for (const auto& K : s_priority) {
            auto k = K;
            std::ranges::replace(k, '\x1f', '/');
            out += " p=" + k;
        }
        return out;
    }

    void init() {
        load();
    }

    void exit() {
        s_saver.flush(); // the coalesced write never runs at compositor exit
        s_priority.clear();
    }

} // namespace NHyprnotify::Policy
