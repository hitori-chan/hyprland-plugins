// hyprnotify/model.cpp — the cards themselves: what one is, how it arrives,
// how long it lives and when it stops being visible. Everything a
// notification DOES between the wire and the glass is decided here.
//
// The split from bus.cpp is by ownership, not by layer: bus.cpp owns the
// connection (the vtable, the signals, the name), parse.cpp owns the
// untrusted payload, and this unit owns the list. The one thing the model
// reaches back over the bus for is the outbound half of a card's life —
// NotificationClosed and the bell's State — which it does through the two
// emit* entry points and nothing else.
//
// RESIDENCY is the model's central rule: a card's BANNER and the card
// itself have separate lifetimes. Expiry takes the banner and leaves the
// card in the shade; only a dismissal, an action or the cap takes the card.
// Everything that opts out of that (the transient hint, progress/OSD blips)
// is one predicate — vanishes() — and it MUST stay the same set everywhere,
// or a suppressed banner strands a card no sweep will ever reach.

#include "common/queries.hpp"

#include "hyprnotify.hpp"

namespace NHyprnotify {

    std::vector<SP<SNotif>> notifs;

    namespace Model {
        static SP<CEventLoopTimer> expiry;
        static uint32_t            nextId     = 1;
        static bool                suspended  = false; // DND
        static uint32_t            heldBanner = 0;     // the popup under the pointer: its countdown is paused

        SP<SNotif>                 byId(uint32_t id) {
            for (const auto& N : notifs)
                if (N->id == id)
                    return N;
            return nullptr;
        }

        // Cards that OPT OUT of residency: they vanish on expiry and never
        // park as a shade row. So their banners must never popup-coalesce — a
        // suppressed banner would strand them, since the expiry sweep only
        // touches banners. transient hint, progress/value, the OSD band.
        // This set MUST stay identical to what the expiry timer vanishes.
        bool vanishes(const SP<SNotif>& n) {
            return n->transient || n->progress >= 0 || inOsdBand(n->id);
        }

        // The one timer serves three clocks: a banner running out, a snoozed
        // card coming back, and an undo window closing. All are deadlines on
        // the same list, so the next wakeup is simply the nearest of any kind.
        void rearmExpiry() {
            if (!expiry)
                return;
            const auto NOW  = Time::steadyNow();
            int64_t    next = -1;
            const auto CONSIDER = [&](const Time::steady_tp& when) {
                // clamp before comparing: -1 is the "none" sentinel, and an
                // overdue card's negative remaining time must still win
                const auto MS = std::max<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(when - NOW).count(), 1);
                if (next < 0 || MS < next)
                    next = MS;
            };
            for (const auto& N : notifs) {
                if (N->snoozed) {
                    CONSIDER(N->snoozeUntil);
                    if (N->snoozeConfirmUntil != Time::steady_tp{})
                        CONSIDER(N->snoozeConfirmUntil); // the row must leave on its own
                    continue;
                }
                if (!N->banner || N->timeoutMs <= 0 || N->waiting || N->id == heldBanner)
                    continue;
                CONSIDER(N->deadline);
            }
            if (next < 0)
                expiry->updateTimeout(std::nullopt);
            else
                expiry->updateTimeout(std::chrono::milliseconds(std::max<int64_t>(next, 1)));
        }

        // A banner must not expire out from under the pointer reading it. The
        // hovered card's clock stops (the sweep and the rearm both skip it),
        // and leaving RESTARTS it rather than resuming the sliver that was
        // left — Android's heads-up does the same when a touch ends: you get
        // the whole read window back, not the tail of it.
        void holdBanner(uint32_t id) {
            if (heldBanner == id)
                return;
            const auto PREV = heldBanner;
            heldBanner      = id;
            if (const auto N = PREV ? byId(PREV) : nullptr; N && N->banner && N->timeoutMs > 0 && !N->waiting)
                N->deadline = Time::steadyNow() + std::chrono::milliseconds((int64_t)N->timeoutMs);
            rearmExpiry();
        }

        std::string stateString() {
            return "center:" + std::to_string(centerVisible() ? 1 : 0) + " live:" + std::to_string(notifs.size()) + " dnd:" + std::to_string(suspended ? 1 : 0);
        }

        // the badge's truth is the shade: bannered popups + resident cards.
        // Never the DND queue (invisible until the resume), never the OSD
        // band (a volume card is an OSD, not a notification).
        std::pair<uint32_t, uint32_t> badgeCounts() {
            uint32_t live = 0, kept = 0;
            for (const auto& N : notifs) {
                if (N->waiting || N->snoozed || inOsdBand(N->id))
                    continue;
                (N->banner ? live : kept)++;
            }
            return {live, kept};
        }

        uint32_t snoozedCount() {
            return (uint32_t)std::ranges::count_if(notifs, [](const auto& N) { return N->snoozed; });
        }

