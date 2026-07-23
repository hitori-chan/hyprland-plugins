// hyprnotify/bus.cpp — the org.freedesktop.Notifications daemon and the model

#include "common/busclient.hpp"
#include "common/icons.hpp"
#include "common/lifecycle.hpp"

#include "hyprnotify.hpp"

#include <hyprland/src/protocols/XDGActivation.hpp>

namespace NHyprnotify {

    std::vector<SP<SNotif>> notifs;

    namespace Bus {
        static const sdbus::InterfaceName     IFACE{"org.freedesktop.Notifications"};
        // the shell's own face on the same object (dunst does the same with
        // org.dunstproject.cmd0): the bar's bell reads State and calls Toggle
        // over the bus — the sanctioned cross-plugin channel, never symbols
        static const sdbus::InterfaceName     CIFACE{"org.hitori.hyprnotify"};

        static std::unique_ptr<sdbus::IObject> obj;
        static NHyprCommon::CBusLink           g_bus;
        static SP<CEventLoopTimer>             expiry;
        static uint32_t                        nextId    = 1;
        static bool                            suspended = false; // DND
        static NHyprCommon::CHop               pendingState;

        // A drain must never run synchronously from here: emits happen inside
        // method handlers, i.e. inside processPendingEvent, and sd-bus dispatch
        // is not re-entrant. Park it on the link's timer instead.
        void pollSoon() {
            g_bus.pollSoon();
        }

        // ---- the model ----

        static SP<SNotif> byId(uint32_t id) {
            for (const auto& N : notifs)
                if (N->id == id)
                    return N;
            return nullptr;
        }

        // ---- history: dismissed/app-closed cards, kept whole for the
        //      center's history view and for recall ----

        static std::vector<SP<SNotif>> history; // oldest first, newest at back
        static uint64_t                histSeq = 0;

        // A card leaving the model is retained — except OSD-band and
        // progress cards (a volume blip is not history) and transients (the
        // hint opts out). The object moves whole: content and decoded image
        // textures stay (a pixel-built avatar's buffer was freed at upload —
        // the texture is the only copy left); text rasters live in render's
        // keyed cache and age out on their own.
        static void retire(const SP<SNotif>& n) {
            if (!n || n->transient || n->progress >= 0 || inOsdBand(n->id))
                return;
            const size_t CAP = std::max<int64_t>(0, cfg.maxHistory->value());
            if (CAP == 0)
                return;
            n->waiting   = false;
            n->banner    = false;
            n->timeoutMs = 0;
            n->hseq      = ++histSeq;
            history.push_back(n);
            while (history.size() > CAP)
                history.erase(history.begin());
        }

        void rearmExpiry() {
            if (!expiry)
                return;
            const auto NOW  = Time::steadyNow();
            int64_t    next = -1;
            for (const auto& N : notifs) {
                if (!N->banner || N->timeoutMs <= 0 || N->waiting)
                    continue;
                // clamp before comparing: -1 is the "none" sentinel, and an
                // overdue card's negative remaining time must still win
                const auto MS = std::max<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(N->deadline - NOW).count(), 1);
                if (next < 0 || MS < next)
                    next = MS;
            }
            if (next < 0)
                expiry->updateTimeout(std::nullopt);
            else
                expiry->updateTimeout(std::chrono::milliseconds(std::max<int64_t>(next, 1)));
        }

        static void emitClosed(uint32_t id, uint32_t reason) {
            if (!obj)
                return;
            try {
                obj->emitSignal("NotificationClosed").onInterface(IFACE).withArguments(id, reason);
            } catch (...) {} // a dead bus must not unwind through the timer/doLater C frames
            pollSoon();
        }

        std::string stateString() {
            return "center:" + std::to_string(centerVisible() ? 1 : 0) + " live:" + std::to_string(notifs.size()) + " hist:" + std::to_string(history.size()) +
                " dnd:" + std::to_string(suspended ? 1 : 0);
        }

