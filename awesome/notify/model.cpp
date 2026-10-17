// awesome/notify/model.cpp — the cards themselves: what one is, how it
// arrives, how long it lives and when it stops being visible. Everything a
// notification DOES between the wire and the glass is decided here.
//
// The split from bus.cpp is by ownership, not by layer: bus.cpp owns the
// connection (the vtable, the signals, the name), parse.cpp owns the
// untrusted payload, and this unit owns the list. The one thing the model
// reaches back over the bus for is the outbound half of a card's life —
// NotificationClosed — which it does through emitClosed and nothing else
// (the old bell State signal is gone: the shell's bell reads this list
// directly, in the same .so).
//
// RESIDENCY is the model's central rule: a card's BANNER and the card
// itself have separate lifetimes. Expiry takes the banner and leaves the
// card in the shade; only a dismissal, an action or the cap takes the card.
// Everything that opts out of that (the transient hint, progress/OSD blips)
// is one predicate — vanishes() — and it MUST stay the same set everywhere,
// or a suppressed banner strands a card no sweep will ever reach.

#include "model.hpp"

#include "../core/jobs.hpp"

namespace NAwesome::Notify {

    std::vector<SP<SNotif>> notifs;

    // The transcript and the legacy join are both chronological (oldest
    // leads, newest ends), and an ordinary body ends on its newest line too —
    // the newest message sits at the END of every card body. The collapsed
    // row must always print the newest.
    std::string collapsedLine(const SP<SNotif>& n) {
        if (n->body.empty())
            return {};
        return lastLine(n->body);
    }

    namespace Model {
        static SP<CEventLoopTimer> expiry;
        static uint32_t            nextId     = 1;
        static bool                suspended  = false; // DND
        static uint32_t            heldBanner = 0;     // the popup under the pointer: its countdown is paused

        // a hostile sender's payload caps: buttons and <img> rows beyond
        // these would draw past the clamped card into the next card's glass,
        // so they are dropped, not clipped
        constexpr size_t MAX_ACTIONS     = 8;
        constexpr size_t MAX_BODY_IMAGES = 4;
        // structured conversation identity: hostile senders can't grow these
        // without bound — they key the merge scan and the message upsert
        constexpr size_t MAX_CONV_ID_BYTES     = 512;
        constexpr size_t MAX_MESSAGE_ID_BYTES  = 512;
        constexpr size_t MAX_SENDER_ID_BYTES   = 512;
        constexpr size_t MAX_SENDER_NAME_BYTES = 256;
        constexpr size_t MAX_CONV_KIND_BYTES   = 32;
        // free-form wire strings are capped codepoint-safe at arrival: the
        // D-Bus message limit is megabytes, and a stored string rides into
        // texture-cache keys and grouping identities for the card's whole
        // life
        constexpr size_t MAX_APP_NAME_BYTES   = 256;
        constexpr size_t MAX_SUMMARY_BYTES    = 1024;
        constexpr size_t MAX_SOURCE_BYTES     = 1024; // app icon / image-path / sound-file / desktop-entry
        constexpr size_t MAX_ACTION_KEY_BYTES = 256;  // id and label
        constexpr size_t MAX_REPLY_TEXT_BYTES = 256;  // placeholder / submit label
        constexpr size_t MAX_BODY_RAW_BYTES   = 32768; // pre-sanitize: the markup intermediate can grow ~5x

        // a fresh body is hostile-able up to the D-Bus message limit; the
        // merged path caps at 8192, so a fresh one earns the same cut,
        // codepoint-safe
        static std::string clipUtf8(std::string s, size_t cap) {
            if (s.size() <= cap)
                return s;
            s.resize(cap);
            while (!s.empty() && ((unsigned char)s.back() & 0xc0) == 0x80)
                s.pop_back();
            if (!s.empty() && (unsigned char)s.back() >= 0xc2)
                s.pop_back(); // a lead byte whose followers the cut took
            return s;
        }

        static std::string capUtf8(std::string s) {
            return clipUtf8(std::move(s), 8192);
        }

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
        // Never the DND queue (invisible until the resume), never what is
        // off the shade (a volume card is an OSD, not a notification).
        std::pair<uint32_t, uint32_t> badgeCounts() {
            uint32_t live = 0, kept = 0;
            for (const auto& N : notifs) {
                if (N->waiting || offShade(N))
                    continue;
                (N->banner ? live : kept)++;
            }
            return {live, kept};
        }

        std::string badgeString() {
            const auto [LIVE, KEPT] = badgeCounts();
            return "banners:" + std::to_string(LIVE) + " resident:" + std::to_string(KEPT);
        }

        // notifs is newest-first, so the first visible card is the newest —
        // the stress gate's single-card batteries make it the top row too
        std::string toplineString() {
            for (const auto& N : notifs)
                if (!N->waiting && !offShade(N))
                    return collapsedLine(N);
            return {};
        }

        // A boolean hint as clients actually send it: bool, but also an int
        // or byte (`notify-send -h int:transient:1` is the common idiom) or
        // a string. nullopt when absent or of no such type.
        static std::optional<bool> hintFlag(const std::map<std::string, sdbus::Variant>& hints, const char* key) {
            const auto IT = hints.find(key);
            if (IT == hints.end())
                return std::nullopt;
            const auto& V = IT->second;
            try {
                if (V.containsValueOfType<bool>())
                    return V.get<bool>();
                if (V.containsValueOfType<int32_t>())
                    return V.get<int32_t>() != 0;
                if (V.containsValueOfType<uint32_t>())
                    return V.get<uint32_t>() != 0;
                if (V.containsValueOfType<uint8_t>())
                    return V.get<uint8_t>() != 0;
                if (V.containsValueOfType<int64_t>())
                    return V.get<int64_t>() != 0;
                if (V.containsValueOfType<std::string>()) {
                    const auto S = V.get<std::string>();
                    return !S.empty() && S != "false" && S != "0";
                }
            } catch (...) {}
            return std::nullopt;
        }

