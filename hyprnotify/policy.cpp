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
// Silence keys on the app; priority keys on the app plus the stable
// conversation ID, because one chat app carries many conversations.

#include "common/persist.hpp"

#include "hyprnotify.hpp"

#include <charconv>

namespace NHyprnotify::Policy {

    // app key -> when the silence lifts, as a wall-clock epoch second. The
    // current hold surface writes 0 (persistent Silent); nonzero deadlines
    // remain readable for policy files written by older releases. Wall clock
    // makes those legacy deadlines elapse across suspend and relog.
    static std::map<std::string, int64_t> s_silenced;
    static std::set<std::string>          s_priority; // app key + US + conversation ID

    // This is editable, client-influenced state read while the compositor is
    // starting. Keep every dimension finite: malformed policy must never turn
    // startup or its diagnostic command into unbounded work.
    static constexpr size_t MAX_POLICY_FILE_BYTES  = 64 * 1024;
    static constexpr size_t MAX_POLICY_LINE_BYTES  = 1024;
    static constexpr size_t MAX_POLICY_ROWS        = 512;
    static constexpr size_t MAX_POLICY_KEY_BYTES   = 512;
    // A persistent-silence row has a verb, two separators, a newline, and a
    // 19-digit positive int64 expiry in addition to its key. Admission must
    // fit even that worst case or save() could create a file load() rejects.
    static constexpr size_t MAX_POLICY_RULES       = MAX_POLICY_FILE_BYTES / (2 + MAX_POLICY_KEY_BYTES + 1 + 19 + 1);
    static constexpr size_t MAX_POLICY_STATE_BYTES = 8192;

