#pragma once
// hyprbar — shared declarations between the plugin's translation units.
// The full picture lives at the top of main.cpp; per-module docs at the top
// of each unit:
//
//   util.cpp     tiny shared helpers, damage, clock/battery state
//   icons.cpp    icon loading + resolution (GTK theme dirs, PNG/SVG, caches)
//   tray.cpp     StatusNotifierWatcher/Host (sdbus-c++)
//   menu.cpp     the menu: dbusmenu for tray items + local client list, and
//                its own painting (Menu::render)
//   menubar.cpp  awesome's Mod+P launcher, and its own painting
//                (Menubar::render)
//   render.cpp   the bar itself (taglist, tasklist, tray, battery, clock),
//                the text cache, the SPaint context, the pass element
//   input.cpp    clicks, scrolls, pointer ownership
//   main.cpp     plugin glue: config, listeners, init/exit
//
// Each surface paints itself, next to the state it paints from: the bar's
// renderer hands a SPaint to Menubar::render/Menu::render rather than reaching
// into their internals.
//
// Everything lives in NHyprbar so no symbol can collide with another
// plugin's at dlopen time.

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/config/supplementary/executor/Executor.hpp>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-names.h>
#include <sdbus-c++/sdbus-c++.h>
#include <cairo/cairo.h>
#include <librsvg/rsvg.h>
#include <drm_fourcc.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>

// plugin-private header: the render types are used unqualified everywhere
using namespace Render;
using namespace Render::GL;

extern HANDLE PHANDLE;

namespace NHyprbar {

    // ---- config (defined in main.cpp, values arrive from theme.lua) ----

    struct SBarConfig {
        SP<Config::Values::CIntValue>    height;
        SP<Config::Values::CIntValue>    fontSize;
        SP<Config::Values::CIntValue>    traySpacing; // awesome's systray_icon_spacing
        SP<Config::Values::CStringValue> font;
        SP<Config::Values::CStringValue> fontIcon; // battery glyphs (awesome's font_icon)
        SP<Config::Values::CStringValue> terminal; // runs Terminal=true menubar entries
        SP<Config::Values::CColorValue>  colBg;
        SP<Config::Values::CColorValue>  colFg;          // normal text: tags, tasks, clock, battery
        SP<Config::Values::CColorValue>  colMuted;       // tray letter fallback
        SP<Config::Values::CColorValue>  colFocus;       // selected menubar entry fg (awesome fg_focus)
        SP<Config::Values::CColorValue>  colActive;      // active tag / focused task fg
        SP<Config::Values::CColorValue>  colActiveBg;    // active tag bg
        SP<Config::Values::CColorValue>  colEmpty;       // disabled/placeholder text
        SP<Config::Values::CColorValue>  colUrgent;      // urgent fg
        SP<Config::Values::CColorValue>  colUrgentBg;    // urgent bg (awesome bg_urgent)
        SP<Config::Values::CColorValue>  colSquareSel;   // taglist square: tag holds the focused window
        SP<Config::Values::CColorValue>  colSquareUnsel; // taglist square: occupied tag
    };
    extern SBarConfig cfg;

    // ---- util.cpp ----

    extern std::string clockText, batteryText, batteryGlyphText;

    double             barHeight();
    void               damageBars(); // covers the menubar's prompt strip while it's open
    std::string        lower(std::string s);
    CHyprColor         color(const SP<Config::Values::CColorValue>& v);
    void               findBattery();
    bool               refreshTexts(); // -> true when the clock/battery text changed

    // awesome's tasklist text: "⌃"/"+"/"✈" state markers, then the title.
    // Fills a caller-owned buffer: it runs per task per frame.
    void taskLabel(const PHLWINDOW& w, std::string& out);

    // Is this window a task of WS? By workspace ID, NEVER by pointer: while a
    // window closes, the monitor's active workspace and the windows' can
    // briefly be two different objects sharing one id, and a pointer test then
    // matches nothing — the whole tasklist blanked for that frame.
    bool isTaskOn(const PHLWINDOW& w, const PHLWORKSPACE& ws);

    // tasklist order: awesome lists clients in ARRIVAL order, stable across
    // raises — windowState()'s list is the Z-order, so the bar keeps its own
    // sequence
    extern std::unordered_map<void*, uint64_t> winSeq;
    extern uint64_t                            winSeqNext;