        // one live popup per app: does another card already hold a banner for
        // this app? A new same-app arrival then lands resident instead of
        // stacking a second popup — the shade folds the extras, the badge
        // counts them (the banner's own timeout is the cooldown window). A
        // banner with no timeout (sticky, critical) is no cooldown: it would
        // silence the app until dismissed. Two conversation cards are two
        // chats (one chat is one card), and each chat alerts on its own.
        static bool appHasBanner(const SP<SNotif>& self) {
            for (const auto& O : notifs)
                if (O != self && !O->waiting && O->banner && O->timeoutMs > 0 && !vanishes(O) && O->appKey == self->appKey && !(O->conversation && self->conversation))
                    return true;
            return false;
        }

        // Feedback, never a notification: the OSD band (volume, brightness,
        // mic, touchpad, the battery blips) and a client's transient card
        // (the spec's "bypass persistence": a screenshot's preview, a
        // client's own OSD). Neither is a shade row or history, nor on the
        // badge. Opening the shade, which replaces the popup column they
        // show on, ends the live ones (absorbPopped), and an OSD posted
        // while it is open makes no card. Critical stays an alert: a band
        // alert waits under the shade like any banner (never a row), a
        // critical transient card is an ordinary one.
        bool offShade(const SP<SNotif>& n) {
            return inOsdBand(n->id) || (n->transient && n->urgency < 2);
        }
        static bool momentary(const SP<SNotif>& n) {
            return offShade(n) && n->urgency < 2;
        }

        void postCard(const SPostCard& c) {
            if (inOsdBand(c.id) && c.urgency < 2 && centerVisible())
                return; // nothing to show it on, and the shade never holds one
            std::map<std::string, sdbus::Variant> hints;
            if (c.value >= 0)
                hints.emplace("value", sdbus::Variant{std::clamp(c.value, 0, 100)});
            if (c.urgency != 1)
                hints.emplace("urgency", sdbus::Variant{c.urgency});
            // We ARE the in-tree OSD sender: being in-process is the pass
            // that lets a FRESH band id pin its replace-in-place slot (a
            // foreign client gets its band id zeroed)
            arrive("osd", c.id, c.icon ? c.icon : "", c.summary ? c.summary : "", c.body ? c.body : "", "", {}, hints, c.expireTimeout, c.osd || inOsdBand(c.id));
        }

        void senderGone(const std::string& name) {
            bool changed = false;
            for (const auto& N : notifs) {
                if (N->sender != name || N->orphaned)
                    continue;
                N->orphaned = true;
                // nobody is listening for the reply any more: drop the field
                changed |= std::exchange(N->canReply, false);
            }
            if (changed)
                notifChanged();
        }

        bool closeOne(uint32_t id, uint32_t reason) {
            const auto BEFORE = notifs.size();
            std::erase_if(notifs, [&](const auto& N) { return N->id == id; });
            if (notifs.size() == BEFORE)
                return false; // spec: unknown IDs must be reportable
            Bus::emitClosed(id, reason);
            notifChanged();
            rearmExpiry();
            return true;
        }

        // Only what the user can SEE is sweepable. The DND queue was never
        // shown, and sweeping it would lose cards the user has not read.
        static bool visible(const SP<SNotif>& n) {
            return !n->waiting;
        }

        void dismissAllLive(bool offShadeToo) {
            // the user's Clear all sweeps the shade's rows: an OSD or a
            // critical band alert waiting under the shade is none of them
            // (the button's target and the badge exclude it too); the
            // scripted reset sweeps everything shown
            const auto SWEPT  = [offShadeToo](const auto& N) { return visible(N) && (offShadeToo || !offShade(N)); };
            const auto BEFORE = notifs.size();
            for (const auto& N : notifs)
                if (SWEPT(N))
                    Bus::emitClosed(N->id, R_DISMISSED);
            std::erase_if(notifs, SWEPT);
            if (notifs.size() == BEFORE)
                return;
            notifChanged();
            rearmExpiry();
        }

        // the bundle's identity: the app key, sub-keyed by the declared
        // group when one is in force — the digest, the fold state and the
        // dismissal all ride on this one string
        std::string groupKeyOf(const SP<SNotif>& n) {
            return n->declaredGroupKey.empty() ? n->appKey : n->appKey + "\x1f" + n->declaredGroupKey;
        }

        bool bundleable(const SP<SNotif>& n) {
            return !n->conversation && !n->appKey.empty();
        }

        void dismissApp(const std::string& key) {
            // exactly the bundle's members: its (app, declared group) key,
            // never the app's other groups, its chats (conversations never
            // bundle) or what is off the shade
            const auto MATCH  = [&](const auto& N) { return visible(N) && !offShade(N) && bundleable(N) && groupKeyOf(N) == key; };
            const auto BEFORE = notifs.size();
            for (const auto& N : notifs)
                if (MATCH(N))
                    Bus::emitClosed(N->id, R_DISMISSED);
            std::erase_if(notifs, MATCH);
            if (notifs.size() == BEFORE)
                return;
            notifChanged();
            rearmExpiry();
        }

        // Opening the center absorbs the popped stack: every bannered card
        // stands down into a parked shade row, so closing the center never
        // re-pops it. The unread count is unchanged (popped + parked both
        // count) — only the on-screen banners go. Ephemerals (transient,
        // progress) keep their banners so the expiry timer still vanishes
        // them on their own clocks; nothing here emits a close for them (the
        // cards are parked, not dismissed). An OSD or a transient card goes
        // for good (momentary). `repopOnClose` false parks for good: the
        // popup's middle click, with no shade whose close would return them.
        void absorbPopped(bool repopOnClose) {
            bool                  changed = false;
            std::vector<uint32_t> gone;
            for (const auto& N : notifs)
                if (momentary(N))
                    gone.push_back(N->id);
            for (const auto ID : gone) {
                std::erase_if(notifs, [&](const auto& N) { return N->id == ID; });
                Bus::emitClosed(ID, R_EXPIRED);
                changed = true;
            }
            if (!gone.empty())
                rearmExpiry();
            for (const auto& N : notifs) {
                if (!N->banner || N->waiting)
                    continue;
                if (vanishes(N))
                    continue;
                N->banner   = false;
                N->absorbed = repopOnClose;
                changed     = true;
            }
            if (changed)
                notifChanged();
        }

