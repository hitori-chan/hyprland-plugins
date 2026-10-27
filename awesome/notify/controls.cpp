// awesome/notify/controls.cpp — the control center's block atop the shade:
// three switches (Do Not Disturb, the microphone, the touchpad) and a
// slider per level (the display, the keyboard light, the volume). The
// values are the system module's, published into core/controls.hpp as it
// learns them, and a move goes back through the doors it registered there;
// Do Not Disturb is the model's own. While the block shows, the system
// posts no capsule for what it shows: a key that moves a level moves its
// slider.
//
// The warm takes one snapshot (controlsHeight) and the draw paints exactly
// it, so every number a draw shows was rastered by the warm before it
// (crash class 4). A dragged slider shows its own value, and keeps showing
// what it asked for until the device reports that level (or HOLD passes):
// a set still in flight never snaps it back.

#include "ui.hpp"

#include "../core/controls.hpp"

namespace NAwesome::Notify {

    using Controls::eLevel;
    using Controls::eSwitch;

    // the block (logical px): its padding, the switches' tiles, a row per level
    inline constexpr double CTL_PADX = 14, CTL_PADY = 12;
    inline constexpr double TILE_H = 52, TILE_GAP = 8, TILE_GLYPH = 18, TILE_GLYPH_Y = 9, TILE_LABEL_Y = 31, TILE_LABEL_H = 16;
    inline constexpr double ROWS_GAP = 8; // the tiles -> the first level
    inline constexpr double SLIDER_H = 30, SLIDER_GLYPH = 16, SLIDER_GAP = 12, TRACK_H = 6, KNOB_D = 14, NUM_W = 26;
    // what a slider asked shows this long while its device answers
    constexpr auto          HOLD = std::chrono::milliseconds(1500);

    enum eTile : uint8_t {
        TILE_DND,
        TILE_MIC,
        TILE_PAD,
        TILE_COUNT,
    };

    // the last warm's values: the draw paints them
    static struct {
        std::array<int, (size_t)eLevel::COUNT_> pct{-1, -1, -1}; // -1: no device, no row
        bool                                    muted = false;   // the volume's
        std::array<int8_t, TILE_COUNT>          on{0, -1, -1};   // -1: no device (dim, inert)
    } snap;

    static struct {
        bool   live  = false;
        eLevel level = eLevel::DISPLAY;
        CBox   track; // the press's layout: the drag maps along it
        int    pct = -1;
    } drag;

    struct SHeld {
        int             pct = -1;
        Time::steady_tp until;
    };
    static std::array<SHeld, (size_t)eLevel::COUNT_> held;
    static SP<CEventLoopTimer>                       heldEnd; // a hold that ran out unanswered shows the device's own level

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

    // the knob's centre runs the track inset by its radius: 0 % and 100 %
    // sit whole inside it
    static double knobX(const CBox& track, int pct) {
        return track.x + KNOB_D / 2 + (track.w - KNOB_D) * std::clamp(pct, 0, 100) / 100.0;
    }
    static int pctAt(const CBox& track, double x) {
        const double SPAN = std::max(1.0, track.w - KNOB_D);
        return std::clamp((int)std::lround((x - track.x - KNOB_D / 2) / SPAN * 100.0), 0, 100);
    }

    double controlsHeight(bool warm) {
        if (warm) {
            for (size_t i = 0; i < snap.pct.size(); i++)
                snap.pct[i] = shownPct((eLevel)i);
            const auto& S = Controls::state();
            snap.muted    = S.levels[(size_t)eLevel::VOLUME].muted;
            snap.on       = {(int8_t)(Model::suspendedNow() ? 1 : 0), S.switches[(size_t)eSwitch::MIC], S.switches[(size_t)eSwitch::TOUCHPAD]};
        }
        const size_t ROWS = (size_t)std::ranges::count_if(snap.pct, [](int p) { return p >= 0; });
        // the last row's glyph sits as far from the bottom as the tiles from the top
        return CTL_PADY + TILE_H + (ROWS ? ROWS_GAP + ROWS * SLIDER_H + CTL_PADY - (SLIDER_H - SLIDER_GLYPH) / 2 : CTL_PADY);
    }

