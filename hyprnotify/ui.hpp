#pragma once
// hyprnotify/ui.hpp — the surface machinery shared by the drawing units
// (popups.cpp, center.cpp, render.cpp) plus the v13 design tokens.
// Private to the plugin: the public module map lives in hyprnotify.hpp.
//
// "center" throughout the drawing code means the SHADE: one list of live
// cards, Android's notification shade.
//
// The texture rule (crash class 4) runs through everything here: cachedText
// builds only while the warm gate allows it, SPaint paints only outside the
// warm, and every glyph a draw needs must have been requested by the warm
// that preceded it — request textures UNCONDITIONALLY in layout code, gate
// only the painting on P.warm.
//
// v13 ("A·ink") — the 2025 Pixel notification posture (spec:
// docs/hyprnotify-v13-spec.md, runnable reference:
// docs/demos/hyprnotify-design-mixer-v13/): the 380px top-right panel sits
// flush under the 26px bar, cards are stroke-free tiles with 37px circular
// lead icons, conversations and bundles open into child (kid) rows, the
// footer is [history] [muted chip] [Clear all] [DND], and the long-press
// hold menu holds the importance choice + snooze. Where the spec and the
// approved demo disagree on a concrete value, the demo wins (ledger A-138).

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

    // control glyphs: the AOSP OSD marks (popups/OSD) plus the v13 hold-menu
    // and footer set (demo symbols, kept as cached strokes in paint.cpp)
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
        DND_BELL_GEAR, // the v13 footer DND mark (the bell with the gear)
        SNOOZE,
        PRIORITY,
        NOTIFICATION_ALERT,
        NOTIFICATION_SILENT,
        EXPAND_MORE,
        EXPAND_LESS,
        HISTORY, // the footer history pill
        GEAR,    // a generic settings mark
        SEND,    // the reply field's paper plane
        MEDIA,   // the collapsed preview's media glyph
        CHECK,   // a selected snooze option
    };

    // the small app glyph inside its identity cell (icons.cpp's fallbacks)
    inline constexpr double IDENTITY_GLYPH_RATIO = 24.0 / 40.0;

    // ---- pixel-parity geometry (the 2026-08-18 ROM captures; ledger A-141).
    // Cards span the island width minus an 8px inset each side; the island
    // keeps a 16px margin from the screen edge. Card gaps are tight (8px).

    inline constexpr double EDGE        = 16;  // panel margin from the screen edge
    inline constexpr double CENTER_W    = 380; // panel width
    inline constexpr double PANEL_PAD   = 8;   // the panel's all-side padding
    inline constexpr double STACK_GAP   = 8;   // card gap

    // card interior (card-relative)
    inline constexpr double CARD_MIN_H      = 72; // the 72dp minimum
    inline constexpr double CARD_PADT       = 12;
    inline constexpr double CARD_PADB       = 12;
    inline constexpr double CARD_PADB_ACTS  = 3; // expanded card with a visible action row
    inline constexpr double CARD_ICON_X     = 16;
    inline constexpr double CARD_ICON_D     = 48; // the ~48px avatar circle
    inline constexpr double CARD_ICON_GAP   = 12;
    inline constexpr double CARD_TEXT_X     = CARD_ICON_X + CARD_ICON_D + CARD_ICON_GAP; // 76
    inline constexpr double CARD_TEXT_INSET = 15;
    inline constexpr double KICK_MIN_H      = 16;
    inline constexpr double KICK_RIGHT      = 46; // room for the expand chip
    inline constexpr double KICK_RIGHT_COUNT = 60; // the count pill is wider
    inline constexpr double TITLE_MT        = 7;
    inline constexpr double BODY_MT         = 8;
    inline constexpr double PREV_MT         = 6; // collapsed conversation/bundle preview
    inline constexpr double PREV_GAP        = 2; // between the two preview lines
    inline constexpr double PREV_ELGAP      = 6; // between the mini avatar and its text
    inline constexpr double MINI_D          = 15; // the preview mini avatar
    inline constexpr double MSG_MT          = 4;  // messages below the kid header
    inline constexpr double MSG_GAP         = 1;  // between message lines
    inline constexpr double ACTS_MT         = 2;  // the action row below the body
    inline constexpr double REPLY_MT        = 8;

    // action text buttons
    inline constexpr double BTN_H     = 44;
    inline constexpr double BTN_MIN_W = 52;
    inline constexpr double BTN_PADX  = 14;
    inline constexpr double BTN_GAP   = 8;
    inline constexpr double BTN_R     = 18;
    inline constexpr double BTN_ICON  = 18; // an action button's leading glyph
    inline constexpr double BTN_ICON_GAP = 6;

    // the expand affordance: a ~32px circular chevron button at the card's
    // top-right (the ROM's grey disc), or the count pill; the touch target
    // stays the 44px column
    inline constexpr double CHEV_BTN_D    = 32;
    inline constexpr double CHEV_BTN_X    = 16; // inset from the card's right edge
    inline constexpr double CHEV_BTN_Y    = 12;
    inline constexpr double CHEV_BTN_GLYPH = 16;
    inline constexpr double CHIP_X        = 15;
    inline constexpr double CHIP_Y        = 15;
    inline constexpr double CHIP_W        = 25;
    inline constexpr double CHIP_H        = 17;
    inline constexpr double CHIP_R        = 9; // stadium
    inline constexpr double CHIP_COUNT_H  = 18;
    inline constexpr double CHIP_COUNT_PL = 9;
    inline constexpr double CHIP_COUNT_PR = 8;
    inline constexpr double CHEV_D         = 12;
    inline constexpr double CHEV_D_COUNT   = 11;
    inline constexpr double CHEV_HIT_W     = 44;
    inline constexpr double CHEV_HIT_H     = 66; // collapsed
    inline constexpr double CHEV_HIT_H_E   = 52; // expanded
    inline constexpr double ICONCOL_W      = 44; // the icon column touch box
    inline constexpr double ICONCOL_H      = 66;

    // child (kid) rows
    inline constexpr double KID_PAD      = 9;
    inline constexpr double KICK2_LINE   = 15; // the small "who • time" line
    inline constexpr double KWHO2_MT     = 2;  // the level-2 name below the time
    inline constexpr double KACTS_MT     = 4;
    inline constexpr double KID_CHEV_HIT = 44; // square
    inline constexpr double KID_CHEV_Y   = 8;  // the chip's top inside the kid
    inline constexpr double HAIR_H       = 1.0; // the hairline between kids
    inline constexpr double REPLY_H      = 44;  // the armed reply field row
    inline constexpr double SEND_D       = 44;  // its send button

    // footer: three equal ~52px controls, full-pill radius
    inline constexpr double FOOTER_MT     = 16;
    inline constexpr double FOOTER_H      = 52;
    inline constexpr double FOOTER_GAP    = 8;
    inline constexpr double FOOTER_PILL_W = 52;
    inline constexpr double FOOTER_R      = 26; // the full pill
    inline constexpr double FOOTER_ICON   = 22;

    // the history panel
    inline constexpr double HIST_INSET  = 14; // from the panel edge
    inline constexpr double HIST_PT     = 10;
    inline constexpr double HIST_PX     = 12;
    inline constexpr double HIST_PB     = 12;
    inline constexpr double HIST_GAP    = 8;
    inline constexpr double HIST_ICON_D = 20;
    inline constexpr double HIST_ITEM_H = 20;

    // the empty state
    inline constexpr double EMPTY_PT = 30;
    inline constexpr double EMPTY_PB = 14;

    // the hold (long-press) menu
    inline constexpr double MENU_PAD       = 15;
    inline constexpr double MENU_HEAD_D    = 37;
    inline constexpr double MENU_HEAD_GAP  = 14;
    inline constexpr double MENU_ROWS_MT   = 16;
    inline constexpr double MENU_ROW_H     = 44;
    inline constexpr double MENU_ROW_H_SEL = 62; // the selected importance row (2-line)
    inline constexpr double MENU_ROW_GAP   = 8;
    inline constexpr double MENU_ICON_D    = 20;
    inline constexpr double MENU_ICON_GAP  = 11;
    inline constexpr double MENU_ROW_PADX  = 15;
    inline constexpr double MENU_ROW_PADY  = 8;
    inline constexpr double MENU_SNOOZE_MT = 8;
    // AOSP's fixed hold-menu choices (15m/30m/1h/2h), shared by the painter,
    // the snooze-index lookup, and the drainHits commit path
    inline constexpr int64_t SNOOZE_OPTS[4] = {900, 1800, 3600, 7200};
    inline constexpr double MENU_OPT_INSET = 46; // option rows indent under the head icon
    inline constexpr double MENU_FOOT_MT   = 21;
    inline constexpr double MENU_BTN_H     = 34;
    inline constexpr double MENU_BTN_PADX  = 14;
    inline constexpr double MENU_BTN_GAP   = 8;
    inline constexpr double MENU_BTN_R     = 18;
    inline constexpr double MENU_DISMISS_W = 88;
    inline constexpr double MENU_DONE_W    = 64;

    // the head-up notification
    inline constexpr double HUN_MIN_H = 106;

    // ---- input gesture constants ----

    // long-press threshold: the demo's Android hold duration
    inline constexpr int64_t LONG_PRESS_MS = 450;
    // past this distance it's a drag, not a hold
    inline constexpr double LONG_PRESS_MOVE = 8.0;

    // horizontal scroll swipe threshold: a deliberate swipe, not a nudge
    inline constexpr double SWIPE_THRESHOLD = 90.0;

    // drag-to-dismiss: more than this many px down dismisses (no live lift;
    // ledger A-138)
    inline constexpr double DRAG_DISMISS_PX = 90;

    // progress, body thumbnails and the popup column keep their v12 knobs —
    // v13 has no progress or thumbnail scenes, the model still carries them
    inline constexpr double PROGRESS_H = 5, PROGRESS_GAP = 8;
    inline constexpr double BODYIMG_H = 96, IMG_GAP = 6, IMG_ROW_GAP = 8;

    // The identity badge, as ratios of the avatar box it rides — AOSP's 2025
    // notification_2025_conversation_icon_container.xml: a 40dp avatar wearing
    // a 20dp badge, of which 16dp is the app glyph and 2dp on each side is the
    // rim, positioned so the GLYPH sits flush with the avatar's bottom-right
    // corner and only the rim protrudes.
    inline constexpr double BADGE_D = 20.0 / 40.0, BADGE_PROT = 2.0 / 40.0, BADGE_INSET = 2.0 / 20.0;

    // panel and card radii: the ROM draws both at 28dp circular arcs
    // (superellipse exponent 2); the `rounding` config remains the user's
    // override for BOTH, `rounding_power` the user's exponent override
    inline constexpr double PIXEL_SHADE_RADIUS    = 28;
    inline constexpr double PIXEL_INTERNAL_RADIUS = 18; // the footer/stadium joints

    // ---- the v13 color sets (common/theme.hpp owns the values) ----

    // `theme` (config: "glass" | "ink") selects the set; glass (the AOSP
    // frost) is the default and ink the opt-in opaque material. A color
    // config still holding its INK default follows the active set; an
    // explicit user color always wins.
    // The v13 spec's material is variant A · ink (the opaque near-black of
    // the AOSP captures; the glass set is the opt-in tray-menu frost).
    inline const Theme::SV13& v13set() {
        static const Theme::SV13 SINK = Theme::INK, SGLASS = Theme::GLASS;
        return cfg.theme && cfg.theme->value() == "glass" ? SGLASS : SINK;
    }
    inline CHyprColor v13col(const SP<Config::Values::CColorValue>& v, uint64_t ink, uint64_t set) {
        if (v && NHyprCommon::color(v).getAsHex() == ink)
            return CHyprColor((int)set);
        return NHyprCommon::color(v);
    }
    inline CHyprColor v13Panel() {
        const auto S = v13set();
        return v13col(cfg.colBg, Theme::INK.panel, S.panel);
    }
    inline CHyprColor v13Card() {
        const auto S = v13set();
        return v13col(cfg.colSurface, Theme::INK.card, S.card);
    }
    inline CHyprColor v13Raised() {
        const auto S = v13set();
        return v13col(cfg.colSurfaceHigh, Theme::INK.raised, S.raised);
    }
    inline CHyprColor v13RaisedH() {
        const auto S = v13set();
        return v13col(cfg.colState, Theme::INK.raisedH, S.raisedH);
    }
    inline CHyprColor v13Rim() {
        const auto S = v13set();
        return v13col(cfg.colFrame, Theme::INK.rim, S.rim);
    }
    inline CHyprColor v13On() {
        const auto S = v13set();
        return v13col(cfg.colFg, Theme::INK.on, S.on);
    }
    inline CHyprColor v13OnT() { // card titles
        const auto S = v13set();
        return v13col(cfg.colTitle, Theme::INK.on, S.on);
    }
    inline CHyprColor v13On82() {
        const auto S = v13set();
        return v13col(cfg.colKicker, Theme::INK.on82, S.on82);
    }
    inline CHyprColor v13On60() {
        const auto S = v13set();
        if (cfg.colKicker && NHyprCommon::color(cfg.colKicker).getAsHex() != Theme::INK.on82)
            return NHyprCommon::color(cfg.colKicker).modifyA(NHyprCommon::color(cfg.colKicker).a * 60.0 / 82.0);
        return CHyprColor((int)S.on60);
    }
    inline CHyprColor v13On40() {
        const auto S = v13set();
        if (cfg.colKicker && NHyprCommon::color(cfg.colKicker).getAsHex() != Theme::INK.on82)
            return NHyprCommon::color(cfg.colKicker).modifyA(NHyprCommon::color(cfg.colKicker).a * 40.0 / 82.0);
        return CHyprColor((int)S.on40);
    }
    inline CHyprColor v13Action() {
        const auto S = v13set();
        return v13col(cfg.colHighlight, Theme::INK.action, S.action);
    }
    inline CHyprColor v13Accent() { // DND-on fill, progress, priority dot
        if (cfg.colHighlight && NHyprCommon::color(cfg.colHighlight).getAsHex() != Theme::INK.action)
            return NHyprCommon::color(cfg.colHighlight);
        return CHyprColor(0xFF32D6FF);
    }
    inline CHyprColor v13Urgent() {
        return NHyprCommon::color(cfg.colUrgent);
    }
    inline CHyprColor v13Chip() {
        return CHyprColor((int)v13set().chip);
    }
    inline CHyprColor v13PillBg() {
        return CHyprColor((int)v13set().pillBg);
    }
    inline CHyprColor v13PillFg() {
        return CHyprColor((int)v13set().pillFg);
    }
    inline CHyprColor v13HeadPillBg() {
        return CHyprColor((int)v13set().headPillBg);
    }
    inline CHyprColor v13HeadPillFg() {
        return CHyprColor((int)v13set().headPillFg);
    }
    inline CHyprColor v13RowLine() {
        return CHyprColor((int)v13set().rowLine);
    }
    inline CHyprColor v13SelRow() {
        return CHyprColor((int)v13set().selRow);
    }
    inline CHyprColor v13SelRowB() {
        return CHyprColor((int)v13set().selRowB);
    }
    inline CHyprColor v13OnAccent() {
        return CHyprColor((int)v13set().onAccent);
    }

    // ---- paint.cpp: context, type scale, motion, config gates ----

    struct SType { // per-frame type roles, physical pt
        int header; // 11 — kicker, small labels
        int title;  // 15 — weight 500
        int body;   // 13
        int small;  // 11
        int action; // 13
        int bar;    // 13 — the footer
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
        void       lineH(const CBox& global, const CHyprColor& c) const; // a 1px horizontal hairline
    };

    double damageMargin(PHLMONITOR m);
    bool   liveBlurNeeded(); // only visible translucent glass requests the compositor blur path

    float rPow();
    int   rPanel(double scale);
    int   rRow(double scale);
    int   rJoint(double scale);

    // shared card recipes — the progress pill, the bounded icon column and
    // the mini avatars; conversation content may carry an application-identity
    // badge.
    bool hasLeadIcon(const SNotif& n);
    void paintProgress(const SPaint& P, double x, double y, double w, int pct, bool critical);
    // the pixel-parity circular chevron button (no count pill)
    void paintChevronButton(const SPaint& P, double x, double y, bool open, bool hov);
    void paintIconColumn(const SPaint& P, const SNotif& n, const CBox& cell, bool withBadge, float rp);
    void paintParticipantAvatar(const SPaint& P, const SNotif& n, std::string_view participant, const CBox& cell);
    // the 15px circular mini avatar of a collapsed conversation preview line
    void paintMiniAvatar(const SPaint& P, const SNotif& n, const std::string& sender, const CBox& cell);
    // a control glyph in a `physicalPx` box, the glyph filling glyphRatio of it
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

    // ---- layout helpers ----

    // text line height in physical px: std::max(pt, lround(pt * 1.4))
    inline int linePx(int pt) {
        return std::max(pt, (int)std::lround(pt * 1.4));
    }

    // true when the texture exists and is ready to paint
    inline bool texReady(const SCachedText* e) {
        return e && e->tex;
    }

    // texture readiness check: has a valid GPU texture ID
    inline bool texReady(const SP<ITexture>& t) {
        return t && t->m_texID != 0;
    }

    // small shared helpers
    std::string        hexOf(const CHyprColor& c);
    std::string        lastLine(const std::string& body); // the collapsed one-liner: the newest message
    std::string        ageString(const Time::steady_tp& t); // bucketed: "now", "5m", "2h", "3d"
    std::string        shortDuration(int64_t seconds);       // the same buckets, for the snooze labels
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

    // ---- row.cpp: the v13 card ----

    // a display item: one card, or a bundle of them (newest first)
    struct SDisp {
        std::vector<SP<SNotif>> items;
        std::string             key; // stable displayed declared/automatic group key
        bool                    classified = false;
    };

    // one card's content, regardless of where it lives:
    //  PLAIN  — a normal notification (title + body, actions when open)
    //  CONV   — a conversation: collapsed previews the newest messages, open
    //           it shows one kid per message (1:1) or per sender (group)
    //  BCHILD — a bundle child: the app icon is the lead, the kid rows are
    //           the bundle's cards (no per-kid actions)
    enum class eCardKind : uint8_t { PLAIN, CONV, BCHILD };

    // Lays out (and outside the warm paints) one card; pushes its hit boxes
    // (the body, the expand chip, the icon column, action buttons, kids).
    void paintCard(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box, bool open, bool more, eCardKind kind);
    // the same code with a context that draws nothing — the layout ruler
    double measureCard(const SPaint& P, const SType& T, const SP<SNotif>& N, double w, bool open, eCardKind kind);

    // a bundle: the lead card of D.items wearing D as its kids
    void   paintBundle(const SPaint& P, const SType& T, const SDisp& D, const CBox& box, bool open, const std::vector<double>& childH);
    double measureBundle(const SPaint& P, const SType& T, const SDisp& D, double w, bool open, const std::vector<double>& childH);

    // a snoozed card's undo row, in the slot the card held
    void   paintSnoozeRow(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box);
    double snoozeRowH();

    // the hold menu (long-press): the importance choice, the snooze section
    // and the Dismiss/Done pair, drawn in the card's own slot
    void   paintHoldMenu(const SPaint& P, const SType& T, const SP<SNotif>& N, const CBox& box, const std::string& groupKey);
    double holdMenuH();

    // the armed reply field: the borderless input + send button. It renders
    // inside the newest expanded kid of a conversation card (ledger A-138:
    // per-notification reply, a deliberate simplification of the demo's
    // per-kid fields).
    void   paintReplyField(const SPaint& P, const SType& T, const SP<SNotif>& N, double x, double y, double w, SCard& card);
    double replyFieldH();

    // collapsed-card preview lines, up to `cap`, newest first:
    //  conversations: [sender, text, media] — "Name • text" (the demo's
    //     group colon "Name: text" is normalized; ledger A-138)
    //  bundles:      [title, body]
    struct SPreviewLine {
        std::string a; // sender name, or the child's title
        std::string b; // the message / body text
        bool        media = false; // a media glyph leads the text
    };
    std::vector<SPreviewLine> previewLines(const SNotif& n, eCardKind kind, size_t cap);

    // ---- popups.cpp / center.cpp: the two surfaces ----

    // The HUN (banner) and the OSD cards below an open shade share the card
    // anatomy; ordinary banners yield to the shade.
    double renderPopups(const SPaint& P, const SType& T, bool osdOnly = false, std::optional<double> startY = std::nullopt, bool measureOnly = false);
    void   renderCenter(const SPaint& P, const SType& T);
    bool   popupsAnimating(bool osdOnly = false); // any arrival spring still running
    bool   centerAnimating(); // the open spring still running

} // namespace NHyprnotify