        // Closing the shade returns what the open parked: an absorbed card
        // gets its banner back, or stays parked under the same one-per-app
        // cap the DND resume applies. Without this, an outside click next to
        // the panel (the offset strip, the screen edge) closed the shade and
        // left the whole stack invisible — the notifications sat in the now
        // closed panel, nowhere on screen. Expiry-parked cards are not
        // absorbed and stay parked: an expired banner does not re-pop because
        // the user closed the panel. A close that does not re-pop (an action
        // leaving) parks them for good: a flag left behind re-popped them at
        // some later, unrelated close.
        void repopAbsorbed(bool repop) {
            const auto NOW     = Time::steadyNow();
            bool       changed = false;
            for (const auto& N : notifs) {
                if (!N->absorbed)
                    continue;
                N->absorbed = false;
                if (!repop)
                    continue;
                changed = true;
                if (N->waiting || vanishes(N))
                    continue;
                N->banner = !(NAwesome::cfg().getI("plugin:awesome:notify:coalesce_popups") && N->urgency < 2 && appHasBanner(N));
                if (N->banner && N->timeoutMs > 0)
                    N->deadline = NOW + std::chrono::milliseconds((int64_t)N->timeoutMs);
            }
            if (changed) {
                notifChanged();
                rearmExpiry(); // the timer may have disarmed while they were parked
            }
        }