        // the badge's truth is the shade: bannered popups + resident cards.
        // Never history (that lives behind the clock), never the DND queue
        // (invisible until the resume), never the OSD band (a volume card
        // is an OSD, not a notification).
        static std::pair<uint32_t, uint32_t> badgeCounts() {
            uint32_t live = 0, kept = 0;
            for (const auto& N : notifs) {
                if (N->waiting || inOsdBand(N->id))
                    continue;
                (N->banner ? live : kept)++;
            }
            return {live, kept};
        }

        // the bar's bell: live + kept + dnd + center, coalesced per model change
        void emitStateSoon() {
            pendingState.arm([]() {
                if (!obj)
                    return;
                try {
                    const auto [LIVE, KEPT] = badgeCounts();
                    obj->emitSignal("State").onInterface(CIFACE).withArguments(LIVE, KEPT, suspended, centerVisible());
                } catch (...) {}
                pollSoon();
            });
        }

        void closeOne(uint32_t id, uint32_t reason) {
            if (const auto N = byId(id))
                retire(N);
            const auto BEFORE = notifs.size();
            std::erase_if(notifs, [&](const auto& N) { return N->id == id; });
            if (notifs.size() == BEFORE)
                return;
            emitClosed(id, reason);
            notifChanged();
            rearmExpiry();
        }

        void dismissAllLive() {
            // the sweep clears what the user can see (banners AND resident
            // shade cards); cards the DND queue holds were never seen and
            // stay for the resume
            const auto BEFORE = notifs.size();
            for (const auto& N : notifs)
                if (!N->waiting) {
                    emitClosed(N->id, R_DISMISSED);
                    retire(N);
                }
            std::erase_if(notifs, [](const auto& N) { return !N->waiting; });
            if (notifs.size() == BEFORE)
                return;
            notifChanged();
            rearmExpiry();
        }

        void dismissApp(const std::string& appKey) {
            const auto BEFORE = notifs.size();
            for (const auto& N : notifs)
                if (!N->waiting && N->appKey == appKey) {
                    emitClosed(N->id, R_DISMISSED);
                    retire(N);
                }
            std::erase_if(notifs, [&](const auto& N) { return !N->waiting && N->appKey == appKey; });
            if (notifs.size() == BEFORE)
                return;
            notifChanged();
            rearmExpiry();
        }

        void invokeAction(uint32_t id, const std::string& key) {
            if (!obj)
                return;
            try {
                // spec 1.3: the token signal precedes the action, so the
                // sender's xdg-activation request can actually raise it —
                // tokenless activates only flag urgent
                if (PROTO::activation)
                    obj->emitSignal("ActivationToken").onInterface(IFACE).withArguments(id, PROTO::activation->mintToken());
                obj->emitSignal("ActionInvoked").onInterface(IFACE).withArguments(id, key);
            } catch (...) {}
            pollSoon();
        }

        // Android shade semantics for history rows: the ORIGINAL action list
        // stays live. Invoking emits ActionInvoked with the ORIGINAL id —
        // best effort, apps still tracking it react (Telegram's mark-as-read
        // does) — and the entry is consumed.
        void invokeHistoryAction(uint64_t hseq, const std::string& key) {
            const auto IT = std::ranges::find_if(history, [&](const auto& H) { return H->hseq == hseq; });
            if (IT == history.end())
                return;
            invokeAction((*IT)->id, key);
            history.erase(IT);
            notifChanged();
        }

        void toggleSuspend() {
            suspended = !suspended;
            if (suspended) {
                notifChanged(); // the center's ⊖ lights up
                return;         // visible cards live out their timeouts; new arrivals queue
            }
            const auto NOW = Time::steadyNow();
            for (const auto& N : notifs) {
                if (!N->waiting)
                    continue;
                N->waiting = false;
                N->banner  = true; // never seen: the resume shows the banner
                if (N->timeoutMs > 0)
                    N->deadline = NOW + std::chrono::milliseconds((int64_t)N->timeoutMs);
            }
            notifChanged();
            rearmExpiry();
        }

