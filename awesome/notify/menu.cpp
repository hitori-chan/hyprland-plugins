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
// The switches persist (policy.cpp). The control center's three menus are
// the same panel, opened under what opened them: the output disc's
// (Output: the sinks, the default checked, each with its kind), Do Not
// Disturb's words (its durations) and Bluetooth's words (Devices: the
// paired ones, a lit disc while connected, "Connecting…",
// "Disconnecting…" or the battery on the right, a refused connect's
// reason under the name; a pick connects or disconnects, a right-click
// asks to forget it, in place of its row; under them, Other Devices:
// Scan/Stop and what the scan found, a pick pairing one, its question
// asked in place of its row). Their rows come from the system module's
// published values and rebuild with every warm.
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

#include <cmath>
#include <ctime>
#include <format>
#include <numbers>

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
        PAIR,        // Bluetooth's: SItem::key's found device pairs (the menu stays)
        PROMPT,      // a question in place of a device's row, its buttons hit by their own keys
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
        uint8_t       fail    = 0; // a device's Controls::SDevice::eFail: a line under its name
        // a prompt's buttons, right to left from its last: "btn:<verb>:<id>";
        // a header's one (Scan)
        std::vector<std::pair<std::string, std::string>> buttons; // key, words
        std::string                                      code;    // a pairing's code, its first `entered` digits typed
        int8_t                                           entered = -1;
        bool                                             spin    = false; // a header's activity: the scan runs
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
    // a device row with a refusal: 5 px over its name, its reason 2 under
    constexpr double DEV_PADY = 5, FAIL_GAP = 2;
    // a prompt: its device's line (26), 4, the question, 6, its 26 px
    // buttons 6 apart, 8 under
    constexpr double PROMPT_TOP = 5, PROMPT_LINE = 26, PROMPT_GAP = 4, PROMPT_ASK_GAP = 6, BTN_H = 26, BTN_GAP = 6,
                     PROMPT_BOTTOM = 8, BTN_PADX = 13;
    // a header with a button: 30 tall, its 24 px pill 4 px off the edge, the
    // activity's 12 px dots 8 px left of it; a pairing's code at 20 px
    constexpr double HEAD_BTN_H = 30, PILL_H = 24, PILL_PADX = 11, PILL_INSET = 4, SPIN = 12, SPIN_GAP = 8, CODE_PX = 20;
    // the activity turns a dot every SPIN_STEP (its box damaged, nothing warmed)
    constexpr auto SPIN_STEP = std::chrono::milliseconds(100);
    constexpr size_t NAME_CP = 24; // a name past this many code points ellipsizes: the verb must stay readable

    static double rowH(const SItem& I) {
        return I.verb == SEP ? SEPH : I.verb == HEADER ? (I.buttons.empty() ? HEADH : HEAD_BTN_H) : I.disc ? DEVH : ROWH;
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
        // what the sender offers comes first: an app's own actions over the
        // system's
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

    // a refused connect's line: what happened (in red), then what to check
    static std::pair<const char*, const char*> failLine(uint8_t f) {
        switch (f) {
            case Controls::SDevice::NO_ANSWER: return {"Didn't answer.", "Is it on and nearby?"};
            case Controls::SDevice::PAIRING_LOST: return {"Pairing lost.", "Put it in pairing mode, then click to pair again."};
            case Controls::SDevice::FAILED: return {"Didn't connect.", "Try it again."};
            case Controls::SDevice::PAIR_FAILED: return {"Pairing failed.", "Is it in pairing mode?"};
            default: return {nullptr, nullptr};
        }
    }

    // the device whose row asks to forget it ("bt:" + its id; "" none)
    static std::string s_forget;
    // the scan's activity: where it was drawn, its tick
    static CBox                s_spinBox;
    static bool                s_spinning = false;
    static SP<CEventLoopTimer> s_spinTick;

    // an ink as Pango markup's foreground and its alpha
    static std::string inkMarkup(const CHyprColor& c) {
        const auto TO8 = [](double v) { return (int)std::lround(std::clamp(v, 0.0, 1.0) * 255); };
        return std::format("foreground=\"#{:02X}{:02X}{:02X}\" fgalpha=\"{}\"", TO8(c.r), TO8(c.g), TO8(c.b), std::max(1, (int)std::lround(std::clamp(c.a, 0.0, 1.0) * 65535)));
    }

    // the scan's activity: eight dots, the head turning a dot every SPIN_STEP
    static void drawSpin(const SPaint& P, const CBox& box, const CHyprColor& ink) {
        const auto   MS   = std::chrono::duration_cast<std::chrono::milliseconds>(Time::steadyNow().time_since_epoch()).count();
        const int    HEAD = (int)(MS / SPIN_STEP.count() % 8);
        const double R = box.w / 2 - 1.4, D = 2.4;
        for (int k = 0; k < 8; k++) {
            const double A   = k * std::numbers::pi / 4 - std::numbers::pi / 2;
            const int    AGE = (HEAD - k + 8) % 8; // 0: the head
            const CBox   DOT{box.x + box.w / 2 + R * std::cos(A) - D / 2, box.y + box.h / 2 + R * std::sin(A) - D / 2, D, D};
            P.rect(DOT, ink.modifyA(ink.a * (1.0 - AGE * 0.11)), (int)std::lround(D / 2 * P.scale), 2.f);
        }
    }

    static Glyph::eGlyph deviceGlyph(Controls::SDevice::eKind k) {
        using K = Controls::SDevice::eKind;
        return k == K::HEADPHONES ? Glyph::eGlyph::HEADPHONES :
            k == K::SPEAKER       ? Glyph::eGlyph::OUTPUT :
            k == K::MOUSE         ? Glyph::eGlyph::MOUSE :
            k == K::KEYBOARD      ? Glyph::eGlyph::KEYBOARD :
            k == K::PHONE         ? Glyph::eGlyph::PHONE :
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
            s_items.push_back({.verb   = HEADER,
                               .label  = "Devices",
                               .detail = B.block == Controls::SBluetooth::HARD_BLOCKED ? "· Off by its hardware switch" :
                                   B.powered                                          ? "" :
                                                                                        "· Bluetooth is off"});
            bool asked = false;
            for (const auto& D : B.devices) {
                const bool BUSY = D.busy != Controls::SDevice::IDLE;
                if (!BUSY && s_forget == "bt:" + D.id) {
                    asked = true;
                    s_items.push_back({.verb    = PROMPT,
                                       .label   = D.name,
                                       .check   = (int8_t)(D.connected ? 1 : 0),
                                       .key     = "bt:" + D.id,
                                       .glyph   = deviceGlyph(D.kind),
                                       .disc    = true,
                                       .detail  = "Forget it? Using it again takes pairing it again.",
                                       .buttons = {{"btn:forget:" + D.id, "Forget"}, {"btn:cancel:" + D.id, "Cancel"}}});
                    continue;
                }
                s_items.push_back({.verb    = DEVICE,
                                   .label   = D.name,
                                   .check   = (int8_t)(D.connected ? 1 : 0),
                                   .key     = "bt:" + D.id,
                                   .glyph   = deviceGlyph(D.kind),
                                   .disc    = true,
                                   .quiet   = !B.powered,
                                   .detail  = D.busy == Controls::SDevice::CONNECTING ? "Connecting…" :
                                        D.busy == Controls::SDevice::DISCONNECTING   ? "Disconnecting…" :
                                        D.connected && D.battery >= 0                ? std::to_string(D.battery) + "%" :
                                                                                       "",
                                   .battery = BUSY || !D.connected ? (int8_t)-1 : D.battery,
                                   .fail    = (uint8_t)D.fail});
            }
            if (!asked)
                s_forget.clear(); // its device went, or got busy
            if (B.devices.empty())
                s_items.push_back({.verb = DEVICE, .label = "No Paired Devices", .dim = true});
            if (!B.powered)
                return;
            // Other Devices: the scan's switch and what it found
            s_items.push_back({.verb = SEP});
            s_items.push_back({.verb = HEADER, .label = "Other Devices", .buttons = {{"btn:scan", B.scanning ? "Stop" : "Scan"}}, .spin = B.scanning});
            for (const auto& D : B.found) {
                const auto& Q = B.prompt;
                if (Q.id == D.id && !Q.code.empty()) {
                    s_items.push_back({.verb    = PROMPT,
                                       .label   = D.name,
                                       .key     = "bt:" + D.id,
                                       .glyph   = deviceGlyph(D.kind),
                                       .disc    = true,
                                       .detail  = Q.confirm ? "Does it show the same code?" : "Type this code on it, then press Return",
                                       .buttons = Q.confirm ? std::vector<std::pair<std::string, std::string>>{{"btn:pair:" + D.id, "Pair"}, {"btn:cancel:" + D.id, "Cancel"}} :
                                                              std::vector<std::pair<std::string, std::string>>{{"btn:cancel:" + D.id, "Cancel"}},
                                       .code    = Q.code,
                                       .entered = Q.entered});
                    continue;
                }
                s_items.push_back({.verb   = PAIR,
                                   .label  = D.name,
                                   .key    = "bt:" + D.id,
                                   .glyph  = deviceGlyph(D.kind),
                                   .disc   = true,
                                   .detail = D.busy == Controls::SDevice::PAIRING ? "Pairing…" : "",
                                   .fail   = (uint8_t)D.fail});
            }
            if (B.scanned && !B.scanning && B.found.empty())
                s_items.push_back({.verb = PAIR, .label = "No Devices Found", .dim = true});
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
        // Bluetooth's scan, its list and its question go with its menu
        if (s_control == ePanelMenu::BLUETOOTH && Controls::doors().menuClosed)
            Controls::doors().menuClosed();
        s_forget.clear();
        s_open  = false;
        s_owner = 0;
        s_control.reset();
        s_items.clear();
        s_box = {};
        refreshBannerHold(); // back to a reply's or the pointer's hold, if any
        notifChanged(); // the last layout's bounds (its drop included) take the damage
    }

    void exit() {
        if (s_spinTick && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(s_spinTick);
        s_spinTick.reset();
        s_spinning = false;
        s_spinBox  = {};
        s_forget.clear();
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

    // a prompt's button: the menu stays, the row comes back (or goes, with
    // its device)
    static void press(const std::string& key) {
        const auto& D = Controls::doors();
        if (key == "btn:scan") {
            if (D.scan)
                D.scan(!Controls::state().bluetooth.scanning);
            return;
        }
        const auto ID = key.substr(key.find(':', 4) + 1);
        if (key.starts_with("btn:forget:") && s_forget == "bt:" + ID && D.forgetDevice)
            D.forgetDevice(ID);
        else if (key.starts_with("btn:pair:") && D.answerPairing)
            D.answerPairing(true);
        else if (key.starts_with("btn:cancel:") && s_forget != "bt:" + ID && D.answerPairing)
            D.answerPairing(false);
        s_forget.clear();
        buildControl(ePanelMenu::BLUETOOTH);
        notifChanged();
    }

    void secondary(int index, const std::string& key) {
        (void)index;
        if (!s_open || s_control != ePanelMenu::BLUETOOTH || !key.starts_with("bt:"))
            return;
        const auto IT = std::ranges::find(s_items, key, &SItem::key);
        if (IT == s_items.end() || IT->verb != DEVICE)
            return;
        s_forget = key;
        buildControl(ePanelMenu::BLUETOOTH);
        notifChanged();
    }

    void pick(int index, const std::string& key) {
        if (!s_open)
            return;
        if (key.starts_with("btn:")) {
            if (s_control == ePanelMenu::BLUETOOTH)
                press(key);
            return;
        }
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
        if (VERB == PROMPT)
            return; // never hit: a press on one lands on the panel
        // a found device pairs, and a lost pairing pairs again: the menu
        // stays, for the questions a pairing asks
        if (VERB == PAIR || (VERB == DEVICE && s_items[index].fail == Controls::SDevice::PAIRING_LOST)) {
            if (s_items[index].detail.empty() && Controls::doors().pairDevice)
                Controls::doors().pairDevice(ROW.substr(3));
            return;
        }
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
                        if ("bt:" + D.id == ROW && D.busy == Controls::SDevice::IDLE)
                            Controls::doors().connectDevice(D.id, !ON);
                return;
            case HEADER:
            case PAIR:
            case PROMPT: return;
        }
    }

#ifdef AWESOME_GATE
    // "label;label*;(label)": the open menu's rows, a check starred, a dim
    // row in parentheses ("" while closed)
    std::string describe() {
        std::string out;
        for (const auto& I : s_items) {
            if (I.verb == SEP || (I.verb == HEADER && I.buttons.empty()))
                continue;
            if (I.verb == HEADER) {
                out += std::string{out.empty() ? "" : ";"} + "[" + I.buttons.front().second + "]";
                continue;
            }
            if (I.verb == PROMPT) {
                out += (out.empty() ? "" : ";") + I.label + " ?";
                if (!I.code.empty())
                    out += " " + I.code + (I.entered >= 0 ? "/" + std::to_string(I.entered) : "");
                for (const auto& [_, WORDS] : I.buttons)
                    out += " [" + WORDS + "]";
                continue;
            }
            out += (out.empty() ? "" : ";") +
                (I.dim ? "(" + I.label + ")" :
                         I.label + (I.check == 1 ? "*" : "") + (I.detail.empty() ? "" : " " + I.detail) + (I.fail ? std::string{" !"} + failLine(I.fail).first : ""));
        }
        return s_open ? out : std::string{};
    }
#endif

    // a context menu opens with its corner at the pointer, one that
    // would leave the monitor flipping to the pointer's other side; the
    // control center's hang 6 px under what opened them, at its edge
    void render(const SPaint& P, const SType& T) {
        if (!s_open || s_items.empty())
            return;
        const auto COLMENU = color(cfg().getColor("plugin:awesome:theme:col_glass")), COLRIM = color(cfg().getColor("plugin:awesome:theme:col_rim")),
                   COLTITLE = color(cfg().getColor("plugin:awesome:notify:col_title")),
                   COLSUB = color(cfg().getColor("plugin:awesome:notify:col_kicker")), COLBODY = color(cfg().getColor("plugin:awesome:notify:col_fg")),
                   COLURGENT = color(cfg().getColor("plugin:awesome:notify:col_urgent"));
        const auto   COLDIM = COLTITLE.modifyA(COLTITLE.a * 0.4);
        // the Pair button: the panel's lit white, its words dark
        static const CHyprColor PAIR_INK{0x16 / 255.0, 0x16 / 255.0, 0x1a / 255.0, 1.0}, KNOB_WHITE{1.0, 1.0, 1.0, 1.0};
        bool                    spun = false; // the scan's activity was drawn
        const auto   PX     = [&](double logical) { return (int)std::lround(logical * P.scale); };
        const bool   CHECKS = std::ranges::any_of(s_items, [](const SItem& I) { return I.check >= 0 && !I.disc; });
        const bool   KINDS  = std::ranges::any_of(s_items, [](const SItem& I) { return I.glyph != Glyph::eGlyph::NONE && !I.disc; });
        const bool   DISCS  = std::ranges::any_of(s_items, [](const SItem& I) { return I.disc; });
        const double LX     = DISCS ? DISC_X + DISC_D + DISC_GAP : (CHECKS ? CHECKED_LABEL_X : LABEL_X) + (KINDS ? KIND_W + KIND_GAP : 0);
        // the control center's menus have their board widths
        const double FIXED = !s_control ? 0 : *s_control == ePanelMenu::OUTPUT ? 232 : *s_control == ePanelMenu::DND ? 220 : 280;
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
        // a device's refusal: its head in the urgent colour, the hint after
        const auto failOf = [&](const SItem& I) -> const SCachedText* {
            const auto [HEAD, HINT] = failLine(I.fail);
            if (!HEAD || !I.disc)
                return nullptr;
            return cachedText(std::format("<span {}>{}</span> {}", inkMarkup(COLURGENT), HEAD, HINT), COLSUB, T.meta, CAPPX, T.meta * 3, 0, true, 400);
        };
        // the resting face of a row's label: its height sizes a refused row
        const auto restOf = [&](const SItem& I) { return cachedText(I.label, I.quiet ? COLSUB : COLTITLE, T.body, labelPx(I), -1, 0, false, 400); };
        // a pairing's code, its typed digits dimmed
        const auto codeOf = [&](const SItem& I) -> const SCachedText* {
            if (I.code.empty())
                return nullptr;
            std::string esc;
            for (const char C : I.code)
                esc += C == '<' ? "&lt;" : C == '>' ? "&gt;" : C == '&' ? "&amp;" : std::string(1, C);
            const size_t TYPED = I.entered > 0 ? std::min(esc.size(), (size_t)I.entered) : 0;
            return cachedText(std::format("<span letter_spacing=\"{}\"><span {}>{}</span>{}</span>", (int)std::lround(0.08 * CODE_PX * P.scale * 1024), inkMarkup(COLSUB),
                                          esc.substr(0, TYPED), esc.substr(TYPED)),
                              COLTITLE, PX(CODE_PX), PX(FIXED > 0 ? FIXED : MAX_W), -1, 0, true, 600);
        };
        // a prompt's question wraps in the label's column
        const auto askOf = [&](const SItem& I) { return cachedText(I.detail, COLSUB, T.meta, CAPPX, T.meta * 3, 0, false, 400); };
        const auto itemH = [&](const SItem& I) {
            if (I.verb == PROMPT)
                return PROMPT_TOP + PROMPT_LINE + PROMPT_GAP + texH(askOf(I), P.scale) + PROMPT_ASK_GAP + BTN_H + PROMPT_BOTTOM;
            const auto F = failOf(I);
            return F ? std::max(DEVH, 2 * DEV_PADY + texH(restOf(I), P.scale) + FAIL_GAP + texH(F, P.scale)) : rowH(I);
        };

        double w = FIXED > 0 ? FIXED : MIN_W, h = 2 * PAD;
        for (const auto& I : s_items) {
            h += itemH(I);
            if (FIXED <= 0 && I.verb != SEP && I.verb != HEADER && I.verb != PROMPT)
                w = std::max(w, texW(cachedText(I.label, I.dim ? COLDIM : COLTITLE, T.body, CAPPX, -1, 0, false, 400), P.scale) + LX + LABEL_X);
        }
        w = std::min(w, std::max(MAX_W, FIXED));
        // the bound: Bluetooth's list taller than the monitor keeps its first
        // paired rows and says how many more (a warm trims; the draw paints
        // what it left)
        if (const double ROOM = P.mon->logicalBox().h - 2 * EDGE; s_control == ePanelMenu::BLUETOOTH && h > ROOM) {
            const auto LAST = [] {
                for (size_t k = s_items.size(); k-- > 0;)
                    if (s_items[k].verb == DEVICE && !s_items[k].dim)
                        return k;
                return s_items.size();
            };
            size_t more = 0, at = s_items.size();
            for (size_t k = LAST(); h > ROOM - ROWH && k < s_items.size(); k = LAST()) {
                h -= itemH(s_items[k]);
                s_items.erase(s_items.begin() + (std::ptrdiff_t)k);
                at = k;
                more++;
            }
            if (more > 0) {
                s_items.insert(s_items.begin() + (std::ptrdiff_t)at, SItem{.verb = DEVICE, .label = std::to_string(more) + " More", .dim = true});
                h += ROWH;
            }
        }

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
            const double RH = itemH(I);
            if (I.verb == SEP) {
                P.rect(CBox{x + LABEL_X, ry + SEPH / 2, w - 2 * LABEL_X, 1.0 / P.scale}, tHairline());
                ry += SEPH;
                continue;
            }
            if (I.verb == HEADER) { // the label, then its note at a lighter weight, or its pill
                const double HX = DISCS ? DISC_X + 2 : CHECK_X;
                if (const auto HT = cachedText(I.label, COLSUB, T.small, CAPPX, -1, 0, false, 600)) {
                    texAt(P, HT, x + HX, ry + (RH - texH(HT, P.scale)) / 2);
                    if (const auto NT = I.detail.empty() ? nullptr : cachedText(I.detail, COLSUB, T.small, CAPPX, -1, 0, false, 500))
                        texAt(P, NT, x + HX + texW(HT, P.scale) + 6, ry + (RH - texH(NT, P.scale)) / 2);
                }
                for (const auto& [KEY, WORDS] : I.buttons) {
                    const bool   BHOV = hovered.kind == SCard::MENU_ITEM && hovered.group == KEY;
                    const auto   WT   = cachedText(WORDS, COLTITLE, T.meta, PX(w), -1, 0, false, 600);
                    const double BW   = texW(WT, P.scale) + 2 * PILL_PADX;
                    const CBox   B{x + w - PAD - PILL_INSET - BW, ry + (RH - PILL_H) / 2, BW, PILL_H};
                    P.rect(B, BHOV ? tFillHover() : tFill(), PX(PILL_H / 2), 2.f);
                    if (WT)
                        texCentered(P, WT, B);
                    SCard btn = recycledCard();
                    btn.kind  = SCard::MENU_ITEM;
                    btn.box   = B;
                    btn.id    = (uint32_t)i;
                    btn.group = KEY;
                    cards.push_back(std::move(btn));
                    if (I.spin) {
                        s_spinBox = CBox{B.x - SPIN_GAP - SPIN, ry + (RH - SPIN) / 2, SPIN, SPIN};
                        drawSpin(P, s_spinBox, COLSUB);
                        spun = true;
                    }
                }
                ry += RH;
                continue;
            }
            const CBox ROW{x, ry, w, RH};
            if (I.verb == PROMPT) { // the device's line, the question, its buttons
                const CBox D{x + DISC_X, ry + PROMPT_TOP, DISC_D, DISC_D};
                P.rect(D, I.check == 1 ? COLTITLE : tFill(), PX(DISC_D / 2), 2.f);
                if (const auto G = Glyph::tex(I.glyph, I.check == 1 ? tLitInk() : COLTITLE, PX(DISC_GLYPH)))
                    P.texIn(G, D);
                if (const auto NAME = restOf(I))
                    texAt(P, NAME, x + LX, D.y + (DISC_D - texH(NAME, P.scale)) / 2);
                const auto   ASK = askOf(I);
                const double AY  = D.y + PROMPT_LINE + PROMPT_GAP;
                if (ASK)
                    texAt(P, ASK, x + LX, AY);
                if (const auto CODE = codeOf(I))
                    texAt(P, CODE, x + LX, AY + texH(ASK, P.scale) + PROMPT_ASK_GAP + (BTN_H - texH(CODE, P.scale)) / 2);
                // a stray press on the question keeps it (its pick is a no-op);
                // its buttons, pushed after, are hit first
                SCard row = recycledCard();
                row.kind  = SCard::MENU_ITEM;
                row.box   = ROW;
                row.id    = (uint32_t)i;
                row.group = I.key;
                cards.push_back(std::move(row));
                double bx = x + w - DISC_X;
                for (const auto& [KEY, WORDS] : I.buttons) {
                    // Forget in red, Pair dark on white, Cancel white on
                    // the fill; its words once, the fill by hover
                    const bool   BHOV   = hovered.kind == SCard::MENU_ITEM && hovered.group == KEY;
                    const bool   PAIRB  = KEY.starts_with("btn:pair:");
                    const auto   INK    = KEY.starts_with("btn:forget:") ? COLURGENT : PAIRB ? PAIR_INK : COLTITLE;
                    const auto   WORDS_ = cachedText(WORDS, INK, T.meta, PX(w), -1, 0, false, 600);
                    const double BW     = texW(WORDS_, P.scale) + 2 * BTN_PADX;
                    const CBox   B{bx - BW, AY + texH(ASK, P.scale) + PROMPT_ASK_GAP, BW, BTN_H};
                    P.rect(B, PAIRB ? (BHOV ? KNOB_WHITE : COLTITLE) : BHOV ? tFillHover() : tFill(), PX(BTN_H / 2), 2.f);
                    if (WORDS_)
                        texCentered(P, WORDS_, B);
                    SCard btn = recycledCard();
                    btn.kind  = SCard::MENU_ITEM;
                    btn.box   = B;
                    btn.id    = (uint32_t)i;
                    btn.group = KEY;
                    cards.push_back(std::move(btn));
                    bx -= BW + BTN_GAP;
                }
                ry += RH;
                continue;
            }
            if (I.dim) { // never lit, never hit: a press on it lands on the panel
                if (const auto LBL = cachedText(I.label, COLDIM, T.body, CAPPX, -1, 0, false, 400); LBL && LBL->tex)
                    texAt(P, LBL, x + LX, ry + (RH - texH(LBL, P.scale)) / 2);
                ry += RH;
                continue;
            }
            // the row under the pointer sits on the pill, its words in their
            // resting ink: a hover only damages, and no lit face needs warming
            // (crash class 4)
            if (hovered.kind == SCard::MENU_ITEM && hovered.id == (uint32_t)i)
                P.rect(CBox{x + PAD, ry, w - 2 * PAD, RH}, tPill(), PX(Theme::RAD_ROW), 2.f);
            const auto LBL  = restOf(I);
            const auto FAIL = failOf(I);
            const double LY = FAIL ? ry + DEV_PADY : ry + (RH - texH(LBL, P.scale)) / 2;
            if (LBL && LBL->tex)
                texAt(P, LBL, x + LX, LY);
            if (FAIL)
                texAt(P, FAIL, x + LX, LY + texH(LBL, P.scale) + FAIL_GAP);
            if (I.disc) { // a device: its glyph in a disc, white while connected
                const CBox D{x + DISC_X, ry + (RH - DISC_D) / 2, DISC_D, DISC_D};
                P.rect(D, I.check == 1 ? COLTITLE : tFill(), PX(DISC_D / 2), 2.f);
                if (const auto G = Glyph::tex(I.glyph, I.check == 1 ? tLitInk() : COLTITLE, PX(DISC_GLYPH)))
                    P.texIn(G, D);
            } else {
                if (I.check == 1)
                    if (const auto G = Glyph::tex(Glyph::eGlyph::CHECK, COLTITLE, (int)std::lround(CHECK_GLYPH * P.scale)))
                        P.texIn(G, CBox{x + CHECK_X, ry, CHECK_W, RH});
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
        // the activity turns on its own tick: its box is damaged, nothing warms
        s_spinning = spun;
        if (!spun)
            s_spinBox = {};
        else if (s_spinTick && !s_spinTick->armed())
            s_spinTick->updateTimeout(SPIN_STEP);
    }

    void init() {
        s_spinTick = makeShared<CEventLoopTimer>(
            std::nullopt,
            [](SP<CEventLoopTimer> self, void*) {
                if (!s_open || !s_spinning || s_spinBox.w <= 0 || !g_pHyprRenderer)
                    return;
                g_pHyprRenderer->damageBox(CBox{s_spinBox}.expand(1));
                self->updateTimeout(SPIN_STEP);
            },
            nullptr);
        g_pEventLoopManager->addTimer(s_spinTick);
    }

} // namespace NAwesome::Notify::CardMenu