        // "Remind me." Android's snooze: the card goes out of sight and comes
        // back later alerting, which is the whole point of asking. Ephemerals
        // are refused: a transient or progress card has nothing to come back
        // to, since expiry takes the card and not just its banner.
        //
        // It does NOT leave at the click. The shade replaces the notification
        // in place with its identity, duration label, and Undo before it goes —
        // which is the whole answer to "there is nothing left to click". For
        // CONFIRM_MS the card holds its slot as a one-line undo row, so the
        // only irreversible verb in the shell stops being irreversible. This
        // is not history: the card never left, and once the window passes it
        // is gone the same way it always was.
        inline constexpr int64_t CONFIRM_MS = 6000;

        std::string              snoozeLabel(const SP<SNotif>& n) {
            const int64_t S = n->snoozeSecs;
            if (S < 3600) {
                const int64_t M = std::max<int64_t>(S / 60, 1);
                return std::to_string(M) + " min";
            }
            const int64_t H = S / 3600;
            return std::to_string(H) + (H == 1 ? " hour" : " hours");
        }

        bool snoozeConfirming(const SP<SNotif>& n) {
            return n->snoozed && n->snoozeConfirmUntil > Time::steadyNow();
        }

        // both clocks restart together: the undo window is a fresh read of the
        // duration, not a countdown that survived the change
        static void armSnooze(const SP<SNotif>& n) {
            const auto NOW        = Time::steadyNow();
            n->snoozeUntil        = NOW + std::chrono::seconds(n->snoozeSecs);
            n->snoozeConfirmUntil = NOW + std::chrono::milliseconds(CONFIRM_MS);
        }

        static void snoozeFor(uint32_t id, int64_t seconds) {
            const auto N = byId(id);
            if (!N || N->waiting || vanishes(N))
                return;
            N->snoozed    = true;
            N->banner     = false;
            N->snoozeSecs = std::max<int64_t>(seconds, 0);
            armSnooze(N);
            notifChanged();
            rearmExpiry();
            Bus::emitStateSoon();
        }

        void snooze(uint32_t id) {
            const auto N = byId(id);
            if (!N || N->snoozed)
                return;
            snoozeFor(id, std::max<int64_t>(cfg.snoozeSeconds->value(), 0));
        }

        // Only inside the window — past it the row is gone and there is
        // nothing to have clicked.
        void snoozeUndo(uint32_t id) {
            const auto N = byId(id);
            if (!N || !snoozeConfirming(N))
                return;
            N->snoozed            = false;
            N->snoozeConfirmUntil = {};
            // it never went, so it does not come back alerting either: the
            // card resumes as the resident shade row the schedule action found it as
            notifChanged();
            rearmExpiry();
            Bus::emitStateSoon();
        }

        // The shade is the undo row's only surface: leaving it commits every
        // pending snooze rather than stranding a window nobody can see.
        void snoozeEndConfirm() {
            bool any = false;
            for (const auto& N : notifs)
                if (N->snoozed && N->snoozeConfirmUntil != Time::steady_tp{}) {
                    N->snoozeConfirmUntil = {};
                    any                   = true;
                }
            if (any)
                rearmExpiry();
        }

        std::string badgeString() {
            const auto [LIVE, KEPT] = badgeCounts();
            return "banners:" + std::to_string(LIVE) + " resident:" + std::to_string(KEPT);
        }

        // one live popup per app: does another card already hold a banner for
        // this app? A new same-app arrival then lands resident instead of
        // stacking a second popup — the shade folds the extras, the badge
        // counts them (the banner's own timeout is the cooldown window).
        static bool appHasBanner(const SP<SNotif>& self) {
            for (const auto& O : notifs)
                if (O != self && !O->waiting && O->banner && !vanishes(O) && O->appKey == self->appKey)
                    return true;
            return false;
        }

        // Every path that can make a card visible uses the same policy: DND
        // and snooze stay silent, critical cards bypass quiet/coalescing
        // policy, and ordinary cards respect the current focused monitor and
        // the app's persisted silence rule. Keeping this in one predicate
        // prevents a queued or snoozed card from waking under different rules
        // than a live arrival.
        static bool bannerEligible(const SP<SNotif>& n) {
            if (!n || n->waiting || n->snoozed)
                return false;
            // Ephemeral cards are still allowed to announce; unlike resident
            // cards they disappear on expiry instead of retreating to the
            // shade, so coalescing or policy suppression would strand their
            // intended short-lived surface.
            if (vanishes(n))
                return true;
            if (n->urgency >= 2)
                return true;
            if (suspended)
                return false;
            if (cfg.quietFullscreen->value() && NHyprCommon::fullscreenOn(Desktop::focusState() ? Desktop::focusState()->monitor() : nullptr))
                return false;
            if (cfg.coalescePopups->value() && appHasBanner(n))
                return false;
            return !Policy::silenced(n->appKey);
        }