    static int64_t                        nowEpoch() {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    static bool expired(int64_t until) {
        return until != 0 && until <= nowEpoch();
    }
    static std::filesystem::path storePath() {
        return NHyprCommon::statePath("hyprnotify", "policy.tsv");
    }

    static bool validKey(std::string_view key) {
        return !key.empty() && key.size() <= MAX_POLICY_KEY_BYTES && key.find_first_of(std::string_view{"\t\r\n\0", 4}) == std::string_view::npos;
    }

    static bool parseExpiry(std::string_view value, int64_t& until) {
        if (value.empty() || !std::ranges::all_of(value, [](unsigned char c) { return std::isdigit(c); }))
            return false;
        const auto [END, ERROR] = std::from_chars(value.data(), value.data() + value.size(), until);
        return ERROR == std::errc{} && END == value.data() + value.size();
    }

    // One rule per line: a verb, a tab, the key, and for a silence an optional
    // tab + expiry. Keys originate in client-supplied app/conversation fields,
    // so they may hold anything but a tab or a newline — the two characters the
    // format spends. A line that is neither verb is skipped rather than fatal:
    // this file is editable state, and a hostile one must not take the session
    // down with it. A silence with no expiry field is persistent.
    static void load() {
        s_silenced.clear();
        s_priority.clear();
        std::error_code statusError;
        const auto      path = storePath();
        if (!std::filesystem::is_regular_file(path, statusError))
            return;
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return;

        // Read one byte beyond the cap: reject an oversized file as a unit
        // instead of retaining an arbitrary prefix while it is being edited.
        std::string contents(MAX_POLICY_FILE_BYTES + 1, '\0');
        f.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        const auto READ = static_cast<size_t>(f.gcount());
        if (READ > MAX_POLICY_FILE_BYTES)
            return;
        contents.resize(READ);

        size_t begin = 0;
        size_t rows  = 0;
        while (begin < contents.size() && rows++ < MAX_POLICY_ROWS && s_silenced.size() + s_priority.size() < MAX_POLICY_RULES) {
            const auto END = contents.find('\n', begin);
            const auto LEN = (END == std::string::npos ? contents.size() : END) - begin;
            const auto advance = [&]() {
                begin = END == std::string::npos ? contents.size() : END + 1;
            };
            const std::string_view line{contents.data() + begin, LEN};
            if (LEN > MAX_POLICY_LINE_BYTES || line.find('\0') != std::string_view::npos || line.size() < 3 || line[1] != '\t') {
                advance();
                continue;
            }

            auto    rest  = line.substr(2);
            int64_t until = 0;
            if (line[0] == 's') {
                if (const auto TAB = rest.rfind('\t'); TAB != std::string_view::npos) {
                    const auto NUM = rest.substr(TAB + 1);
                    if (!parseExpiry(NUM, until)) {
                        advance();
                        continue;
                    }
                    rest = rest.substr(0, TAB);
                }
            }
            if (!validKey(rest)) {
                advance();
                continue;
            }
            if (line[0] == 's') {
                if (!expired(until)) // a rule that lapsed while we were away never loads
                    s_silenced.emplace(std::string{rest}, until);
            } else if (line[0] == 'p')
                s_priority.emplace(rest);
            advance();
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
    // — an app name, the desktop-entry hint, a conversation ID — so one holding
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

    // US separates two independently scrubbed halves. Scrub a client-supplied
    // US too, or distinct app/conversation pairs could alias the same key.
    static std::string convKey(const std::string& appKey, const std::string& conversationId) {
        auto APP = storeKey(appKey), CONVERSATION = storeKey(conversationId);
        std::ranges::replace(APP, '\x1f', ' ');
        std::ranges::replace(CONVERSATION, '\x1f', ' ');
        return APP + "\x1f" + CONVERSATION;
    }

    static void refreshDerivedSections(const std::string& appKey, bool appSilent) {
        for (const auto& N : notifs)
            if (N->appKey == appKey && !N->sectionExplicit)
                N->section = appSilent || N->urgency == 0 ? "silent" : "alerting";
    }

    static bool pruneExpired() {
        const auto BEFORE = s_silenced.size();
        std::erase_if(s_silenced, [](const auto& E) { return expired(E.second); });
        if (s_silenced.size() == BEFORE)
            return false;
        for (const auto& N : notifs)
            if (!N->sectionExplicit)
                N->section = s_silenced.contains(storeKey(N->appKey)) || N->urgency == 0 ? "silent" : "alerting";
        s_saver.dirty();
        return true;
    }

    // Lazy expiry: arrivals and event-loop warm passes drop lapsed rules. No
    // timer earns its keep for this; draw-side count/ranking reads stay pure.
    bool silenced(const std::string& appKey) {
        if (appKey.empty())
            return false;
        const auto APP = storeKey(appKey);
        if (!validKey(APP))
            return false;
        const auto IT = s_silenced.find(APP);
        if (IT == s_silenced.end())
            return false;
        if (!expired(IT->second))
            return true;
        s_silenced.erase(IT);
        refreshDerivedSections(appKey, false);
        s_saver.dirty();
        return false;
    }

    int64_t silenceRemaining(const std::string& appKey) {
        if (appKey.empty())
            return -1;
        const auto APP = storeKey(appKey);
        if (!validKey(APP))
            return -1;
        const auto IT = s_silenced.find(APP);
        if (IT == s_silenced.end())
            return -1;
        if (expired(IT->second)) {
            s_silenced.erase(IT);
            refreshDerivedSections(appKey, false);
            s_saver.dirty();
            return -1;
        }
        if (IT->second == 0)
            return 0;
        return std::max<int64_t>(0, IT->second - nowEpoch());
    }

    // how many rules are in force — the footer's count, so a silence you set
    // once is never invisible again
    size_t silencedCount() {
        return s_silenced.size();
    }

    void refreshExpired() {
        pruneExpired();
    }

    bool priority(const std::string& appKey, const std::string& conversationId) {
        if (appKey.empty() || conversationId.empty())
            return false;
        const auto CONV = convKey(appKey, conversationId);
        return validKey(CONV) && s_priority.contains(CONV);
    }

    eAlertingMode mode(const std::string& appKey, const std::string& conversationId, bool conversation) {
        if (silenced(appKey))
            return eAlertingMode::SILENT;
        if (conversation && priority(appKey, conversationId))
            return eAlertingMode::PRIORITY;
        return eAlertingMode::DEFAULT;
    }

    // The hold surface stages one Android-style channel mode and commits it on
    // Done. Linux notifications do not expose Android channel IDs, so Silent is
    // necessarily per application while Priority remains per conversation.
    // For a conversation target, update both maps as one transaction so the
    // target cannot remain app-silent and conversation-priority by accident.
    bool setMode(const std::string& appKey, const std::string& conversationId, bool conversation, eAlertingMode selected) {
        if (appKey.empty())
            return false;

        const auto APP     = storeKey(appKey);
        const auto CONV    = conversation && !conversationId.empty() ? convKey(appKey, conversationId) : std::string{};
        if (!validKey(APP) || (!CONV.empty() && !validKey(CONV)))
            return false;
        const bool DROP_SILENCE  = selected != eAlertingMode::SILENT && s_silenced.contains(APP);
        const bool DROP_PRIORITY = !CONV.empty() && selected != eAlertingMode::PRIORITY && s_priority.contains(CONV);
        const bool ADD_SILENCE   = selected == eAlertingMode::SILENT && !s_silenced.contains(APP);
        const bool ADD_PRIORITY  = selected == eAlertingMode::PRIORITY && !CONV.empty() && !s_priority.contains(CONV);
        const size_t NEXT_RULES  = s_silenced.size() + s_priority.size() - (size_t)DROP_SILENCE - (size_t)DROP_PRIORITY + (size_t)ADD_SILENCE + (size_t)ADD_PRIORITY;
        if (NEXT_RULES > MAX_POLICY_RULES)
            return false;
        bool       changed = false;

        if (selected == eAlertingMode::SILENT) {
            const auto IT = s_silenced.find(APP);
            changed |= IT == s_silenced.end() || IT->second != 0;
            s_silenced.insert_or_assign(APP, 0);
        } else
            changed |= s_silenced.erase(APP) > 0;

        if (!CONV.empty()) {
            if (selected == eAlertingMode::PRIORITY)
                changed |= s_priority.insert(CONV).second;
            else
                changed |= s_priority.erase(CONV) > 0;
        }

        const bool ON = selected == eAlertingMode::PRIORITY;
        if (!CONV.empty())
            for (const auto& N : notifs)
                if (N->appKey == appKey && N->conversationId == conversationId)
                    N->priority = ON;
        refreshDerivedSections(appKey, selected == eAlertingMode::SILENT);

        if (!changed)
            return false;
        s_saver.dirty();
        Bus::emitStateSoon();
        return true;
    }

    bool setSilenceUntil(const std::string& appKey, int64_t seconds) {
        if (appKey.empty())
            return false;

        const auto APP = storeKey(appKey);
        if (!validKey(APP))
            return false;

        const size_t NEXT_RULES = s_silenced.size() + (s_silenced.contains(APP) ? 0 : 1);
        if (NEXT_RULES > MAX_POLICY_RULES)
            return false;

        const int64_t UNTIL = seconds == 0 ? 0 : nowEpoch() + seconds;
        const auto    IT    = s_silenced.find(APP);
        const bool    CHANGED = IT == s_silenced.end() || IT->second != UNTIL;

        if (!CHANGED)
            return false;

        s_silenced.insert_or_assign(APP, UNTIL);
        refreshDerivedSections(appKey, true);
        s_saver.dirty();
        Bus::emitStateSoon();
        return true;
    }

    void unsilenceAll() {
        if (s_silenced.empty())
            return;
        s_silenced.clear();
        for (const auto& N : notifs)
            if (!N->sectionExplicit)
                N->section = N->urgency == 0 ? "silent" : "alerting";
        s_saver.dirty();
        notifChanged();
        Bus::emitStateSoon();
    }

    // "silenced:a,b priority:c,d" — the debug line, and what the gate reads.
    // A legacy timed rule prints its remaining seconds so diagnostics preserve
    // the on-disk distinction until that rule expires.
    std::string stateString() {
        refreshExpired();
        const auto  NOW = nowEpoch();
        std::string out = "silenced:" + std::to_string(silencedCount());
        const auto append = [&](const std::string& value, size_t limit) {
            if (out.size() + value.size() <= limit) {
                out += value;
                return true;
            }
            if (out.size() + 3 <= limit)
                out += "...";
            return false;
        };
        for (const auto& [K, UNTIL] : s_silenced)
            if (!append(" s=" + K + (UNTIL ? "+" + std::to_string(UNTIL - NOW) : ""), MAX_POLICY_STATE_BYTES / 2))
                break;
        out += " priority:" + std::to_string(s_priority.size());
        for (const auto& K : s_priority) {
            auto k = K;
            std::ranges::replace(k, '\x1f', '/');
            if (!append(" p=" + k, MAX_POLICY_STATE_BYTES))
                break;
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
