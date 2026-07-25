#pragma once
// hyprnotify/ui.hpp — the surface machinery shared by the drawing units
// (popups.cpp, center.cpp, render.cpp). Private to the plugin: the public
// module map lives in hyprnotify.hpp.
//
// "center" throughout the drawing code means the SHADE: one list of live
// cards, Android's notification shade.
//
// The texture rule (crash class 4) runs through everything here: cachedText
// builds only while the warm gate allows it, SPaint paints only outside the
// warm, and every glyph a draw needs must have been requested by the warm
// that preceded it — request textures UNCONDITIONALLY in layout code, gate
// only the painting on P.warm.

#include "common/glass.hpp"
#include "common/texcache.hpp"

#include "hyprnotify.hpp"

namespace NHyprnotify {

    namespace Theme = NHyprCommon::Theme; // the glass·ink tokens
    using NHyprCommon::animationsOn;      // and their runtime side (glass.hpp)
    using NHyprCommon::blurOn;
    using NHyprCommon::blurRadius;
    using NHyprCommon::tAccentDim;
    using NHyprCommon::tFill;
    using NHyprCommon::tFill2;
    using NHyprCommon::tOnAccent;

    // the warm/draw state machine — common/texcache.hpp
    inline NHyprCommon::CWarmGate warmGate;

    // ---- layout constants (logical px; the decided spec) ----

    inline constexpr double EDGE = 10;                           // right screen inset
    inline constexpr double PADX = 14, PADY = 11, ICON_GAP = 12; // popup card padding
    inline constexpr double HEAD_GAP = 3, TITLE_GAP = 4;         // header -> title -> body
    inline constexpr double PROGRESS_H = 5, PROGRESS_GAP = 8;
    inline constexpr double HERO_CAP = 110, HERO_TEXT_MIN = 60;
    inline constexpr double BTN_H = 26, BTN_PADX = 10, BTN_GAP = 4, BTN_ROW_GAP = 6, BTN_ICON = 15, BTN_ICON_GAP = 5;
    inline constexpr double BODYIMG_H = 96, IMG_GAP = 6, IMG_ROW_GAP = 8;
    inline constexpr double XCIRC = 20; // the hover-✕ / group-✕ circle

    // The identity badge, as ratios of the avatar box it rides. AOSP's
    // notification_template_conversation_icon_container.xml: a 48dp avatar
    // wearing a 20dp badge that protrudes 4dp past its right and bottom
    // edges, with the app glyph 12dp inside that badge.
    inline constexpr double BADGE_D = 20.0 / 48.0, BADGE_PROT = 4.0 / 48.0, BADGE_INSET = 4.0 / 20.0;

    inline constexpr double CENTER_W = 360; // the shade's height is the monitor's (see renderCenter)
    inline constexpr double ROW_PADT = 9, ROW_PADX = 12, ROW_PADB = 10, ROW_ICON = 40, ROW_ICON_GAP = 10, ROW_GAP = 8;
    inline constexpr double CHEV = 24;                       // the fold chevron circle
    inline constexpr double MANAGE_D = 20, MANAGE_GAP = 4;   // the hover-revealed manage strip, beside the chevron
    inline constexpr double CHILD_ICON = 28, CHILD_GAP = 2; // segmented group children
    inline constexpr double PREV_ICON = 16;                 // digest preview avatars
    inline constexpr double PILL_H = 20;                    // the count pill
    inline constexpr double BAR_BTN = 34, BAR_PADT = 4, BAR_PADX = 10, BAR_PADB = 12, BAR_GAP = 8;
    inline constexpr double BODY_PADT = 10, BODY_PADX = 10, BODY_PADB = 10;
    inline constexpr double STACK_GAP = 3; // the joint gap that merges the rows into one column

    // ---- paint.cpp: context, type scale, motion, config gates ----

    struct SType { // per-frame type roles, physical pt
        int header, title, body, small, action, bar;
    };
    SType typeScale(double scale);

