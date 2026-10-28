// awesome/notify/ui.hpp — the surface machinery shared by the notify
// drawing units (popups, center, card, stack, menu, surface). Private to
// the module: its map lives at the top of model.hpp.
//
// "center" throughout means the SHADE: one list of live cards, Android's
// notification shade.
//
// The texture rule (crash class 4) runs through everything: cachedText
// builds only while the canvas gate allows it, SPaint paints only outside
// the warm, and every glyph a draw needs must have been requested by the
// warm that preceded it — request textures UNCONDITIONALLY in layout code,
// gate only the painting on P.warm.

#pragma once

#include "model.hpp"

#include "../core/canvas.hpp"
#include "../core/controls.hpp"
#include "../core/glyph.hpp"
#include "../core/theme.hpp"

namespace NAwesome::Notify {

    // the paint context is the canvas' (rect/glass/frost/ring/shadow/tex/
    // texIn/texFit/texStretch); the warm gate is the canvas'
    using SPaint = NAwesome::SPaint;
    struct SCachedText;

    // ---- layout constants (logical px; the Frost spec) ----

    inline constexpr double EDGE = 12; // the column's right screen inset

    // The cards' top below the monitor's top: offset_y, but never above the
    // workarea — the strip reserves its band, and a card over it would be
    // drawn where the strip takes the press (a taller shell:height than
    // offset_y once put the top cards' clicks on the bar).
    inline double cardsTop(const PHLMONITOR& mon) {
        const double OFF = (double)NAwesome::cfg().getI("plugin:awesome:notify:offset_y");
        return mon ? std::max(OFF, mon->logicalBoxMinusReserved().y - mon->logicalBox().y) : OFF;
    }
    // what the monitor reserves at its bottom edge (a dock, a bottom bar):
    // the center stops above it, or its last card sits under the dock and
    // the dock takes the presses
    inline double cardsBottomReserved(const PHLMONITOR& mon) {
        if (!mon)
            return 0;
        const auto MB = mon->logicalBox();
        const auto WA = mon->logicalBoxMinusReserved();
        return std::max(0.0, (MB.y + MB.h) - (WA.y + WA.h));
    }

    // the card
    inline constexpr double PADX = 14, PADY = 12, ICON_GAP = 12;
    inline constexpr double META_GAP = 8;   // title -> the meta at the row's right
    inline constexpr double ROW_GAP = 2;    // the title row -> the body
    inline constexpr double BLOCK_GAP = 10; // the full-width blocks: the hero, the reply field
    inline constexpr double PROGRESS_H = 4, PROGRESS_GAP = 8;
    inline constexpr double HERO_CAP = 160;
    inline constexpr double BODYIMG_H = 96, IMG_GAP = 6, IMG_ROW_GAP = 8;
    inline constexpr double FIELD_H = 30, SEND_D = 22, SEND_ARROW = 12; // the reply field, its send circle and arrow
    inline constexpr double FIELD_PADL = 12, FIELD_GAP = 6, FIELD_PADR = 4; // the text's inset, its room before the circle, the circle's off the end
    inline constexpr double PILL_H = 16;               // the unread count beside a chat's title
    inline constexpr double HANG = 12; // a chat's hanging indent: every line that continues a message

    // a chip's drop under it (paintChip)
    inline constexpr double CHIP_DROP = 8, CHIP_SINK = 3;

    // The identity badge, as ratios of the avatar it rides: an 18 px disc on
    // a 36 px face whose white ring (1.5 px) cuts the app glyph free of
    // whatever the face is, overhanging the corner by 3 px. It is one raster
    // at its device size (icons.cpp's composeBadge): an even diameter, so
    // the disc centres on a pixel corner, and a ring of whole pixels.
    inline constexpr double BADGE_D = 18.0 / 36.0, BADGE_OUT = 3.0 / 36.0, BADGE_RING = 1.5 / 18.0;
    inline int              badgePx(int facePx) {
        return std::max(8, 2 * (int)std::lround(facePx * BADGE_D / 2));
    }
    inline int badgeRingPx(int badgePx) {
        return std::max(1, (int)std::lround(badgePx * BADGE_RING));
    }

