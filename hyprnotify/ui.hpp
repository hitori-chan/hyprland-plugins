#pragma once
// hyprnotify/ui.hpp — the surface machinery shared by the drawing units
// (popups.cpp, center.cpp, render.cpp). Private to the plugin: the public
// module map lives in hyprnotify.hpp.
//
// The texture rule (crash class 4) runs through everything here: cachedText
// builds only while the warm gate allows it, SPaint paints only outside the
// warm, and every glyph a draw needs must have been requested by the warm
// that preceded it — request textures UNCONDITIONALLY in layout code, gate
// only the painting on P.warm.

#include "common/texcache.hpp"
#include "common/theme.hpp"

#include "hyprnotify.hpp"

namespace NHyprnotify {

    namespace Theme = NHyprCommon::Theme; // the glass·ink tokens

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
    inline constexpr double BADGE = 13; // the identity corner badge

    inline constexpr double CENTER_W = 360, CENTER_MAXH = 430;
    inline constexpr double ROW_PADT = 9, ROW_PADX = 12, ROW_PADB = 10, ROW_ICON = 34, ROW_ICON_GAP = 10, ROW_GAP = 8;
    inline constexpr double CHEV = 24;                      // the chevron circle
    inline constexpr double CHILD_ICON = 28, CHILD_GAP = 2; // segmented group children
    inline constexpr double PREV_ICON = 16;                 // digest preview avatars
    inline constexpr double PILL_H = 20;                    // the count pill
    inline constexpr double BAR_BTN = 34, BAR_PADT = 4, BAR_PADX = 10, BAR_PADB = 12, BAR_GAP = 8;
    inline constexpr double HEAD_PADT = 13, HEAD_PADX = 14, HEAD_PADB = 9; // the history view's title
    inline constexpr double BODY_PADT = 10, BODY_PADX = 10, BODY_PADB = 10;

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

    bool   animationsOn(); // animations=0 is the motion kill switch
    bool   blurOn();
    double blurRadius();   // px the glass grows damage by
    double damageMargin(PHLMONITOR m);

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
    std::string        esc(const std::string& raw); // RAW string -> markup-safe (app names)
    std::string        hexOf(const CHyprColor& c);
    std::string        lastLine(const std::string& body); // the collapsed one-liner: the newest message
    std::string        ageString(const Time::steady_tp& t); // bucketed: "now", "5m", "2h", "3d"

    // ---- render.cpp: frame state the drawing units read ----

    extern SHover hovered;      // current hover, for fills and reveals
    extern double lastContentH; // popup column / panel extents, for the pass bounding box
    extern double lastContentW;

    // ---- popups.cpp / center.cpp: the two surfaces ----

    void renderPopups(const SPaint& P, const SType& T);
    void renderCenter(const SPaint& P, const SType& T);
    bool popupsAnimating(); // any arrival spring still running
    bool centerAnimating(); // the open spring still running
    // center.cpp owns the fold/view state; render.cpp resets it via setCenter

} // namespace NHyprnotify