        bool suspendedNow() {
            return suspended;
        }

        // the client sent -1 (or a recall re-arms): per-urgency defaults;
        // critical stays until dismissed
        static float defaultTimeout(uint8_t urgency) {
            return urgency >= 2 ? 0.f : (float)(urgency == 0 ? cfg.timeoutLow->value() : cfg.timeoutNormal->value());
        }

        // Cap the stack: the oldest non-critical goes first; only an
        // all-critical stack starts losing its oldest critical. The newest
        // card at begin() always survives (the scan stops short of it), and
        // an evicted card stays recallable from history (retire).
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
                retire(*victim);
                notifs.erase(victim);
                emitClosed(VID, R_UNDEFINED);
            }
        }

        // Pop a retained card back onto the stack as a fresh notification
        // (naughty/dunst history-pop): new id, fresh timeout; the arrival
        // stamp stays original, so its age line keeps telling the truth.
        bool recallAt(uint64_t hseq) {
            const auto IT = std::ranges::find_if(history, [&](const auto& H) { return H->hseq == hseq; });
            if (IT == history.end())
                return false;
            auto n = *IT;
            history.erase(IT);
            n->hseq    = 0;
            n->waiting = false;
            n->banner  = true;
            do {
                n->id = nextId++;
                if (nextId == 0)
                    nextId = 1;
            } while (byId(n->id) || inOsdBand(n->id));
            n->timeoutMs = defaultTimeout(n->urgency);
            if (n->timeoutMs > 0)
                n->deadline = Time::steadyNow() + std::chrono::milliseconds((int64_t)n->timeoutMs);
            notifs.insert(notifs.begin(), n);
            evictOverflow();
            notifChanged();
            rearmExpiry();
            return true;
        }

        void recall() {
            if (!history.empty())
                recallAt(history.back()->hseq);
        }

        void eraseHistory(uint64_t hseq) {
            if (std::erase_if(history, [&](const auto& H) { return H->hseq == hseq; }))
                notifChanged();
        }

        void eraseHistoryApp(const std::string& appKey) {
            if (std::erase_if(history, [&](const auto& H) { return H->appKey == appKey; }))
                notifChanged();
        }

        void clearHistory() {
            if (history.empty())
                return;
            history.clear();
            notifChanged();
        }

        const std::vector<SP<SNotif>>& historyView() {
            return history;
        }

        size_t historySize() {
            return history.size();
        }

        // ---- incoming payload massage ----

        // We advertise body-markup, so the whitelisted Pango tags pass through
        // live; everything else is neutralized into content. Two client dialects
        // must both come out right: a markup-aware client escapes its reserved
        // chars ("a &amp;amp; b"), a naive one sends them raw ("a & b") — a bare
        // '&'/'<' that forms no entity/tag is escaped so Pango renders it
        // verbatim either way. Disallowed tags are dropped (spec: "filter them
        // out"); ALLOW_LINKS adds <a> (hyperlinks phase). <img> never reaches
        // here — it is extracted before sanitizing (body-images phase).
        static bool allowedTag(const std::string& name, bool allowLinks) {
            return name == "b" || name == "i" || name == "u" || name == "span" || name == "br" || (allowLinks && name == "a");
        }

        static std::string sanitizeMarkup(const std::string& in, bool allowLinks = false) {
            std::string out;
            out.reserve(in.size() + 16);
            for (size_t i = 0; i < in.size();) {
                const char CH = in[i];
                if (CH == '<') {
                    size_t j = i + 1;
                    if (j < in.size() && in[j] == '/')
                        j++;
                    const size_t NS = j;
                    while (j < in.size() && std::isalpha((unsigned char)in[j]))
                        j++;
                    if (j > NS) {
                        const auto END = in.find('>', j);
                        if (END != std::string::npos) {
                            std::string name = in.substr(NS, j - NS);
                            std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });
                            if (name == "br")
                                out += '\n'; // a line break, whatever the card does with it
                            else if (allowedTag(name, allowLinks))
                                out += in.substr(i, END - i + 1); // live tag, verbatim (Pango validates attrs)
                            // else: disallowed tag, dropped
                            i = END + 1;
                            continue;
                        }
                    }
                    out += "&lt;"; // a bare '<' that forms no tag: literal
                    i++;
                    continue;
                }
                if (CH == '&') {
                    const auto END = in.find(';', i);
                    if (END != std::string::npos && END - i <= 10) {
                        const auto E = in.substr(i + 1, END - i - 1);
                        if (E == "amp" || E == "lt" || E == "gt" || E == "quot" || E == "apos" || (E.size() > 1 && E[0] == '#')) {
                            out += in.substr(i, END - i + 1); // a real entity: Pango decodes it
                            i = END + 1;
                            continue;
                        }
                    }
                    out += "&amp;"; // a bare '&': literal
                    i++;
                    continue;
                }
                if (CH != '\r')
                    out += CH;
                i++;
            }
            return out;
        }

        static std::string oneLine(std::string s) {
            for (auto& c : s)
                if (c == '\n')
                    c = ' ';
            return s;
        }

        // A path (file:// or absolute) is taken verbatim; anything else is a
        // freedesktop icon NAME resolved against the theme (dunst/mako do this,
        // and so should a compositor daemon). "" = nothing usable.
        static std::string resolveImage(std::string s, int sizePx) {
            if (s.empty())
                return "";
            if (s.starts_with("file://"))
                s.erase(0, 7);
            if (s.starts_with('/'))
                return s;
            return NHyprCommon::resolveIconName(s, sizePx);
        }

        // <img src="..."> is not a Pango tag; pull it from the body before the
        // markup sanitizer would drop it, resolve each src (path or themed name),
        // and return the thumbnails — removing the tags from the text. http(s)
        // and data: srcs aren't fetched, so they're skipped.
        static std::vector<std::string> extractImages(std::string& body, int sizePx) {
            std::vector<std::string> out;
            for (size_t i = 0; i < body.size();) {
                if (body[i] != '<') {
                    i++;
                    continue;
                }
                size_t j = i + 1;
                while (j < body.size() && std::isalpha((unsigned char)body[j]))
                    j++;
                std::string name = body.substr(i + 1, j - i - 1);
                std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });
                if (name != "img") {
                    i++;
                    continue;
                }
                const auto END = body.find('>', j);
                if (END == std::string::npos)
                    break;
                const auto  TAG = body.substr(i, END - i + 1);
                std::string tl  = TAG; // case-insensitive attr search, same offsets as TAG
                std::ranges::transform(tl, tl.begin(), [](unsigned char c) { return std::tolower(c); });
                std::string src;
                if (const auto SP = tl.find("src"); SP != std::string::npos)
                    if (const auto Q = TAG.find_first_of("\"'", SP); Q != std::string::npos)
                        if (const auto Q2 = TAG.find(TAG[Q], Q + 1); Q2 != std::string::npos)
                            src = TAG.substr(Q + 1, Q2 - Q - 1);
                if (!src.empty() && !src.starts_with("http") && !src.starts_with("data:"))
                    if (const auto P = resolveImage(src, sizePx); !P.empty())
                        out.push_back(P);
                body.erase(i, END - i + 1); // drop the tag from the text
            }
            return out;
        }

        // the spec's image-data: width, height, rowstride, has_alpha,
        // bits_per_sample, channels, RGB(A) bytes -> premultiplied BGRA
        using ImageData = sdbus::Struct<int32_t, int32_t, int32_t, bool, int32_t, int32_t, std::vector<uint8_t>>;

        static void unpackImageData(SNotif& n, const ImageData& d) {
            const int32_t W = std::get<0>(d), H = std::get<1>(d), STRIDE = std::get<2>(d), BPS = std::get<4>(d), CH = std::get<5>(d);
            const auto&   DATA = std::get<6>(d);
            // STRIDE must cover a row: a lying stride (0) would let a tiny
            // message claim gigapixel W*H and the resize below map it all.
            // The 16 MP cap bounds a genuine ~128 MB image-data (the D-Bus
            // message max) that would otherwise map + premultiply in full.
            if (W <= 0 || H <= 0 || (int64_t)W * H > (16 << 20) || BPS != 8 || (CH != 3 && CH != 4) || (int64_t)STRIDE < (int64_t)W * CH || DATA.size() < (size_t)STRIDE * (H - 1) + (size_t)W * CH)
                return;
            n.pixels.resize((size_t)W * H * 4);
            for (int32_t y = 0; y < H; y++) {
                const uint8_t* row = DATA.data() + (size_t)y * STRIDE;
                uint8_t*       out = n.pixels.data() + (size_t)y * W * 4;
                for (int32_t x = 0; x < W; x++) {
                    const uint8_t R = row[x * CH], G = row[x * CH + 1], B = row[x * CH + 2], A = CH == 4 ? row[x * CH + 3] : 255;
                    out[x * 4]     = (uint8_t)(B * A / 255);
                    out[x * 4 + 1] = (uint8_t)(G * A / 255);
                    out[x * 4 + 2] = (uint8_t)(R * A / 255);
                    out[x * 4 + 3] = A;
                }
            }
            n.pw        = W;
            n.ph        = H;
            n.hasPixels = true;
            // keep only what a card can ever paint: warm frees visible cards'
            // buffers after upload, but an off-screen card would hold its
            // full-size pixmap until it scrolls on. 2x card width covers the
            // hero layout at any monitor scale; warm still scales exactly.
            shrinkPixels(n, std::max((int)cfg.width->value() * 2, (int)cfg.maxIcon->value() * 3));
        }

        // Join an appended conversation body under the cap: newest lines
        // append at the back, oldest lines drop off the front whole.
        static std::string joinAppend(const std::string& oldBody, const std::string& add) {
            std::string joined = oldBody.empty() ? add : oldBody + "\n" + add;
            constexpr size_t CAP = 8192;
            while (joined.size() > CAP) {
                const auto NL = joined.find('\n');
                if (NL == std::string::npos) {
                    joined.erase(0, joined.size() - CAP);
                    break;
                }
                joined.erase(0, NL + 1);
            }
            return joined;
        }

        static uint32_t handleNotify(const std::string& appName, uint32_t replacesId, const std::string& appIcon, const std::string& summary, const std::string& body,
                                     const std::vector<std::string>& actions, const std::map<std::string, sdbus::Variant>& hints, int32_t expireTimeout) {
            uint32_t id = replacesId;

            // x-canonical-append (notify-osd's extension; Telegram sends it on
            // every message): a fresh Notify matching a live card's app +
            // summary rides the replace path with the bodies joined — one
            // conversation, one growing card, one history entry at retire.
            // The OSD band never appends.
            std::string appendOnto;
            if (id == 0) {
                bool append = false;
                if (const auto IT = hints.find("x-canonical-append"); IT != hints.end())
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
                    const auto SUM = oneLine(sanitizeMarkup(summary));
                    for (const auto& N : notifs)
                        if (!inOsdBand(N->id) && N->appName == appName && N->summary == SUM) {
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
            n->banner  = true;              // a replace re-alerts (the OSD sweep relies on it)
            n->appName = appName;
            n->summary = oneLine(sanitizeMarkup(summary));
            std::string bodyText = body;
            n->bodyImages.clear();
            for (const auto& P : extractImages(bodyText, std::max(64, (int)cfg.maxIcon->value() * 2)))
                n->bodyImages.push_back({P});
            n->body = sanitizeMarkup(bodyText, /*allowLinks=*/true);
            if (!appendOnto.empty())
                n->body = joinAppend(appendOnto, n->body);

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
            for (const auto* KEY : {"image-data", "image_data", "icon_data"})
                if (const auto IT = hints.find(KEY); IT != hints.end() && n->pixels.empty())
                    try {
                        unpackImageData(*n, IT->second.get<ImageData>());
                    } catch (...) {}
            if (n->pixels.empty()) {
                std::string cand;
                for (const auto* KEY : {"image-path", "image_path"})
                    if (const auto IT = hints.find(KEY); IT != hints.end() && cand.empty())
                        try {
                            cand = IT->second.get<std::string>();
                        } catch (...) {}
                n->image = resolveImage(cand, ICONPX);
            }
            std::string desktopEntry;
            if (const auto IT = hints.find("desktop-entry"); IT != hints.end())
                try {
                    desktopEntry = IT->second.get<std::string>();
                } catch (...) {}
            n->identity = resolveImage(appIcon, ICONPX);
            if (n->identity.empty() && !desktopEntry.empty())
                n->identity = resolveImage(desktopEntry, ICONPX);
            // grouping keys on app identity: the desktop-entry id, else the name
            n->appKey = !desktopEntry.empty() ? desktopEntry : appName;

            // actions arrive as [id0,label0, id1,label1, ...]. "default" is the
            // body-click target (no button of its own); every other pair becomes
            // a button. A lone named action also doubles as the body-click
            // default, so a one-action card fires by clicking anywhere.
            n->defaultAction.clear();
            n->actions.clear();
            for (size_t i = 0; i + 1 < actions.size(); i += 2) {
                if (actions[i] == "default")
                    n->defaultAction = actions[i];
                else if (!actions[i + 1].empty()) // an empty label has no button to draw
                    n->actions.push_back(SAction{.id = actions[i], .label = actions[i + 1]});
            }
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

            if (expireTimeout > 0)
                n->timeoutMs = expireTimeout;
            else if (expireTimeout == 0)
                n->timeoutMs = 0;
            else // -1: the client leaves it to us
                n->timeoutMs = defaultTimeout(n->urgency);
            if (n->timeoutMs > 0 && !n->waiting) // a queued card's clock starts at the resume
                n->deadline = Time::steadyNow() + std::chrono::milliseconds((int64_t)n->timeoutMs);

            if (!n->waiting) // a suspended arrival is invisible: no warm, no damage
                notifChanged();

            // sound: a shown arrival plays sound-file/sound-name through the
            // libcanberra player unless the client suppresses it. DND-queued
            // (waiting) arrivals stay silent; the resume doesn't replay.
            if (!n->waiting) {
                bool        suppress = false;
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
            emitStateSoon();
            return id;
        }

        // ---- the daemon ----

        void init() {
            g_bus.onLost = [](const std::string& err) {
                HyprlandAPI::addNotification(PHANDLE, "[hyprnotify] bus lost, notifications disabled: " + err, CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
            };
            g_bus.dropOwned = []() { obj.reset(); };
            try {
                g_bus.open(false, "org.freedesktop.Notifications");
                obj = sdbus::createObject(*g_bus.conn(), sdbus::ObjectPath{"/org/freedesktop/Notifications"});

                obj->addVTable(sdbus::registerMethod("Notify")
                                   .withInputParamNames("app_name", "replaces_id", "app_icon", "summary", "body", "actions", "hints", "expire_timeout")
                                   .withOutputParamNames("id")
                                   .implementedAs([](std::string appName, uint32_t replacesId, std::string appIcon, std::string summary, std::string body,
                                                     std::vector<std::string> actions, std::map<std::string, sdbus::Variant> hints,
                                                     int32_t expireTimeout) { return handleNotify(appName, replacesId, appIcon, summary, body, actions, hints, expireTimeout); }),
                               // ignore_dbusclose (dunst's knob): an app revoking its
                               // own notification (Telegram on read-elsewhere) is
                               // ignored — the card lives out its banner and still
                               // retires into history on dismissal. Only the bus path
                               // is gated; user dismissals and expiry are untouched.
                               sdbus::registerMethod("CloseNotification").withInputParamNames("id").implementedAs([](uint32_t id) {
                                   if (cfg.ignoreDbusClose->value())
                                       return;
                                   closeOne(id, R_CLOSED);
                                   emitStateSoon();
                               }),
                               sdbus::registerMethod("GetCapabilities").withOutputParamNames("capabilities").implementedAs([]() {
                                   return std::vector<std::string>{"actions", "action-icons", "body", "body-markup", "body-hyperlinks", "body-images", "icon-static", "persistence", "sound"};
                               }),
                               sdbus::registerMethod("GetServerInformation").withOutputParamNames("name", "vendor", "version", "spec_version").implementedAs([]() {
                                   return std::tuple<std::string, std::string, std::string, std::string>{"hyprnotify", "hitori", VERSION, "1.3"};
                               }),
                               sdbus::registerSignal("NotificationClosed").withParameters<uint32_t, uint32_t>("id", "reason"),
                               sdbus::registerSignal("ActionInvoked").withParameters<uint32_t, std::string>("id", "action_key"),
                               sdbus::registerSignal("ActivationToken").withParameters<uint32_t, std::string>("id", "activation_token"))
                    .forInterface(IFACE);

                // the shell face: the bar's bell toggles the center and reads
                // the badge counts here
                obj->addVTable(sdbus::registerMethod("Toggle").implementedAs([]() { queueCenterToggle(); }),
                               sdbus::registerMethod("State").withOutputParamNames("live", "kept", "dnd", "center").implementedAs([]() {
                                   const auto [LIVE, KEPT] = badgeCounts();
                                   return std::tuple<uint32_t, uint32_t, bool, bool>{LIVE, KEPT, suspended, centerVisible()};
                               }),
                               sdbus::registerSignal("State").withParameters<uint32_t, uint32_t, bool, bool>("live", "kept", "dnd", "center"))
                    .forInterface(CIFACE);

                expiry = makeShared<CEventLoopTimer>(
                    std::nullopt,
                    [](SP<CEventLoopTimer>, void*) {
                        // RESIDENCY: a due banner emits reason 1 EXPIRED once
                        // and hides only the popup — the card stays in the
                        // shade until dismissed/acted. Transient and progress
                        // (OSD) cards vanish entirely.
                        const auto NOW     = Time::steadyNow();
                        bool       changed = false;
                        std::vector<uint32_t> gone;
                        for (const auto& N : notifs) {
                            if (!N->banner || N->timeoutMs <= 0 || N->waiting || N->deadline > NOW)
                                continue;
                            if (N->transient || N->progress >= 0 || inOsdBand(N->id)) {
                                gone.push_back(N->id);
                                continue;
                            }
                            N->banner = false;
                            emitClosed(N->id, R_EXPIRED);
                            changed = true;
                        }
                        for (const auto ID : gone) {
                            std::erase_if(notifs, [&](const auto& N) { return N->id == ID; });
                            emitClosed(ID, R_EXPIRED);
                        }
                        if (changed || !gone.empty()) {
                            notifChanged();
                            emitStateSoon();
                        }
                        rearmExpiry();
                    },
                    nullptr);
                g_pEventLoopManager->addTimer(expiry);

                g_bus.sync(); // drain anything queued during setup — the vtable is registered, nothing dispatches early
            } catch (const std::exception& E) {
                // most likely another daemon owns the name (dunst still installed)
                HyprlandAPI::addNotification(PHANDLE, std::string{"[hyprnotify] disabled: "} + E.what(), CHyprColor{1.0, 0.6, 0.2, 1.0}, 6000);
                obj.reset();
                g_bus.close();
            }
        }

        void exit() {
            g_bus.close(); // fd sources out BEFORE the connection dies
            if (expiry && g_pEventLoopManager)
                g_pEventLoopManager->removeTimer(expiry);
            expiry.reset();
            notifs.clear();
            history.clear();
            suspended = false;
        }
    }

} // namespace NHyprnotify
