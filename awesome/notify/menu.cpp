// awesome/notify/menu.cpp — the card menu, what macOS's Options offers.
// Holding left on any card (a banner, a center card, a stack) opens it at
// the pointer:
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
// The switches persist (policy.cpp). A right click on a level in the
// control center opens the same menu for that control: the keyboard
// light's (Follow the Room) and the volume's (its outputs, the default
// checked); the system module's values, picked through its doors.
//
// The menu is the notify layer's own,
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
        OUTPUT, // a control's: SItem::output becomes the default sink
    };

    struct SItem {
        eVerb       verb;
        std::string label;
        std::string action;
        uint32_t    output = 0;
        int8_t      check  = -1;    // a control's row: 0 unchecked, 1 checked; -1 no check column
        bool        dim    = false; // shown, never picked (nothing to choose)
        std::string key;            // a control's row: who it is across a rebuild (the pick finds it by this)
    };

    // the board's menu: 5 px of padding, 28 px rows whose labels sit 15 px
    // in, 11 px separators (a hairline 5 px off either row)
    constexpr double PAD = 5, ROWH = 28, SEPH = 11, LABEL_X = 15, MIN_W = 200, MAX_W = 320;
    // a menu with checks: the mark's 12 px glyph centered in a column 10 px
    // in, the labels after it
    constexpr double CHECK_X = 10, CHECK_W = 14, CHECK_GLYPH = 12, CHECKED_LABEL_X = 30;
    constexpr size_t NAME_CP = 24; // a name past this many code points ellipsizes: the verb must stay readable

    static bool                            s_open = false;
    static uint32_t                        s_owner = 0;
    static std::optional<Controls::eLevel> s_control; // a control's menu, not a card's
    static uint64_t           s_rev   = 0;   // the card's revision the menu was opened over
    static std::string        s_app, s_chat; // the targets, captured at the open
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
        const auto APP = clipName(s_appLabel);
        if (!s_chat.empty()) {
            const auto CHAT = clipName(s_chatLabel);
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

    // a control's rows, from what the system module published; rebuilt by
    // sync() while open, so a list that lands late shows
    static void buildControl(Controls::eLevel l) {
        s_items.clear();
        if (l == Controls::eLevel::VOLUME) {
            for (const auto& O : Controls::state().outputs)
                s_items.push_back({.verb = OUTPUT, .label = O.name, .output = O.id, .check = (int8_t)(O.current ? 1 : 0), .key = "out:" + std::to_string(O.id)});
            if (s_items.empty())
                s_items.push_back({.verb = OUTPUT, .label = "No Output Devices", .dim = true});
        }
    }

    bool openControl(Controls::eLevel l, const Vector2D& at) {
        if (l != Controls::eLevel::VOLUME)
            return false;
        close();
        s_open    = true;
        s_owner   = 0;
        s_control = l;
        s_at      = at;
        s_box     = {};
        buildControl(l);
        notifChanged(); // warm the labels, then damage
        return true;
    }

    bool open(uint32_t id, const Vector2D& at) {
        const auto N = target(id);
        if (!N)
            return false;
        s_open    = true;
        s_owner   = id;
        s_control.reset();
        s_rev   = N->rev;
        s_app   = N->appKey;
        s_chat  = Policy::chatKey(*N);
        s_appLabel  = Policy::appLabel(*N);
        s_chatLabel = Policy::chatLabel(*N);
        s_at    = at;
        s_box   = {};
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
        s_control.reset();
        s_items.clear();
        s_box = {};
        refreshBannerHold(); // back to a reply's or the pointer's hold, if any
        notifChanged(); // the last layout's bounds (its drop included) take the damage
    }

    void exit() {
        s_open  = false;
        s_owner = 0;
        s_control.reset();
        s_items.clear();
        s_app.clear();
        s_chat.clear();
        s_appLabel.clear();
        s_chatLabel.clear();
        s_box = {};
    }

    void sync() {
        if (s_open && s_control)
            buildControl(*s_control);
        else if (s_open && !target(s_owner))
            close(); // its card died (closed, expired away, replaced into feedback)
    }

    void pick(int index, const std::string& key) {
        if (!s_open)
            return;
        // a control's rows rebuild with every warm (a sink came or went
        // between the draw and the pick): the row is the one with its key
        if (!key.empty()) {
            const auto IT = std::ranges::find(s_items, key, &SItem::key);
            if (IT == s_items.end())
                return close();
            index = (int)(IT - s_items.begin());
        }
        if (index < 0 || (size_t)index >= s_items.size())
            return;
        if (s_items[index].dim)
            return;
        const auto VERB = s_items[index].verb;
        const auto KEY  = s_items[index].action;
        const auto OUT  = s_items[index].output;
        const auto APP  = s_app;
        const auto CHAT = s_chat;
        const auto APPL = s_appLabel, CHATL = s_chatLabel;
        const auto ID   = s_owner;
        const auto REV  = s_rev;
        close();
        switch (VERB) {
            case SEP: return;
            case REPLY: replyOpen(ID); return;
            case ACTION: invokeLive(ID, KEY, REV); return;
            case MUTE_HOUR:
                Policy::mute(CHAT, (int64_t)std::time(nullptr) + 3600, CHATL);
                Model::lowerBanners(APP);
                return;
            case MUTE_TOMORROW:
                Policy::mute(CHAT, tomorrowMorning(), CHATL);
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
            case OUTPUT:
                if (Controls::doors().setOutput)
                    Controls::doors().setOutput(OUT);
                return;
        }
    }

#ifdef AWESOME_GATE
    // "label;label*;(label)": the open menu's rows, a check starred, a dim
    // row in parentheses ("" while closed)
    std::string describe() {
        std::string out;
        for (const auto& I : s_items) {
            if (I.verb == SEP)
                continue;
            out += (out.empty() ? "" : ";") + (I.dim ? "(" + I.label + ")" : I.label + (I.check == 1 ? "*" : ""));
        }
        return s_open ? out : std::string{};
    }
#endif

    // macOS opens a context menu with its corner at the pointer, one that
    // would leave the monitor flipping to the pointer's other side
    void render(const SPaint& P, const SType& T) {
        if (!s_open || s_items.empty())
            return;
        const auto COLMENU = color(cfg().getColor("plugin:awesome:notify:col_menu_bg")), COLRIM = color(cfg().getColor("plugin:awesome:notify:col_frame")),
                   COLTITLE = color(cfg().getColor("plugin:awesome:notify:col_title")), COLACC = color(cfg().getColor("plugin:awesome:notify:col_highlight"));
        const auto COLDIM = COLTITLE.modifyA(COLTITLE.a * 0.4);
        const bool CHECKS = std::ranges::any_of(s_items, [](const SItem& I) { return I.check >= 0; });
        const double LX    = CHECKS ? CHECKED_LABEL_X : LABEL_X;
        const int  CAPPX = (int)std::lround((MAX_W - LX - LABEL_X) * P.scale);

        double w = MIN_W, h = 2 * PAD;
        for (const auto& I : s_items) {
            h += I.verb == SEP ? SEPH : ROWH;
            if (I.verb != SEP)
                w = std::max(w, texW(cachedText(I.label, I.dim ? COLDIM : COLTITLE, T.body, CAPPX, -1, 0, false, 400), P.scale) + LX + LABEL_X);
        }
        w = std::min(w, MAX_W);

        const auto MB = P.mon->logicalBox();
        double     x  = s_at.x, y = s_at.y;
        if (x + w > MB.x + MB.w - EDGE)
            x = std::max(MB.x + EDGE, s_at.x - w);
        if (y + h > MB.y + MB.h - EDGE)
            y = std::max(MB.y + EDGE, s_at.y - h);
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
            if (I.dim) { // never lit, never hit: a press on it lands on the panel
                if (const auto LBL = cachedText(I.label, COLDIM, T.body, CAPPX, -1, 0, false, 400); LBL && LBL->tex)
                    P.tex(LBL->tex, x + LX, ry + (ROWH - texH(LBL, P.scale)) / 2);
                ry += ROWH;
                continue;
            }
            // the selection is the accent text, never a fill (the Frost
            // rule). Both faces are asked for on every pass: a hover only
            // damages, so the lit one must already be warm (crash class 4)
            const bool HOV = hovered.kind == SCard::MENU_ITEM && hovered.id == (uint32_t)i;
            const auto LIT = cachedText(I.label, COLACC, T.body, CAPPX, -1, 0, false, 400);
            const auto LBL = HOV ? LIT : cachedText(I.label, COLTITLE, T.body, CAPPX, -1, 0, false, 400);
            if (LBL && LBL->tex)
                P.tex(LBL->tex, x + LX, ry + (ROWH - texH(LBL, P.scale)) / 2);
            if (I.check == 1) {
                const auto MARK = Glyph::tex(Glyph::eGlyph::CHECK, COLACC, (int)std::lround(CHECK_GLYPH * P.scale));
                const auto PLAIN = Glyph::tex(Glyph::eGlyph::CHECK, COLTITLE, (int)std::lround(CHECK_GLYPH * P.scale));
                if (const auto G = HOV ? MARK : PLAIN)
                    P.texIn(G, CBox{x + CHECK_X, ry, CHECK_W, ROWH});
            }
            SCard row = recycledCard();
            row.kind  = SCard::MENU_ITEM;
            row.box   = ROW;
            row.id    = (uint32_t)i;
            row.group = I.key;
            cards.push_back(std::move(row));
            ry += ROWH;
        }
    }

} // namespace NAwesome::Notify::CardMenu