        bool closeOne(uint32_t id, uint32_t reason) {
            const auto BEFORE = notifs.size();
            std::erase_if(notifs, [&](const auto& N) { return N->id == id; });
            if (notifs.size() == BEFORE)
                return false;
            Bus::emitClosed(id, reason);
            notifChanged();
            rearmExpiry();
            return true;
        }

        // Only what the user can SEE is sweepable. The DND queue was never
        // shown, and a snoozed card was deliberately put away — clearing the
        // shade must not quietly cancel a reminder the user asked for.
        static bool visible(const SP<SNotif>& n) {
            return !n->waiting && !n->snoozed && !inOsdBand(n->id);
        }

        void dismissAllLive() {
            const auto BEFORE = notifs.size();
            for (const auto& N : notifs)
                if (visible(N))
                    Bus::emitClosed(N->id, R_DISMISSED);
            std::erase_if(notifs, [](const auto& N) { return visible(N); });
            if (notifs.size() == BEFORE)
                return;
            notifChanged();
            rearmExpiry();
        }

        void dismissGroup(const std::string& groupKey) {
            const auto MATCHES = [&](const SP<SNotif>& N) { return visible(N) && Pixel::displayGroupKey(N->appKey, N->declaredGroupKey, N->section) == groupKey; };
            const auto BEFORE = notifs.size();
            for (const auto& N : notifs)
                if (MATCHES(N))
                    Bus::emitClosed(N->id, R_DISMISSED);
            std::erase_if(notifs, MATCHES);
            if (notifs.size() == BEFORE)
                return;
            notifChanged();
            rearmExpiry();
        }

        // Opening the center absorbs the popped stack: every bannered card
        // stands down into a parked shade row, so closing the center never
        // re-pops it. The unread count is unchanged (popped + parked both
        // count) — only the on-screen banners go. Ephemerals (transient,
        // progress/OSD) keep their banners so the expiry timer still vanishes
        // them on their own clocks; nothing here emits a close (the cards are
        // parked, not dismissed).
        void absorbPopped() {
            bool changed = false;
            for (const auto& N : notifs) {
                if (!N->banner || N->waiting)
                    continue;
                if (vanishes(N))
                    continue;
                N->banner = false;
                changed   = true;
            }
            if (changed) {
                notifChanged();
                Bus::emitStateSoon();
            }
        }

        void toggleSuspend() {
            suspended = !suspended;
            if (suspended) {
                notifChanged(); // the center's DND control lights up
                return;         // visible cards live out their timeouts; new arrivals queue
            }
            const auto NOW = Time::steadyNow();
            // newest-first, so the freshest per app takes the one popup slot
            // and the rest resume resident — the same one-per-app cap the live
            // arrival path applies, so DND-off never floods the screen
            for (const auto& N : notifs) {
                if (!N->waiting)
                    continue;
                N->waiting = false;
                N->banner  = bannerEligible(N); // never seen: resume applies the live policy
                if (N->banner && N->timeoutMs > 0)
                    N->deadline = NOW + std::chrono::milliseconds((int64_t)N->timeoutMs);
            }
            notifChanged();
            rearmExpiry();
        }

        bool suspendedNow() {
            return suspended;
        }

        // The client sent -1: critical always sticks
        // (a message that demands an answer waits on screen). Everything else
        // runs a clock and then retreats to the shade — the center is the
        // safety net now, so a normal banner need not linger. Ephemerals (low
        // urgency, the transient hint, progress/OSD blips) run the short low
        // clock; normal urgency runs timeout_normal.
        // An explicit expire_timeout never lands here.
        static float defaultTimeout(const SNotif& n) {
            if (n.urgency >= 2)
                return 0.f;
            if (n.urgency == 0 || n.transient || n.progress >= 0)
                return (float)cfg.timeoutLow->value();
            return (float)cfg.timeoutNormal->value();
        }

        static std::string conversationBody(const SNotif& n) {
            if (n.messages.empty())
                return n.body;

            std::string  out;
            const size_t START = Pixel::presentedMessageStart(n.messages);
            for (size_t i = n.messages.size(); i-- > START;) {
                const auto& M = n.messages[i];
                if (M.text.empty())
                    continue;
                std::string line;
                if (n.conversationKind == "group" && !M.senderName.empty()) {
                    line += Parse::oneLine(Parse::sanitizeMarkup(M.senderName));
                    line += ": ";
                }
                line += M.text;
                if (out.empty()) {
                    out = Parse::clipUtf8(line, Parse::MAX_BODY_BYTES);
                    continue;
                }
                if (line.size() + 1 + out.size() > Parse::MAX_BODY_BYTES)
                    break;
                out.insert(0, "\n");
                out.insert(0, line);
            }
            return out;
        }

