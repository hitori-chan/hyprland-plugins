// awesome/notify/menu.cpp — the card menu, a card's options.
// Holding left on any card (a banner, a center card, a stack) opens it at
// the pointer:
//
//   Reply                          a card that takes a reply: opens its field
//   <the sender's actions>         in Notify order ("Mark as read")
//   ───
//   Mute <chat> for 1 Hour         a conversation only; "Unmute <chat>"
//   Mute <chat> Until Tomorrow     while a mute holds (the next 08:00:
//                                  "Until 08:00" while it is still today)
//   ───
//   Turn Off <App> Banners         toggles: a label says what the click
//   Silence <App> Sounds           does ("Turn On…", "Allow…" once off)
//   ───
//   Close All from <App>
//
// The switches persist (policy.cpp). The same panel lists Do Not Disturb's
// durations under the shade heading's moon, its right edge on the moon's.
//
// The menu is the notify layer's own, drawn last and hit first: it holds
// its card's banner while up, and closes on a pick, a press anywhere else,
// Esc, the lock or a native capture, the shade opening or closing, the
// cards moving monitor, and its card's death. An action fires as the
// revision the menu was opened over (a replace since fires nothing). Picks
// run from the click drain, never inside the input emission (crash class
// 6).
#include "ui.hpp"

#include "../core/walltimer.hpp"

#include <cmath>
#include <ctime>
#include <format>

namespace NAwesome::Notify::CardMenu {

    enum eVerb : uint8_t {
        SEP = 0,
        REPLY,
        ACTION, // SItem::action: the sender's action key
        MUTE_HOUR,
        MUTE_TOMORROW,
        UNMUTE,
        BANNERS,
        SOUNDS,
        CLOSE_APP,
        DND_FOR,     // Do Not Disturb for SItem::minutes
        DND_MORNING, // ... until the next 08:00
        DND_FOREVER, // ... until turned off
    };

    struct SItem {
        eVerb       verb;
        std::string label;
        std::string action;
        uint32_t    minutes = 0;
    };

    // the board's menu: 5 px of padding, 28 px rows whose labels sit 15 px
    // in, 11 px separators (a hairline 5 px off either row); Do Not
    // Disturb's is one width, 6 px under the moon
    constexpr double PAD = 5, ROWH = 28, SEPH = 11, LABEL_X = 15, MIN_W = 200, MAX_W = 320, DND_W = 220, ANCHOR_GAP = 6;
    constexpr size_t NAME_CP = 24; // a name past this many code points ellipsizes: the verb must stay readable

    static bool               s_open  = false;
    static uint32_t           s_owner = 0;
    static bool               s_dnd   = false; // Do Not Disturb's durations, not a card's menu
    static CBox               s_anchor;        // ... under the moon
    static uint64_t           s_rev = 0;       // the card's revision the menu was opened over
    static std::string        s_app, s_chat;   // the targets, captured at the open
    static std::string        s_appLabel, s_chatLabel; // ... and the names their rows keep
    static Vector2D           s_at;
    static std::vector<SItem> s_items;
    static CBox               s_box; // global logical, as last laid out

    static std::string clipName(const std::string& s) {
        size_t cp = 0;
        for (size_t i = 0; i < s.size(); i++)
            if (((unsigned char)s[i] & 0xC0) != 0x80 && cp++ == NAME_CP)
                return s.substr(0, i) + "…";
        return s;
    }

    // feedback (the OSD band, a transient card) is not a notification: it
    // has no switches to flip
    static SP<SNotif> target(uint32_t id) {
        const auto N = Model::byId(id);
        return N && !N->waiting && !Model::offShade(N) && !N->appKey.empty() ? N : nullptr;
    }

    bool canOpen(uint32_t id) {
        return target(id) != nullptr;
    }

    bool isOpen() {
        return s_open;
    }
    uint32_t owner() {
        return s_open ? s_owner : 0;
    }

    static void build(const SNotif& n) {
        s_items.clear();
        // what the sender offers comes first: an app's own actions over the
        // system's
        if (n.canReply)
            s_items.push_back({REPLY, n.replySubmitText.empty() ? "Reply" : clipName(n.replySubmitText)});
        for (const auto& A : n.actions)
            s_items.push_back({ACTION, clipName(A.label), A.id});
        if (!s_items.empty())
            s_items.push_back({SEP});
        const auto APP = clipName(s_appLabel);
        if (!s_chat.empty()) {
            const auto CHAT = clipName(s_chatLabel);
            if (Policy::mutedUntil(s_chat) > 0)
                s_items.push_back({UNMUTE, "Unmute " + CHAT});
            else {
                s_items.push_back({MUTE_HOUR, "Mute " + CHAT + " for 1 Hour"});
                s_items.push_back({MUTE_TOMORROW, "Mute " + CHAT + (nextMorningText().starts_with("Tomorrow") ? " Until Tomorrow" : " Until 08:00")});
            }
            s_items.push_back({SEP});
        }
        const int FLAGS = Policy::appFlags(s_app);
        s_items.push_back({BANNERS, (FLAGS & Policy::BANNERS_OFF ? "Turn On " : "Turn Off ") + APP + " Banners"});
        s_items.push_back({SOUNDS, (FLAGS & Policy::SOUNDS_OFF ? "Allow " : "Silence ") + APP + " Sounds"});
        s_items.push_back({SEP});
        s_items.push_back({CLOSE_APP, "Close All from " + APP});
    }

