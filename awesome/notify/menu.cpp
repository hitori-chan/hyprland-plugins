// awesome/notify/menu.cpp — the card menu, macOS's Options. A card with
// actions drops it from its Options button; holding left on any card (a
// banner, a center card, a stack) opens it at the pointer:
//
//   Reply                          a card that takes a reply: opens its field
//   <the sender's actions>         in Notify order ("Mark as read")
//   ───
//   Mute <chat> for 1 Hour         a conversation only; "Unmute <chat>"
//   Mute <chat> Until Tomorrow     while a mute holds (tomorrow = 08:00)
//   ───
//   Turn Off <App> Banners         toggles: a label says what the click
//   Silence <App> Sounds           does ("Turn On…", "Allow…" once off)
//   ───
//   Close All from <App>
//
// The switches persist (policy.cpp). The menu is the notify layer's own,
// drawn last and hit first: it holds its card's banner while up, and
// closes on a pick, a press anywhere else, Esc, the lock or a native
// capture, the shade opening or closing, the cards moving monitor, and its
// card's death. An action fires as the revision the menu was opened over
// (a replace since fires nothing). Picks run from the click drain, never
// inside the input emission (crash class 6).
#include "ui.hpp"

#include <ctime>

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
    };

    struct SItem {
        eVerb       verb;
        std::string label;
        std::string action;
    };

    // the board's menu: 5 px of padding, 28 px rows whose labels sit 15 px
    // in, 11 px separators (a hairline 5 px off either row)
    constexpr double PAD = 5, ROWH = 28, SEPH = 11, LABEL_X = 15, MIN_W = 200, MAX_W = 320;
    constexpr double DROP = 3; // Options: the menu's top under its button
    constexpr size_t NAME_CP = 24; // a name past this many code points ellipsizes: the verb must stay readable

    static bool               s_open = false;
    static uint32_t           s_owner = 0;
    static uint64_t           s_rev   = 0;   // the card's revision the menu was opened over
    static std::string        s_app, s_chat; // the targets, captured at the open
    static Vector2D           s_at;
    static CBox               s_anchor; // Options: the button it drops from; w = 0 -> at s_at
    static std::vector<SItem> s_items;
    static CBox               s_box; // global logical, as last laid out

    static std::string clipName(const std::string& s) {
        size_t cp = 0;
        for (size_t i = 0; i < s.size(); i++)
            if (((unsigned char)s[i] & 0xC0) != 0x80 && cp++ == NAME_CP)
                return s.substr(0, i) + "…";
        return s;
    }

    static int64_t tomorrowMorning() {
        const auto NOW = std::time(nullptr);
        std::tm    tm{};
        localtime_r(&NOW, &tm);
        tm.tm_mday += 1;
        tm.tm_hour  = 8;
        tm.tm_min   = 0;
        tm.tm_sec   = 0;
        tm.tm_isdst = -1;
        return (int64_t)std::mktime(&tm);
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
        // what the sender offers comes first, as macOS lists an app's own
        // actions over the system's
        if (n.canReply)
            s_items.push_back({REPLY, n.replySubmitText.empty() ? "Reply" : clipName(n.replySubmitText), {}});
        for (const auto& A : n.actions)
            s_items.push_back({ACTION, clipName(A.label), A.id});
        if (!s_items.empty())
            s_items.push_back({SEP, {}, {}});
        const auto APP = clipName(n.appName.empty() ? n.appKey : n.appName);
        if (!s_chat.empty()) {
            const auto CHAT = clipName(n.conversationTitle.empty() ? n.summary : n.conversationTitle);
            if (Policy::mutedUntil(s_chat) > 0)
                s_items.push_back({UNMUTE, "Unmute " + CHAT, {}});
            else {
                s_items.push_back({MUTE_HOUR, "Mute " + CHAT + " for 1 Hour", {}});
                s_items.push_back({MUTE_TOMORROW, "Mute " + CHAT + " Until Tomorrow", {}});
            }
            s_items.push_back({SEP, {}, {}});
        }
        const int FLAGS = Policy::appFlags(s_app);
        s_items.push_back({BANNERS, (FLAGS & Policy::BANNERS_OFF ? "Turn On " : "Turn Off ") + APP + " Banners", {}});
        s_items.push_back({SOUNDS, (FLAGS & Policy::SOUNDS_OFF ? "Allow " : "Silence ") + APP + " Sounds", {}});
        s_items.push_back({SEP, {}, {}});
        s_items.push_back({CLOSE_APP, "Close All from " + APP, {}});
    }

    static bool openOn(uint32_t id, const Vector2D& at, const CBox& anchor) {
        const auto N = target(id);
        if (!N)
            return false;
        s_open   = true;
        s_owner  = id;
        s_rev    = N->rev;
        s_app    = N->appKey;
        s_chat   = Policy::chatKey(*N);
        s_at     = at;
        s_anchor = anchor;
        s_box    = {};
        build(*N);
        refreshBannerHold(); // the banner under the menu must not expire out from under it
        notifChanged();      // warm the labels, then damage
        return true;
    }

    bool open(uint32_t id, const Vector2D& at) {
        return openOn(id, at, {});
    }

    bool openBelow(uint32_t id, const CBox& anchor) {
        return openOn(id, Vector2D{anchor.x, anchor.y + anchor.h}, anchor);
    }

    void close() {
        if (!s_open)
            return;
        s_open  = false;
        s_owner = 0;
        s_items.clear();
        s_box = {};
        refreshBannerHold(); // back to a reply's or the pointer's hold, if any
        notifChanged(); // the last layout's bounds (its drop included) take the damage
    }

    void exit() {
        s_open  = false;
        s_owner = 0;
        s_items.clear();
        s_app.clear();
        s_chat.clear();
        s_box = {};
    }

    void sync() {
        if (s_open && !target(s_owner))
            close(); // its card died (closed, expired away, replaced into feedback)
    }

    void pick(int index) {
        if (!s_open || index < 0 || (size_t)index >= s_items.size())
            return;
        const auto VERB = s_items[index].verb;
        const auto KEY  = s_items[index].action;
        const auto APP  = s_app;
        const auto CHAT = s_chat;
        const auto ID   = s_owner;
        const auto REV  = s_rev;
        close();
        switch (VERB) {
            case SEP: return;
            case REPLY: replyOpen(ID); return;
            case ACTION: invokeLive(ID, KEY, REV); return;
            case MUTE_HOUR:
                Policy::mute(CHAT, (int64_t)std::time(nullptr) + 3600);
                Model::lowerBanners(APP);
                return;
            case MUTE_TOMORROW:
                Policy::mute(CHAT, tomorrowMorning());
                Model::lowerBanners(APP);
                return;
            case UNMUTE: Policy::mute(CHAT, 0); return;
            case BANNERS: {
                const bool OFF = !(Policy::appFlags(APP) & Policy::BANNERS_OFF);
                Policy::setAppFlag(APP, Policy::BANNERS_OFF, OFF);
                if (OFF)
                    Model::lowerBanners(APP);
                return;
            }
            case SOUNDS: Policy::setAppFlag(APP, Policy::SOUNDS_OFF, !(Policy::appFlags(APP) & Policy::SOUNDS_OFF)); return;
            case CLOSE_APP: Model::dismissAppAll(APP); return;
        }
    }

    // macOS opens a context menu with its corner at the pointer, one that
    // would leave the monitor flipping to the pointer's other side; Options
    // drops its menu from the button, right edges aligned, and lifts it over
    // the button where the screen ends below
    void render(const SPaint& P, const SType& T) {
        if (!s_open || s_items.empty())
            return;
        const auto COLMENU = color(cfg().getColor("plugin:awesome:notify:col_menu_bg")), COLRIM = color(cfg().getColor("plugin:awesome:notify:col_frame")),
                   COLTITLE = color(cfg().getColor("plugin:awesome:notify:col_title")), COLACC = color(cfg().getColor("plugin:awesome:notify:col_highlight"));
        const int  CAPPX = (int)std::lround((MAX_W - 2 * LABEL_X) * P.scale);

        double w = MIN_W, h = 2 * PAD;
        for (const auto& I : s_items) {
            h += I.verb == SEP ? SEPH : ROWH;
            if (I.verb != SEP)
                w = std::max(w, texW(cachedText(I.label, COLTITLE, T.body, CAPPX, -1, 0, false, 400), P.scale) + 2 * LABEL_X);
        }
        w = std::min(w, MAX_W);

        const auto MB = P.mon->logicalBox();
        double     x  = s_at.x, y = s_at.y;
        if (s_anchor.w > 0) {
            x = std::max(MB.x + EDGE, s_anchor.x + s_anchor.w - w);
            y = s_anchor.y + s_anchor.h + DROP;
            if (y + h > MB.y + MB.h - EDGE)
                y = std::max(MB.y + EDGE, s_anchor.y - DROP - h);
        } else {
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
            const CBox ROW{x, ry, w, ROWH};
            // the selection is the accent text, never a fill (the Frost
            // rule). Both faces are asked for on every pass: a hover only
            // damages, so the lit one must already be warm (crash class 4)
            const bool HOV = hovered.kind == SCard::MENU_ITEM && hovered.id == (uint32_t)i;
            const auto LIT = cachedText(I.label, COLACC, T.body, CAPPX, -1, 0, false, 400);
            const auto LBL = HOV ? LIT : cachedText(I.label, COLTITLE, T.body, CAPPX, -1, 0, false, 400);
            if (LBL && LBL->tex)
                P.tex(LBL->tex, x + LABEL_X, ry + (ROWH - texH(LBL, P.scale)) / 2);
            SCard row = recycledCard();
            row.kind  = SCard::MENU_ITEM;
            row.box   = ROW;
            row.id    = (uint32_t)i;
            cards.push_back(std::move(row));
            ry += ROWH;
        }
    }

} // namespace NAwesome::Notify::CardMenu
