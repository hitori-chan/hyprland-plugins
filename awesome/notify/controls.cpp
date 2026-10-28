// awesome/notify/controls.cpp — the control center: the panel under the
// bar's switch glyph, in the cards' column. A glass module per level a
// device answers for (the display, the keyboard light, the volume): its
// name and value, then a button-glyph and the track to the right edge, a
// white knob on it — with the keyboard's Follow the Room (a light sensor)
// and the volume's output pill (two outputs or more) in the module's title
// row. Under the modules, the capsule row: Bluetooth left (an adapter only) and Do Not Disturb right, alone
// across without an adapter; a capsule's circle switches it, its words
// open its menu (the paired devices; the durations). The small buttons
// name themselves after a 500 ms rest. The values are the system module's,
// published into core/controls.hpp as it learns them, and a move goes back
// through the doors it registered there; Do Not Disturb is the model's
// own. While the panel shows, the system posts no capsule for what it
// shows: a key that moves a level moves its slider. Banners keep coming,
// under the panel.
//
// The warm takes one snapshot (controlsHeight) and the draw paints exactly
// it, so every word a draw shows was rastered by the warm before it (crash
// class 4): both faces of a capsule's words, every button's name. A
// dragged slider shows its own value, and keeps showing what it asked for
// until the device reports that level (or HOLD passes): a set still in
// flight never snaps it back. A light of few levels (a keyboard's off, low,
// high) snaps its knob to them and reads in percent.

#include "ui.hpp"

#include "../core/controls.hpp"
#include "../core/walltimer.hpp"

namespace NAwesome::Notify {

    using Controls::eLevel;
    using Controls::eSwitch;

    // the panel (logical px): a 64 px module per level, 12 apart — 9 px over
    // a 24 px title row, 2, a 20 px slider row — then the capsule row 12
    // under the last, 64 tall, its two capsules 12 apart
    inline constexpr double MOD_H = 64, MOD_TOP = 9, SEC_PADX = 16;
    inline constexpr double TITLE_H = 24, SLIDE_Y = 26, SLIDE_H = 20, TRACK_H = 6, KNOB = 16, VALUE_GAP = 6;
    // a slider's glyph and its button (28 round it), the track 12 after it
    inline constexpr double GLYPH = 16, BTN = 28, GLYPH_GAP = 12;
    // a title row's own control: a 24 px disc, or the output's pill
    inline constexpr double HEAD_D = 24, HEAD_GLYPH = 14, HEAD_REACH = 4, PILL_PADL = 7, PILL_PADR = 5, CHEV = 10;
    inline constexpr double PANEL_GAP = 12, CAPS_H = 64, CAP_DOT = 40, CAP_INSET = 12, CAP_GLYPH = 18, CAP_TEXT_X = 64, CAP_PADR = 10, CHEV_GAP = 3;
    // a button's name: a dark note left of it (outside the panel for a
    // slider's, 24 px off its glyph)
    inline constexpr double TIP_H = 24, TIP_PADX = 9, TIP_GAP = 6, TIP_OUT = 24;
    // what a slider asked shows this long while its device answers; a
    // button rests this long under the pointer before it names itself
    constexpr auto HOLD = std::chrono::milliseconds(1500), TIP_DELAY = std::chrono::milliseconds(500);

    enum eTile : uint8_t {
        TILE_DND,      // Do Not Disturb's circle
        TILE_DND_MENU, // ... its words: the durations
        TILE_BT,       // Bluetooth's circle
        TILE_BT_MENU,  // ... its words: the devices
        TILE_ROOM,     // the keyboard's title row: follow the room
        TILE_OUTPUT,   // the volume's title row: the outputs
        TILE_KBD,      // the keyboard light's glyph: off, or back on
        TILE_MUTE,     // the speaker: mute
        TILE_COUNT,
    };

