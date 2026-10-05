// awesome/notify/ui.hpp — the surface machinery shared by the notify
// drawing units (popups.cpp, center.cpp, row.cpp, surface.cpp). Private to
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
#include "../core/theme.hpp"

namespace NAwesome::Notify {

    // the paint context is the canvas' (toPhys/rect/glass/border/ring/
    // shadow/tex/texIn/texFit/texStretch); the warm gate is the canvas'
    using SPaint = NAwesome::SPaint;

    // ---- layout constants (logical px; the decided spec) ----

    inline constexpr double EDGE = 10; // right screen inset

    // The cards' top below the monitor's top: offset_y, but never above the
    // workarea — the strip reserves its band, and a card over it would be
    // drawn where the strip takes the press (a taller shell:height than
    // offset_y once put the top cards' clicks on the bar).
    inline double cardsTop(const PHLMONITOR& mon) {
        const double OFF = (double)NAwesome::cfg().getI("plugin:awesome:notify:offset_y");
        return mon ? std::max(OFF, mon->logicalBoxMinusReserved().y - mon->logicalBox().y) : OFF;
    }
    // what the monitor reserves at its bottom edge (a dock, a bottom bar):
    // the shade stops above it, or its footer sits under the dock and the
    // dock takes the presses
    inline double cardsBottomReserved(const PHLMONITOR& mon) {
        if (!mon)
            return 0;
        const auto MB = mon->logicalBox();
        const auto WA = mon->logicalBoxMinusReserved();
        return std::max(0.0, (MB.y + MB.h) - (WA.y + WA.h));
    }
    inline constexpr double PADX = 14, PADY = 11, ICON_GAP = 12; // popup card padding
    inline constexpr double HEAD_GAP = 3, TITLE_GAP = 4; // header -> title -> body
    inline constexpr double PROGRESS_H = 5, PROGRESS_GAP = 8;
    inline constexpr double HERO_CAP = 110, HERO_TEXT_MIN = 60;
    inline constexpr double BTN_H = 26, BTN_PADX = 10, BTN_GAP = 4, BTN_ROW_GAP = 6, BTN_ICON = 15, BTN_ICON_GAP = 5;
    inline constexpr double BODYIMG_H = 96, IMG_GAP = 6, IMG_ROW_GAP = 8;
    inline constexpr double XCIRC = 20; // the hover-✕ / group-✕ circle

    // The identity badge, as ratios of the avatar box it rides — AOSP's 2025
    // notification_2025_conversation_icon_container.xml: a 40dp avatar wearing
    // a 20dp badge, of which 16dp is the app glyph and 2dp on each side is the
    // rim, positioned so the GLYPH sits flush with the avatar's bottom-right
    // corner (AOSP writes that margin out as 40 - 16 - 2 = 22dp) and only the
    // rim protrudes. AOSP halved the rim and grew the glyph.
    inline constexpr double BADGE_D = 20.0 / 40.0, BADGE_PROT = 2.0 / 40.0, BADGE_INSET = 2.0 / 20.0;

    inline constexpr double CENTER_W = 360; // the shade's width; its height is the monitor's
    inline constexpr double ROW_PADT = 9, ROW_PADX = 12, ROW_PADB = 10, ROW_ICON = 40, ROW_ICON_GAP = 10;
    inline constexpr double CHEV = 24; // the fold chevron circle
    inline constexpr double CHILD_ICON = 28, CHILD_GAP = 2; // segmented group children
    inline constexpr double PREV_ICON = 16; // digest preview avatars
    inline constexpr double PILL_H = 20; // the count pill
    inline constexpr double BAR_BTN = 34, BAR_PADT = 4, BAR_PADX = 10, BAR_PADB = 12, BAR_GAP = 8;
    inline constexpr double BODY_PADT = 10, BODY_PADX = 10, BODY_PADB = 10;
    inline constexpr double STACK_GAP = 3; // the joint gap that merges the rows into one column

    // ---- paint.cpp: the type scale, motion, radii, card recipes ----