        static void rebuildConversationParticipants(SNotif& n, int iconPx) {
            auto previous = std::move(n.participants);
            n.participants.clear();
            const auto INDICES = Pixel::latestDistinctParticipantIndices(n.messages);
            n.participants.reserve(INDICES.size());
            for (const auto INDEX : INDICES) {
                const auto&  M = n.messages[INDEX];
                SParticipant P{.key = Pixel::participantKey(M.senderId, M.senderName), .name = M.senderName, .iconSource = M.senderIcon};
                if (const auto OLD = std::ranges::find_if(previous, [&](const auto& item) { return item.key == P.key && item.name == P.name && item.iconSource == P.iconSource; });
                    OLD != previous.end()) {
                    P = std::move(*OLD);
                } else
                    P.icon = Parse::resolveImage(P.iconSource, iconPx);
                n.participants.push_back(std::move(P));
            }
        }

        // Cap the stack: the oldest non-critical goes first; only an
        // all-critical stack starts losing its oldest critical. The newest
        // card at begin() always survives (the scan stops short of it).
        static void evictOverflow() {
            const size_t CAP = std::max((int64_t)1, cfg.maxNotifs->value());
            while (notifs.size() > CAP) {
                auto victim = notifs.end() - 1;
                for (auto it = notifs.end() - 1; it != notifs.begin(); --it)
                    if ((*it)->urgency < 2) {
                        victim = it;
                        break;
                    }
                const auto VID = (*victim)->id;
                notifs.erase(victim);
                Bus::emitClosed(VID, R_UNDEFINED);
            }
        }

        // ---- arrival ----

