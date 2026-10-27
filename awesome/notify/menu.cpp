// awesome/notify/menu.cpp — the card menu, what macOS's Options offers.
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
// The switches persist (policy.cpp). The control center's three menus are
// the same panel, opened under what opened them: the output pill's
// (Output: the sinks, the default checked, each with its kind), Do Not
// Disturb's words (its durations) and Bluetooth's words (Devices: the
// paired ones, a lit disc while connected, "Connecting…" or the battery
// on the right; a pick connects or disconnects). Their rows come from the
// system module's published values and rebuild with every warm.
//
// The menu is the notify layer's own,
// drawn last and hit first: it holds its card's banner while up, and
// closes on a pick, a press anywhere else, Esc, the lock or a native
// capture, the shade opening or closing, the cards moving monitor, and its
// card's death. An action fires as the revision the menu was opened over
// (a replace since fires nothing). Picks run from the click drain, never
// inside the input emission (crash class 6).
#include "ui.hpp"

#include "../core/walltimer.hpp"

#include <ctime>

namespace NAwesome::Notify::CardMenu {

    enum eVerb : uint8_t {
        SEP = 0,
        HEADER, // a section's quiet title: never lit, never hit
        REPLY,
        ACTION, // SItem::action: the sender's action key
        MUTE_HOUR,
        MUTE_TOMORROW,
        UNMUTE,
        BANNERS,
        SOUNDS,
        CLOSE_APP,
        OUTPUT,      // the output menu's: SItem::output becomes the default sink
        DND_FOR,     // Do Not Disturb's: for SItem::output minutes
        DND_MORNING, // ... until the next 08:00
        DND_FOREVER, // ... until turned off
        DEVICE,      // Bluetooth's: SItem::key's device connects, or disconnects (check 1)
    };

    struct SItem {
        eVerb       verb;
        std::string label;
        std::string action;
        uint32_t    output = 0;
        int8_t      check  = -1;    // a control's row: 0 unchecked, 1 checked; -1 no check column
        bool        dim    = false; // shown, never picked (nothing to choose)
        std::string key;            // a control's row: who it is across a rebuild (the pick finds it by this)
        // the panel's rows: a leading kind glyph (an output's), or a device's
        // in a 26 px disc lit while connected (check 1); the words on the
        // right (a header's after its label), a device's battery after them
        Glyph::eGlyph glyph   = Glyph::eGlyph::NONE;
        bool          disc    = false;
        bool          quiet   = false; // the label in the secondary ink (Bluetooth off)
        std::string   detail;
        int8_t        battery = -1;
    };

    // the board's menu: 5 px of padding, 28 px rows whose labels sit 15 px
    // in, 11 px separators (a hairline 5 px off either row), 22 px headers,
    // 36 px device rows
    constexpr double PAD = 5, ROWH = 28, SEPH = 11, HEADH = 22, DEVH = 36, LABEL_X = 15, MIN_W = 200, MAX_W = 320;
    // a menu with checks: the mark's 12 px glyph centered in a column 10 px
    // in, the labels after it (and a kind glyph's column between)
    constexpr double CHECK_X = 10, CHECK_W = 14, CHECK_GLYPH = 12, CHECKED_LABEL_X = 30, KIND_W = 14, KIND_GAP = 8;
    // a device row: the disc 8 px in, the name 10 px after it
    constexpr double DISC_X = 8, DISC_D = 26, DISC_GLYPH = 14, DISC_GAP = 10;
    // the battery after a device's percent (the panel's 22:11 outline)
    constexpr double BAT_W = 22, BAT_H = 11, BAT_GAP = 5;
    constexpr size_t NAME_CP = 24; // a name past this many code points ellipsizes: the verb must stay readable

    static double rowH(const SItem& I) {
        return I.verb == SEP ? SEPH : I.verb == HEADER ? HEADH : I.disc ? DEVH : ROWH;
    }