    // the center
    inline constexpr double HEADER_H = 28, HEADER_GAP = 8; // the heading's row ("Notifications", "Clear All"), then the column's 8 px
    inline constexpr double CAP_H = 24, CAP_PADX = 10; // its glass capsules, and a stack's
    inline constexpr double EMPTY_H = 56;
    inline constexpr double STATUS_H = 36; // the status line under the heading while DND or quiet holds
    inline constexpr double TIER_GAP = 8;  // added between the ranking's groups (critical, chats, apps, silent)
    inline constexpr double FOLD_H   = 44; // the silent fold's row
    inline constexpr double SHEAD_H  = 28; // a spread stack's header: its app's name, Show Less
    // A collapsed stack (macOS): its newest card over a deck of up to two
    // layers, each 12 px narrower a side and 8 px lower than the one above;
    // only their bottom 30 px ever shows past the card.
    inline constexpr double DECK_INSET = 12, DECK_STEP = 8, DECK_H = 30, DECK_ROUND = 16;
    inline constexpr size_t DECK_LAYERS = 2;

    // ---- paint.cpp: the type scale, motion, radii, card recipes ----

    struct SType { // per-frame type roles, physical px
        int meta, title, body, small, action, heading;
    };
    SType typeScale(double scale);

    double damageMargin(PHLMONITOR m);

    // the configured card radius and its superellipse; physical px
    float  rPow();
    int    rCard(double scale);

    // one load per card for every surface that shows it: the banner and the
    // center ask for the same sizes, so a card moving between them reuses
    // its textures
    void   prepareCard(const SP<SNotif>& n, double scale);

    // shared card recipes; layout code computes presence via hasLeadIcon.
    // `shade`: a center card, which leads with a hero's thumbnail where a
    // banner shows the hero itself
    const SP<ITexture>& contentFace(const SNotif& n, bool shade);
    bool                hasLeadIcon(const SNotif& n, bool shade = false);
    void                paintProgress(const SPaint& P, double x, double y, double w, int pct, bool critical);
    void                paintIconColumn(const SPaint& P, const SNotif& n, const CBox& cell, bool withBadge, float rp, bool shade = false);
    // the app's identity on the corner of a face filling `cell`
    void                paintBadge(const SPaint& P, const SNotif& n, const CBox& cell);
    // a small control floating on glass (a stack's Show Less, "+N more"):
    // its own drop, the glass, the rim
    void                paintChip(const SPaint& P, const CBox& box, bool hover);
    // the glass under the pointer lifts a step, as macOS's platter does
    CHyprColor          liftedGlass(const CHyprColor& c);
    // a centered texture in a logical cell
    void                texCentered(const SPaint& P, const SCachedText* t, const CBox& cell);

    float  easeOutCubic(float t);
    float  easeOutBack(float t); // the spatial overshoot
    float  animT(const Time::steady_tp& since, int ms); // 0..1 clamped

    // ---- text.cpp: the keyed raster cache ----

    struct SCachedText {
        SP<ITexture>       tex; // null = rastered to nothing; still a cached result
        std::vector<SLink> links; // physical px rel rects (body markup only)
        int                pad = 0; // physical px of shadow margin around the text (shadowed text only)
    };

    // The body line's REAL metrics (the font's own, measured once per
    // style): the layout used to estimate 1.35x the point size, which
    // undershot the default font by ~7% — a seven-line transcript
    // rendered six, and the clip took the newest line once the order went
    // chronological.
    int    bodyBudgetPx(double scale, int lines); // physical px for whole lines

