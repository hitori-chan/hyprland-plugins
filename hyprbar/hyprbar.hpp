#pragma once
// hyprbar — shared declarations between the plugin's translation units.
// The full picture lives at the top of main.cpp; per-module docs at the top
// of each unit:
//
//   render.cpp    the bar's SKELETON: the strip, the texture cache, the
//                 SPaint context, the pass element, one window walk (SFrame)
//                 and the widget slots — awesome's wibox
//   taglist.cpp   the nine kanji tags               (widget)
//   tasklist.cpp  arrival order, markers, the middle (widget)
//   tray.cpp      StatusNotifierWatcher/Host (sdbus-c++) + its strip cells
//   battery.cpp   gauge state, alerts, Android's pill (widget)
//   clock.cpp     awesome's textclock               (widget)
//   layoutbox.cpp the per-tag layout registry       (widget)
//   icons.cpp     icon loading + resolution (GTK theme dirs, PNG/SVG, caches)
//   menu.cpp      the menu: dbusmenu for tray items + local client list, and
//                 its own painting (Menu::render)
//   menubar.cpp   awesome's Mod+P launcher, and its own painting
//                 (Menubar::render)
//   util.cpp      tiny shared helpers: geometry, damage, colors, strings
//   input.cpp     clicks, scrolls, pointer ownership — swallowing and
//                 deferral here, what a cell DOES with its widget
//   main.cpp      plugin glue: config, listeners, init/exit
//
// Each surface paints itself, next to the state it paints from: the skeleton
// hands a SPaint (and the widgets an SFrame) to everything that draws rather
// than reaching into internals.
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
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/config/supplementary/executor/Executor.hpp>

