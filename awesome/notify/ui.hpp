// awesome/notify/ui.hpp — the surface machinery shared by the notify
// drawing units (popups, center, card, bundle, menu, surface). Private to
// the module: the public map lives in notify.hpp.
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
    inline constexpr double PADX = 14, PADY = 12, CHILD_PADY = 9, ICON_GAP = 12;
    inline constexpr double META_GAP = 8;   // title -> the meta at the row's right
    inline constexpr double ROW_GAP = 2;    // the title row -> the body
    inline constexpr double BLOCK_GAP = 10; // the full-width blocks: hero, buttons, the reply field
    inline constexpr double PROGRESS_H = 4, PROGRESS_GAP = 8;
    inline constexpr double HERO_CAP = 160;
    inline constexpr double BTN_H = 28, BTN_PADX = 10, BTN_GAP = 6, BTN_ICON = 15, BTN_ICON_GAP = 6;
    inline constexpr double BODYIMG_H = 96, IMG_GAP = 6, IMG_ROW_GAP = 8;
    inline constexpr double FIELD_H = 32, SEND_D = 24; // the reply field and its send circle
    inline constexpr double PILL_H = 16;               // the unread count beside a chat's title

    // The identity badge, as ratios of the avatar it rides: an 18 px disc on
    // a 36 px face whose white ring (1.5 px) cuts the app glyph free of
    // whatever the face is, overhanging the corner by 3 px.
    inline constexpr double BADGE_D = 18.0 / 36.0, BADGE_OUT = 3.0 / 36.0, BADGE_RING = 1.5 / 18.0;

    // the center
    inline constexpr double HEADER_H = 28, HEADER_GAP = 4; // "Notifications", the moon, "Clear All"
    inline constexpr double GROUP_HEAD_H = 40, GROUP_ICON = 20, CIRCLE = 22, TAIL_H = 30; // a bundle's header and its "+N more"
    inline constexpr double SCRIM_W = 640; // the dusk behind the column, from the screen's right edge
    inline constexpr double EMPTY_H = 56;

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
    // a glass button: a filled pill, its label (and icon) centered
    void                paintButton(const SPaint& P, const CBox& box, const SCachedText* label, const SP<ITexture>& icon, bool hover);
    // a centered texture in a logical cell
    void                texCentered(const SPaint& P, const SCachedText* t, const CBox& cell);

    float  easeOutCubic(float t);
    float  easeOutBack(float t); // the spatial overshoot
    float  animT(const Time::steady_tp& since, int ms); // 0..1 clamped

    // ---- text.cpp: the keyed raster cache ----

    struct SCachedText {
        SP<ITexture>       tex; // null = rastered to nothing; still a cached result
        std::vector<SLink> links; // physical px rel rects (body markup only)
    };

    // The body line's REAL metrics (the font's own, measured once per
    // style): the layout used to estimate 1.35x the point size, which
    // undershot the default font by ~7% — a seven-line transcript
    // rendered six, and the clip took the newest line once the order went
    // chronological.
    double bodyLineH(double scale);         // logical px, one body line
    int    bodyBudgetPx(double scale, int lines); // physical px for whole lines

    // Content + style + width IS the key: a replace or an age-bucket move
    // simply misses to a new key. Builds only while the gate allows;
    // a draw-side miss flags the rewarm. maxHpx >= 0 is a pixel budget
    // (rounded down to whole lines, tail ellipsized); < 0 is one line per
    // paragraph, ellipsized. linkCol non-null collects <a href> rects.
    // headCut ellipsizes at the START: an entry, whose newest characters
    // (the caret's end) must stay in view.
    const SCachedText* cachedText(const std::string& text, const CHyprColor& col, int pt, int maxWpx, int maxHpx, float lineSp, bool markup, int weight,
                                  const CHyprColor* linkCol = nullptr, bool headCut = false);

    const SCachedText* scrimTex(); // the center's dusk, a horizontal ramp

    double texH(const SCachedText* e, double scale);
    double texW(const SCachedText* e, double scale);

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
            CENTER, // the center: clock time, the reply field, folds
            CHILD,  // inside a bundle: no icon, no glass of its own
        };
        eWhere where = BANNER;
        bool   open  = true; // CENTER: folded = the title row and the newest line
    };

    // Lays out and (outside a warm) paints one card at box.x/y, box.w wide;
    // returns its height and fills `hit`.
    double renderCard(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box, const SCardForm& F, SCard& hit);
    // the same code with a context that draws nothing — the budget's ruler
    double measureCard(const SPaint& P, const SType& T, const SP<SNotif>& N, double w, const SCardForm& F);

    // ---- bundle.cpp: an app's cards folded into one ----

    // a display item: one card, or an app's bundle of them (newest first)
    struct SDisp {
        std::vector<SP<SNotif>> items;
        std::string             key; // the app key (bundles)
    };

    double digestH(const SType& T, size_t count, double scale); // the folded bundle's height
    double groupChromeH(bool cut);                              // an open bundle's header (+ its tail when cut)
    void   paintDigest(const SPaint& P, const SType& T, const SDisp& D, const CBox& box);
    void   paintGroup(const SPaint& P, const SType& T, const SDisp& D, const CBox& box, const std::vector<double>& childH);

    // ---- popups.cpp / center.cpp: the surfaces ----

    void renderPopups(const SPaint& P, const SType& T);
    void renderCapsules(const SPaint& P, const SType& T); // the OSD band, top center
    void renderCenter(const SPaint& P, const SType& T);
    bool popupsAnimating(); // any arrival spring still running
    bool centerAnimating(); // the open spring still running

    // ---- menu.cpp: the card menu (hold left on a card) ----

    namespace CardMenu {
        bool     canOpen(uint32_t id); // a notification (never feedback) the user can see
        bool     open(uint32_t id, const Vector2D& at);
        void     close();
        bool     isOpen();
        uint32_t owner(); // the card it belongs to; 0 = none open
        void     pick(int index); // from the click drain
        void     sync();          // after a model change: its card died -> close
        void     render(const SPaint& P, const SType& T); // its panel, then its rows (hit first)
        void     exit();          // teardown: forget it, no damage, no warm
    }

    // the current hover (surface.cpp owns it; setHovered is its door) —
    // the drawing units read it for fills and reveals
    extern SHover hovered;

    // the center's dusk behind its column, global logical; w 0 = none
    // (center.cpp writes it, surface.cpp's bounds and damage read it)
    extern CBox lastScrim;

} // namespace NAwesome::Notify
