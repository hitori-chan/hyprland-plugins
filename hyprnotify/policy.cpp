// hyprnotify/policy.cpp — the rules the USER sets, and the only state in the
// plugin that outlives the session.
//
// Until now the whole policy vocabulary was one global switch: DND on, DND
// off. Every other implementation has more — dunst has rules, mako has
// criteria, swaync has per-app filtering, Android has channels — because
// "not this app" and "not right now" are different wishes, and the second
// one does not solve the first. Two notches cover almost all of it:
//
//   SILENCED (per app)          the app's cards stop taking the screen: no
//                               banner, no sound, straight to the shade, and
//                               ranked down with the quiet ones. Android's
//                               "Silent"; dunst's skip_display. Critical
//                               still punches through, exactly as it does
//                               through DND — silencing an app is not
//                               agreeing to miss an alarm.
//   PRIORITY (per conversation) the opposite wish, and Android's priority
//                               conversations: this chat sorts above every
//                               other card but a critical one, and its badge
//                               wears the ring AOSP keeps hidden behind
//                               visibility=gone until you mark someone.
//
// Both are keyed by what the shade already groups by, so nothing here is a
// per-app branch in code — it is per-app state the user typed with a click.
// Silence keys on the app; priority keys on the app plus the sender, because
// one chat app carries many people.

#include "common/persist.hpp"

#include "hyprnotify.hpp"

namespace NHyprnotify::Policy {

    static std::set<std::string> s_silenced; // app keys
    static std::set<std::string> s_priority; // app key + US + sender

    static std::filesystem::path storePath() {
        return NHyprCommon::statePath("hyprnotify", "policy.tsv");
    }

    // One rule per line: a verb, a tab, the key. Keys are user-facing strings
    // (app names, chat titles) so they may hold anything but a tab or a
    // newline — the two characters the format spends. A line that is neither
    // verb is skipped rather than fatal: this file is editable state, and a
    // hostile one must not take the session down with it.
    static void load() {
        s_silenced.clear();
        s_priority.clear();
        std::ifstream f(storePath());
        std::string   line;
        while (std::getline(f, line)) {
            if (line.size() < 3 || line[1] != '\t')
                continue;
            const auto KEY = line.substr(2);
            if (KEY.empty())
                continue;
            if (line[0] == 's')
                s_silenced.insert(KEY);
            else if (line[0] == 'p')
                s_priority.insert(KEY);
        }
    }

    static void save() {
        std::string out;
        for (const auto& K : s_silenced)
            out += "s\t" + K + "\n";
        for (const auto& K : s_priority)
            out += "p\t" + K + "\n";
        NHyprCommon::writeAtomic(storePath(), out);
    }

    static NHyprCommon::CSaver s_saver{save};

    // the sender's own line in the store; a tab would split the row, and the
    // summary is the one key half that arrives from the wire
    static std::string convKey(const std::string& appKey, const std::string& sender) {
        std::string k = appKey + "\x1f" + sender;
        for (auto& c : k)
            if (c == '\t' || c == '\n')
                c = ' ';
        return k;
    }

    bool silenced(const std::string& appKey) {
        return !appKey.empty() && s_silenced.contains(appKey);
    }

    bool priority(const std::string& appKey, const std::string& sender) {
        return !appKey.empty() && s_priority.contains(convKey(appKey, sender));
    }

    // A rule change is retroactive: the cards already in the shade re-rank
    // and re-badge under it, which is the only reading of "silence this app"
    // that isn't a lie about the six of its cards you are looking at.
    void toggleSilence(const std::string& appKey) {
        if (appKey.empty())
            return;
        if (const auto IT = s_silenced.find(appKey); IT != s_silenced.end())
            s_silenced.erase(IT);
        else
            s_silenced.insert(appKey);
        s_saver.dirty();
        notifChanged();
        Bus::emitStateSoon();
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
        // the paint reads the flag, not the store — ASSIGN it rather than
        // flipping, so a card whose flag ever drifted lands back in step
        for (const auto& N : notifs)
            if (N->appKey == appKey && N->summary == sender)
                N->priority = ON;
        s_saver.dirty();
        notifChanged();
        Bus::emitStateSoon();
    }

    // "silenced:a,b priority:c,d" — the debug line, and what the gate reads
    std::string stateString() {
        std::string out = "silenced:" + std::to_string(s_silenced.size());
        for (const auto& K : s_silenced)
            out += " s=" + K;
        out += " priority:" + std::to_string(s_priority.size());
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
        s_silenced.clear();
        s_priority.clear();
    }

} // namespace NHyprnotify::Policy