#include <linux/input-event-codes.h>
#include <poll.h>
#include <libudev.h>
#include <wayland-server-core.h>
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
#include <cstdlib>
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
        SP<Config::Values::CStringValue> terminal; // runs Terminal=true menubar entries
        SP<Config::Values::CColorValue>  colBg;
        SP<Config::Values::CColorValue>  colFg;          // normal text: tags, tasks, clock
        SP<Config::Values::CColorValue>  colMuted;       // tray letter fallback
        SP<Config::Values::CColorValue>  colFocus;       // selected menubar entry fg (awesome fg_focus)
        SP<Config::Values::CColorValue>  colActive;      // active tag / focused task fg
        SP<Config::Values::CColorValue>  colActiveBg;    // active tag bg
        SP<Config::Values::CColorValue>  colEmpty;       // disabled/placeholder text
        SP<Config::Values::CColorValue>  colUrgent;      // urgent fg
        SP<Config::Values::CColorValue>  colUrgentBg;    // urgent bg (awesome bg_urgent)
        SP<Config::Values::CColorValue>  colSquareSel;   // taglist square: tag holds the focused window
        SP<Config::Values::CColorValue>  colSquareUnsel; // taglist square: occupied tag
        SP<Config::Values::CColorValue>  colFrame;       // menu panel frame
        SP<Config::Values::CColorValue>  colCharging;    // battery fill charging/defending (Android's charging green)
        SP<Config::Values::CColorValue>  colLow;         // battery fill <= 20% (Android's error red)
        SP<Config::Values::CColorValue>  colSave;        // battery fill in power save (Android's warning yellow)
    };
    extern SBarConfig cfg;

    // ---- util.cpp ----

    double      barHeight();
    void        damageBars(); // covers the menubar's prompt strip while it's open
    std::string lower(std::string s);
    CHyprColor  color(const SP<Config::Values::CColorValue>& v);

    // Is this window a task of WS? By workspace ID, NEVER by pointer: while a
    // window closes, the monitor's active workspace and the windows' can
    // briefly be two different objects sharing one id, and a pointer test then
    // matches nothing — the whole tasklist blanked for that frame.
    bool isTaskOn(const PHLWINDOW& w, const PHLWORKSPACE& ws);

    // first UTF-8 character (never a split sequence), uppercased when ASCII —
    // the icon-cell letter fallback
    std::string letterOf(const std::string& s);

    // ---- widget state entry points (each in its widget's unit) ----

    namespace Clock {
        bool refresh(); // -> true when the text changed
        void exit();
    }

    namespace Battery {
        void init(); // find the gauge, first read, arm the udev watch
        bool refresh();
        void alerts(); // battery-watch.sh folded in: edge-triggered plug/low/critical
        void exit();
    }

    namespace Tasklist {
        // awesome lists clients in ARRIVAL order, stable across raises —
        // windowState()'s list is the Z-order, so the bar keeps its own
        // sequence, fed by the skeleton's window walk
        uint64_t seqOf(void* w);
        // awesome's tasklist text: "\u2303"/"+"/"\u2708" state markers, then the
        // title; fills a caller-owned buffer (it runs per task per frame)
        void label(const PHLWINDOW& w, std::string& out);
        void forget(void* w); // the window is gone
        void exit();

        // awesome's client.minimized — a per-window flag the compositor lacks.
        // minimize() hides the window (setHidden: unrendered, xdg-suspended, no
        // frame callbacks) and, if tiled, frees its layout slot; the window
        // stays on its workspace and keeps its tasklist row (drawn muted).
        // restore() reverses it in place. isMinimized gates isTaskOn (util.cpp)
        // so hidden-by-us windows still list, unlike swallowed ones.
        bool isMinimized(const PHLWINDOW& w);
        void minimize(const PHLWINDOW& w);
        void restore(const PHLWINDOW& w);
        void minimizeFocused(); // Mod+N: minimize the focused window
        void restoreLast();     // Mod+Ctrl+N: awful.client.restore — last minimized on a viewed tag
        // honor a client minimizing itself (xdg set_minimized, e.g. a CSD
        // minimize button) — attached per window from window.open
        void watchMinimize(const PHLWINDOW& w);
    }

    void layoutboxExit();

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

    // Some textures are resolved OUTSIDE the warm pass, from the event loop: a
    // dbusmenu icon-name arrives in a reply, a client-list row is built in a
    // deferred click. warming gates texture creation, but the real safety
    // condition is "not inside a render" (inRenderBar) — which these contexts
    // never are. This token grants the permission around such a resolve; the
    // caller damages after, so the new icon gets its own frame. Never
    // construct it inside a render.
    struct SWarmToken {
        bool prev = warming;
        SWarmToken() {
            warming = true;
        }
        ~SWarmToken() {
            warming = prev;
        }
    };

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
        // fire a desktop notification over the tray's connection (urgency
        // 0/1/2; timeoutMs 0 = the daemon's default — sticky for critical)
        void notify(const std::string& app, uint32_t replacesId, const std::string& icon, const std::string& summary, const std::string& body, uint8_t urgency, int32_t timeoutMs);
        void init();
        void exit();
        void onServiceDropped(const std::string& service); // defined in menu.cpp
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
        CBox               mb;           // the monitor's logical box
        double             h  = 0;       // bar height
        int                pt = 0;       // text size in pt, already scaled
        size_t*            fp = nullptr; // frame fingerprint: widgets whose drawn content
                                         // can change without damage fold a hash in here

        CBox toPhys(const CBox& global) const; // global logical -> monitor physical
        void rect(const CBox& global, const CHyprColor& c, int round = 0) const;
        void border(const CBox& global, const CHyprColor& c, int round, int sizePx) const; // frame ring: one call, not four rects
        void tex(const SP<ITexture>& t, const CBox& physBox) const;                        // pre-computed physical box
        void texIn(const SP<ITexture>& t, const CBox& cell) const;                         // centered in a logical cell
    };

    // Text -> cached GPU texture. Built ONLY by the warm pass; a miss during a
    // draw returns null rather than building (the texture rule).
    SP<ITexture> textTex(const std::string& text, const CHyprColor& col, int pt, int maxWidth = 0, const std::string& font = "");

    // ---- clickable regions, rebuilt each frame by render.cpp ----

    struct IWidget;
    struct SHit {
        CBox            box;              // global logical
        IWidget*        widget = nullptr; // owns what this cell does
        int             tag    = 0;       // taglist: workspace id
        PHLWINDOWREF    window;           // tasklist
        WP<Tray::SItem> tray;             // tray
        double          anchorX = 0;      // menu anchor (cell-fixed)
        double          clickX  = 0;      // where the press landed (input.cpp fills it)
        PHLMONITORREF   mon;
    };
    extern std::map<uint64_t, std::vector<SHit>> hitboxes; // per monitor id

    // ---- the widget model (awesome's wibar, compiled) ----
    //
    // The bar is a skeleton (render.cpp: the strip, the texture machinery,
    // the pass element, ONE walk of the window list) hosting widgets in
    // awesome's align layout: a left slot, the tasklist filling the middle,
    // a right slot. Each widget lives in its own unit NEXT TO the state it
    // paints from, and owns what its cells do on click and scroll; the
    // skeleton owns geometry, damage and the texture rule.

    // what the skeleton's single window walk found, shared by every widget
    struct SFrame {
        PHLWORKSPACE                                       ws;    // the monitor's active workspace
        PHLWINDOW                                          focus; // frame-scoped strong refs: SFrame never outlives renderBar
        WORKSPACEID                                        focusWs     = WORKSPACE_INVALID;
        bool                                               urgent[10]  = {}; // workspaces 1..9
        int                                                windows[10] = {};
        const std::vector<std::pair<uint64_t, PHLWINDOW>>* tasks       = nullptr; // this workspace's tasks, arrival order
        // the frame palette, fetched once — color() memoizes but still
        // hashes per call, and the taglist alone makes dozens
        CHyprColor fg, active, activeBg, urgentFg, urgentBg, squareSel, squareUnsel, minimized;
    };

    struct IWidget {
        virtual ~IWidget() = default;
        // width in logical px. Runs in both modes like draw — the warm pass
        // builds the textures the width depends on. The tasklist fills the
        // middle and returns 0 here.
        virtual double fit(const SPaint& P, const SFrame& F) = 0;
        // paint into the cell and push hits; the SPaint helpers no-op during
        // warm, so one layout serves both modes (the texture rule)
        virtual void draw(const SPaint& P, const SFrame& F, const CBox& box) = 0;
        // a click on one of this widget's hits — input.cpp owns swallowing
        // and defers this out of the input emission
        virtual void onHit(const SHit& h, uint32_t bit, bool super) {}
        // wheel. Step-widgets coalesce notches through input.cpp's single
        // deferred hop (axis events arrive several per dispatch, and a lone
        // overwritten doLaterLock loses steps)...
        virtual bool accumulatesScroll() const {
            return false;
        }
        virtual void onScrollSteps(int steps, PHLMONITOR mon) {}
        // ...anything else acts immediately and must be async-safe: never a
        // workspace/focus change inside the emission
        virtual void onScroll(const SHit& h, int dir) {}
    };

    // the widget instances, one per unit (function-local statics: no
    // cross-TU construction order, and a same-map reload re-enters clean)
    IWidget& taglistWidget();   // taglist.cpp
    IWidget& tasklistWidget();  // tasklist.cpp
    IWidget& trayWidget();      // tray.cpp
    IWidget& batteryWidget();   // battery.cpp
    IWidget& clockWidget();     // clock.cpp
    IWidget& layoutboxWidget(); // layoutbox.cpp

    // ---- menu.cpp ----

    namespace Menu {
        void                    render(const SPaint& P); // menu.cpp — the open panels, cascade by cascade

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
            double                            width     = 0; // measured at warm; 0 = remeasure
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