    struct SPaint {
        PHLMONITOR mon;
        bool       warm  = false;
        double     scale = 1.0;
        float      alpha = 1.f; // motion: the arriving surface fades in
        double     dy    = 0;   // motion: slide offset, painting only — hit boxes stay final
        Vector2D   monPos;

        CBox       toPhys(const CBox& global) const; // global logical -> monitor physical
        void       rect(const CBox& global, const CHyprColor& c, int round = 0, float rp = 2.f) const;
        void       glass(const CBox& global, const CHyprColor& c, int round, float rp) const; // translucent + live blur
        void       shadow(const CBox& global, int round, float rp, int range) const;
        void       ring(const CBox& global, const CHyprColor& c, int round, float rp) const; // 1px hairline border
        void       tex(const SP<ITexture>& t, double gx, double gy) const;                   // native px at a logical pos
        void       texFit(const SP<ITexture>& t, const CBox& cell, int round = 0, float rp = 2.f) const;
    };

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
        uint64_t           gen = 0;
    };

    // Content + style + width IS the key: a replace or an age-bucket move
    // simply misses to a new key. Builds only while the warm gate allows;
    // a draw-side miss flags the rewarm. maxHpx < 0 caps LINES
    // (single-paragraph text only); linkCol non-null collects <a href> rects.
    const SCachedText* cachedText(const std::string& text, const CHyprColor& col, int pt, int maxWpx, int maxHpx, float lineSp, bool markup, int weight,
                                  const CHyprColor* linkCol = nullptr);

    double             texH(const SCachedText* e, double scale);
    double             texW(const SCachedText* e, double scale);

    void               textCacheTick();  // a full warm begins: advance the grace generation
    void               textCacheSweep(); // a full warm ended: evict what no recent warm wanted
    void               textCacheClear();

    // small shared helpers
    std::string        hexOf(const CHyprColor& c);
    std::string        lastLine(const std::string& body); // the collapsed one-liner: the newest message
    std::string        ageString(const Time::steady_tp& t); // bucketed: "now", "5m", "2h", "3d"

    // the layout passes compose row strings per frame: build them into a
    // reused buffer (capacity retained; ONE composition live at a time) and
    // memoize the markup color hex — cachedText copies only on a cache miss
    std::string&       scratch();
    void               appendEsc(std::string& dst, const std::string& raw);
    const std::string& hexOfCached(const CHyprColor& c);

    // ---- render.cpp: frame state the drawing units read ----

    extern SHover hovered;      // current hover, for fills and reveals
    extern double lastContentH; // popup column / panel extents, for the pass bounding box
    extern double lastContentW;

    // ---- row.cpp: one shade row, and the two faces of an app bundle ----

    // a display item: one card, or an app's bundle of them (newest first)
    struct SDisp {
        std::vector<SP<SNotif>> items;
        std::string             key; // the app key (bundles)
    };

    // Singles and bundle children are the same row in different clothes.
    struct SRowStyle {
        double iconPx;       // 40 rows, 28 children
        bool   withBadge;    // children ride plain avatars — the header owns identity
        bool   headerHasApp; // singles: "App • age"; children: age only
        bool   hasChevron;   // singles fold; expanded-bundle children are always open
        bool   canReply;     // the inline-reply field; conversations never bundle, so children never need it
        bool   manage;       // the silence/priority strip; a child's app is managed from its bundle header
    };
    inline constexpr SRowStyle ROW_SINGLE{ROW_ICON, true, true, true, true, true};
    inline constexpr SRowStyle ROW_CHILD{CHILD_ICON, false, false, false, false, false};

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
    double groupHeadH();                                        // an expanded bundle's header row

    // ---- popups.cpp / center.cpp: the two surfaces ----

    void renderPopups(const SPaint& P, const SType& T);
    void renderCenter(const SPaint& P, const SType& T);
    bool popupsAnimating(); // any arrival spring still running
    bool centerAnimating(); // the open spring still running
    // center.cpp owns the fold/paging state; render.cpp resets it via setCenter

} // namespace NHyprnotify