    static bool                            s_open = false;
    static uint32_t                        s_owner = 0;
    static std::optional<ePanelMenu>       s_control; // a control center menu, not a card's
    static CBox                            s_anchor;  // ... under what opened it
    static bool                            s_right = false; // ... its right edge on the anchor's
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
                s_items.push_back({MUTE_TOMORROW, "Mute " + CHAT + (nextMorningText().starts_with("Tomorrow") ? " Until Tomorrow" : " Until 08:00"), {}});
            }
            s_items.push_back({SEP, {}, {}});
        }
        const int FLAGS = Policy::appFlags(s_app);
        s_items.push_back({BANNERS, (FLAGS & Policy::BANNERS_OFF ? "Turn On " : "Turn Off ") + APP + " Banners", {}});
        s_items.push_back({SOUNDS, (FLAGS & Policy::SOUNDS_OFF ? "Allow " : "Silence ") + APP + " Sounds", {}});
        s_items.push_back({SEP, {}, {}});
        s_items.push_back({CLOSE_APP, "Close All from " + APP, {}});
    }

    static Glyph::eGlyph deviceGlyph(Controls::SDevice::eKind k) {
        using K = Controls::SDevice::eKind;
        return k == K::HEADPHONES ? Glyph::eGlyph::HEADPHONES :
            k == K::SPEAKER       ? Glyph::eGlyph::OUTPUT :
            k == K::MOUSE         ? Glyph::eGlyph::MOUSE :
            k == K::KEYBOARD      ? Glyph::eGlyph::KEYBOARD :
                                    Glyph::eGlyph::BLUETOOTH;
    }

    // the control center's rows, from what the system module published;
    // rebuilt by sync() while open, so a list that lands late shows
    static void buildControl(ePanelMenu m) {
        s_items.clear();
        if (m == ePanelMenu::OUTPUT) {
            s_items.push_back({.verb = HEADER, .label = "Output"});
            for (const auto& O : Controls::state().outputs)
                s_items.push_back({.verb   = OUTPUT,
                                   .label  = O.name,
                                   .output = O.id,
                                   .check  = (int8_t)(O.current ? 1 : 0),
                                   .key    = "out:" + std::to_string(O.id),
                                   .glyph  = outputGlyph(O.name)});
            if (s_items.size() == 1)
                s_items.push_back({.verb = OUTPUT, .label = "No Output Devices", .dim = true});
        } else if (m == ePanelMenu::DND) {
            for (const auto& [LABEL, MIN] : {std::pair{"For 30 Minutes", 30u}, {"For 1 Hour", 60u}, {"For 3 Hours", 180u}})
                s_items.push_back({.verb = DND_FOR, .label = LABEL, .output = MIN, .key = "dnd:" + std::to_string(MIN)});
            s_items.push_back({.verb = DND_MORNING, .label = "Until " + nextMorningText(), .key = "dnd:morning"});
            s_items.push_back({.verb = DND_FOREVER, .label = "Until Turned Off", .key = "dnd:off"});
        } else {
            const auto& B = Controls::state().bluetooth;
            s_items.push_back({.verb = HEADER, .label = "Devices", .detail = B.powered ? "" : "· Bluetooth is off"});
            for (const auto& D : B.devices)
                s_items.push_back({.verb    = DEVICE,
                                   .label   = D.name,
                                   .check   = (int8_t)(D.connected ? 1 : 0),
                                   .key     = "bt:" + D.id,
                                   .glyph   = deviceGlyph(D.kind),
                                   .disc    = true,
                                   .quiet   = !B.powered,
                                   .detail  = D.connecting ? "Connecting…" : D.connected && D.battery >= 0 ? std::to_string(D.battery) + "%" : "",
                                   .battery = D.connecting || !D.connected ? (int8_t)-1 : D.battery});
            if (B.devices.empty())
                s_items.push_back({.verb = DEVICE, .label = "No Paired Devices", .dim = true});
        }
    }

    bool openControl(ePanelMenu m, const CBox& anchor, bool alignRight) {
        close();
        s_open    = true;
        s_owner   = 0;
        s_control = m;
        s_anchor  = anchor;
        s_right   = alignRight;
        s_box     = {};
        buildControl(m);
        notifChanged(); // warm the labels, then damage
        return true;
    }

    std::optional<ePanelMenu> openControlMenu() {
        return s_open ? s_control : std::nullopt;
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
        const auto ROW  = s_items[index].key;
        const auto OUT  = s_items[index].output;
        const bool ON   = s_items[index].check == 1;
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
            case OUTPUT:
                if (Controls::doors().setOutput)
                    Controls::doors().setOutput(OUT);
                return;
            case DND_FOR: Model::setSuspend((int64_t)std::time(nullptr) + (int64_t)OUT * 60); return;
            case DND_MORNING: Model::setSuspend(nextMorning()); return;
            case DND_FOREVER: Model::setSuspend(-1); return;
            case DEVICE:
                // its key is "bt:" + the device's id; a connect in flight is left be
                if (const auto& B = Controls::state().bluetooth; Controls::doors().connectDevice)
                    for (const auto& D : B.devices)
                        if ("bt:" + D.id == ROW && !D.connecting)
                            Controls::doors().connectDevice(D.id, !ON);
                return;
            case HEADER: return;
        }
    }

