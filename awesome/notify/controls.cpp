// awesome/notify/controls.cpp — the control center: macOS's panel of glass
// modules under the bar's switch glyph, in the cards' column. A first row
// of Do Not Disturb's capsule (its circle, its name, its state) and the
// microphone's and the touchpad's round buttons; under it a module per
// level (the display, the keyboard light, the volume): its name and value,
// the track between its low and high glyphs, and a corner button where the
// level has one (follow the room, the output). The values are the system
// module's, published into core/controls.hpp as it learns them, and a move
// goes back through the doors it registered there; Do Not Disturb is the
// model's own. While the panel shows, the system posts no capsule for what
// it shows: a key that moves a level moves its slider. Banners keep coming,
// under the panel.
//
// The warm takes one snapshot (controlsHeight) and the draw paints exactly
// it, so every word a draw shows was rastered by the warm before it (crash
// class 4). A dragged slider shows its own value, and keeps showing what it
// asked for until the device reports that level (or HOLD passes): a set
// still in flight never snaps it back. A light of few levels (a keyboard's
// off, low, high) is a stepped slider: notches at its levels, the drag
// snapping to them.

#include "ui.hpp"

#include "../core/controls.hpp"
#include "../core/walltimer.hpp"

namespace NAwesome::Notify {

    using Controls::eLevel;
    using Controls::eSwitch;

    // the panel (logical px): modules 12 apart; the first row's capsule and
    // round buttons, then a module per level
    inline constexpr double MOD_GAP = 12, ROW_H = 64, BTN_D = 64, BTN_GLYPH = 22;
    inline constexpr double CAP_PAD = 12, CAP_DOT = 40, CAP_GLYPH = 18, CAP_TEXT_GAP = 12;
    inline constexpr double MOD_H = 64, MOD_R = 20, MOD_PADX = 16, TITLE_Y = 11, TITLE_H = 18;
    inline constexpr double SLIDE_Y = 32, SLIDE_H = 26, TRACK_H = 6, LOW_GLYPH = 14, HIGH_GLYPH = 18, END_GAP = 10;
    inline constexpr double CORNER_D = 28, CORNER_GLYPH = 16, CORNER_GAP = 12, NOTCH = 4;
    // what a slider asked shows this long while its device answers
    constexpr auto          HOLD = std::chrono::milliseconds(1500);

    enum eTile : uint8_t {
        TILE_DND,
        TILE_MIC,
        TILE_PAD,
        TILE_ROOM,   // the keyboard light's corner: follow the room
        TILE_OUTPUT, // the volume's corner: its outputs' menu
        TILE_COUNT,
    };

    // the last warm's values: the draw paints them
    static struct {
        std::array<Controls::SLevel, (size_t)eLevel::COUNT_> levels{}; // pct -1: no device, no module
        std::array<int8_t, TILE_COUNT>                       on{0, -1, -1, -1, -1}; // -1: none (dim, inert, or no corner)
        int64_t                                              dndEnd = 0;
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

    static bool            s_on = false;
    static Time::steady_tp s_openedAt;
    static bool            s_animating = false;
    static CBox            s_outputBtn; // the last layout's output button: its menu opens under it

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

    // a notch's centre: the ends sit inside the track's rounding
    static double notchX(const CBox& track, int step, int steps) {
        return track.x + TRACK_H / 2 + (track.w - TRACK_H) * step / steps;
    }

    // what a level's module says it is at
    static std::string valueText(eLevel l, const Controls::SLevel& v) {
        if (l == eLevel::VOLUME && v.muted)
            return "Muted";
        if (v.steps == 1 || v.steps == 2) {
            static constexpr std::array<std::array<const char*, 3>, 2> NAMES = {{{"Off", "On", ""}, {"Off", "Low", "High"}}};
            return NAMES[v.steps - 1][stepOf(v.pct, v.steps)];
        }
        return std::to_string(v.pct) + "%";
    }

    static std::string dndText() {
        if (snap.on[TILE_DND] <= 0)
            return "Off";
        return snap.dndEnd > 0 ? "Until " + untilText(snap.dndEnd) : "On";
    }

    bool controlsVisible() {
        return s_on;
    }

    bool controlsAnimating() {
        if (s_animating && animT(s_openedAt, Theme::MOTION_SPATIAL) >= 1.f)
            s_animating = false;
        return s_animating;
    }