    void renderControls(const SPaint& P, const SType& T, const CBox& block) {
        const double X = block.x, Y = block.y, W = block.w;
        const auto COLTITLE = color(cfg().getColor("plugin:awesome:notify:col_title")), COLSUB = color(cfg().getColor("plugin:awesome:notify:col_kicker")),
                   COLACC = color(cfg().getColor("plugin:awesome:notify:col_highlight")), COLBG = color(cfg().getColor("plugin:awesome:theme:col_glass")),
                   COLRIM = color(cfg().getColor("plugin:awesome:theme:col_rim")), COLURGENT = color(cfg().getColor("plugin:awesome:notify:col_urgent"));
        const auto COLDIM = COLSUB.modifyA(COLSUB.a * 0.6);
        const auto PX     = [&](double logical) { return (int)std::lround(logical * P.scale); };

        P.frost(block, COLBG, COLRIM, rCard(P.scale), rPow());

        // ---- the switches: lit (the accent, as text) while on ----
        static constexpr std::array<const char*, TILE_COUNT> LABEL = {"Do Not Disturb", "Microphone", "Touchpad"};
        const double                                         TW    = (W - 2 * CTL_PADX - (size_t)(TILE_COUNT - 1) * TILE_GAP) / (size_t)TILE_COUNT;
        for (size_t i = 0; i < TILE_COUNT; i++) {
            const CBox   TB{X + CTL_PADX + i * (TW + TILE_GAP), Y + CTL_PADY, TW, TILE_H};
            const int8_t ON  = snap.on[i];
            const bool   HOV = ON >= 0 && hovered.kind == SCard::CTRL_SWITCH && hovered.id == i;
            P.rect(TB, HOV ? tFillHover() : tFill(), PX(Theme::RAD_BUTTON), rPow());
            const auto GLYPH = i == TILE_DND ? Glyph::eGlyph::MOON :
                i == TILE_MIC                ? (ON == 0 ? Glyph::eGlyph::MIC_OFF : Glyph::eGlyph::MIC) :
                                               (ON == 0 ? Glyph::eGlyph::TOUCHPAD_OFF : Glyph::eGlyph::TOUCHPAD);
            if (const auto G = Glyph::tex(GLYPH, ON < 0 ? COLDIM : ON ? COLACC : COLTITLE, PX(TILE_GLYPH)))
                P.texIn(G, CBox{TB.x, TB.y + TILE_GLYPH_Y, TB.w, TILE_GLYPH});
            texCentered(P, cachedText(LABEL[i], ON < 0 ? COLDIM : ON ? COLACC : COLSUB, T.small, PX(TB.w), -1, 0, false, 500), CBox{TB.x, TB.y + TILE_LABEL_Y, TB.w, TILE_LABEL_H});
            SCard c = recycledCard();
            c.kind  = SCard::CTRL_SWITCH;
            c.id    = (uint32_t)i;
            c.box   = TB;
            cards.push_back(std::move(c));
        }

        // ---- the levels: the capsule's row (glyph, track, number), draggable ----
        double ry = Y + CTL_PADY + TILE_H + ROWS_GAP;
        for (size_t l = 0; l < snap.pct.size(); l++) {
            const int PCT = snap.pct[l];
            if (PCT < 0)
                continue;
            const bool MUTED = (eLevel)l == eLevel::VOLUME && snap.muted;
            const CBox ROW{X + CTL_PADX, ry, W - 2 * CTL_PADX, SLIDER_H};
            const auto GLYPH = (eLevel)l == eLevel::DISPLAY ? Glyph::eGlyph::SUN :
                (eLevel)l == eLevel::KEYBOARD               ? Glyph::eGlyph::KEYBOARD_LIGHT :
                                                              (MUTED ? Glyph::eGlyph::SPEAKER_MUTED : Glyph::eGlyph::SPEAKER);
            if (const auto G = Glyph::tex(GLYPH, MUTED ? COLURGENT : COLTITLE, PX(SLIDER_GLYPH)))
                P.texIn(G, CBox{ROW.x, ROW.y, SLIDER_GLYPH, SLIDER_H});
            const double TX0 = ROW.x + SLIDER_GLYPH + SLIDER_GAP, TX1 = ROW.x + ROW.w - NUM_W - SLIDER_GAP;
            const CBox   TRK{TX0, ROW.y + (SLIDER_H - TRACK_H) / 2, std::max(KNOB_D, TX1 - TX0), TRACK_H};
            const int    TR = PX(TRACK_H / 2);
            const double KX = knobX(TRK, PCT);
            P.rect(TRK, tTrack(), TR, 2.f);
            P.rect(CBox{TRK.x, TRK.y, KX - TRK.x, TRACK_H}, MUTED ? tFillHover() : COLTITLE, TR, 2.f);
            const CBox KNOB{KX - KNOB_D / 2, ROW.y + (SLIDER_H - KNOB_D) / 2, KNOB_D, KNOB_D};
            P.shadow(CBox{KNOB}.expand(3).translate(Vector2D{0.0, 1.0}), PX(KNOB_D / 2 + 3), 2.f, 3);
            P.rect(KNOB, MUTED ? COLSUB : COLTITLE, PX(KNOB_D / 2), 2.f);
            // past 100 % says so (a level set elsewhere); the track stops full
            if (const auto NUM = cachedText(std::to_string(PCT), MUTED ? COLSUB : COLTITLE, T.meta, PX(NUM_W * 2), -1, 0, false, 600); NUM && NUM->tex)
                P.tex(NUM->tex, ROW.x + ROW.w - texW(NUM, P.scale), ROW.y + (SLIDER_H - texH(NUM, P.scale)) / 2);
            SCard c = recycledCard();
            c.kind  = SCard::CTRL_SLIDER;
            c.id    = (uint32_t)l;
            c.box   = ROW;
            c.track = TRK;
            cards.push_back(std::move(c));
            ry += SLIDER_H;
        }
    }