    // ---- the texture rule (render.cpp explains the why) ----
    //
    // A texture cannot be painted by the frame that created it, and creating
    // one mid-draw silently swallows every later draw in the same element. So
    // textures are built ONLY by the warm pass, one frame ahead, from the event
    // loop. These two flags make that structural rather than a thing every call
    // site has to remember:
    extern bool warming;  // warmBars() is building; the ONLY time a texture may be created
    extern bool texStale; // a draw ran ahead of the screen (texture never warmed, or labels
                          // flipped under a scissored repaint) -> warm + repaint

    // Build every texture the next frame will paint. Safe to call from
    // anywhere: it no-ops inside a render, which is exactly where building
    // would break. A monitor scopes the walk to it; a scoped warm marks what
    // it touches but never advances or evicts the cache generation.
    void warmBars(PHLMONITOR only = nullptr);

    // The bar's content changed: build its textures now (we are in the event
    // loop, not a render) and damage the strip. Callers that run before the
    // state settles want main.cpp's deferred path instead.
    inline void barChanged() {
        warmBars();
        damageBars();
    }

    // awesome's awful.layout.inc: cycle the focused monitor's active
    // workspace through the layout registry (render.cpp — a single entry
    // until other layouts get implemented; the bar only carries the state).
    void layoutInc(int dir);

    // ---- icons.cpp ----

    SP<ITexture> loadPng(const std::string& path);
    SP<ITexture> loadPngBytes(const std::vector<uint8_t>& data); // dbusmenu icon-data blobs
    std::string  resolveIconPath(const std::string& name, const std::string& extraDir = "");
    void         buildIconDirs();
    SP<ITexture> appIcon(const std::string& klass);                           // window class -> texture
    SP<ITexture> namedIcon(const std::string& name);                          // icon name/path -> texture
    SP<ITexture> trayIcon(const std::string& name, const std::string& theme); // + the item's own theme dir
    void         iconsExit();

    // ---- tray.cpp ----

    namespace Tray {
        inline constexpr const char* SNI = "org.kde.StatusNotifierItem";

        struct SItem {
            std::string                    service, path;
            std::unique_ptr<sdbus::IProxy> proxy;
            std::string                    iconName, themePath;
            std::string                    menuPath; // dbusmenu object path, "" = none
            std::string                    status;   // SNI Status: Passive hides the icon, NeedsAttention swaps the icon set
            bool                           itemIsMenu = false;
            std::vector<uint8_t>           pixels; // premultiplied BGRA (DRM ARGB8888)
            int                            pw = 0, ph = 0;
            SP<ITexture>                   tex;
            bool                           dirty = true;
        };

        extern std::unique_ptr<sdbus::IConnection> conn;
        extern std::vector<SP<SItem>>              items;

        void                                       pollSoon(); // pull the next DBus poll tick close after a send
        void                                       init();
        void                                       exit();
        void                                       onServiceDropped(const std::string& service); // defined in menu.cpp
    }

    // ---- painting a surface (render.cpp) ----
    //
    // Handed to each surface's own renderer so the bar, the menubar strip and
    // the menu panels paint themselves next to their state instead of inside
    // one function reaching into all three. The helpers are no-ops during the
    // warm pass, so ONE layout serves both modes — see the texture rule.
    struct SHit;
    struct SPaint {
        PHLMONITOR         mon;
        std::vector<SHit>* hits  = nullptr; // clickable regions, appended as we go
        bool               warm  = false;
        double             scale = 1.0;
        CBox               mb;         // the monitor's logical box
        double             h  = 0;     // bar height
        int                pt = 0;     // text size in pt, already scaled

        CBox               toPhys(const CBox& global) const;               // global logical -> monitor physical
        void               rect(const CBox& global, const CHyprColor& c) const;
        void               tex(const SP<ITexture>& t, const CBox& physBox) const;  // pre-computed physical box
        void               texIn(const SP<ITexture>& t, const CBox& cell) const;   // centered in a logical cell
    };

    // Text -> cached GPU texture. Built ONLY by the warm pass; a miss during a
    // draw returns null rather than building (the texture rule).
    SP<ITexture> textTex(const std::string& text, const CHyprColor& col, int pt, int maxWidth = 0, const std::string& font = "");

    // ---- clickable regions, rebuilt each frame by render.cpp ----

    struct SHit {
        CBox box; // global logical
        enum : uint8_t {
            TAG,
            TASK,
            TRAY,
            LAYOUT
        } kind              = TAG;
        int             tag = 0;     // TAG: workspace id
        PHLWINDOWREF    window;      // TASK
        WP<Tray::SItem> tray;        // TRAY
        double          anchorX = 0; // menu anchor
        PHLMONITORREF   mon;
    };
    extern std::map<uint64_t, std::vector<SHit>> hitboxes; // per monitor id