        void toggleSuspend() {
            suspended = !suspended;
            if (suspended) {
                notifChanged(); // the shade's ⊖ lights up
                return;         // visible cards live out their timeouts; new arrivals queue
            }
            const auto NOW = Time::steadyNow();
            // newest-first, so the freshest per app takes the one popup slot
            // and the rest resume resident — the same one-per-app cap the
            // live arrival path applies, so DND-off never floods the screen
            for (const auto& N : notifs) {
                if (!N->waiting)
                    continue;
                N->waiting = false;
                N->banner  = !(NAwesome::cfg().getI("plugin:awesome:notify:coalesce_popups") && N->urgency < 2 && !vanishes(N) && appHasBanner(N)); // never seen: resume shows one banner per app
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
                return (float)NAwesome::cfg().getI("plugin:awesome:notify:timeout_low");
            return (float)NAwesome::cfg().getI("plugin:awesome:notify:timeout_normal");
        }

        // Cap the stack: the oldest short-lived card goes first (one that
        // vanishes on expiry anyway: an OSD script posting a new value card
        // per key step must not push the shade's notifications out), then
        // the oldest non-critical; only an all-critical stack starts losing
        // its oldest critical. The newest card at begin() always survives
        // (the scan stops short of it).
        static void evictOverflow() {
            const size_t CAP = (size_t)std::max(1, NAwesome::cfg().getI("plugin:awesome:notify:max_notifs"));
            while (notifs.size() > CAP) {
                auto victim = notifs.end() - 1;
                int  rank   = 2; // 0 short-lived, 1 non-critical, 2 critical: the lowest goes
                for (auto it = notifs.end() - 1; it != notifs.begin() && rank > 0; --it) {
                    const int R = (*it)->urgency >= 2 ? 2 : vanishes(*it) ? 0 : 1;
                    if (R < rank) {
                        rank   = R;
                        victim = it;
                    }
                }
                const auto VID   = (*victim)->id;
                const bool SHOWN = !(*victim)->waiting;
                notifs.erase(victim);
                Bus::emitClosed(VID, R_UNDEFINED);
                // a DND-held arrival warms nothing itself: the shown card it
                // pushed out must still leave the screen
                if (SHOWN)
                    notifChanged();
            }
        }

        // image-data a card keeps until its first upload — and a coalesced,
        // resident, DND-held or below-the-fold card is not uploaded for as
        // long as it lives (up to the hero cap, ~2 MB, each). Past this many
        // bytes model-wide the oldest give up the hero size for an icon's.
        constexpr size_t MAX_RETAINED_PIXEL_BYTES = 32u << 20;
        static void      boundRetainedPixels(int iconPx) {
            size_t total = 0;
            for (const auto& N : notifs)
                total += N->pixels.size();
            const size_t ICON_BYTES = (size_t)iconPx * iconPx * 4;
            for (auto it = notifs.rbegin(); it != notifs.rend() && total > MAX_RETAINED_PIXEL_BYTES; ++it) {
                auto& N = **it;
                if (N.pixels.size() <= ICON_BYTES)
                    continue;
                const size_t BEFORE = N.pixels.size();
                shrinkPixels(N, iconPx);
                total -= BEFORE - N.pixels.size();
            }
        }

        // Raw image-data waiting on the worker, model-wide: past this many
        // bytes an arrival unpacks inline (a burst of screenshots is a burst
        // of stalls, not an unbounded backlog of payloads held in memory).
        // Counted down by the done's capture, whether it runs or is dropped.
        constexpr size_t MAX_UNPACK_BYTES_IN_FLIGHT = 64u << 20;
        static size_t    unpackBytesInFlight        = 0;
        struct SUnpackBytes {
            size_t n;
            ~SUnpackBytes() {
                unpackBytesInFlight -= n;
            }
        };

        // A card's image-data: a large payload unpacks on the raster worker
        // (premultiplying and decimating a screenshot is tens of ms of the
        // dispatch); the card leads with a reserved column until it lands.
        static void takeImageData(SNotif& n, Parse::ImageData&& d, int capPx, int iconPx) {
            const auto [W, H] = Parse::imageDataSize(d);
            if (W <= 0)
                return; // malformed or oversized: the card falls back to image-path
            const size_t BYTES = std::get<6>(d).size();
            if ((int64_t)W * H > ASYNC_IMAGE_PIXELS && unpackBytesInFlight + BYTES <= MAX_UNPACK_BYTES_IN_FLIGHT) {
                auto           SRC    = std::make_shared<Parse::ImageData>(std::move(d));
                auto           OUT    = std::make_shared<std::pair<SPixels, uint64_t>>();
                const uint64_t TICKET = imageJobTicket();
                unpackBytesInFlight += BYTES;
                auto       HELD   = std::make_shared<SUnpackBytes>(BYTES);
                const bool QUEUED = NAwesome::CRasterWorker::inst().submit(
                    [SRC, OUT, capPx]() mutable {
                        OUT->first  = Parse::unpackImageData(*SRC, capPx);
                        OUT->second = Raster::hash(OUT->first);
                        SRC.reset(); // the payload is freed here, not on the loop
                    },
                    [OUT, TICKET, HELD, iconPx]() {
                        for (const auto& N : notifs)
                            if (N->pixelsJob == TICKET) {
                                N->pixelsJob = 0;
                                if (OUT->first.valid()) {
                                    N->pixels     = std::move(OUT->first.px);
                                    N->pw         = OUT->first.w;
                                    N->ph         = OUT->first.h;
                                    N->pixelsHash = OUT->second;
                                    boundRetainedPixels(iconPx);
                                } else
                                    N->hasPixels = false; // the card keeps its identity
                                notifChanged();
                                return;
                            }
                    });
                if (QUEUED) {
                    n.hasPixels = true;
                    n.pixelsJob = TICKET;
                    return;
                }
                d = std::move(*SRC); // refused: unpack it here
            }
            auto P = Parse::unpackImageData(d, capPx);
            if (!P.valid())
                return;
            n.pixelsHash = Raster::hash(P);
            n.pixels     = std::move(P.px);
            n.pw         = P.w;
            n.ph         = P.h;
            n.hasPixels  = true;
        }

        // the conversation's display body: the latest `limit` kept messages,
        // CHRONOLOGICAL — the oldest line leads and the newest ENDS the body,
        // so an arrival lands at the bottom and pushes the oldest out of the
        // top of the window. Group senders are prefixed by name.
        static std::string buildConversationBody(const SP<SNotif>& n, size_t limit);

        const std::string& conversationBody(const SP<SNotif>& n, size_t limit) {
            if (n->messages.empty())
                return n->body;
            if (n->convMemo.rev == n->rev && n->convMemo.limit == limit)
                return n->convMemo.body;
            n->convMemo = {.rev = n->rev, .limit = limit, .body = buildConversationBody(n, limit)};
            return n->convMemo.body;
        }

        static std::string buildConversationBody(const SP<SNotif>& n, size_t limit) {

            std::vector<std::string> LINES;
            const size_t             START = Pixel::presentedMessageStart(n->messages, limit);
            for (size_t i = START; i < n->messages.size(); i++) {
                const auto& M = n->messages[i];
                if (M.text.empty())
                    continue;
                std::string line;
                if (n->conversationKind == "group" && !M.senderName.empty()) {
                    line += Parse::oneLine(Parse::sanitizeMarkup(M.senderName));
                    line += ": ";
                }
                line += M.text;
                LINES.push_back(capUtf8(std::move(line)));
            }
            // the 8 KB cap evicts from the OLDEST end — the newest message is
            // what the card exists to show (n ≤ 7, so the rebuild is cheap)
            std::string out;
            for (size_t FRONT = 0; FRONT < LINES.size(); FRONT++) {
                std::string cand;
                for (size_t i = FRONT; i < LINES.size(); i++) {
                    if (!cand.empty())
                        cand += "\n";
                    cand += LINES[i];
                }
                if (cand.size() <= 8192) {
                    out = std::move(cand);
                    break;
                }
            }
            return out;
        }

        // A name resolve whose miss may only be the walk budget's: true when
        // it deferred (the caller keeps the name for the warm's retry).
        static std::string resolveName(const std::string& name, int iconPx, bool& deferred) {
            NAwesome::iconDeferred() = false;
            auto P = Parse::resolveImage(name, iconPx);
            deferred |= P.empty() && NAwesome::iconDeferred();
            return P;
        }

        // the identity: app_icon, else the desktop entry's own Icon= (the
        // index's), else the entry's name as an icon name. True: a resolve
        // was deferred and the identity is still empty.
        static bool resolveIdentity(SNotif& n, int iconPx) {
            bool deferred          = false;
            n.identityFromDesktop  = false;
            n.identity             = resolveName(n.appIconSource, iconPx, deferred);
            if (n.identity.empty() && !n.desktopEntry.empty()) {
                // the entry's own Icon= (the F2 index) beats an icon-name
                // collision; while the index has not reached the entry the
                // name stand-in holds, and pollDesktopIndex upgrades it
                NAwesome::iconDeferred() = false;
                if (const auto I = resolveDesktopEntryIcon(n.desktopEntry, iconPx); !I.empty())
                    n.identity = I;
                else {
                    deferred |= NAwesome::iconDeferred();
                    n.identity            = resolveName(n.desktopEntry, iconPx, deferred);
                    n.identityFromDesktop = true;
                }
            }
            return deferred && n.identity.empty();
        }

        // the distinct senders of the kept messages, newest first, capped —
        // their textures ride on the participants, not on the messages
        static void rebuildConversationParticipants(SNotif& n, int iconPx) {
            auto previous = std::move(n.participants);
            n.participants.clear();
            const auto INDICES = Pixel::latestDistinctParticipantIndices(n.messages);
            n.participants.reserve(INDICES.size());
            for (const auto INDEX : INDICES) {
                const auto& M = n.messages[INDEX];
                SParticipant P{.key = Pixel::participantKey(M.senderId, M.senderName), .name = M.senderName, .iconSource = M.senderIcon};
                if (const auto OLD = std::ranges::find_if(previous, [&](const auto& item) { return item.key == P.key && item.name == P.name && item.iconSource == P.iconSource; });
                    OLD != previous.end())
                    P = std::move(*OLD);
                else {
                    bool deferred = false;
                    P.icon        = resolveName(P.iconSource, iconPx, deferred);
                    P.iconRetry   = deferred;
                }
                n.participants.push_back(std::move(P));
            }
        }

        // ---- arrival ----

        uint32_t arrive(const std::string& appName, uint32_t replacesId, const std::string& appIcon, const std::string& summary, const std::string& body,
                        const std::string& sender, const std::vector<std::string>& actions, const std::map<std::string, sdbus::Variant>& hints, int32_t expireTimeout,
                        bool osd) {
            uint32_t id = replacesId;

            // A card belongs to the bus name that created it: its replies,
            // actions and activation token go there. A replaces_id or a
            // merge from another client would rebind it (the user's next
            // reply typed into a hijacked chat), so another client's id
            // starts a fresh card instead (as Plasma does). A card whose
            // sender left the bus is ownerless: the app's next instance
            // continues it. In-process posts (sender "") own theirs.
            const auto TAKEABLE = [&sender](const SNotif& N) { return N.sender == sender || N.orphaned; };
            if (id != 0)
                if (const auto N = byId(id); N && !TAKEABLE(*N))
                    id = 0;

            // cap the wire strings before they become card members (see the
            // MAX_*_BYTES above)
            const std::string APP     = clipUtf8(appName, MAX_APP_NAME_BYTES);
            const std::string APPICON = clipUtf8(appIcon, MAX_SOURCE_BYTES);
            const std::string SUM     = clipUtf8(summary, MAX_SUMMARY_BYTES);
            const std::string TXT     = clipUtf8(body, MAX_BODY_RAW_BYTES);

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
            // typed hint readers: nullopt = absent (or unparseable), and
            // "sent but empty" stays distinct — a replace that clears a
            // conversation-id must tear the conversation state down. An
            // over-cap opaque id is REJECTED, not clipped: a truncated id
            // would merge two different chats.
            const auto optStrHint = [&](const char* key, const char* alias, size_t cap, bool opaque = false) -> std::optional<std::string> {
                const auto LOOK = [&](const char* K) -> std::optional<std::string> {
                    const auto IT = hints.find(K);
                    if (IT == hints.end())
                        return std::nullopt;
                    try {
                        const auto value = IT->second.get<std::string>();
                        if (opaque && value.size() > cap)
                            return std::nullopt;
                        return opaque ? value : clipUtf8(value, cap);
                    } catch (...) {}
                    return std::nullopt;
                };
                if (const auto V = LOOK(key); V)
                    return V;
                return alias ? LOOK(alias) : std::nullopt;
            };
            const auto optBoolHint = [&](const char* key, const char* alias) -> std::optional<bool> {
                const auto LOOK = [&](const char* K) -> std::optional<bool> {
                    const auto IT = hints.find(K);
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
                if (const auto V = LOOK(key); V)
                    return V;
                return alias ? LOOK(alias) : std::nullopt;
            };
            const auto optU32Hint = [&](const char* key, const char* alias) -> std::optional<uint32_t> {
                const auto LOOK = [&](const char* K) -> std::optional<uint32_t> {
                    const auto IT = hints.find(K);
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
                if (const auto V = LOOK(key); V)
                    return V;
                return alias ? LOOK(alias) : std::nullopt;
            };
            const auto optI64Hint = [&](const char* key, const char* alias) -> std::optional<int64_t> {
                const auto LOOK = [&](const char* K) -> std::optional<int64_t> {
                    const auto IT = hints.find(K);
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
                if (const auto V = LOOK(key); V)
                    return V;
                return alias ? LOOK(alias) : std::nullopt;
            };

            const std::string DESKTOP = clipUtf8(strHint("desktop-entry"), MAX_SOURCE_BYTES);
            // grouping identity; control bytes out — the bundle key joins
            // (app, group) on \x1f, and an app named with one split wrong
            const std::string APPKEY = [&] {
                std::string k = !DESKTOP.empty() ? DESKTOP : APP;
                std::erase_if(k, [](unsigned char c) { return c < 0x20; });
                return k;
            }();
            const std::string CAT     = strHint("category");
            const bool        CATEGORY_CONVERSATION = CAT.starts_with("im.") || CAT == "im" || CAT.starts_with("call.") || CAT == "call";

            // the structured conversation hints: the plain names are the
            // de-facto extension, the x-notify-* forms the private alias —
            // neither sits in the published spec's hint table
            const auto        CONV_ID_HINT        = optStrHint("conversation-id", "x-notify-conversation-id", MAX_CONV_ID_BYTES, true);
            const auto        CONV_TITLE_HINT     = optStrHint("conversation-title", "x-notify-conversation-title", 512);
            const auto        CONV_KIND_HINT      = optStrHint("conversation-kind", "x-notify-conversation-kind", MAX_CONV_KIND_BYTES, true);
            const auto        SENDER_ID_HINT      = optStrHint("sender-id", "x-notify-sender-id", MAX_SENDER_ID_BYTES, true);
            const auto        SENDER_NAME_HINT    = optStrHint("sender-name", "x-notify-sender-name", MAX_SENDER_NAME_BYTES);
            const auto        SENDER_ICON_HINT    = optStrHint("sender-icon", "x-notify-sender-icon", 512, true);
            const auto        MESSAGE_ID_HINT     = optStrHint("message-id", "x-notify-message-id", MAX_MESSAGE_ID_BYTES, true);
            const auto        DECLARED_GROUP_HINT = optStrHint("x-notify-group-key", nullptr, MAX_CONV_ID_BYTES, true);
            const auto        UNREAD_HINT         = optU32Hint("unread-count", "x-notify-unread-count");
            const auto        HISTORIC_HINT       = optBoolHint("message-historic", "x-notify-message-historic");
            const auto        MESSAGE_TIME_HINT   = optI64Hint("message-time", "x-notify-message-timestamp");
            const std::string CONV_ID        = CONV_ID_HINT ? *CONV_ID_HINT : std::string{};
            const std::string MESSAGE_ID     = MESSAGE_ID_HINT ? *MESSAGE_ID_HINT : std::string{};
            const std::string SENDER_NAME    = SENDER_NAME_HINT ? Parse::oneLine(Parse::sanitizeMarkup(*SENDER_NAME_HINT)) : std::string{};
            const bool        HISTORIC       = HISTORIC_HINT.value_or(false);

            // THE CONVERSATION MERGE (Android's MessagingStyle): every message
            // of one chat is ONE card. The primary contract is the sender's
            // stable conversation-id: display text and category alone are
            // insufficient — two Telegram chats can share a title, and a
            // browser can reuse visible text for unrelated site alerts.
            // Without a conversation-id the legacy contract stands: a fresh
            // Notify whose app + summary matches a live card rides the replace
            // path with the bodies joined (the fd.o conversation categories —
            // the summary IS the sender or the room — and x-canonical-append).
            // Cards that vanish never merge (a suppressed banner would strand
            // them), nor does the OSD band.
            std::string appendOnto;
            bool        canonicalAppend = false; // x-canonical-append: this card joins a conversation
            if (id == 0) {
                if (!CONV_ID.empty()) {
                    for (const auto& N : notifs)
                        if (!inOsdBand(N->id) && !vanishes(N) && TAKEABLE(*N) && Pixel::matchesConversation(APPKEY, CONV_ID, N->appKey, N->conversationId)) {
                            id = N->id;
                            break;
                        }
                } else {
                    const bool append = CATEGORY_CONVERSATION || hintFlag(hints, "x-canonical-append").value_or(false);
                    canonicalAppend = append;
                    if (append) {
                        // the CLIPPED summary, as stored: the raw one was both
                        // unbounded work and never equal past the clip
                        const auto KEY = Parse::oneLine(Parse::sanitizeMarkup(SUM, false, /*plainText=*/true));
                        // never onto a structured conversation: its body is
                        // rebuilt from its message log, and a joined body
                        // upserted as one message duplicated the transcript
                        for (const auto& N : notifs)
                            if (!inOsdBand(N->id) && !vanishes(N) && TAKEABLE(*N) && N->conversationId.empty() && N->appKey == APPKEY && N->summary == KEY) {
                                id         = N->id;
                                appendOnto = N->body;
                                break;
                            }
                    }
                }
            }

            // The reserved band is not a capability: its ids are for our
            // in-process OSD posts only, live or not, so a client can't pin a
            // band id, take over the volume card on screen, or ride the
            // band's DND bypass.
            if (inOsdBand(id) && !osd)
                id = 0;

            if (id == 0) {
                // Fresh ids count up from a low counter and skip any that's
                // still live, so they never collide with a displayed
                // notification. Crucially the counter is NOT dragged up to a
                // seen replaces_id (as it once was): the OSD scripts pin ids
                // in the 9990s, and bumping past 9991 handed the next fresh
                // notification 9992 — the brightness OSD's id — so a
                // keypress hijacked it. Low fresh ids and the pinned band
                // stay disjoint.
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
                // through (the urgency parse below lifts it back out), and the
                // OSD band, the user's own feedback (a volume key pressed
                // under DND must still answer)
                n->waiting = suspended && !inOsdBand(id);
                notifs.insert(notifs.begin(), n); // newest on top; a replace keeps its slot
                evictOverflow();
            }

            // Which conversation this arrival belongs to. A replace that
            // carries a conversation-id aims at that chat; one without it
            // keeps the target's conversation only for the same app.
            const bool        SAME_APP          = EXISTING && n->appKey == APPKEY;
            const std::string EFFECTIVE_CONV_ID = CONV_ID_HINT ? CONV_ID : SAME_APP ? n->conversationId : std::string{};
            const bool        SAME_CONVERSATION = EXISTING && !EFFECTIVE_CONV_ID.empty() && SAME_APP && n->conversationId == EFFECTIVE_CONV_ID;
            // x-canonical-append is a conversation by definition: its body is
            // the joined transcript, chronological, like the category's
            const bool CONVERSATION = CATEGORY_CONVERSATION || canonicalAppend || !EFFECTIVE_CONV_ID.empty() || (SAME_CONVERSATION && n->conversation);
            // the arrival below may rewrite messages and kind. One serial for
            // every card, not a per-card count: a closed id that a client
            // reuses starts a new card, whose first rev must not equal the
            // old card's in a stale click
            static uint64_t revSerial = 0;
            n->rev = ++revSerial;
            if (!SAME_CONVERSATION) {
                n->messages.clear();
                n->unreadCount = 0;
            }

            // before the re-alert below: DND puts a shown card back as it was
            const bool WAS_WAITING = EXISTING && n->waiting, WAS_BANNER = EXISTING && n->banner, WAS_ABSORBED = EXISTING && n->absorbed;
            n->arrived = Time::steadyNow(); // a replace refreshes the age, like a new arrival would
            // A replace re-alerts (the OSD sweep relies on it); the merge
            // above keeps aiming a chat's new messages at the card that holds
            // it, so one card stays the whole conversation.
            n->banner   = true;
            n->absorbed = false; // a re-arrival re-banners, whatever parked the previous one
            n->appName = APP;
            n->sender   = sender; // the X11 activation lookup resolves the app's window by this pid
            n->orphaned = false;
            n->summary = Parse::oneLine(Parse::sanitizeMarkup(SUM, false, /*plainText=*/true));
            std::string bodyText = TXT;
            n->bodyImages.clear();
            for (const auto& P : Parse::extractImages(bodyText, std::max(64, (int)NAwesome::cfg().getI("plugin:awesome:notify:max_icon") * 2), MAX_BODY_IMAGES))
                n->bodyImages.push_back(SBodyImage{.src = P.src, .alt = P.alt});
            n->body = capUtf8(Parse::sanitizeMarkup(bodyText, /*allowLinks=*/true));
            if (CONVERSATION) {
                // the conversation's lines must stay one message each: the
                // sender's leading bold line would otherwise be the body's
                // newest line, hiding the message under it
                n->body = Parse::foldSenderPrefix(std::move(n->body));
            }
            if (!appendOnto.empty())
                n->body = Parse::joinAppend(appendOnto, n->body);

            // the structured conversation state: identity, kind, icon, the
            // app's declared group, and — when a conversation-id is in force
            // — the bounded message log the body is rebuilt from
            const int ICONPX = std::max(8, (int)NAwesome::cfg().getI("plugin:awesome:notify:max_icon"));
            n->conversationId = EFFECTIVE_CONV_ID;
            if (CONV_TITLE_HINT)
                n->conversationTitle = CONV_TITLE_HINT->empty() ? n->summary : Parse::oneLine(Parse::sanitizeMarkup(*CONV_TITLE_HINT, false, /*plainText=*/true));
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
            if (DECLARED_GROUP_HINT) {
                n->declaredGroupKey = *DECLARED_GROUP_HINT;
                std::erase_if(n->declaredGroupKey, [](unsigned char c) { return c < 0x20; }); // the \x1f join, as the app key
            } else if (!SAME_APP)
                n->declaredGroupKey.clear();

            if (!EFFECTIVE_CONV_ID.empty()) {
                const auto viewOf = [](const std::optional<std::string>& value) -> std::optional<std::string_view> {
                    return value ? std::optional<std::string_view>{*value} : std::nullopt;
                };
                const auto SENDER_NAME_VIEW = SENDER_NAME_HINT ? std::optional<std::string_view>{SENDER_NAME} : std::nullopt;
                const auto MUTATION = Pixel::upsertMessage(n->messages, MESSAGE_ID, n->body, viewOf(SENDER_ID_HINT), SENDER_NAME_VIEW, viewOf(SENDER_ICON_HINT), MESSAGE_TIME_HINT, HISTORIC_HINT);
                rebuildConversationParticipants(*n, ICONPX);
                n->body        = conversationBody(n, Pixel::MAX_PRESENTED_CONVERSATION_MESSAGES);
                n->unreadCount = Pixel::updatedUnreadCount(n->unreadCount, UNREAD_HINT, HISTORIC, MUTATION);
            } else {
                n->unreadCount = 0;
                n->participants.clear();
            }

            n->urgency  = 1;
            n->progress = -1;
            n->image.clear();
            n->identity.clear();
            n->pixels.clear();
            n->hasPixels = false;
            n->pw = n->ph = 0;
            n->pixelsJob  = 0; // a job for the replaced image lands on no card
            n->pixelsHash = 0;
            n->scaled.reset();
            n->imageRetry.clear();
            n->identityRetry = false;

            if (const auto IT = hints.find("urgency"); IT != hints.end())
                try {
                    n->urgency = IT->second.get<uint8_t>();
                } catch (...) {
                    try {
                        n->urgency = (uint8_t)std::clamp(IT->second.get<int32_t>(), 0, 2);
                    } catch (...) {}
                }
            // DND holds a re-arrival as it holds a new one (a replace or a
            // conversation merge re-alerts otherwise): a card already shown —
            // a banner, or a row in the shade — is updated in place, silently,
            // as it was; one still queued stays queued for the resume
            bool quiet = false;
            if (EXISTING && suspended && n->urgency < 2 && !inOsdBand(n->id) && !WAS_WAITING) {
                quiet       = true;
                n->banner   = WAS_BANNER;
                n->absorbed = WAS_ABSORBED;
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
            const int PIXCAP = std::max((int)NAwesome::cfg().getI("plugin:awesome:notify:width") * 2, (int)NAwesome::cfg().getI("plugin:awesome:notify:max_icon") * 3);
            for (const auto* KEY : {"image-data", "image_data", "icon_data"})
                if (const auto IT = hints.find(KEY); IT != hints.end() && !n->hasPixels)
                    try {
                        takeImageData(*n, IT->second.get<Parse::ImageData>(), PIXCAP, ICONPX);
                    } catch (...) {}
            if (!n->pixels.empty())
                boundRetainedPixels(ICONPX);
            if (!n->hasPixels) {
                std::string cand;
                for (const auto* KEY : {"image-path", "image_path"})
                    if (const auto IT = hints.find(KEY); IT != hints.end() && cand.empty())
                        try {
                            cand = IT->second.get<std::string>();
                        } catch (...) {}
                if (!cand.empty())
                    cand = clipUtf8(std::move(cand), MAX_SOURCE_BYTES);
                bool deferred = false;
                n->image      = resolveName(cand, ICONPX, deferred);
                if (deferred)
                    n->imageRetry = std::move(cand);
            }
            n->desktopEntry  = DESKTOP;
            n->appIconSource = APPICON;
            n->identityRetry = resolveIdentity(*n, ICONPX);
            n->namesDeferred = n->identityRetry || !n->imageRetry.empty() || std::ranges::any_of(n->participants, [](const auto& P) { return P.iconRetry; });
            if (n->namesDeferred)
                iconNamesDeferred();
            n->appKey = APPKEY;

            // The inline-reply protocol (KDE's, which Telegram/Fractal
            // speak): the sender adds an action keyed "inline-reply" only
            // when the server advertises the capability, and expects
            // NotificationReplied back. It is NOT a button — it opens the
            // row's reply field.
            n->canReply         = false;
            n->replyPlaceholder = clipUtf8(strHint("x-kde-reply-placeholder-text"), MAX_REPLY_TEXT_BYTES);
            n->replySubmitText  = clipUtf8(strHint("x-kde-reply-submit-button-text"), MAX_REPLY_TEXT_BYTES);

            // actions arrive as [id0,label0, id1,label1, ...]. Every named
            // pair becomes a button; "default" is the card's primary and gets
            // NO button on either surface — the spec defines it as "the
            // default action (usually invoked by clicking the
            // notification)" and says implementations are free not to
            // display it, so a body click is what fires it and a button would
            // only duplicate that.
            n->defaultAction.clear();
            n->actions.clear();
            for (size_t i = 0; i + 1 < actions.size(); i += 2) {
                const auto AID = clipUtf8(actions[i], MAX_ACTION_KEY_BYTES);
                if (AID == "default")
                    n->defaultAction = AID;
                else if (AID == "inline-reply") {
                    n->canReply = true;
                    if (n->replySubmitText.empty())
                        n->replySubmitText = clipUtf8(actions[i + 1], MAX_REPLY_TEXT_BYTES); // the sender's own "Reply" label
                } else if (!actions[i + 1].empty() && n->actions.size() < MAX_ACTIONS) { // an empty label has no button to draw
                    const auto LBL = clipUtf8(actions[i + 1], MAX_ACTION_KEY_BYTES);
                    n->actions.push_back(SAction{.id = std::move(AID), .label = std::move(LBL)});
                }
            }
            // a lone named action doubles as the body-click default; it keeps
            // its own button too, since it was given a label to show
            if (n->defaultAction.empty() && n->actions.size() == 1)
                n->defaultAction = n->actions.front().id;

            n->resident    = hintFlag(hints, "resident").value_or(false);
            n->actionIcons = hintFlag(hints, "action-icons").value_or(false);
            n->transient   = hintFlag(hints, "transient").value_or(false);

            // Conversation identity (an fd.o category or a stable
            // conversation-id) outranks ordinary cards and never bundles
            // into an app digest (Android keeps every chat its own card).
            // Ordering and merging only — no per-app casing.
            n->conversation = CONVERSATION;

            if (n->urgency >= 2) // critical never expires on its own (spec; README)
                n->timeoutMs = 0;
            else if (expireTimeout > 0)
                n->timeoutMs = expireTimeout;
            else if (expireTimeout == 0)
                n->timeoutMs = 0;
            else // -1: the client leaves it to us
                n->timeoutMs = defaultTimeout(*n);
            if (n->timeoutMs > 0 && !n->waiting) // a queued card's clock starts at the resume
                n->deadline = Time::steadyNow() + std::chrono::milliseconds((int64_t)n->timeoutMs);

            // one live popup per app: while this app already shows a banner,
            // a new non-critical arrival is born resident — it lands
            // silently in the shade (folded, badge-counted), no second
            // popup, no repeat sound. A replace re-alerting its own live
            // card is not a second banner (appHasBanner skips self).
            // Critical always punches through. Banners show over a
            // fullscreen window too — the ecosystem default; DND and the
            // shade are the user's escape hatches.
            const bool SOFT      = !n->waiting && n->urgency < 2 && !vanishes(n);
            const bool COALESCED = SOFT && NAwesome::cfg().getI("plugin:awesome:notify:coalesce_popups") && appHasBanner(n);
            if (COALESCED)
                n->banner = false;

            if (!n->waiting) // a suspended arrival is invisible: no warm, no damage
                notifChanged();

            // sound: a shown arrival plays sound-file/sound-name through the
            // libcanberra player unless the client suppresses it. DND-queued
            // (waiting) arrivals stay silent; the resume doesn't replay.
            if (!n->waiting) {
                // a coalesced or DND-quiet arrival announces itself neither;
                // the hint can only add silence, never lift ours
                const bool  suppress = COALESCED || quiet || hintFlag(hints, "suppress-sound").value_or(false);
                std::string soundFile, soundName;
                if (const auto IT = hints.find("sound-file"); IT != hints.end())
                    try {
                        soundFile = IT->second.get<std::string>();
                    } catch (...) {}
                if (const auto IT = hints.find("sound-name"); IT != hints.end())
                    try {
                        soundName = IT->second.get<std::string>();
                    } catch (...) {}
                soundFile = clipUtf8(std::move(soundFile), MAX_SOURCE_BYTES);
                soundName = clipUtf8(std::move(soundName), MAX_SOURCE_BYTES);
                soundFile = Parse::fileUriPath(std::move(soundFile));
                const std::string CMD{NAwesome::cfg().getS("plugin:awesome:notify:sound_command")};
                if (!suppress && !CMD.empty()) {
                    if (!soundFile.empty())
                        NAwesome::Jobs::inst().spawn(NAwesome::Jobs::SOUND, {CMD.c_str(), "-f", soundFile.c_str(), nullptr});
                    else if (!soundName.empty())
                        NAwesome::Jobs::inst().spawn(NAwesome::Jobs::SOUND, {CMD.c_str(), "-i", soundName.c_str(), nullptr});
                }
            }

            rearmExpiry();
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
                        if (!N->banner || N->timeoutMs <= 0 || N->waiting || N->id == heldBanner || N->deadline > NOW)
                            continue;
                        if (vanishes(N)) {
                            gone.push_back(N->id);
                            continue;
                        }
                        // into the shade: it still exists (we advertise
                        // persistence), so no NotificationClosed — libnotify
                        // and GLib forget a closed id, and its actions in the
                        // shade then reached nobody
                        N->banner = false;
                        changed   = true;
                    }
                    for (const auto ID : gone) {
                        std::erase_if(notifs, [&](const auto& N) { return N->id == ID; });
                        Bus::emitClosed(ID, R_EXPIRED);
                    }
                    if (changed || !gone.empty())
                        notifChanged();
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

    void retryDeferredNames(SNotif& n) {
        if (!n.namesDeferred)
            return;
        const int ICONPX = std::max(8, (int)NAwesome::cfg().getI("plugin:awesome:notify:max_icon"));
        bool      again  = false;
        if (!n.imageRetry.empty()) {
            bool deferred = false;
            n.image       = Model::resolveName(n.imageRetry, ICONPX, deferred);
            if (!deferred)
                n.imageRetry.clear(); // landed, or a true miss
            again |= deferred;
        }
        if (n.identityRetry) {
            n.identityRetry = Model::resolveIdentity(n, ICONPX);
            again |= n.identityRetry;
        }
        for (auto& P : n.participants)
            if (P.iconRetry) {
                bool deferred = false;
                P.icon        = Model::resolveName(P.iconSource, ICONPX, deferred);
                P.iconRetry   = deferred;
                again |= deferred;
            }
        n.namesDeferred = again;
        if (again)
            iconNamesDeferred();
    }

} // namespace NAwesome::Notify