    // ---- input (input.cpp calls these; each runs in its emission and
    //      defers its work: the doors arm their own hops) ----

    void controlsDragStart(const SCard& c, const Vector2D& pos) {
        drag.live  = true;
        drag.level = (eLevel)c.id;
        drag.track = c.track;
        drag.pct   = -1;
        controlsDragMove(pos);
    }

    void controlsDragMove(const Vector2D& pos) {
        if (!drag.live)
            return;
        // per motion: an ask (and a warm) only when the percent moved
        if (const int PCT = pctAt(drag.track, pos.x); PCT != drag.pct) {
            drag.pct = PCT;
            ask(drag.level, PCT);
        }
    }

    bool controlsDragging() {
        return drag.live;
    }

    // the release, a lock, the shade closing: the last ask holds
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
        const auto& S = Controls::state();
        const auto& D = Controls::doors();
        switch (tile) {
            case TILE_DND: Model::toggleSuspend(); break;
            case TILE_MIC:
                if (S.switches[(size_t)eSwitch::MIC] >= 0 && D.setSwitch)
                    D.setSwitch(eSwitch::MIC, S.switches[(size_t)eSwitch::MIC] == 0);
                break;
            case TILE_PAD:
                if (S.switches[(size_t)eSwitch::TOUCHPAD] >= 0 && D.setSwitch)
                    D.setSwitch(eSwitch::TOUCHPAD, S.switches[(size_t)eSwitch::TOUCHPAD] == 0);
                break;
            default: break;
        }
    }

    // the volume's glyph: mute, or back on
    void controlsToggleMute() {
        if (Controls::doors().setMuted && Controls::state().levels[(size_t)eLevel::VOLUME].pct >= 0)
            Controls::doors().setMuted(!Controls::state().levels[(size_t)eLevel::VOLUME].muted);
    }

    // the shade opened: the levels as they are now, and no capsule over them
    void controlsOpened() {
        Controls::panelShowing() = true;
        if (Controls::doors().refresh)
            Controls::doors().refresh();
    }

    void controlsClosed() {
        Controls::panelShowing() = false;
        drag.live = false;
        held.fill({});
        if (heldEnd)
            heldEnd->updateTimeout(std::nullopt);
    }

    void controlsInit() {
        Controls::changedHook() = []() {
            if (centerVisible())
                notifChanged();
        };
        heldEnd = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { notifChanged(); }, nullptr);
        g_pEventLoopManager->addTimer(heldEnd);
    }

    void controlsExit() {
        Controls::changedHook() = nullptr;
        controlsClosed();
        if (heldEnd && g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(heldEnd);
        heldEnd.reset();
    }

} // namespace NAwesome::Notify