        uint32_t arrive(const std::string& appName, uint32_t replacesId, const std::string& appIcon, const std::string& summary, const std::string& body,
                        const std::vector<std::string>& actions, const std::map<std::string, sdbus::Variant>& hints, int32_t expireTimeout) {
            uint32_t id = replacesId;

            const std::string APP_NAME = Parse::clipUtf8(appName, Parse::MAX_APP_NAME_BYTES);
            const std::string APP_ICON = Parse::boundedOpaque(appIcon, Parse::MAX_SOURCE_BYTES);
            const std::string SUMMARY  = Parse::clipUtf8(summary, Parse::MAX_SUMMARY_BYTES);
            const std::string BODY     = Parse::clipUtf8(body, Parse::MAX_BODY_BYTES);

            // Identity and grouping hints are read before the main parse. They
            // are optional extensions; the standard Notify signature remains
            // unchanged for existing clients.
            const auto optStrHint = [&](const char* key, const size_t cap, bool opaque = false) -> std::optional<std::string> {
                if (const auto IT = hints.find(key); IT != hints.end())
                    try {
                        const auto value = IT->second.get<std::string>();
                        if (opaque && value.size() > cap)
                            return std::nullopt;
                        return opaque ? value : Parse::clipUtf8(value, cap);
                    } catch (...) {}
                return std::nullopt;
            };
            const auto strHint     = [&](const char* key, const size_t cap, bool opaque = false) { return optStrHint(key, cap, opaque).value_or(""); };
            const auto optBoolHint = [&](const char* key) -> std::optional<bool> {
                const auto IT = hints.find(key);
                if (IT == hints.end())
                    return std::nullopt;
                try {
                    return IT->second.get<bool>();
                } catch (...) {
                    try {
                        const auto S = IT->second.get<std::string>();
                        if (S == "1" || S == "true" || S == "yes")
                            return true;
                        if (S == "0" || S == "false" || S == "no")
                            return false;
                    } catch (...) {}
                }
                return std::nullopt;
            };
            const auto optU32Hint = [&](const char* key) -> std::optional<uint32_t> {
                const auto IT = hints.find(key);
                if (IT == hints.end())
                    return std::nullopt;
                try {
                    return IT->second.get<uint32_t>();
                } catch (...) {
                    try {
                        return (uint32_t)std::max<int32_t>(0, IT->second.get<int32_t>());
                    } catch (...) {}
                }
                return std::nullopt;
            };
            const auto optI64Hint = [&](const char* key) -> std::optional<int64_t> {
                const auto IT = hints.find(key);
                if (IT == hints.end())
                    return std::nullopt;
                try {
                    return IT->second.get<int64_t>();
                } catch (...) {
                    try {
                        const auto VALUE = IT->second.get<uint64_t>();
                        if (VALUE <= (uint64_t)std::numeric_limits<int64_t>::max())
                            return (int64_t)VALUE;
                    } catch (...) {}
                }
                return std::nullopt;
            };
            const auto        DESKTOP_HINT        = optStrHint("desktop-entry", Parse::MAX_SOURCE_BYTES, true);
            const auto        CONV_ID_HINT        = optStrHint("x-hyprnotify-conversation-id", Parse::MAX_CONVERSATION_ID_BYTES, true);
            const auto        CONV_TITLE_HINT     = optStrHint("x-hyprnotify-conversation-title", Parse::MAX_SUMMARY_BYTES);
            const auto        CONV_KIND_HINT      = optStrHint("x-hyprnotify-conversation-kind", Parse::MAX_CONVERSATION_KIND_BYTES, true);
            const auto        CONV_ICON_HINT      = optStrHint("x-hyprnotify-conversation-icon", Parse::MAX_SOURCE_BYTES, true);
            const auto        SENDER_ID_HINT      = optStrHint("x-hyprnotify-sender-id", Parse::MAX_SENDER_ID_BYTES, true);
            const auto        SENDER_NAME_HINT    = optStrHint("x-hyprnotify-sender-name", Parse::MAX_SENDER_NAME_BYTES);
            const auto        SENDER_ICON_HINT    = optStrHint("x-hyprnotify-sender-icon", Parse::MAX_SOURCE_BYTES, true);
            const auto        MESSAGE_ID_HINT     = optStrHint("x-hyprnotify-message-id", Parse::MAX_MESSAGE_ID_BYTES, true);
            const auto        DECLARED_GROUP_HINT = optStrHint("x-hyprnotify-group-key", Parse::MAX_CONVERSATION_ID_BYTES, true);
            const auto        SECTION_HINT        = optStrHint("x-hyprnotify-section", Parse::MAX_HINT_TEXT_BYTES, true);

            const auto        REPLACE_TARGET = id != 0 ? byId(id) : nullptr;
            const std::string DESKTOP        = DESKTOP_HINT ? *DESKTOP_HINT : REPLACE_TARGET ? REPLACE_TARGET->desktopEntry : std::string{};
            const std::string APPKEY         = !DESKTOP.empty() ? DESKTOP : !DESKTOP_HINT && REPLACE_TARGET && !REPLACE_TARGET->appKey.empty() ? REPLACE_TARGET->appKey : APP_NAME;
            const std::string CAT            = strHint("category", Parse::MAX_HINT_TEXT_BYTES, true);
            const std::string CONV_ID        = CONV_ID_HINT.value_or("");
            const std::string MESSAGE_ID     = MESSAGE_ID_HINT.value_or("");
            const std::string SENDER_NAME    = SENDER_NAME_HINT ? Parse::oneLine(Parse::sanitizeMarkup(*SENDER_NAME_HINT)) : std::string{};
            const bool        CATEGORY_CONVERSATION = CAT.starts_with("im.") || CAT == "im" || CAT.starts_with("call.") || CAT == "call";
            const auto        UNREAD_HINT           = optU32Hint("x-hyprnotify-unread-count");
            const auto        HISTORIC_HINT         = optBoolHint("x-hyprnotify-message-historic");
            const bool        HISTORIC              = HISTORIC_HINT.value_or(false);
            const auto        MESSAGE_TIME_HINT     = optI64Hint("x-hyprnotify-message-timestamp");

            // A conversation can merge only when the sender supplied an
            // explicit stable chat identity. Display text and category alone
            // are insufficient: two Telegram chats can share a title, and
            // Firefox can reuse visible text for unrelated site alerts.
            if (id == 0 && CONV_ID_HINT && !CONV_ID.empty())
                for (const auto& N : notifs)
                    if (!inOsdBand(N->id) && !vanishes(N) && Pixel::matchesConversation(APPKEY, CONV_ID, N->appKey, N->conversationId)) {
                        id = N->id;
                        break;
                    }

            // replaces_id names an existing server-owned notification. The
            // first use of one of our private OSD ids is the one exception:
            // in-tree OSD senders mark it explicitly, so an ordinary client
            // cannot enter the reserved band merely by choosing 9990..9999.
            bool PRIVATE_OSD = false;
            if (inOsdBand(id))
                if (const auto IT = hints.find("x-hitori-osd"); IT != hints.end())
                    try {
                        PRIVATE_OSD = IT->second.get<bool>();
                    } catch (...) {}
            if (id != 0 && !byId(id) && !PRIVATE_OSD)
                id = 0;

            if (id == 0) {
                // Fresh ids count up from a low counter and skip any that's
                // still live, so they never collide with a displayed
                // notification. Crucially the counter is NOT dragged up to a
                // seen replaces_id (as it once was): the OSD scripts pin ids
                // in the 9990s, and bumping past 9991 handed the next fresh
                // notification 9992 — the brightness OSD's id — so a keypress
                // hijacked it. Low fresh ids and the pinned band stay disjoint.
                do {
                    id = nextId++;
                    if (nextId == 0)
                        nextId = 1; // wrap: 0 means "no id"
                } while (byId(id) || inOsdBand(id));
            }

            auto       n        = byId(id);
            const bool EXISTING = n != nullptr;
            if (!n) {
                n = makeShared<SNotif>();
                n->id = id;
                n->born = Time::steadyNow(); // the arrival spring keys here, never on `arrived`
                // DND collects silently — except critical, which punches
                // through (the urgency parse below lifts it back out)
                n->waiting = suspended;
                notifs.insert(notifs.begin(), n); // newest on top; a replace keeps its slot
                evictOverflow();
            }

            const bool        SAME_APP          = EXISTING && n->appKey == APPKEY;
            const std::string EFFECTIVE_CONV_ID = CONV_ID_HINT ? CONV_ID : SAME_APP ? n->conversationId : std::string{};
            const bool        SAME_CONVERSATION = EXISTING && !EFFECTIVE_CONV_ID.empty() && SAME_APP && n->conversationId == EFFECTIVE_CONV_ID;
            const bool        CONVERSATION      = CATEGORY_CONVERSATION || !EFFECTIVE_CONV_ID.empty() || (SAME_CONVERSATION && n->conversation);
            if (!SAME_CONVERSATION) {
                n->messages.clear();
                n->unreadCount = 0;
            }

            n->arrived = Time::steadyNow(); // a replace refreshes the age, like a new arrival would
            // A replace re-alerts (the OSD sweep relies on it) — unless the
            // card is SNOOZED. The merge above deliberately keeps aiming a
            // chat's new messages at the card that holds it, snoozed or not,
            // so that one card stays the whole conversation; without this the
            // snooze would last precisely until the sender next said anything.
            n->banner  = !n->snoozed;
            n->appName = APP_NAME;
            n->desktopEntry = DESKTOP;
            n->summary = Parse::oneLine(Parse::sanitizeMarkup(SUMMARY));
            std::string bodyText = BODY;
            n->bodyImages.clear();
            for (auto P : Parse::extractImages(bodyText, std::max(64, (int)cfg.maxIcon->value() * 2)))
                n->bodyImages.push_back(std::move(P));
            n->body = Parse::sanitizeMarkup(bodyText, /*allowLinks=*/true);

            const int ICONPX  = std::max(8, (int)cfg.maxIcon->value());
            n->conversationId = EFFECTIVE_CONV_ID;
            if (CONV_TITLE_HINT)
                n->conversationTitle = CONV_TITLE_HINT->empty() ? n->summary : Parse::oneLine(Parse::sanitizeMarkup(*CONV_TITLE_HINT));
            else if (!SAME_CONVERSATION)
                n->conversationTitle = n->summary;
            if (CONV_KIND_HINT) {
                const auto KIND = Pixel::normalizeConversationKind(*CONV_KIND_HINT);
                if (!KIND.empty())
                    n->conversationKind = KIND;
                else if (!SAME_CONVERSATION)
                    n->conversationKind.clear();
            } else if (!SAME_CONVERSATION)
                n->conversationKind = !EFFECTIVE_CONV_ID.empty() ? "one-to-one" : "";
            if (n->conversationKind.empty() && !EFFECTIVE_CONV_ID.empty())
                n->conversationKind = "one-to-one";
            if (CONV_ICON_HINT)
                n->conversationIconSource = *CONV_ICON_HINT;
            else if (!SAME_CONVERSATION) {
                n->conversationIconSource.clear();
                n->conversationIcon.clear();
            }
            if (CONV_ICON_HINT)
                n->conversationIcon = Parse::resolveImage(n->conversationIconSource, ICONPX);
            if (DECLARED_GROUP_HINT)
                n->declaredGroupKey = *DECLARED_GROUP_HINT;
            else if (!SAME_APP)
                n->declaredGroupKey.clear();

            if (!EFFECTIVE_CONV_ID.empty()) {
                const auto viewOf = [](const std::optional<std::string>& value) -> std::optional<std::string_view> {
                    return value ? std::optional<std::string_view>{*value} : std::nullopt;
                };
                const auto SENDER_NAME_VIEW = SENDER_NAME_HINT ? std::optional<std::string_view>{SENDER_NAME} : std::nullopt;
                const auto MUTATION =
                    Pixel::upsertMessage(n->messages, MESSAGE_ID, n->body, viewOf(SENDER_ID_HINT), SENDER_NAME_VIEW, viewOf(SENDER_ICON_HINT), MESSAGE_TIME_HINT, HISTORIC_HINT);
                rebuildConversationParticipants(*n, ICONPX);
                n->body        = conversationBody(*n);
                n->unreadCount = Pixel::updatedUnreadCount(n->unreadCount, UNREAD_HINT, HISTORIC, MUTATION);
            } else {
                n->unreadCount = 0;
                n->participants.clear();
            }

            n->urgency  = 1;
            n->progress = -1;
            n->image.clear();
            n->identity.clear();
            n->appIcon = APP_ICON;
            n->pixels.clear();
            n->hasPixels = false;
            n->pw = n->ph = 0;

            if (const auto IT = hints.find("urgency"); IT != hints.end())
                try {
                    n->urgency = IT->second.get<uint8_t>();
                } catch (...) {
                    try {
                        n->urgency = (uint8_t)std::clamp(IT->second.get<int32_t>(), 0, 2);
                    } catch (...) {}
                }
            if (n->waiting && n->urgency >= 2)
                n->waiting = false; // critical bypasses DND
            if (const auto IT = hints.find("value"); IT != hints.end())
                try {
                    n->progress = std::clamp(IT->second.get<int32_t>(), 0, 100);
                } catch (...) {
                    try {
                        n->progress = (int)std::min(IT->second.get<uint32_t>(), 100u);
                    } catch (...) {}
                }

            if (SECTION_HINT) {
                const auto SECTION = Pixel::normalizeSection(*SECTION_HINT);
                n->section         = SECTION;
                n->sectionExplicit = !SECTION.empty();
            } else if (!SAME_APP) {
                n->section.clear();
                n->sectionExplicit = false;
            }
            if (!n->sectionExplicit)
                n->section = Policy::silenced(APPKEY) || n->urgency == 0 ? "silent" : "alerting";

            // The icon anatomy (Android's, per the design contract): CONTENT
            // image-data/image-path is a sender avatar only for conversations.
            // IDENTITY (app_icon, else desktop-entry Icon=) leads every other
            // notification alone. Missing identity gets the deterministic
            // generic application mark during the warm pass.
            if (CONVERSATION) {
                const int PIXCAP = std::max((int)cfg.width->value() * 2, (int)cfg.maxIcon->value() * 3);
                for (const auto* KEY : {"image-data", "image_data", "icon_data"})
                    if (const auto IT = hints.find(KEY); IT != hints.end() && n->pixels.empty())
                        try {
                            Parse::unpackImageData(*n, IT->second.get<Parse::ImageData>(), PIXCAP);
                        } catch (...) {}
                if (n->pixels.empty()) {
                    std::string cand;
                    for (const auto* KEY : {"image-path", "image_path"})
                        if (const auto IT = hints.find(KEY); IT != hints.end() && cand.empty())
                            try {
                                cand = Parse::boundedOpaque(IT->second.get<std::string>(), Parse::MAX_SOURCE_BYTES);
                            } catch (...) {}
                    n->image = Parse::resolveImage(cand, ICONPX);
                }
            }
            n->identity = Parse::resolveImage(APP_ICON, ICONPX);
            n->identityFromDesktop = n->identity.empty() && !DESKTOP.empty();
            if (n->identityFromDesktop)
                n->identity = resolveDesktopEntryIcon(DESKTOP, ICONPX);
            n->appKey = APPKEY;

            // The inline-reply protocol (KDE's, which Telegram/Fractal speak):
            // the sender adds an action keyed "inline-reply" only when the
            // server advertises the capability, and expects NotificationReplied
            // back. It is NOT a button — it opens the row's reply field.
            n->canReply         = false;
            n->replyActionText  = {};
            n->replyPlaceholder = strHint("x-kde-reply-placeholder-text", Parse::MAX_HINT_TEXT_BYTES);
            n->replySubmitText  = strHint("x-kde-reply-submit-button-text", Parse::MAX_HINT_TEXT_BYTES);

            // actions arrive as [id0,label0, id1,label1, ...]. Every named pair
            // becomes a button; "default" is the card's primary and gets NO
            // button on either surface — the spec defines it as "the default
            // action (usually invoked by clicking the notification)" and says
            // implementations are free not to display it, so a body click is
            // what fires it and a button would only duplicate that.
            n->defaultAction.clear();
            n->actions.clear();
            for (size_t i = 0, pairs = 0; i + 1 < actions.size() && pairs < Parse::MAX_ACTION_PAIRS; i += 2, pairs++) {
                const auto ID    = Parse::boundedOpaque(actions[i], Parse::MAX_ACTION_ID_BYTES);
                const auto LABEL = Parse::clipUtf8(actions[i + 1], Parse::MAX_ACTION_LABEL_BYTES);
                if (ID.empty())
                    continue;
                if (ID == "default")
                    n->defaultAction = ID;
                else if (ID == "inline-reply") {
                    n->canReply = true;
                    n->replyActionText = LABEL;
                } else if (!LABEL.empty()) // an empty label has no button to draw
                    n->actions.push_back(SAction{.id = ID, .label = LABEL});
            }
            n->resident = false;
            if (const auto IT = hints.find("resident"); IT != hints.end())
                try {
                    n->resident = IT->second.get<bool>();
                } catch (...) {}
            n->actionIcons = false;
            if (const auto IT = hints.find("action-icons"); IT != hints.end())
                try {
                    n->actionIcons = IT->second.get<bool>();
                } catch (...) {}
            n->transient = false;
            if (const auto IT = hints.find("transient"); IT != hints.end())
                try {
                    n->transient = IT->second.get<bool>();
                } catch (...) {}

            // Conversation identity controls ranking and message merging.
            // Display grouping remains a separate app/section contract, so
            // multiple chats can remain distinct children under one header.
            n->conversation = CONVERSATION;
            n->priority     = !n->conversationId.empty() && Policy::priority(APPKEY, n->conversationId);

            if (expireTimeout > 0)
                n->timeoutMs = expireTimeout;
            else if (expireTimeout == 0)
                n->timeoutMs = 0;
            else // -1: the client leaves it to us
                n->timeoutMs = defaultTimeout(*n);
            if (n->timeoutMs > 0 && !n->waiting) // a queued card's clock starts at the resume
                n->deadline = Time::steadyNow() + std::chrono::milliseconds((int64_t)n->timeoutMs);

            // one live popup per app: while this app already shows a banner, a
            // new non-critical arrival is born resident — it lands silently in
            // the shade (folded, badge-counted), no second popup, no repeat
            // sound. A replace re-alerting its own live card is not a second
            // banner (appHasBanner skips self). Critical always punches through.
            // Presenting, gaming, watching: a real fullscreen window means the
            // screen is spoken for, so the banner is held back and the card
            // lands straight in the shade instead. Nothing is lost — residency
            // is exactly that safety net — and critical still punches through,
            // as it does through DND.
            // And a silenced app asked for exactly this, permanently: its
            // cards land in the shade without ever taking the screen.
            n->banner = bannerEligible(n);

            if (!n->waiting) // a suspended arrival is invisible: no warm, no damage
                notifChanged();

            // sound: a shown arrival plays sound-file/sound-name through the
            // libcanberra player unless the client suppresses it. DND-queued
            // (waiting) arrivals stay silent; the resume doesn't replay.
            if (!n->waiting) {
                bool        suppress = !n->banner || n->snoozed; // held-back cards do not announce themselves
                std::string soundFile, soundName;
                if (const auto IT = hints.find("suppress-sound"); IT != hints.end())
                    try {
                        suppress = IT->second.get<bool>();
                    } catch (...) {}
                if (const auto IT = hints.find("sound-file"); IT != hints.end())
                    try {
                        soundFile = Parse::boundedOpaque(IT->second.get<std::string>(), Parse::MAX_SOURCE_BYTES);
                    } catch (...) {}
                if (const auto IT = hints.find("sound-name"); IT != hints.end())
                    try {
                        soundName = Parse::boundedOpaque(IT->second.get<std::string>(), Parse::MAX_HINT_TEXT_BYTES);
                    } catch (...) {}
                if (soundFile.starts_with("file://"))
                    soundFile.erase(0, 7);
                const std::string CMD = cfg.soundCommand->value();
                if (!suppress && !CMD.empty()) {
                    if (!soundFile.empty())
                        spawnDetached({CMD.c_str(), "-f", soundFile.c_str(), nullptr});
                    else if (!soundName.empty())
                        spawnDetached({CMD.c_str(), "-i", soundName.c_str(), nullptr});
                }
            }

            rearmExpiry();
            Bus::emitStateSoon();
            return id;
        }