    double controlsHeight(bool warm) {
        if (warm) {
            for (size_t i = 0; i < snap.levels.size(); i++) {
                snap.levels[i]     = Controls::state().levels[i];
                snap.levels[i].pct = shownPct((eLevel)i);
            }
            const auto& S = Controls::state();
            snap.on       = {(int8_t)(Model::suspendedNow() ? 1 : 0), S.switches[(size_t)eSwitch::MIC], S.switches[(size_t)eSwitch::TOUCHPAD],
                             S.switches[(size_t)eSwitch::ROOM], (int8_t)(S.outputs.size() > 1 ? 0 : -1)};
            snap.dndEnd   = Model::suspendedNow() ? Model::suspendEnd() : 0;
        }
        const size_t MODS = (size_t)std::ranges::count_if(snap.levels, [](const auto& v) { return v.pct >= 0; });
        return ROW_H + MODS * (MOD_GAP + MOD_H);
    }

    // where the banners start while the panel shows: under it
    double controlsReach() {
        return s_on ? controlsHeight(false) + MOD_GAP : 0;
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
                   COLBG = color(cfg().getColor("plugin:awesome:theme:col_glass")), COLRIM = color(cfg().getColor("plugin:awesome:theme:col_rim")),
                   COLURGENT = color(cfg().getColor("plugin:awesome:notify:col_urgent"));
        const auto COLDIM = COLSUB.modifyA(COLSUB.a * 0.6);
        const auto PX     = [&](double logical) { return (int)std::lround(logical * P.scale); };
        const auto HOV    = [&](eTile t) { return hovered.kind == SCard::CTRL_SWITCH && hovered.id == t; };
        const auto TILE   = [&](eTile t, const CBox& box) {
            SCard c = recycledCard();
            c.kind  = SCard::CTRL_SWITCH;
            c.id    = (uint32_t)t;
            c.box   = box;
            cards.push_back(std::move(c));
        };
        // a lit control is white with the deepened accent for its glyph; a
        // control off is glass, its glyph white; one with no device is dim
        const auto INK = [&](int8_t on) { return on < 0 ? COLDIM : on ? tLitInk() : COLTITLE; };
        const auto DISC = [&](const CBox& box, int8_t on, bool hover, bool glassUnder) {
            const int R = PX(box.h / 2);
            if (glassUnder)
                P.frost(box, hover && on == 0 ? liftedGlass(COLBG) : COLBG, COLRIM, R, 2.f);
            if (on > 0)
                P.rect(box, COLTITLE.modifyA(hover ? 0.86f : 1.f), R, 2.f);
            else if (!glassUnder)
                P.rect(box, hover ? tFillHover() : tFill(), R, 2.f);
        };

        // the column's dead space between the modules swallows its clicks
        {
            SCard pc;
            pc.kind = SCard::PANEL;
            pc.box  = CBox{X, Y, W, H};
            cards.push_back(pc);
        }

        // ---- the first row: Do Not Disturb's capsule, the two buttons ----
        {
            const double CW = W - 2 * (BTN_D + MOD_GAP);
            const CBox   CAP{X, Y, CW, ROW_H};
            const bool   CH = HOV(TILE_DND);
            P.frost(CAP, CH ? liftedGlass(COLBG) : COLBG, COLRIM, PX(ROW_H / 2), 2.f);
            const CBox DOT{CAP.x + CAP_PAD, CAP.y + (ROW_H - CAP_DOT) / 2, CAP_DOT, CAP_DOT};
            DISC(DOT, snap.on[TILE_DND], false, false);
            if (const auto G = Glyph::tex(Glyph::eGlyph::MOON, INK(snap.on[TILE_DND]), PX(CAP_GLYPH)))
                P.texIn(G, DOT);
            const double TX = DOT.x + CAP_DOT + CAP_TEXT_GAP, TW = CAP.x + CAP.w - CAP_PAD - TX;
            const auto   NAME = cachedText("Do Not Disturb", COLTITLE, T.title, PX(TW), -1, 0, false, 600);
            const auto   STATE = cachedText(dndText(), COLSUB, T.meta, PX(TW), -1, 0, false, 400);
            const double NH = texH(NAME, P.scale), SH = texH(STATE, P.scale), TY = CAP.y + (ROW_H - NH - 1 - SH) / 2;
            if (NAME && NAME->tex)
                P.tex(NAME->tex, TX, TY);
            if (STATE && STATE->tex)
                P.tex(STATE->tex, TX, TY + NH + 1);
            TILE(TILE_DND, CAP);

            for (const auto [T_, GLYPH_ON, GLYPH_OFF, SLOT] : {std::tuple{TILE_MIC, Glyph::eGlyph::MIC, Glyph::eGlyph::MIC_OFF, 0},
                                                               std::tuple{TILE_PAD, Glyph::eGlyph::TOUCHPAD, Glyph::eGlyph::TOUCHPAD_OFF, 1}}) {
                const int8_t ON = snap.on[T_];
                const CBox   B{CAP.x + CAP.w + MOD_GAP + SLOT * (BTN_D + MOD_GAP), Y, BTN_D, BTN_D};
                DISC(B, ON, ON >= 0 && HOV(T_), true);
                if (const auto G = Glyph::tex(ON == 0 ? GLYPH_OFF : GLYPH_ON, INK(ON), PX(BTN_GLYPH)))
                    P.texIn(G, B);
                TILE(T_, B);
            }
        }

        // ---- a module per level ----
        double my = Y + ROW_H + MOD_GAP;
        s_outputBtn = {};
        for (size_t l = 0; l < snap.levels.size(); l++) {
            const auto& V = snap.levels[l];
            if (V.pct < 0)
                continue;
            const auto LEVEL = (eLevel)l;
            const bool MUTED = LEVEL == eLevel::VOLUME && V.muted;
            const CBox MOD{X, my, W, MOD_H};
            P.frost(MOD, COLBG, COLRIM, PX(MOD_R), rPow());
            my += MOD_H + MOD_GAP;

            // the corner: the keyboard light follows the room, the volume
            // picks its output
            const eTile CORNER = LEVEL == eLevel::KEYBOARD ? TILE_ROOM : LEVEL == eLevel::VOLUME ? TILE_OUTPUT : TILE_COUNT;
            const bool  HASC   = CORNER != TILE_COUNT && snap.on[CORNER] >= 0;
            const CBox  CB{MOD.x + MOD.w - MOD_PADX - CORNER_D, MOD.y + (MOD_H - CORNER_D) / 2, CORNER_D, CORNER_D};
            const double RIGHT = HASC ? CB.x - CORNER_GAP : MOD.x + MOD.w - MOD_PADX;

            static constexpr std::array<const char*, 3> NAME = {"Display", "Keyboard", "Sound"};
            if (const auto N = cachedText(NAME[l], COLTITLE, T.title, PX(W), -1, 0, false, 600); N && N->tex)
                P.tex(N->tex, MOD.x + MOD_PADX, MOD.y + TITLE_Y + (TITLE_H - texH(N, P.scale)) / 2);
            if (const auto VT = cachedText(valueText(LEVEL, V), MUTED ? COLURGENT : COLSUB, T.title, PX(W / 2), -1, 0, false, 400); VT && VT->tex)
                P.tex(VT->tex, RIGHT - texW(VT, P.scale), MOD.y + TITLE_Y + (TITLE_H - texH(VT, P.scale)) / 2);

            // the slider: low glyph, track, high glyph
            const auto LOW = LEVEL == eLevel::DISPLAY ? Glyph::eGlyph::SUN_LOW :
                LEVEL == eLevel::KEYBOARD             ? Glyph::eGlyph::KEYBOARD :
                                                        (MUTED ? Glyph::eGlyph::SPEAKER_MUTED : Glyph::eGlyph::SPEAKER_LOW);
            const auto HIGH = LEVEL == eLevel::DISPLAY ? Glyph::eGlyph::SUN : LEVEL == eLevel::KEYBOARD ? Glyph::eGlyph::KEYBOARD_BRIGHT : Glyph::eGlyph::SPEAKER;
            const double SY = MOD.y + SLIDE_Y;
            const CBox   LOWB{MOD.x + MOD_PADX, SY, LOW_GLYPH, SLIDE_H}, HIGHB{RIGHT - HIGH_GLYPH, SY, HIGH_GLYPH, SLIDE_H};
            if (const auto G = Glyph::tex(LOW, MUTED ? COLURGENT : COLTITLE, PX(LOW_GLYPH)))
                P.texIn(G, LOWB);
            if (const auto G = Glyph::tex(HIGH, COLTITLE, PX(HIGH_GLYPH)))
                P.texIn(G, HIGHB);
            const double TX0 = LOWB.x + LOW_GLYPH + END_GAP, TX1 = HIGHB.x - END_GAP;
            const CBox   TRK{TX0, SY + (SLIDE_H - TRACK_H) / 2, std::max(TRACK_H * 2, TX1 - TX0), TRACK_H};
            const int    TR    = PX(TRACK_H / 2);
            const int    STEPS = V.steps;
            // past 100 % (a level set elsewhere) the track stops full
            const double FILL = STEPS > 0 ? (stepOf(V.pct, STEPS) > 0 ? notchX(TRK, stepOf(V.pct, STEPS), STEPS) + TRACK_H / 2 - TRK.x : 0) :
                                            TRK.w * std::clamp(V.pct, 0, 100) / 100.0;
            P.rect(TRK, tTrack(), TR, 2.f);
            if (FILL > 0)
                P.rect(CBox{TRK.x, TRK.y, std::max(FILL, TRACK_H), TRACK_H}, MUTED ? tFillHover() : COLTITLE, TR, 2.f);
            // a stepped light's notches: dark on the fill, grey past it
            if (STEPS > 0 && STEPS <= 10)
                for (int k = 0; k <= STEPS; k++) {
                    const double NX = notchX(TRK, k, STEPS);
                    P.rect(CBox{NX - NOTCH / 2, TRK.y + (TRACK_H - NOTCH) / 2, NOTCH, NOTCH}, NX < TRK.x + FILL ? COLBG.modifyA(1.f) : COLSUB, PX(NOTCH / 2), 2.f);
                }

            SCard c = recycledCard();
            c.kind  = SCard::CTRL_SLIDER;
            c.id    = (uint32_t)l;
            c.box   = MOD;
            c.track = TRK;
            cards.push_back(std::move(c));

            if (HASC) {
                const int8_t ON = snap.on[CORNER];
                DISC(CB, CORNER == TILE_ROOM ? ON : 0, HOV(CORNER), false);
                if (const auto G = Glyph::tex(CORNER == TILE_ROOM ? Glyph::eGlyph::ROOM : Glyph::eGlyph::OUTPUT, INK(CORNER == TILE_ROOM ? ON : 0), PX(CORNER_GLYPH)))
                    P.texIn(G, CB);
                TILE(CORNER, CB);
                if (CORNER == TILE_OUTPUT)
                    s_outputBtn = CB;
            }
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

    // a tile's click, from the hit drain
    void controlsSwitch(uint32_t tile) {
        const auto& S    = Controls::state();
        const auto& D    = Controls::doors();
        const auto  FLIP = [&](eSwitch s) {
            if (S.switches[(size_t)s] >= 0 && D.setSwitch)
                D.setSwitch(s, S.switches[(size_t)s] == 0);
        };
        switch (tile) {
            case TILE_DND: Model::toggleSuspend(); break;
            case TILE_MIC: FLIP(eSwitch::MIC); break;
            case TILE_PAD: FLIP(eSwitch::TOUCHPAD); break;
            case TILE_ROOM: FLIP(eSwitch::ROOM); break;
            case TILE_OUTPUT:
                // anchored at the button's right edge: the menu, too wide
                // for the column's edge, flips left and hangs under it
                if (s_outputBtn.w > 0)
                    CardMenu::openControl(eLevel::VOLUME, Vector2D{s_outputBtn.x + s_outputBtn.w, s_outputBtn.y + s_outputBtn.h + 4});
                break;
            default: break;
        }
    }

    // the volume's low glyph: mute, or back on
    void controlsToggleMute() {
        if (Controls::doors().setMuted && Controls::state().levels[(size_t)eLevel::VOLUME].pct >= 0)
            Controls::doors().setMuted(!Controls::state().levels[(size_t)eLevel::VOLUME].muted);
    }

    static void closed() {
        Controls::panelShowing() = false;
        drag.live                = false;
        s_animating              = false;
        s_outputBtn              = {};
        held.fill({});
        if (heldEnd)
            heldEnd->updateTimeout(std::nullopt);
    }

    // Event-loop only. The panel and the notification center share the
    // column: one opening closes the other.
    void setControls(bool on) {
        if (on == s_on)
            return;
        CardMenu::close(); // an output menu belongs to the panel going
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
    }

    void controlsExit() {
        Controls::changedHook() = nullptr;
        s_on = false;
        closed();
        if (heldEnd && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(heldEnd);
        heldEnd.reset();
    }

} // namespace NAwesome::Notify