    // the last warm's values: the draw paints them
    static struct {
        std::array<Controls::SLevel, (size_t)eLevel::COUNT_> levels{}; // pct -1: no device, no section
        int8_t                                               room     = -1; // the keyboard's room: -1 no sensor
        bool                                                 outputs  = false; // two or more: the pill
        Glyph::eGlyph                                        outGlyph = Glyph::eGlyph::OUTPUT;
        std::string                                          outName;
        bool                                                 dnd    = false;
        bool                                                 bt = false, btOn = false; // an adapter; its power
        bool                                                 btAlarm = false; // its state is a refusal: in red
        std::string                                          btText;
        std::string                                          dndText; // its clock text, once per warm
    } snap;

    static struct {
        bool   live  = false;
        eLevel level = eLevel::DISPLAY;
        CBox   track; // the press's layout: the drag maps along it
        int    steps = 0;
        int    pct   = -1;
    } drag;

    struct SHeld {
        int             pct = -1;
        Time::steady_tp until;
    };
    static std::array<SHeld, (size_t)eLevel::COUNT_> held;
    static SP<CEventLoopTimer>                       heldEnd; // a hold that ran out unanswered shows the device's own level

    static bool                           s_on = false;
    static Time::steady_tp                s_openedAt;
    static bool                           s_animating = false;
    static std::array<CBox, TILE_COUNT>   s_tiles;  // the last layout's: what a menu hangs under, what a name sits beside
    static int                            s_kbdLast = -1; // the keyboard light's last lit level: its glyph lights it again
    // a button's name: the one the pointer rests on, and the one showing
    static eTile               s_tipWant = TILE_COUNT, s_tipFor = TILE_COUNT;
    static SP<CEventLoopTimer> tipTimer;

    static int shownPct(eLevel l) {
        if (drag.live && drag.level == l)
            return drag.pct;
        const int PUB = Controls::state().levels[(size_t)l].pct;
        auto&     H   = held[(size_t)l];
        if (H.pct >= 0 && (PUB == H.pct || Time::steadyNow() >= H.until))
            H.pct = -1;
        return H.pct >= 0 && PUB >= 0 ? H.pct : PUB;
    }

    // the level asked through the system's door; it shows at once
    static void ask(eLevel l, int pct) {
        pct = std::clamp(pct, 0, 100);
        auto& H = held[(size_t)l];
        H.pct   = pct;
        H.until = Time::steadyNow() + HOLD;
        if (heldEnd)
            heldEnd->updateTimeout(HOLD + std::chrono::milliseconds(50));
        if (Controls::doors().setLevel)
            Controls::doors().setLevel(l, pct);
        notifChanged();
    }

    // a stepped light's level nearest a percent, and back
    static int stepOf(int pct, int steps) {
        return (int)std::lround(std::clamp(pct, 0, 100) * steps / 100.0);
    }
    static int pctOfStep(int step, int steps) {
        return (int)std::lround(100.0 * step / steps);
    }

    static int pctAt(const CBox& track, double x, int steps) {
        const int PCT = std::clamp((int)std::lround((x - track.x) / std::max(1.0, track.w) * 100.0), 0, 100);
        return steps > 0 ? pctOfStep(stepOf(PCT, steps), steps) : PCT;
    }