    struct SType { // per-frame type roles, physical pt
        int header, title, body, small, action, bar;
    };
    SType typeScale(double scale);

    double damageMargin(PHLMONITOR m);

    // The radius family. ONE configured card radius; the panel that wraps the
    // rows is rounder, a row inside it is tighter, and the joint that merges
    // stacked children is tighter still. Physical px, so they take the scale.
    float  rPow();
    int    rPanel(double scale);
    int    rRow(double scale);
    int    rJoint(double scale);

    // shared card recipes — the progress pill and the content-first icon
    // column (lead avatar wearing the identity corner badge); layout code
    // computes presence itself via hasLeadIcon
    bool   hasLeadIcon(const SNotif& n);
    void   paintProgress(const SPaint& P, double x, double y, double w, int pct, bool critical);
    void   paintIconColumn(const SPaint& P, const SNotif& n, const CBox& cell, bool withBadge, float rp);

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
    // dir 0 = down, 1 = up; a px-square canvas, the Material chevron centered
    const SCachedText* chevronTex(int dir, const CHyprColor& col, int px);

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
    std::string        lastLine(const std::string& body); // any body's newest line ends it

    // the layout passes compose row strings per frame: build them into a
    // reused buffer (capacity retained; ONE composition live at a time) and
    // memoize the markup color hex — cachedText copies only on a cache miss
    std::string&       scratch();
    void               appendEsc(std::string& dst, const std::string& raw);
    const std::string& hexOfCached(const CHyprColor& c);

    // ---- row.cpp: one shade row, and the two faces of an app bundle ----

    // a display item: one card, or an app's bundle of them (newest first)
    struct SDisp {
        std::vector<SP<SNotif>> items;
        std::string             key; // the app key (bundles)
    };

    // Singles and bundle children are the same row in different clothes.
    struct SRowStyle {
        double iconPx; // 40 rows, 28 children
        bool   withBadge; // children ride plain avatars — the header owns identity
        bool   headerHasApp; // singles: "App • age"; children: age only
        bool   hasChevron; // singles fold; expanded-bundle children are always open
        bool   canReply; // the inline-reply field; conversations never bundle, so children never need it
    };
    inline constexpr SRowStyle ROW_SINGLE{ROW_ICON, true, true, true, true};
    inline constexpr SRowStyle ROW_CHILD{CHILD_ICON, false, false, false, false};

    // Lays out (and outside the warm paints) one row; returns its height and
    // fills the card's hit boxes. `more` drives the chevron: an open row can
    // always be folded, a collapsed one only offers it when there is
    // something behind it.
    double renderRow(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box, bool open, bool more, const SRowStyle& ST, SCard& card, bool child);
    // the same code with a context that draws nothing — the budget's ruler
    double measureRow(const SPaint& P, const SType& T, const SP<SNotif>& N, double w, bool open, const SRowStyle& ST);

    // the three things the placement pass can put in a slot; each paints its
    // own fill and pushes its own hit card
    void   paintSingle(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box, bool open, bool more);
    void   paintDigest(const SPaint& P, const SType& T, const SDisp& D, const CBox& box);
    void   paintGroup(const SPaint& P, const SType& T, const SDisp& D, const CBox& box, const std::vector<double>& childH);

    double digestH(const SType& T, size_t count, double scale); // the folded bundle's height
    double groupHeadH(); // an expanded bundle's header row

    // ---- popups.cpp / center.cpp: the two surfaces ----

    void renderPopups(const SPaint& P, const SType& T);
    void renderCenter(const SPaint& P, const SType& T);
    bool popupsAnimating(); // any arrival spring still running
    bool centerAnimating(); // the open spring still running

    // the current hover (surface.cpp owns it; setHovered is its door) —
    // the drawing units read it for fills and reveals
    extern SHover hovered;

    // the last layout's extents, logical (popups.cpp / center.cpp write
    // them; surface.cpp's boundingBox reads them for the pass)
    extern double lastContentH;
    extern double lastContentW;

} // namespace NAwesome::Notify
