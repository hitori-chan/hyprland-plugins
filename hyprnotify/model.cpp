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
        // park as a shade row. So they must never coalesce either — a
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
        // It does NOT leave at the click. Android replaces the notification in
        // place with "Snoozed for 1 hour ▾ · Undo" and only then lets it go —
        // which is the whole answer to "there is nothing left to click". For
        // CONFIRM_MS the card holds its slot as a one-line undo row, so the
        // only irreversible verb in the shell stops being irreversible. This
        // is not history: the card never left, and once the window passes it
        // is gone the same way it always was.
        inline constexpr int64_t CONFIRM_MS = 6000;

        // The ˅ ladder, Android's own durations. snooze_seconds keeps its
        // meaning as the duration a bare ◷ takes; the ladder is where the ˅
        // goes next, so a configured default that is not on it is still the
        // starting point.
        inline constexpr int64_t RUNGS[] = {900, 1800, 3600, 7200};
        inline constexpr size_t  NRUNGS  = sizeof(RUNGS) / sizeof(RUNGS[0]);

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

        void snoozeFor(uint32_t id, int64_t seconds) {
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
            // card resumes as the resident shade row the ◷ found it as
            notifChanged();
            rearmExpiry();
            Bus::emitStateSoon();
        }

        // the next rung ABOVE what is in force, wrapping — so a duration the
        // ladder does not hold (a configured 10 min, the panel's own) still
        // has an obvious next step
        void snoozeCycle(uint32_t id) {
            const auto N = byId(id);
            if (!N || !snoozeConfirming(N))
                return;
            const int64_t CUR = N->snoozeSecs;
            N->snoozeSecs     = RUNGS[0]; // nothing above it: wrap to the bottom
            for (size_t i = 0; i < NRUNGS; i++)
                if (RUNGS[i] > CUR) {
                    N->snoozeSecs = RUNGS[i];
                    break;
                }
            armSnooze(N);
            notifChanged();
            rearmExpiry();
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
            if (cfg.quietFullscreen->value() && NHyprCommon::fullscreenOn(Desktop::focusState() ? Desktop::focusState()->monitor() : nullptr))
                return false;
            if (cfg.coalescePopups->value() && appHasBanner(n))
                return false;
            return !Policy::silenced(n->appKey);
        }

        void closeOne(uint32_t id, uint32_t reason) {
            const auto BEFORE = notifs.size();
            std::erase_if(notifs, [&](const auto& N) { return N->id == id; });
            if (notifs.size() == BEFORE)
                return;
            Bus::emitClosed(id, reason);
            notifChanged();
            rearmExpiry();
        }

        // Only what the user can SEE is sweepable. The DND queue was never
        // shown, and a snoozed card was deliberately put away — clearing the
        // shade must not quietly cancel a reminder the user asked for.
        static bool visible(const SP<SNotif>& n) {
            return !n->waiting && !n->snoozed;
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

        void dismissApp(const std::string& appKey) {
            const auto BEFORE = notifs.size();
            for (const auto& N : notifs)
                if (visible(N) && N->appKey == appKey)
                    Bus::emitClosed(N->id, R_DISMISSED);
            std::erase_if(notifs, [&](const auto& N) { return visible(N) && N->appKey == appKey; });
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
                notifChanged(); // the center's ⊖ lights up
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

            // Two hints are read before the main parse: the merge decision
            // below needs the grouping key and the category before there is
            // a card to hang them on.
            const auto strHint = [&](const char* key) -> std::string {
                if (const auto IT = hints.find(key); IT != hints.end())
                    try {
                        return IT->second.get<std::string>();
                    } catch (...) {}
                return "";
            };
            const std::string DESKTOP = strHint("desktop-entry");
            const std::string APPKEY  = !DESKTOP.empty() ? DESKTOP : appName; // grouping identity
            const std::string CAT     = strHint("category");
            const bool CONVERSATION   = CAT.starts_with("im.") || CAT == "im" || CAT.starts_with("call.") || CAT == "call";

            // THE CONVERSATION MERGE (Android's MessagingStyle): every message
            // of one chat is ONE card. A fresh Notify whose app + summary
            // matches a live card rides the replace path with the bodies
            // joined, so a chatty sender grows a single card instead of
            // stacking a row per message. Two triggers: the fd.o conversation
            // categories (im.*/call.* — the summary IS the sender or the room),
            // and x-canonical-append (notify-osd's extension) for apps that ask
            // for it without a category. Cards that vanish never merge (a
            // suppressed banner would strand them), nor does the OSD band.
            std::string appendOnto;
            if (id == 0) {
                bool append = CONVERSATION;
                if (const auto IT = hints.find("x-canonical-append"); !append && IT != hints.end())
                    try {
                        append = IT->second.get<bool>();
                    } catch (...) {
                        try {
                            const auto S = IT->second.get<std::string>();
                            append       = !S.empty() && S != "false" && S != "0";
                        } catch (...) {
                            try {
                                append = IT->second.get<uint8_t>() != 0;
                            } catch (...) {}
                        }
                    }
                if (append) {
                    const auto SUM = Parse::oneLine(Parse::sanitizeMarkup(summary));
                    for (const auto& N : notifs)
                        if (!inOsdBand(N->id) && !vanishes(N) && N->appKey == APPKEY && N->summary == SUM) {
                            id         = N->id;
                            appendOnto = N->body;
                            break;
                        }
                }
            }

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

            auto n = byId(id);
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

            n->arrived = Time::steadyNow(); // a replace refreshes the age, like a new arrival would
            // A replace re-alerts (the OSD sweep relies on it) — unless the
            // card is SNOOZED. The merge above deliberately keeps aiming a
            // chat's new messages at the card that holds it, snoozed or not,
            // so that one card stays the whole conversation; without this the
            // snooze would last precisely until the sender next said anything.
            n->banner  = !n->snoozed;
            n->appName = appName;
            n->summary = Parse::oneLine(Parse::sanitizeMarkup(summary));
            std::string bodyText = body;
            n->bodyImages.clear();
            for (const auto& P : Parse::extractImages(bodyText, std::max(64, (int)cfg.maxIcon->value() * 2)))
                n->bodyImages.push_back({P});
            n->body = Parse::sanitizeMarkup(bodyText, /*allowLinks=*/true);
            if (!appendOnto.empty())
                n->body = Parse::joinAppend(appendOnto, n->body);

            n->urgency  = 1;
            n->progress = -1;
            n->image.clear();
            n->identity.clear();
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

            // The icon anatomy (Android's, per the design contract): the
            // CONTENT image (image-data / image-path) owns the icon column;
            // the IDENTITY (app_icon param, else the desktop-entry hint)
            // rides it as a corner badge — or leads alone when there is no
            // content. Nothing at all = a text-only card.
            const int ICONPX = std::max(8, (int)cfg.maxIcon->value());
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
                            cand = IT->second.get<std::string>();
                        } catch (...) {}
                n->image = Parse::resolveImage(cand, ICONPX);
            }
            n->identity = Parse::resolveImage(appIcon, ICONPX);
            if (n->identity.empty() && !DESKTOP.empty())
                n->identity = Parse::resolveImage(DESKTOP, ICONPX);
            n->appKey = APPKEY;

            // The inline-reply protocol (KDE's, which Telegram/Fractal speak):
            // the sender adds an action keyed "inline-reply" only when the
            // server advertises the capability, and expects NotificationReplied
            // back. It is NOT a button — it opens the row's reply field.
            n->canReply         = false;
            n->replyPlaceholder = strHint("x-kde-reply-placeholder-text");
            n->replySubmitText  = strHint("x-kde-reply-submit-button-text");

            // actions arrive as [id0,label0, id1,label1, ...]. Every named pair
            // becomes a button; "default" is the card's primary and gets NO
            // button on either surface — the spec defines it as "the default
            // action (usually invoked by clicking the notification)" and says
            // implementations are free not to display it, so a body click is
            // what fires it and a button would only duplicate that.
            n->defaultAction.clear();
            n->actions.clear();
            for (size_t i = 0; i + 1 < actions.size(); i += 2) {
                if (actions[i] == "default")
                    n->defaultAction = actions[i];
                else if (actions[i] == "inline-reply") {
                    n->canReply = true;
                    if (n->replySubmitText.empty())
                        n->replySubmitText = actions[i + 1]; // the sender's own "Reply" label
                } else if (!actions[i + 1].empty()) // an empty label has no button to draw
                    n->actions.push_back(SAction{.id = actions[i], .label = actions[i + 1]});
            }
            // a lone named action doubles as the body-click default; it keeps
            // its own button too, since it was given a label to show
            if (n->defaultAction.empty() && n->actions.size() == 1)
                n->defaultAction = n->actions.front().id;

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

            // fd.o category: conversations rank high in the shade and never
            // bundle into an app digest (Android keeps every chat its own
            // card). Ordering and merging only — no per-app casing.
            n->conversation = CONVERSATION;
            n->priority     = CONVERSATION && Policy::priority(APPKEY, n->summary);

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
                        soundFile = IT->second.get<std::string>();
                    } catch (...) {}
                if (const auto IT = hints.find("sound-name"); IT != hints.end())
                    try {
                        soundName = IT->second.get<std::string>();
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