    Glyph::eGlyph outputGlyph(const std::string& name) {
        std::string low = name;
        std::ranges::transform(low, low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        for (const char* WORD : {"head", "bud", "airpod", "earphone", "earbud"})
            if (low.contains(WORD))
                return Glyph::eGlyph::HEADPHONES;
        return Glyph::eGlyph::OUTPUT;
    }

    static bool hasTip(eTile t) {
        return t == TILE_ROOM || t == TILE_OUTPUT || t == TILE_KBD || t == TILE_MUTE;
    }

    static std::string tipText(eTile t) {
        switch (t) {
            case TILE_ROOM: return "Follow the Room";
            case TILE_OUTPUT: return "Output · " + snap.outName;
            case TILE_KBD: return snap.levels[(size_t)eLevel::KEYBOARD].pct > 0 ? "Light Off" : "Light On";
            case TILE_MUTE: return snap.levels[(size_t)eLevel::VOLUME].muted ? "Unmute" : "Mute";
            default: return {};
        }
    }

    static std::string valueText(eLevel l, const Controls::SLevel& v) {
        return l == eLevel::VOLUME && v.muted ? "Muted" : std::to_string(v.pct) + "%";
    }

    static std::string dndText(bool on, int64_t end) {
        if (!on)
            return "Off";
        return end > 0 ? "Until " + untilText(end) : "On";
    }

    // what Bluetooth's capsule says under its name: what runs, then what was
    // refused (alarm: in red), then what is connected
    static std::string btText(const Controls::SBluetooth& B, bool& alarm) {
        alarm = false;
        if (B.block == Controls::SBluetooth::HARD_BLOCKED) {
            alarm = true;
            return "Blocked";
        }
        if (!B.powered) {
            alarm = B.powerFailed;
            return alarm ? "Didn't Turn On" : "Off";
        }
        using D_ = Controls::SDevice;
        size_t      on = 0;
        bool        connecting = false, disconnecting = false;
        D_::eFail   fail = D_::NO_FAIL;
        std::string name;
        for (const auto& D : B.devices) {
            connecting |= D.busy == D_::CONNECTING;
            disconnecting |= D.busy == D_::DISCONNECTING;
            if (fail == D_::NO_FAIL)
                fail = D.fail;
            if (D.connected && on++ == 0)
                name = D.name;
        }
        if (connecting)
            return "Connecting…";
        if (disconnecting)
            return "Disconnecting…";
        if (fail != D_::NO_FAIL) {
            alarm = true;
            return Controls::failWords(fail);
        }
        return on == 1 ? name : on > 1 ? std::to_string(on) + " Devices" : "On";
    }

    bool controlsVisible() {
        return s_on;
    }

    bool controlsAnimating() {
        if (s_animating && animT(s_openedAt, Theme::MOTION_SPATIAL) >= 1.f)
            s_animating = false;
        return s_animating;
    }

    static size_t sections() {
        return (size_t)std::ranges::count_if(snap.levels, [](const auto& v) { return v.pct >= 0; });
    }

    static double levelsHeight() {
        const size_t N = sections();
        return N == 0 ? 0 : N * MOD_H + (N - 1) * PANEL_GAP;
    }

    double controlsHeight(bool warm) {
        if (warm) {
            for (size_t i = 0; i < snap.levels.size(); i++) {
                snap.levels[i]     = Controls::state().levels[i];
                snap.levels[i].pct = shownPct((eLevel)i);
            }
            const auto& S = Controls::state();
            if (const int K = S.levels[(size_t)eLevel::KEYBOARD].pct; K > 0)
                s_kbdLast = K;
            snap.room    = S.switches[(size_t)eSwitch::ROOM];
            snap.outputs = S.outputs.size() > 1;
            snap.outName.clear();
            for (const auto& O : S.outputs)
                if (O.current)
                    snap.outName = O.name;
            snap.outGlyph = outputGlyph(snap.outName);
            snap.dnd      = Model::suspendedNow();
            snap.dndText  = dndText(snap.dnd, snap.dnd ? Model::suspendEnd() : 0);
            snap.bt       = S.bluetooth.present;
            snap.btOn     = S.bluetooth.powered;
            snap.btText   = btText(S.bluetooth, snap.btAlarm);
        }
        const double LH = levelsHeight();
        return (LH > 0 ? LH + PANEL_GAP : 0) + CAPS_H;
    }

    // where the banners start while the panel shows: under it
    double controlsReach() {
        return s_on ? controlsHeight(false) + PANEL_GAP : 0;
    }

    void renderControls(const SPaint& PIN, const SType& T) {
        SPaint P = PIN;
        if (s_animating) { // the open spring: fade + a 6 px rise, as the center's
            const float AT = animT(s_openedAt, Theme::MOTION_SPATIAL);
            if (AT >= 1.f)
                s_animating = false;
            else {
                P.alpha *= easeOutCubic(AT);
                P.dy -= (1.0 - easeOutBack(AT)) * 6.0;
            }
        }

        const auto   MB = P.mon->logicalBox();
        const double W  = std::max((double)cfg().getI("plugin:awesome:notify:width"), 120.0);
        const double X = MB.x + MB.w - EDGE - W, Y = MB.y + cardsTop(P.mon);
        const double H = controlsHeight(P.warm);
        const auto   COLTITLE = color(cfg().getColor("plugin:awesome:notify:col_title")), COLSUB = color(cfg().getColor("plugin:awesome:notify:col_kicker")),
                   COLBODY = color(cfg().getColor("plugin:awesome:notify:col_fg")), COLBG = color(cfg().getColor("plugin:awesome:theme:col_glass")),
                   COLRIM = color(cfg().getColor("plugin:awesome:theme:col_rim")), COLURGENT = color(cfg().getColor("plugin:awesome:notify:col_urgent"));
        static const CHyprColor KNOB_COL{1.0, 1.0, 1.0, 1.0}, KNOB_DROP{0.0, 0.0, 0.0, 0.4}, MUTED_FILL{235 / 255.0, 235 / 255.0, 245 / 255.0, 0.38},
            PILL_OPEN{1.0, 1.0, 1.0, 0.2};
        const auto PX   = [&](double logical) { return (int)std::lround(logical * P.scale); };
        const auto HOV  = [&](eTile t) { return hovered.kind == SCard::CTRL_SWITCH && hovered.id == t; };
        const auto MENU = CardMenu::openControlMenu();
        const auto TILE = [&](eTile t, const CBox& box) {
            SCard c = recycledCard();
            c.kind  = SCard::CTRL_SWITCH;
            c.id    = (uint32_t)t;
            c.box   = box;
            cards.push_back(std::move(c));
        };
        std::array<CBox, TILE_COUNT> tipAt{}; // what each button's name sits beside

        // the column's dead space between the controls swallows its clicks
        {
            SCard pc;
            pc.kind = SCard::PANEL;
            pc.box  = CBox{X, Y, W, H};
            cards.push_back(pc);
        }
        s_tiles.fill({});

        // ---- the levels' modules ----
        const double LH = levelsHeight();
        double       my = Y;
        for (size_t l = 0; l < snap.levels.size(); l++) {
            const auto& V = snap.levels[l];
            if (V.pct < 0)
                continue;
            const CBox MOD{X, my, W, MOD_H};
            P.frost(MOD, COLBG, COLRIM, rCard(P.scale), rPow());
            my += MOD_H + PANEL_GAP;
            const double sy    = MOD.y + MOD_TOP;
            const auto   LEVEL = (eLevel)l;
            const bool   MUTED = LEVEL == eLevel::VOLUME && V.muted;
            const int    PCT   = std::clamp(V.pct, 0, 100); // past 100 % (a level set elsewhere) the track stops full
            const double RY    = sy + SLIDE_Y;
            const CBox   G{X + SEC_PADX, RY + (SLIDE_H - GLYPH) / 2, GLYPH, GLYPH};
            const double TX0 = G.x + GLYPH + GLYPH_GAP;
            const CBox   TRK{TX0, RY + (SLIDE_H - TRACK_H) / 2, std::max(TRACK_H * 2, X + W - SEC_PADX - TX0), TRACK_H};

            // the module is the wheel's and the drag's (a press on its
            // track); its buttons, pushed after, are hit before it
            SCard c = recycledCard();
            c.kind  = SCard::CTRL_SLIDER;
            c.id    = (uint32_t)l;
            c.box   = MOD;
            c.track = TRK;
            cards.push_back(std::move(c));

            // the title row: the name, its value beside it
            static constexpr std::array<const char*, 3> NAME = {"Display", "Keyboard", "Sound"};
            if (const auto N = cachedText(NAME[l], COLTITLE, T.title, PX(W / 2), -1, 0, false, 600)) {
                texAt(P, N, X + SEC_PADX, sy + (TITLE_H - texH(N, P.scale)) / 2);
                if (const auto VT = cachedText(valueText(LEVEL, V), COLSUB, T.title, PX(W / 2), -1, 0, false, 400))
                    texAt(P, VT, X + SEC_PADX + texW(N, P.scale) + VALUE_GAP, sy + (TITLE_H - texH(VT, P.scale)) / 2);
            }

            // ... and its own control, flush with the track's end
            if (LEVEL == eLevel::KEYBOARD && snap.room >= 0) {
                const CBox D{X + W - SEC_PADX - HEAD_D, sy, HEAD_D, HEAD_D};
                P.rect(D, snap.room ? COLTITLE : HOV(TILE_ROOM) ? tFillHover() : tFill(), PX(HEAD_D / 2), 2.f);
                if (const auto GT = Glyph::tex(Glyph::eGlyph::ROOM, snap.room ? tLitInk() : COLTITLE, PX(HEAD_GLYPH)))
                    P.texIn(GT, D);
                TILE(TILE_ROOM, CBox{D}.expand(HEAD_REACH));
                tipAt[TILE_ROOM] = s_tiles[TILE_ROOM] = D;
            } else if (LEVEL == eLevel::VOLUME && snap.outputs) {
                const double PW = PILL_PADL + HEAD_GLYPH + 1 + CHEV + PILL_PADR;
                const CBox   PB{X + W - SEC_PADX - PW, sy, PW, HEAD_D};
                P.rect(PB, MENU == ePanelMenu::OUTPUT ? PILL_OPEN : HOV(TILE_OUTPUT) ? tFillHover() : tFill(), PX(HEAD_D / 2), 2.f);
                if (const auto GT = Glyph::tex(snap.outGlyph, COLTITLE, PX(HEAD_GLYPH)))
                    P.texIn(GT, CBox{PB.x + PILL_PADL, PB.y, HEAD_GLYPH, HEAD_D});
                if (const auto GT = Glyph::tex(Glyph::eGlyph::CHEVRON_DOWN, COLTITLE, PX(CHEV)))
                    P.texIn(GT, CBox{PB.x + PILL_PADL + HEAD_GLYPH + 1, PB.y, CHEV, HEAD_D});
                TILE(TILE_OUTPUT, CBox{PB}.expand(HEAD_REACH));
                tipAt[TILE_OUTPUT] = s_tiles[TILE_OUTPUT] = PB;
            }

            // the slider row: the glyph (the keyboard's and the speaker's are
            // buttons), the track, the fill, the knob
            const CBox BTNBOX{G.x - (BTN - GLYPH) / 2, G.y - (BTN - GLYPH) / 2, BTN, BTN};
            if (LEVEL == eLevel::DISPLAY) {
                if (const auto GT = Glyph::tex(Glyph::eGlyph::SUN, COLTITLE, PX(GLYPH)))
                    P.texIn(GT, G);
            } else if (LEVEL == eLevel::KEYBOARD) {
                if (HOV(TILE_KBD))
                    P.rect(BTNBOX, tFill(), PX(BTN / 2), 2.f);
                if (const auto GT = Glyph::tex(V.pct > 0 ? Glyph::eGlyph::LIGHT_ON : Glyph::eGlyph::LIGHT_OFF, COLTITLE, PX(GLYPH)))
                    P.texIn(GT, G);
                TILE(TILE_KBD, BTNBOX);
                tipAt[TILE_KBD] = s_tiles[TILE_KBD] = G;
            } else {
                if (HOV(TILE_MUTE))
                    P.rect(BTNBOX, tFill(), PX(BTN / 2), 2.f);
                // its waves by level, the ones above it faint; silent, crossed
                if (!MUTED && V.pct > 0) {
                    const auto WAVES = V.pct > 66 ? Glyph::eGlyph::SPEAKER_3 : V.pct > 33 ? Glyph::eGlyph::SPEAKER_2 : Glyph::eGlyph::SPEAKER_1;
                    if (WAVES != Glyph::eGlyph::SPEAKER_3)
                        if (const auto GT = Glyph::tex(Glyph::eGlyph::SPEAKER_3, COLTITLE.modifyA(COLTITLE.a * 0.3f), PX(GLYPH)))
                            P.texIn(GT, G);
                    if (const auto GT = Glyph::tex(WAVES, COLTITLE, PX(GLYPH)))
                        P.texIn(GT, G);
                } else if (const auto GT = Glyph::tex(Glyph::eGlyph::SPEAKER_MUTED, COLTITLE, PX(GLYPH)))
                    P.texIn(GT, G);
                TILE(TILE_MUTE, BTNBOX);
                tipAt[TILE_MUTE] = s_tiles[TILE_MUTE] = G;
            }
            const int    TR = PX(TRACK_H / 2);
            const double FW = TRK.w * PCT / 100.0;
            P.rect(TRK, tTrack(), TR, 2.f);
            if (FW > 0.5)
                P.rect(CBox{TRK.x, TRK.y, FW, TRACK_H}, MUTED ? MUTED_FILL : COLTITLE, TR, 2.f);
            const CBox KB{TRK.x + FW - KNOB / 2, RY + (SLIDE_H - KNOB) / 2, KNOB, KNOB};
            P.shadow(CBox{KB}.expand(4).translate(Vector2D{0.0, 1.0}), PX(KNOB / 2), 2.f, 4, KNOB_DROP);
            P.rect(KB, KNOB_COL, PX(KNOB / 2), 2.f);
        }

        // ---- the capsule row: Bluetooth left, Do Not Disturb right ----
        const double RY = Y + (LH > 0 ? LH + PANEL_GAP : 0);
        const auto   capsule = [&](bool bt, const CBox& CAP) {
            P.frost(CAP, COLBG, COLRIM, PX(CAPS_H / 2), 2.f);
            const bool ON = bt ? snap.btOn : snap.dnd;
            const CBox DOT{CAP.x + CAP_INSET, CAP.y + CAP_INSET, CAP_DOT, CAP_DOT};
            P.rect(DOT, ON ? COLTITLE : tFill(), PX(CAP_DOT / 2), 2.f);
            if (const auto GT = Glyph::tex(bt ? Glyph::eGlyph::BLUETOOTH : Glyph::eGlyph::MOON, ON ? tLitInk() : COLTITLE, PX(CAP_GLYPH)))
                P.texIn(GT, DOT);
            // the words: the name over the state and its chevron, the state
            // brightening under the pointer or while its menu is open (the
            // glass never lightens), a refusal in red; both faces warm
            const eTile  MT = bt ? TILE_BT_MENU : TILE_DND_MENU;
            const bool   LIT = HOV(MT) || MENU == (bt ? ePanelMenu::BLUETOOTH : ePanelMenu::DND);
            const bool   ALARM = bt && snap.btAlarm;
            const double TX = CAP.x + CAP_TEXT_X, TW = CAP.w - CAP_TEXT_X - CAP_PADR;
            const auto&  WORDS = bt ? snap.btText : snap.dndText;
            const auto   NAME  = cachedText(bt ? "Bluetooth" : "Do Not Disturb", COLTITLE, T.title, PX(TW), -1, 0, false, 600);
            const auto   QUIET = cachedText(WORDS, ALARM ? COLURGENT : COLSUB, T.meta, PX(TW - CHEV_GAP - CHEV), -1, 0, false, 400);
            const auto   LOUD  = cachedText(WORDS, ALARM ? COLURGENT : COLBODY, T.meta, PX(TW - CHEV_GAP - CHEV), -1, 0, false, 400);
            const auto   STATE = LIT ? LOUD : QUIET;
            const auto   CHEVQ = Glyph::tex(Glyph::eGlyph::CHEVRON_DOWN, ALARM ? COLURGENT : COLSUB, PX(CHEV));
            const auto   CHEVL = Glyph::tex(Glyph::eGlyph::CHEVRON_DOWN, ALARM ? COLURGENT : COLBODY, PX(CHEV));
            const auto   CHEVT = LIT ? CHEVL : CHEVQ;
            const double NH = texH(NAME, P.scale), SH = texH(STATE, P.scale), TY = CAP.y + (CAPS_H - NH - SH) / 2;
            if (NAME)
                texAt(P, NAME, TX, TY);
            if (STATE) {
                texAt(P, STATE, TX, TY + NH);
                if (CHEVT)
                    P.texIn(CHEVT, CBox{TX + texW(STATE, P.scale) + CHEV_GAP, TY + NH, CHEV, SH});
            }
            TILE(bt ? TILE_BT : TILE_DND, CBox{CAP.x, CAP.y, CAP_TEXT_X, CAPS_H});
            TILE(MT, CBox{CAP.x + CAP_TEXT_X, CAP.y, CAP.w - CAP_TEXT_X, CAPS_H});
            s_tiles[MT] = CAP; // its menu hangs under the whole capsule
        };
        if (snap.bt) {
            const double CW = (W - PANEL_GAP) / 2;
            capsule(true, CBox{X, RY, CW, CAPS_H});
            capsule(false, CBox{X + CW + PANEL_GAP, RY, CW, CAPS_H});
        } else
            capsule(false, CBox{X, RY, W, CAPS_H});

        // ---- a button's name ----
        // every present one's words are warm: the rest only shows them
        for (const auto TT : {TILE_ROOM, TILE_OUTPUT, TILE_KBD, TILE_MUTE}) {
            if (tipAt[TT].w <= 0)
                continue;
            const auto TEXT = cachedText(tipText(TT), COLTITLE, T.meta, PX(W), -1, 0, false, 500);
            if (TT != s_tipFor || !TEXT)
                continue;
            const CBox&  A    = tipAt[TT];
            const bool   HEAD = TT == TILE_ROOM || TT == TILE_OUTPUT;
            const double TW   = texW(TEXT, P.scale) + 2 * TIP_PADX;
            const CBox   TB{(HEAD ? A.x - TIP_GAP : A.x - TIP_OUT) - TW, HEAD ? A.y : A.y + (A.h - TIP_H) / 2, TW, TIP_H};
            P.note(TB);
            texCentered(P, TEXT, TB);
            SCard c; // its damage and bounds; never hit
            c.kind = SCard::CAPSULE;
            c.box  = CBox{TB}.expand(SPaint::noteReach());
            cards.push_back(c);
        }
    }

    // ---- input (input.cpp calls these; each runs in its emission and
    //      defers its work: the doors arm their own hops) ----

    void controlsDragStart(const SCard& c, const Vector2D& pos) {
        drag.live  = true;
        drag.level = (eLevel)c.id;
        drag.track = c.track;
        drag.steps = Controls::state().levels[c.id].steps;
        drag.pct   = -1;
        controlsDragMove(pos);
    }

    void controlsDragMove(const Vector2D& pos) {
        if (!drag.live)
            return;
        // per motion: an ask (and a warm) only when the level moved
        if (const int PCT = pctAt(drag.track, pos.x, drag.steps); PCT != drag.pct) {
            drag.pct = PCT;
            ask(drag.level, PCT);
        }
    }

    bool controlsDragging() {
        return drag.live;
    }

    // the release, a lock, the panel closing: the last ask holds
    void controlsDragEnd() {
        drag.live = false;
    }

    // a notch steps a level as its key does — 5 %, at least one step of a
    // coarse light (an absolute percent can round back to where it was),
    // a volume past 100 % never pulled down — and the slider follows the
    // device's answer
    void controlsWheel(eLevel l, int steps) {
        if (steps == 0 || Controls::state().levels[(size_t)l].pct < 0 || !Controls::doors().step)
            return;
        held[(size_t)l] = {}; // a drag's hold must not mask the steps
        Controls::doors().step(l, steps);
    }

    static void tipHide() {
        if (tipTimer)
            tipTimer->updateTimeout(std::nullopt);
        if (std::exchange(s_tipFor, TILE_COUNT) != TILE_COUNT)
            notifChanged();
    }

    // the pointer moved between the panel's parts (surface.cpp's hover door):
    // resting on a button names it after TIP_DELAY; leaving hides the name
    void controlsHover() {
        const eTile T = s_on && hovered.kind == SCard::CTRL_SWITCH && hovered.id < TILE_COUNT && hasTip((eTile)hovered.id) ? (eTile)hovered.id : TILE_COUNT;
        if (T == s_tipWant)
            return;
        s_tipWant = T;
        tipHide();
        if (T != TILE_COUNT && tipTimer)
            tipTimer->updateTimeout(TIP_DELAY);
    }

    // a tile's click, from the hit drain: it hides the name it showed
    void controlsSwitch(uint32_t tile) {
        const auto& S = Controls::state();
        const auto& D = Controls::doors();
        tipHide();
        switch (tile) {
            case TILE_DND: Model::toggleSuspend(); break;
            case TILE_DND_MENU: CardMenu::openControl(ePanelMenu::DND, s_tiles[TILE_DND_MENU], /*alignRight=*/true); break;
            case TILE_BT: // a hard block is its owner's switch: the capsule says so
                if (S.bluetooth.present && S.bluetooth.block != Controls::SBluetooth::HARD_BLOCKED && D.setBluetooth)
                    D.setBluetooth(!S.bluetooth.powered);
                break;
            case TILE_BT_MENU: CardMenu::openControl(ePanelMenu::BLUETOOTH, s_tiles[TILE_BT_MENU], /*alignRight=*/false); break;
            case TILE_ROOM:
                if (S.switches[(size_t)eSwitch::ROOM] >= 0 && D.setSwitch)
                    D.setSwitch(eSwitch::ROOM, S.switches[(size_t)eSwitch::ROOM] == 0);
                break;
            case TILE_OUTPUT: CardMenu::openControl(ePanelMenu::OUTPUT, s_tiles[TILE_OUTPUT], /*alignRight=*/true); break;
            case TILE_KBD: { // out, or back to the level it last had (a coarse light's first step)
                const auto& K = S.levels[(size_t)eLevel::KEYBOARD];
                if (K.pct < 0)
                    break;
                ask(eLevel::KEYBOARD, K.pct > 0 ? 0 : s_kbdLast > 0 ? s_kbdLast : K.steps > 0 ? pctOfStep(1, K.steps) : 50);
                break;
            }
            case TILE_MUTE:
                if (D.setMuted && S.levels[(size_t)eLevel::VOLUME].pct >= 0)
                    D.setMuted(!S.levels[(size_t)eLevel::VOLUME].muted);
                break;
            default: break;
        }
    }

    static void closed() {
        Controls::panelShowing() = false;
        if (Controls::doors().panelClosed)
            Controls::doors().panelClosed();
        drag.live                = false;
        s_animating              = false;
        s_tiles.fill({});
        s_tipWant = s_tipFor = TILE_COUNT;
        if (tipTimer)
            tipTimer->updateTimeout(std::nullopt);
        held.fill({});
        if (heldEnd)
            heldEnd->updateTimeout(std::nullopt);
    }

    // Event-loop only. The panel and the notification center share the
    // column: one opening closes the other.
    void setControls(bool on) {
        if (on == s_on)
            return;
        CardMenu::close(); // a menu belongs to the panel going
        s_on = on;
        if (!on)
            closed();
        else {
            setCenter(false, /*repop=*/true);
            // the levels as they are now, and no capsule over them
            Controls::panelShowing() = true;
            if (Controls::doors().refresh)
                Controls::doors().refresh();
            if (animationsOn()) {
                s_openedAt  = Time::steadyNow();
                s_animating = true;
            }
        }
        notifChanged();
    }

    void controlsInit() {
        Controls::changedHook() = []() {
            if (s_on)
                notifChanged();
        };
        heldEnd = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { notifChanged(); }, nullptr);
        g_pEventLoopManager->addTimer(heldEnd);
        // the rest ran out on a button: its name shows (no menu over it)
        tipTimer = makeShared<CEventLoopTimer>(
            std::nullopt,
            [](SP<CEventLoopTimer>, void*) {
                if (!s_on || s_tipWant == TILE_COUNT || CardMenu::isOpen() || s_tipFor == s_tipWant)
                    return;
                s_tipFor = s_tipWant;
                notifChanged();
            },
            nullptr);
        g_pEventLoopManager->addTimer(tipTimer);
    }

    void controlsExit() {
        Controls::changedHook() = nullptr;
        s_on = false;
        closed();
        for (auto* T : {&heldEnd, &tipTimer}) {
            if (*T && g_pEventLoopManager)
                g_pEventLoopManager->removeTimer(*T);
            T->reset();
        }
    }

#ifdef AWESOME_GATE
    // "<tile name>" of the button name showing ("" none): the gate's
    std::string controlsTip() {
        return s_tipFor == TILE_COUNT ? std::string{} : tipText(s_tipFor);
    }

    // Bluetooth's capsule state as the last warm drew it, "!" in red
    std::string controlsBt() {
        return snap.btText + (snap.btAlarm ? "!" : "");
    }
#endif

} // namespace NAwesome::Notify
