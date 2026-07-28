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

    // app key -> when the silence lifts, as a wall-clock epoch second. 0 is
    // "always", the rule that never lifts. Wall clock and not steady time
    // because this outlives the session it was set in: a suspend, a reboot
    // and a week off all have to count against "mute for an hour".
    static std::map<std::string, int64_t> s_silenced;
    static std::set<std::string>          s_priority; // app key + US + sender

    static int64_t                        nowEpoch() {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    static bool expired(int64_t until) {
        return until != 0 && until <= nowEpoch();
    }
    // "today" is not a duration, it is a time of day: iOS's "Mute for Today"
    // lasts until tomorrow morning, not for a rolling 24 hours.
    static int64_t untilTomorrow() {
        const auto T = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm    lt{};
        localtime_r(&T, &lt);
        const int64_t ELAPSED = (int64_t)lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
        return std::max<int64_t>(86400 - ELAPSED, 60);
    }

    static std::filesystem::path storePath() {
        return NHyprCommon::statePath("hyprnotify", "policy.tsv");
    }

    // One rule per line: a verb, a tab, the key, and for a silence an optional
    // tab + expiry. Keys are user-facing strings (app names, chat titles) so
    // they may hold anything but a tab or a newline — the two characters the
    // format spends. A line that is neither verb is skipped rather than fatal:
    // this file is editable state, and a hostile one must not take the session
    // down with it. A silence with no expiry field is one written before they
    // existed, and means always.
    static void load() {
        s_silenced.clear();
        s_priority.clear();
        std::ifstream f(storePath());
        std::string   line;
        while (std::getline(f, line)) {
            if (line.size() < 3 || line[1] != '\t')
                continue;
            auto    rest  = line.substr(2);
            int64_t until = 0;
            if (line[0] == 's') {
                if (const auto TAB = rest.rfind('\t'); TAB != std::string::npos) {
                    // a trailing field that is not a number is part of the key
                    // (nothing scrubs tabs out of a hand-edited file)
                    const auto NUM = rest.substr(TAB + 1);
                    if (!NUM.empty() && std::ranges::all_of(NUM, [](unsigned char c) { return std::isdigit(c); })) {
                        try {
                            until = std::stoll(NUM);
                            rest  = rest.substr(0, TAB);
                        } catch (...) {}
                    }
                }
            }
            if (rest.empty())
                continue;
            if (line[0] == 's') {
                if (!expired(until)) // a rule that lapsed while we were away never loads
                    s_silenced.emplace(rest, until);
            } else if (line[0] == 'p')
                s_priority.insert(rest);
        }
    }

    static void save() {
        std::string out;
        for (const auto& [K, UNTIL] : s_silenced)
            if (!expired(UNTIL))
                out += "s\t" + K + "\t" + std::to_string(UNTIL) + "\n";
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

    // Lazy expiry: a lapsed rule is dropped the next time anyone asks about
    // it, which is every arrival and every paint. No timer earns its keep for
    // this — nothing has to HAPPEN at the moment a silence lifts, the app
    // simply banners again the next time it speaks.
    bool silenced(const std::string& appKey) {
        if (appKey.empty())
            return false;
        const auto IT = s_silenced.find(storeKey(appKey));
        if (IT == s_silenced.end())
            return false;
        if (!expired(IT->second))
            return true;
        s_silenced.erase(IT);
        s_saver.dirty();
        return false;
    }

    // how many rules are in force — the footer's count, so a silence you set
    // once is never invisible again
    size_t silencedCount() {
        std::erase_if(s_silenced, [](const auto& E) { return expired(E.second); });
        return s_silenced.size();
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
        const auto KEY = storeKey(appKey);
        if (const auto IT = s_silenced.find(KEY); IT != s_silenced.end())
            s_silenced.erase(IT);
        else
            s_silenced.emplace(KEY, 0); // the quick toggle is the permanent one
        s_saver.dirty();
        notifChanged();
        Bus::emitStateSoon();
    }

    // The timed variants iOS puts first: "Mute for 1 Hour", "Mute for Today".
    // Permanent is still available, but it stops being the only thing a click
    // can mean — it is the choice people regret, and the one whose rule then
    // sits in a file nobody looks at. seconds 0 = always, < 0 = today; a
    // re-silence replaces the standing rule rather than stacking beside it.
    void silenceFor(const std::string& appKey, int64_t seconds) {
        if (appKey.empty())
            return;
        if (seconds < 0)
            seconds = untilTomorrow();
        s_silenced[storeKey(appKey)] = seconds > 0 ? nowEpoch() + seconds : 0;
        s_saver.dirty();
        notifChanged();
        Bus::emitStateSoon();
    }

    void unsilence(const std::string& appKey) {
        if (appKey.empty() || s_silenced.erase(storeKey(appKey)) == 0)
            return;
        s_saver.dirty();
        notifChanged();
        Bus::emitStateSoon();
    }

    void unsilenceAll() {
        if (s_silenced.empty())
            return;
        s_silenced.clear();
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

    // "silenced:a,b priority:c,d" — the debug line, and what the gate reads.
    // A timed rule prints its remaining seconds, so the gate can tell "muted
    // for an hour" from "muted for good" without reading the clock itself.
    std::string stateString() {
        const auto  NOW = nowEpoch();
        std::string out = "silenced:" + std::to_string(silencedCount());
        for (const auto& [K, UNTIL] : s_silenced)
            out += " s=" + K + (UNTIL ? "+" + std::to_string(UNTIL - NOW) : "");
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