    // ---- menu.cpp ----

    namespace Menu {
        void render(const SPaint& P); // menu.cpp — the open panels, cascade by cascade

        inline constexpr double ROWH = 24, SEPH = 8, PAD = 4, ARROWH = 16;

        // per-level rows sentinel indices for the scroll arrows of an overflowing panel
        inline constexpr int SCROLL_UP = -2, SCROLL_DOWN = -3;

        // dbusmenu toggle-type
        enum : uint8_t {
            TG_NONE = 0,
            TG_CHECK,
            TG_RADIO
        };

        struct SEntry {
            int32_t      id = 0;
            std::string  label;
            std::string  display; // label + toggle/submenu decorations, fixed at load
            bool         enabled = true, separator = false, submenu = false;
            uint8_t      toggle      = TG_NONE;
            int32_t      toggleState = 0;     // 0 off, 1 on, anything else indeterminate
            bool         alert       = false; // disposition warning/alert
            PHLWINDOWREF win;                 // client-list mode: the window this row jumps to
            SP<ITexture> icon;                // client-list app icon / dbusmenu icon-name or icon-data
        };

        // one open panel; a submenu cascades out as the next level, GTK-style
        struct SLevel {
            int32_t                           parentId  = 0;  // dbusmenu id whose children this shows (0 = root)
            int                               parentIdx = -1; // the row in the previous level this cascades from
            std::vector<SEntry>               entries;
            double                            width     = 0;  // measured at warm; 0 = remeasure
            int                               widthPt   = 0;
            int                               hover     = -1;
            int                               scrollTop = 0;     // first visible entry when overflowing
            int                               maxScroll = 0;     // set at render: the last scrollTop that still fills the panel
            bool                              overflow  = false; // set at render: taller than the screen, scrolls
            CBox                              box;               // global logical, set at render
            std::vector<std::pair<CBox, int>> rows;              // row box -> entry index, set at render
        };

        extern bool                isOpen;
        extern bool                isLocal; // client list, no dbus behind it
        extern std::vector<SLevel> levels;
        extern double              anchorX;
        extern PHLMONITORREF       mon;

        double                     levelHeight(const SLevel& l);
        void                       damageMenu();
        void                       close();
        void                       exit(); // close + tear the hover timer out of the event loop
        void                       openFor(SP<Tray::SItem> it, double ax, PHLMONITORREF m);
        void                       openClients(double ax, PHLMONITORREF m);
        void                       openSub(size_t level, int entryIdx); // cascade that row's children
        void                       closeDeeperThan(size_t level);
        void                       hoverIntent(size_t level, int row); // GTK's popup delay: open/close cascades
        void                       activate(const SEntry& en);         // leaf rows only
    }

    // ---- menubar.cpp ----

    namespace Menubar {
        void render(const SPaint& P); // menubar.cpp — the prompt strip below the bar

        struct SCategory {
            const char* name;
            const char* appType; // the .desktop Categories= token
            const char* icon;
        };
        extern const SCategory CATEGORIES[];
        extern const int       NCATS;

        struct SApp {
            std::string name, lname, exec, lexec, icon;
            int         category = -1; // index into CATEGORIES, -1 = none
            bool        terminal = false;
        };

        // the filtered list: categories, then apps, then the trailing
        // "Exec: <query>" entry that runs the raw typed text as a command
        struct SShown {
            int cat = -1, app = -1; // neither set = the Exec entry
        };

        extern std::vector<SApp>   apps;
        extern std::vector<SShown> shown;
        extern bool                isOpen;
        extern std::string         typed;
        extern size_t              cursor;     // byte offset into typed, always at a UTF-8 boundary
        extern int                 currentCat; // >= 0: drilled into CATEGORIES[i]
        extern int                 sel, first;
        extern PHLMONITORREF       mon;

        void                       open();
        void                       close();
        void                       toggleDeferred(); // hl.plugin.hyprbar.menubar(), deferred out of the call
        void                       onKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info);
        void                       exit();
    }

    // ---- render.cpp ----

    void onRenderStage(eRenderStage stage);
    void renderExit();

    // ---- input.cpp ----

    void onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info);
    void onMouseAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info);
    void onMouseMove(const Vector2D& pos, Event::SCallbackInfo& info);
    void releasePointer();
    void inputExit();

} // namespace NHyprbar