    // Content + style + width IS the key: a replace or an age-bucket move
    // simply misses to a new key. Builds only while the gate allows;
    // a draw-side miss flags the rewarm. maxHpx >= 0 is a pixel budget
    // (rounded down to whole lines, tail ellipsized); < 0 is one line per
    // paragraph, ellipsized. linkCol non-null collects <a href> rects.
    // headCut ellipsizes at the START: an entry, whose newest characters
    // (the caret's end) must stay in view.
    // shadow: the drop the text wears (core/text.hpp), in a margin of
    // SCachedText::pad: DESKTOP bare on the wallpaper, none over glass.
    // texW/texH are the ink's size; texAt and texCentered place the ink.
    // indentPx: a hanging indent, every line of a paragraph but its first
    const SCachedText* cachedText(const std::string& text, const CHyprColor& col, int pt, int maxWpx, int maxHpx, float lineSp, bool markup, int weight,
                                  const CHyprColor* linkCol = nullptr, bool headCut = false, eShadow shadow = eShadow::NONE, int indentPx = 0);

    // A chat card's body (one paragraph per message), fitted from the
    // newest end into capPx (Transcript::fitTail) by the layout the body's
    // raster uses — the same width, links and hanging indent. Memoized on
    // the card (SNotif::fitMemo).
    const std::string& fittedTranscript(const SP<SNotif>& n, int pt, int widthPx, int capPx, int indentPx, const CHyprColor* linkCol);

    double texH(const SCachedText* e, double scale);
    double texW(const SCachedText* e, double scale);
    void   texAt(const SPaint& P, const SCachedText* e, double x, double y); // the ink's top-left at (x, y)

    void   textCacheTick(); // a full warm begins: advance the grace generation
    void   textCacheSweep(); // a full warm ended: evict what no recent warm wanted
    void   decodeWarmBegin(); // a full warm begins: no decode job is wanted yet
    void   decodeWarmEnd();   // a full warm ended: free the finished decodes no card wanted
    void   textCacheClear();

    // small shared helpers
    std::string        hexOf(const CHyprColor& c);
    std::string        ageString(const Time::steady_tp& t); // bucketed: "now", "5m", "2h", "3d"
    std::string        clockString(std::time_t t);          // the center's: "14:02" today, "24 Oct" before
    std::string        lastLine(const std::string& body); // any body's newest line ends it

    // the layout passes compose row strings per frame: build them into a
    // reused buffer (capacity retained; ONE composition live at a time) and
    // memoize the markup color hex — cachedText copies only on a cache miss
    std::string&       scratch();
    void               appendEsc(std::string& dst, const std::string& raw);
    const std::string& hexOfCached(const CHyprColor& c);

    // ---- card.cpp: one card, wherever it shows ----

    struct SCardForm {
        enum eWhere : uint8_t {
            BANNER, // the column: relative age, the hero, inline images, max_height
            CENTER, // the center: clock time, folds
        };
        eWhere where = BANNER;
        bool   open  = true;  // CENTER: folded = the title row and the newest line
        bool   deck  = false; // CENTER: a collapsed stack's top card — its hit is the stack's
        bool   fresh = false; // CENTER: new since the center last showed it: its time wears the accent
    };

    // the card's glass lifts: the pointer is on it, or its menu is open
    bool cardLit(uint32_t id, SCard::eKind kind);

    // Lays out and (outside a warm) paints one card at box.x/y, box.w wide;
    // returns its height and fills `hit`.
    double renderCard(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box, const SCardForm& F, SCard& hit);
    // the same code with a context that draws nothing — the budget's ruler
    double measureCard(const SPaint& P, const SType& T, const SP<SNotif>& N, double w, const SCardForm& F);

    // ---- stack.cpp: an app's cards stacked under the newest ----

    // a display item: one card, an app's stack of them (newest first), or
    // the silent fold
    struct SDisp {
        std::vector<SP<SNotif>> items;
        std::string             key;  // the app key (stacks)
        uint8_t                 tier = 0; // its group in the ranking: a wider gap where it changes
        bool                    fold = false; // the silent fold: its items are every silent card
    };