    // rebuilt by sync() while open: "Until 08:00" turns into tomorrow's at
    // midnight
    static void buildDnd() {
        s_items.clear();
        for (const auto& [LABEL, MIN] : {std::pair{"For 30 Minutes", 30u}, {"For 1 Hour", 60u}, {"For 3 Hours", 180u}})
            s_items.push_back({.verb = DND_FOR, .label = LABEL, .minutes = MIN});
        s_items.push_back({.verb = DND_MORNING, .label = "Until " + nextMorningText()});
        s_items.push_back({.verb = DND_FOREVER, .label = "Until Turned Off"});
    }

    bool openDnd(const CBox& anchor) {
        close();
        s_open   = true;
        s_owner  = 0;
        s_dnd    = true;
        s_anchor = anchor;
        s_box    = {};
        buildDnd();
        notifChanged(); // warm the labels, then damage
        return true;
    }

    bool open(uint32_t id, const Vector2D& at) {
        const auto N = target(id);
        if (!N)
            return false;
        s_open      = true;
        s_owner     = id;
        s_dnd       = false;
        s_rev       = N->rev;
        s_app       = N->appKey;
        s_chat      = Policy::chatKey(*N);
        s_appLabel  = Policy::appLabel(*N);
        s_chatLabel = Policy::chatLabel(*N);
        s_at        = at;
        s_box       = {};
        build(*N);
        refreshBannerHold(); // the banner under the menu must not expire out from under it
        notifChanged();      // warm the labels, then damage
        return true;
    }

    void close() {
        if (!s_open)
            return;
        s_open  = false;
        s_owner = 0;
        s_dnd   = false;
        s_items.clear();
        s_box = {};
        refreshBannerHold(); // back to a reply's or the pointer's hold, if any
        notifChanged();      // the last layout's bounds (its drop included) take the damage
    }

    void exit() {
        s_open  = false;
        s_owner = 0;
        s_dnd   = false;
        s_items.clear();
        s_app.clear();
        s_chat.clear();
        s_appLabel.clear();
        s_chatLabel.clear();
        s_box = {};
    }

    void sync() {
        if (s_open && s_dnd)
            buildDnd();
        else if (s_open && !target(s_owner))
            close(); // its card died (closed, expired away, replaced into feedback)
    }

    void pick(int index) {
        if (!s_open || index < 0 || (size_t)index >= s_items.size())
            return;
        const auto I    = s_items[index];
        const auto APP  = s_app;
        const auto CHAT = s_chat;
        const auto APPL = s_appLabel, CHATL = s_chatLabel;
        const auto ID   = s_owner;
        const auto REV  = s_rev;
        close();
        switch (I.verb) {
            case SEP: return;
            case REPLY: replyOpen(ID); return;
            case ACTION: invokeLive(ID, I.action, REV); return;
            case MUTE_HOUR:
                Policy::mute(CHAT, (int64_t)std::time(nullptr) + 3600, CHATL);
                Model::lowerBanners(APP);
                return;
            case MUTE_TOMORROW:
                Policy::mute(CHAT, nextMorning(), CHATL);
                Model::lowerBanners(APP);
                return;
            case UNMUTE: Policy::mute(CHAT, 0); return;
            case BANNERS: {
                const bool OFF = !(Policy::appFlags(APP) & Policy::BANNERS_OFF);
                Policy::setAppFlag(APP, Policy::BANNERS_OFF, OFF, APPL);
                if (OFF)
                    Model::lowerBanners(APP);
                return;
            }
            case SOUNDS: Policy::setAppFlag(APP, Policy::SOUNDS_OFF, !(Policy::appFlags(APP) & Policy::SOUNDS_OFF), APPL); return;
            case CLOSE_APP: Model::dismissAppAll(APP); return;
            case DND_FOR: Model::setSuspend((int64_t)std::time(nullptr) + (int64_t)I.minutes * 60); return;
            case DND_MORNING: Model::setSuspend(nextMorning()); return;
            case DND_FOREVER: Model::setSuspend(-1); return;
        }
    }

#ifdef AWESOME_GATE
    // "label;label": the open menu's rows ("" while closed)
    std::string describe() {
        std::string out;
        for (const auto& I : s_items)
            if (I.verb != SEP)
                out += (out.empty() ? "" : ";") + I.label;
        return s_open ? out : std::string{};
    }