        // ---- lifecycle ----

        void init() {
            expiry = makeShared<CEventLoopTimer>(
                std::nullopt,
                [](SP<CEventLoopTimer>, void*) {
                    // RESIDENCY: a due banner emits reason 1 EXPIRED once
                    // and hides only the popup — the card stays in the
                    // shade until dismissed/acted. Transient and progress
                    // (OSD) cards vanish entirely.
                    const auto            NOW     = Time::steadyNow();
                    bool                  changed = false;
                    std::vector<uint32_t> gone;
                    for (const auto& N : notifs) {
                        if (N->snoozed) { // the other clocks: an undo window, then a card coming back
                            if (N->snoozeConfirmUntil != Time::steady_tp{} && N->snoozeConfirmUntil <= NOW) {
                                N->snoozeConfirmUntil = {}; // the undo row retires; the snooze itself runs on
                                changed               = true;
                            }
                            if (N->snoozeUntil > NOW)
                                continue;
                            N->snoozed            = false;
                            N->snoozeConfirmUntil = {};
                            // it alerts again — that IS the reminder — unless
                            // the app has since been silenced. `arrived` is
                            // left alone (the age line tells the truth about
                            // when it came); `born` re-keys the arrival spring
                            // so the banner slides in rather than blinking on.
                            N->banner = bannerEligible(N);
                            N->born   = NOW;
                            if (N->banner && N->timeoutMs > 0)
                                N->deadline = NOW + std::chrono::milliseconds((int64_t)N->timeoutMs);
                            changed = true;
                            continue;
                        }
                        if (!N->banner || N->timeoutMs <= 0 || N->waiting || N->id == heldBanner || N->deadline > NOW)
                            continue;
                        if (vanishes(N)) {
                            gone.push_back(N->id);
                            continue;
                        }
                        N->banner = false;
                        Bus::emitClosed(N->id, R_EXPIRED);
                        changed = true;
                    }
                    for (const auto ID : gone) {
                        std::erase_if(notifs, [&](const auto& N) { return N->id == ID; });
                        Bus::emitClosed(ID, R_EXPIRED);
                    }
                    if (changed || !gone.empty()) {
                        notifChanged();
                        Bus::emitStateSoon();
                    }
                    rearmExpiry();
                },
                nullptr);
            g_pEventLoopManager->addTimer(expiry);
        }

        void exit() {
            if (expiry && g_pEventLoopManager)
                g_pEventLoopManager->removeTimer(expiry);
            expiry.reset();
            notifs.clear();
            suspended  = false;
            heldBanner = 0;
        }
    }

} // namespace NHyprnotify