    // the deck's layers under a collapsed stack of `count` cards, and how far
    // they reach below its top card
    size_t deckLayers(size_t count);
    double deckDepth(size_t count);
    // the deck under a top card at `top` (paint it first: the card covers it)
    void   paintDeck(const SPaint& P, const CBox& top, size_t count);
    // an expanded stack's header: its app's name, Show Less right-aligned on
    // glass of its own; pushes its hit
    void   paintStackHead(const SPaint& P, const SType& T, const SDisp& D, const CBox& row);
    // the silent fold's row: two of its apps' icons, the count and the apps,
    // a chevron; pushes its hit
    void   paintSilentFold(const SPaint& P, const SType& T, const SDisp& D, const CBox& row);
    // "+N more" under an expanded stack the page cuts
    void   paintStackTail(const SPaint& P, const SType& T, size_t more, const CBox& row);

    // ---- popups.cpp / center.cpp: the surfaces ----

    void renderPopups(const SPaint& P, const SType& T);
    void renderCapsules(const SPaint& P, const SType& T); // the OSD band, top center
    void renderCenter(const SPaint& P, const SType& T);
    std::string centerDescribe(); // the gate's: the status line and the rows as the last warm built them (defined in its build only)
    bool popupsAnimating(); // any arrival spring still running
    bool centerAnimating(); // the open spring still running

    // ---- controls.cpp: the control center ----

    // its three menus (the card menu's panel, opened under what opened them)
    enum class ePanelMenu : uint8_t {
        OUTPUT,    // the output pill's: the sinks
        DND,       // Do Not Disturb's words: its durations
        BLUETOOTH, // Bluetooth's words: the paired devices
    };

    bool   controlsAnimating(); // its open spring still running
    double controlsHeight(bool warm); // the warm snapshots the values; both ask the height
    double controlsReach(); // how far the open panel pushes the banners down (0 shut)
    void   renderControls(const SPaint& P, const SType& T); // draws the panel, pushes its hits
    void   controlsDragStart(const SCard& c, const Vector2D& pos); // a left press on a level's track
    void   controlsDragMove(const Vector2D& pos);
    bool   controlsDragging();
    void   controlsDragEnd(); // the release, a lock, the panel closing: the last ask holds
    void   controlsWheel(Controls::eLevel l, int steps); // 5 % a notch, up raises
    void   controlsSwitch(uint32_t tile);                // a button's click
    void   controlsHover();                              // the hover moved: a button's name shows after a rest
    // a sink's kind by its name (headphones, else a loudspeaker): the pill's
    // glyph and the output menu's
    Glyph::eGlyph outputGlyph(const std::string& name);
    std::string   controlsTip(); // the gate's: the button name showing ("" none; defined in its build only)
    std::string   controlsBt();  // the gate's: Bluetooth's capsule state ("!" in red)

    // ---- input.cpp: acting on a card ----

    // fire the card's action (empty: its primary) as the revision `rev` drew
    // it, closing it unless resident; the center goes with it (input.cpp)
    void invokeLive(uint32_t id, const std::string& action, std::optional<uint64_t> rev = std::nullopt);

    // ---- menu.cpp: the card menu (hold left on a card) ----

    namespace CardMenu {
        bool     canOpen(uint32_t id); // a notification (never feedback) the user can see
        bool     open(uint32_t id, const Vector2D& at); // the hold: its corner at the pointer
        // a control center menu, 6 px under `anchor`, its right edge on the
        // anchor's or its left
        bool     openControl(ePanelMenu m, const CBox& anchor, bool alignRight);
        std::optional<ePanelMenu> openControlMenu(); // which one is open, if any
        void     close();
        bool     isOpen();
        uint32_t owner(); // the card it belongs to; 0 = none open
        void     pick(int index, const std::string& key); // from the click drain; a control's row by its key
        void     sync();          // after a model change: its card died -> close
        void     render(const SPaint& P, const SType& T); // its panel, then its rows (hit first)
        void     exit();          // teardown: forget it, no damage, no warm
        std::string describe(); // the gate's: the open menu's rows (defined in its build only)
    }

    // the current hover (surface.cpp owns it; setHovered is its door) —
    // the drawing units read it for fills and reveals
    extern SHover hovered;

} // namespace NAwesome::Notify