    // "x y": the middle of the open menu's first row whose label starts so
    std::string rowAt(const std::string& prefix) {
        for (const auto& C : cards)
            if (C.kind == SCard::MENU_ITEM && C.id < s_items.size() && s_items[C.id].label.starts_with(prefix))
                return std::format("{} {}", (int)std::lround(C.box.x + C.box.w / 2), (int)std::lround(C.box.y + C.box.h / 2));
        return {};
    }
#endif

    // a card's menu opens with its corner at the pointer, one that would
    // leave the monitor flipping to the pointer's other side; Do Not
    // Disturb's hangs under the moon, over it without the room
    void render(const SPaint& P, const SType& T) {
        if (!s_open || s_items.empty())
            return;
        const auto   COLMENU = color(cfg().getColor("plugin:awesome:theme:col_glass")), COLRIM = color(cfg().getColor("plugin:awesome:theme:col_rim")),
                   COLTITLE = color(cfg().getColor("plugin:awesome:notify:col_title"));
        const double WMAX  = s_dnd ? DND_W : MAX_W;
        const int    CAPPX = (int)std::lround((WMAX - 2 * LABEL_X) * P.scale);
        const auto   labelOf = [&](const SItem& I) { return cachedText(I.label, COLTITLE, T.body, CAPPX, -1, 0, false, 400); };

        double w = s_dnd ? DND_W : MIN_W, h = 2 * PAD;
        for (const auto& I : s_items) {
            h += I.verb == SEP ? SEPH : ROWH;
            if (!s_dnd && I.verb != SEP)
                w = std::max(w, texW(labelOf(I), P.scale) + 2 * LABEL_X);
        }
        w = std::min(w, WMAX);

        const auto MB = P.mon->logicalBox();
        double     x, y;
        if (s_dnd) {
            x = std::clamp(s_anchor.x + s_anchor.w - w, MB.x + EDGE, std::max(MB.x + EDGE, MB.x + MB.w - EDGE - w));
            y = s_anchor.y + s_anchor.h + ANCHOR_GAP;
            if (y + h > MB.y + MB.h - EDGE) // no room under it: over it
                y = std::max(MB.y + EDGE, s_anchor.y - ANCHOR_GAP - h);
        } else {
            x = s_at.x, y = s_at.y;
            if (x + w > MB.x + MB.w - EDGE)
                x = std::max(MB.x + EDGE, s_at.x - w);
            if (y + h > MB.y + MB.h - EDGE)
                y = std::max(MB.y + EDGE, s_at.y - h);
        }
        s_box = CBox{x, y, w, h};

        P.frost(s_box, COLMENU, COLRIM, (int)std::lround(Theme::RAD_MENU * P.scale), (float)Theme::ROUNDING_POWER, SPaint::eDepth::MENU);
        SCard panel = recycledCard();
        panel.kind  = SCard::MENU;
        panel.box   = s_box;
        panel.id    = s_owner;
        cards.push_back(std::move(panel));

        double ry = y + PAD;
        for (size_t i = 0; i < s_items.size(); i++) {
            const auto& I = s_items[i];
            if (I.verb == SEP) {
                P.rect(CBox{x + LABEL_X, ry + SEPH / 2, w - 2 * LABEL_X, 1.0 / P.scale}, tHairline());
                ry += SEPH;
                continue;
            }
            // the row under the pointer sits on the pill, its words in their
            // resting ink: a hover only damages, and no lit face needs warming
            // (crash class 4)
            if (hovered.kind == SCard::MENU_ITEM && hovered.id == (uint32_t)i)
                P.rect(CBox{x + PAD, ry, w - 2 * PAD, ROWH}, tPill(), (int)std::lround(Theme::RAD_ROW * P.scale), 2.f);
            if (const auto LBL = labelOf(I); LBL && LBL->tex)
                texAt(P, LBL, x + LABEL_X, ry + (ROWH - texH(LBL, P.scale)) / 2);
            SCard row = recycledCard();
            row.kind  = SCard::MENU_ITEM;
            row.box   = CBox{x, ry, w, ROWH};
            row.id    = (uint32_t)i;
            cards.push_back(std::move(row));
            ry += ROWH;
        }
    }

} // namespace NAwesome::Notify::CardMenu
