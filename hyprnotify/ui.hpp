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

    namespace Theme = NHyprCommon::Theme; // shared semantic defaults
    using NHyprCommon::animationsOn;      // runtime config helpers (glass.hpp)
    using NHyprCommon::blurOn;
    using NHyprCommon::blurRadius;
    inline CHyprColor surface() {
        return NHyprCommon::color(cfg.colSurface);
    }
    inline CHyprColor surfaceHigh() {
        return NHyprCommon::color(cfg.colSurfaceHigh);
    }
    inline CHyprColor stateLayer() {
        return NHyprCommon::color(cfg.colState);
    }
    inline CHyprColor onHighlight() {
        return NHyprCommon::color(cfg.colOnHighlight);
    }
    inline CHyprColor onUrgent() {
        return NHyprCommon::color(cfg.colOnUrgent);
    }

    // the warm/draw state machine — common/texcache.hpp
    inline NHyprCommon::CWarmGate warmGate;

    // ---- layout constants (logical px; the decided spec) ----

    inline constexpr double EDGE = 16;                           // Pixel notification side inset
    inline constexpr double PADX = 16, PADY = 16, ICON_GAP = 16; // Pixel notification margin/content start
    inline constexpr double HEAD_GAP = 3, TITLE_GAP = 4;         // header -> title -> body
    inline constexpr double PROGRESS_H = 5, PROGRESS_GAP = 8;
    inline constexpr double BTN_H = 34, BTN_PADX = 12, BTN_GAP = 8, BTN_ROW_GAP = 6, BTN_ICON = 18, BTN_ICON_GAP = 6;
    inline constexpr double BODYIMG_H = 96, IMG_GAP = 6, IMG_ROW_GAP = 8;

    // Texture-backed marks cover deterministic identity, trusted system OSD
    // semantics, and every notification control. A common 24dp viewport keeps
    // optical size independent of fonts, source files, and aspect ratio.
    enum class eControlIcon : uint8_t {
        APPS,
        BATTERY,
        BRIGHTNESS,
        VOLUME,
        VOLUME_MUTED,
        MICROPHONE,
        MICROPHONE_MUTED,
        TOUCHPAD,
        TOUCHPAD_DISABLED,
        DO_NOT_DISTURB,
        SNOOZE,
        PRIORITY,
        NOTIFICATION_ALERT,
        NOTIFICATION_SILENT,
        EXPAND_MORE,
        EXPAND_LESS,
    };

    // The identity badge, as ratios of the avatar box it rides — AOSP's 2025
    // notification_2025_conversation_icon_container.xml: a 40dp avatar wearing
    // a 20dp badge, of which 16dp is the app glyph and 2dp on each side is the
    // rim, positioned so the GLYPH sits flush with the avatar's bottom-right
    // corner (AOSP writes that margin out as 40 - 16 - 2 = 22dp) and only the
    // rim protrudes. The 2021 template these were taken from spent 4dp on the
    // rim and left 12dp for the glyph; at our 40-44px icons that was a 10px
    // app icon inside a 3px ring. AOSP halved the rim and grew the glyph.
    inline constexpr double BADGE_D = 20.0 / 40.0, BADGE_PROT = 2.0 / 40.0, BADGE_INSET = 2.0 / 20.0;
    inline constexpr double  IDENTITY_GLYPH_RATIO = 24.0 / 40.0; // Android small icon inside its 40dp identity cell

    inline constexpr double  CENTER_W = 380; // desktop maximum; clamp to monitor width - 2*EDGE
    inline constexpr double  ROW_PADT = 16, ROW_PADX = 16, ROW_PADB = 16, ROW_ICON = 40, ROW_ICON_GAP = 16;
    inline constexpr double MANAGE_GAP = 4;                 // management row spacing
    inline constexpr double MANAGE_PADT = 12, MANAGE_PADB = 12, MANAGE_HEAD = 52, MANAGE_SECTION_GAP = 10;
    inline constexpr double MANAGE_ACTION_H = 36;
    inline constexpr double  MENU_ROW_H = 44, MENU_SELECTED_H = 62, MENU_ROW_GAP = 6, MENU_GLYPH_D = 24, MENU_GLYPH_GAP = 12;
    inline constexpr double  CHILD_ICON = 24, CHILD_GAP = 0.5;                          // Pixel group children and divider
    inline constexpr double  PILL_W = 26, PILL_H = 18, PILL_HIT = 40, CONTENT_END = 60; // Pixel expander and reserved end column
    inline constexpr double SNOOZE_H = 38;                  // the undo row, one line of chrome
    inline constexpr double  BAR_BTN = 34, BAR_PADT = 4, BAR_PADX = 10, BAR_PADB = 8, BAR_GAP = 8;
    inline constexpr double BAR_ICON = 12;
    inline constexpr double BODY_PADT = 10, BODY_PADX = 10, BODY_PADB = 10;
    inline constexpr double  STACK_GAP           = 8; // Pixel notification-to-notification breathing room
    inline constexpr uint8_t MANAGE_DONE_PART = 6;
    inline constexpr uint8_t MANAGE_DISMISS_PART = 7;

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
        void       ring(const CBox& global, const CHyprColor& c, int round, float rp, double px = 1.0) const; // border, px logical thick
        void       tex(const SP<ITexture>& t, double gx, double gy) const;                   // native px at a logical pos
        void       texClipped(const SP<ITexture>& t, double gx, double gy, const CBox& clip) const;
        void       texFit(const SP<ITexture>& t, const CBox& cell, int round = 0, float rp = 2.f) const;
        void       texCover(const SP<ITexture>& t, const CBox& cell, int round = 0, float rp = 2.f) const;
    };

    double damageMargin(PHLMONITOR m);
    bool                    liveBlurNeeded(); // only visible translucent glass requests the compositor blur path

    inline constexpr double PIXEL_SHADE_RADIUS    = 16; // heritage card radius
    inline constexpr double PIXEL_INTERNAL_RADIUS = 16; // consistent internal joints

    // Pixel's shade and connected-child radii are independent resources.
    // The rounding config remains the outer notification radius override.
    float  rPow();
    int    rPanel(double scale);
    int    rRow(double scale);
    int    rJoint(double scale);

    // shared card recipes — the progress pill and the bounded icon column;
    // conversation content may carry an application-identity badge.
    bool   hasLeadIcon(const SNotif& n);
    void   paintProgress(const SPaint& P, double x, double y, double w, int pct, bool critical);
    void   paintIconColumn(const SPaint& P, const SNotif& n, const CBox& cell, bool withBadge, float rp);
    void         paintParticipantAvatar(const SPaint& P, const SNotif& n, std::string_view participant, const CBox& cell);
    SP<ITexture> controlIcon(eControlIcon icon, int physicalPx, const CHyprColor& color, double glyphRatio = 1.0);
    void         controlIconCacheClear();

    float  easeOutCubic(float t);
    float  easeOutBack(float t); // the spatial overshoot
    float  animT(const Time::steady_tp& since, int ms); // 0..1 clamped

    // ---- text.cpp: the keyed raster cache ----

    struct SCachedText {
        SP<ITexture>       tex;   // null = rastered to nothing; still a cached result
        std::vector<SLink> links; // physical px rel rects (body markup only)
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
    std::string        shortDuration(int64_t seconds);       // the same buckets, for a standing timed silence
    const std::string& bodyForDisplay(const SNotif& n); // image alt text after a failed local decode
    const std::string& titleForDisplay(const SNotif& n);

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
    extern double centerOsdReserve; // logical height reserved below an open shade for OSD cards

    // ---- row.cpp: one shade row, and the two faces of an app bundle ----

    // a display item: one card, or an app's bundle of them (newest first)
    struct SDisp {
        std::vector<SP<SNotif>> items;
        std::string             key; // stable displayed declared/automatic group key
        bool                    classified = false;
    };

    // Singles and bundle children are the same row in different clothes.
    struct SRowStyle {
        double iconPx;       // 40 for singles; bundle children are text-only
        bool   withBadge;    // conversation singles may wear the app badge
        bool   headerHasApp; // singles: "App • age"; children: age only
        bool   canReply;     // the inline-reply field is available on visible expanded children too
    };
    inline constexpr SRowStyle ROW_SINGLE{ROW_ICON, true, true, true};
    inline constexpr SRowStyle ROW_CHILD{CHILD_ICON, false, false, true};

    // Lays out (and outside the warm paints) one row; returns its height and
    // fills the card's hit boxes. `more` records whether the Pixel pill can
    // reveal hidden content.
    double renderRow(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box, bool open, bool more, const SRowStyle& ST, SCard& card, bool child);
    // the same code with a context that draws nothing — the layout ruler
    double measureRow(const SPaint& P, const SType& T, const SP<SNotif>& N, double w, bool open, const SRowStyle& ST, bool child = false);

    // the three things the placement pass can put in a slot; each paints its
    // own fill and pushes its own hit card
    void   paintSingle(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box, bool open, bool more);
    void   paintDigest(const SPaint& P, const SType& T, const SDisp& D, const CBox& box);
    void   paintGroup(const SPaint& P, const SType& T, const SDisp& D, const CBox& box, const std::vector<double>& childH);
    // a snoozed card's undo row, in the slot the card held
    void   paintSnoozeRow(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box);

    // ---- the manage panel: what Android's long-press holds, as a row state ----
    //
    // A long press turns a row into this instead of opening a floating menu. Same
    // ergonomics — full-width labelled targets, every verb named — with none
    // of a second surface's cost: no z-order, no outside
    // -click grab, no damage region of its own, and it rides the fold
    // machinery that already exists. Only one row is ever in this state.
    enum class eManageEntryKind : uint8_t {
        ALERTING,
        SNOOZE,
        SILENCE_TIMED, // submenu parent for timed silence options
    };

    struct SMenuEntry {
        eControlIcon          icon;
        std::string           label;
        std::string           description;
        eManageEntryKind      kind;
        Policy::eAlertingMode mode;
        int64_t               silenceSeconds = 0; // for SILENCE_TIMED: duration in seconds (0 = forever)
        bool                  selected = false;
    };
    std::vector<SMenuEntry> menuEntries(const SP<SNotif>& N, bool bundle = false);
    double                  managePanelH(const SP<SNotif>& N, const std::string& group = {});
    void                    paintManagePanel(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box, const std::string& group = {});

    double digestH(const SType& T, size_t count, double scale); // the folded bundle's height
    double groupHeadH();                                        // an expanded bundle's header row
    double snoozeRowH();                                        // the undo row: one line, fixed

    // ---- popups.cpp / center.cpp: the two surfaces ----

    // Ordinary banners yield to the shade. OSD-band cards render below an open
    // shade, using the same popup card geometry and a measured gap.
    double renderPopups(const SPaint& P, const SType& T, bool osdOnly = false, std::optional<double> startY = std::nullopt, bool measureOnly = false);
    void renderCenter(const SPaint& P, const SType& T);
    bool popupsAnimating(bool osdOnly = false); // any arrival spring still running
    bool centerAnimating(); // the open spring still running
    // center.cpp owns the fold/paging state; render.cpp resets it via setCenter

} // namespace NHyprnotify