#ifdef AWESOME_GATE
    // "label;label*;(label)": the open menu's rows, a check starred, a dim
    // row in parentheses ("" while closed)
    std::string describe() {
        std::string out;
        for (const auto& I : s_items) {
            if (I.verb == SEP || I.verb == HEADER)
                continue;
            out += (out.empty() ? "" : ";") + (I.dim ? "(" + I.label + ")" : I.label + (I.check == 1 ? "*" : "") + (I.detail.empty() ? "" : " " + I.detail));
        }
        return s_open ? out : std::string{};
    }
#endif

    // macOS opens a context menu with its corner at the pointer, one that
    // would leave the monitor flipping to the pointer's other side; the
    // control center's hang 6 px under what opened them, at its edge
    void render(const SPaint& P, const SType& T) {
        if (!s_open || s_items.empty())
            return;
        const auto COLMENU = color(cfg().getColor("plugin:awesome:theme:col_glass")), COLRIM = color(cfg().getColor("plugin:awesome:theme:col_rim")),
                   COLTITLE = color(cfg().getColor("plugin:awesome:notify:col_title")), COLACC = color(cfg().getColor("plugin:awesome:notify:col_highlight")),
                   COLSUB = color(cfg().getColor("plugin:awesome:notify:col_kicker")), COLBODY = color(cfg().getColor("plugin:awesome:notify:col_fg")),
                   COLURGENT = color(cfg().getColor("plugin:awesome:notify:col_urgent"));
        const auto   COLDIM = COLTITLE.modifyA(COLTITLE.a * 0.4);
        const auto   PX     = [&](double logical) { return (int)std::lround(logical * P.scale); };
        const bool   CHECKS = std::ranges::any_of(s_items, [](const SItem& I) { return I.check >= 0 && !I.disc; });
        const bool   KINDS  = std::ranges::any_of(s_items, [](const SItem& I) { return I.glyph != Glyph::eGlyph::NONE && !I.disc; });
        const bool   DISCS  = std::ranges::any_of(s_items, [](const SItem& I) { return I.disc; });
        const double LX     = DISCS ? DISC_X + DISC_D + DISC_GAP : (CHECKS ? CHECKED_LABEL_X : LABEL_X) + (KINDS ? KIND_W + KIND_GAP : 0);
        // the control center's menus have their board widths
        const double FIXED = !s_control ? 0 : *s_control == ePanelMenu::OUTPUT ? 232 : *s_control == ePanelMenu::DND ? 220 : 268;
        const int    CAPPX = (int)std::lround(((FIXED > 0 ? FIXED : MAX_W) - LX - LABEL_X) * P.scale);

        // the right-hand words come first: the label's room is what they leave
        const auto detailOf = [&](const SItem& I) { return I.detail.empty() || I.verb == HEADER ? nullptr : cachedText(I.detail, COLSUB, T.meta, CAPPX, -1, 0, false, 400); };
        const auto rightW   = [&](const SItem& I) {
            const auto D = detailOf(I);
            return (D ? texW(D, P.scale) : 0.0) + (I.battery >= 0 ? BAT_GAP + BAT_W : 0.0);
        };
        const auto labelPx = [&](const SItem& I) {
            const double R = rightW(I);
            return R > 0 ? std::max(1, CAPPX - PX(R + 12)) : CAPPX;
        };

        double w = FIXED > 0 ? FIXED : MIN_W, h = 2 * PAD;
        for (const auto& I : s_items) {
            h += rowH(I);
            if (FIXED <= 0 && I.verb != SEP && I.verb != HEADER)
                w = std::max(w, texW(cachedText(I.label, I.dim ? COLDIM : COLTITLE, T.body, CAPPX, -1, 0, false, 400), P.scale) + LX + LABEL_X);
        }
        w = std::min(w, std::max(MAX_W, FIXED));

        const auto MB = P.mon->logicalBox();
        double     x, y;
        if (s_control) {
            x = s_right ? s_anchor.x + s_anchor.w - w : s_anchor.x;
            y = s_anchor.y + s_anchor.h + 6;
            if (y + h > MB.y + MB.h - EDGE) // no room under it: over it
                y = std::max(MB.y + EDGE, s_anchor.y - 6 - h);
            x = std::clamp(x, MB.x + EDGE, std::max(MB.x + EDGE, MB.x + MB.w - EDGE - w));
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
            const auto&  I  = s_items[i];
            const double RH = rowH(I);
            if (I.verb == SEP) {
                P.rect(CBox{x + LABEL_X, ry + SEPH / 2, w - 2 * LABEL_X, 1.0 / P.scale}, tHairline());
                ry += SEPH;
                continue;
            }
            if (I.verb == HEADER) { // the label, then its note at a lighter weight
                const double HX = DISCS ? DISC_X + 2 : CHECK_X;
                if (const auto HT = cachedText(I.label, COLSUB, T.small, CAPPX, -1, 0, false, 600)) {
                    texAt(P, HT, x + HX, ry + (HEADH - texH(HT, P.scale)) / 2);
                    if (const auto NT = I.detail.empty() ? nullptr : cachedText(I.detail, COLSUB, T.small, CAPPX, -1, 0, false, 500))
                        texAt(P, NT, x + HX + texW(HT, P.scale) + 6, ry + (HEADH - texH(NT, P.scale)) / 2);
                }
                ry += HEADH;
                continue;
            }
            const CBox ROW{x, ry, w, RH};
            if (I.dim) { // never lit, never hit: a press on it lands on the panel
                if (const auto LBL = cachedText(I.label, COLDIM, T.body, CAPPX, -1, 0, false, 400); LBL && LBL->tex)
                    texAt(P, LBL, x + LX, ry + (RH - texH(LBL, P.scale)) / 2);
                ry += RH;
                continue;
            }
            // the selection is the accent text, never a fill (the Frost
            // rule). Both faces are asked for on every pass: a hover only
            // damages, so the lit one must already be warm (crash class 4)
            const bool HOV = hovered.kind == SCard::MENU_ITEM && hovered.id == (uint32_t)i;
            const int  LPX = labelPx(I);
            const auto LIT = cachedText(I.label, COLACC, T.body, LPX, -1, 0, false, 400);
            const auto LBL = HOV ? LIT : cachedText(I.label, I.quiet ? COLSUB : COLTITLE, T.body, LPX, -1, 0, false, 400);
            if (LBL && LBL->tex)
                texAt(P, LBL, x + LX, ry + (RH - texH(LBL, P.scale)) / 2);
            if (I.disc) { // a device: its glyph in a disc, white while connected
                const CBox D{x + DISC_X, ry + (RH - DISC_D) / 2, DISC_D, DISC_D};
                P.rect(D, I.check == 1 ? COLTITLE : tFill(), PX(DISC_D / 2), 2.f);
                if (const auto G = Glyph::tex(I.glyph, I.check == 1 ? tLitInk() : COLTITLE, PX(DISC_GLYPH)))
                    P.texIn(G, D);
            } else {
                if (I.check == 1) {
                    const auto MARK  = Glyph::tex(Glyph::eGlyph::CHECK, COLACC, (int)std::lround(CHECK_GLYPH * P.scale));
                    const auto PLAIN = Glyph::tex(Glyph::eGlyph::CHECK, COLTITLE, (int)std::lround(CHECK_GLYPH * P.scale));
                    if (const auto G = HOV ? MARK : PLAIN)
                        P.texIn(G, CBox{x + CHECK_X, ry, CHECK_W, RH});
                }
                if (I.glyph != Glyph::eGlyph::NONE)
                    if (const auto G = Glyph::tex(I.glyph, COLBODY, PX(KIND_W)))
                        P.texIn(G, CBox{x + LX - KIND_GAP - KIND_W, ry, KIND_W, RH});
            }
            // the right: the words, then a battery's outline with its level
            double rx = x + w - 14;
            if (I.battery >= 0) {
                rx -= BAT_W;
                const CBox BB{rx, ry + (RH - BAT_H) / 2, BAT_W, BAT_H};
                if (const auto G = Glyph::tex(Glyph::eGlyph::BATTERY, COLSUB, PX(BAT_W), PX(BAT_H)))
                    P.texIn(G, BB);
                const double FW = 15.6 * std::clamp((int)I.battery, 0, 100) / 100.0;
                if (FW > 0.5)
                    P.rect(CBox{BB.x + 2.2, BB.y + 2.2, FW, 6.6}, I.battery <= 20 ? COLURGENT : COLBODY, PX(1.6), 2.f);
                rx -= BAT_GAP;
            }
            if (const auto D = detailOf(I))
                texAt(P, D, rx - texW(D, P.scale), ry + (RH - texH(D, P.scale)) / 2);
            SCard row = recycledCard();
            row.kind  = SCard::MENU_ITEM;
            row.box   = ROW;
            row.id    = (uint32_t)i;
            row.group = I.key;
            cards.push_back(std::move(row));
            ry += RH;
        }
    }

} // namespace NAwesome::Notify::CardMenu
