// awesome/shell/shell.hpp — shared declarations between the shell module's
// translation units. The module is the port of the old hyprbar: the bar
// strip and its widgets, the menubar prompt, the dbusmenu renderer.
//
//   strip.cpp       the strip's SKELETON: the texture cache, the SPaint
//                   context, the canvas layer, one window walk (SFrame)
//                   and the widget slots — awesome's wibox
//   taglist.cpp     the nine kanji tags                 (widget)
//   tasklist.cpp    the state-marker labels and the middle (widget view —
//                   the STATE lives in the windows module)
//   tray.cpp        StatusNotifierWatcher/Host (sdbus-c++) + its strip cells
//   bell.cpp        the notification bell + badge, reading Notify::Model
//                   directly — no bus (widget)
//   battery.cpp     gauge state, alerts, Android's pill (widget)
//   clock.cpp       awesome's textclock                (widget)
//   icons.cpp       icon loading + resolution (GTK theme dirs, PNG/SVG)
//   menu.cpp        the menu: dbusmenu for tray items + local client list
//   menubar.cpp     awesome's Mod+P launcher
//   input.cpp       clicks, scrolls, pointer ownership
//   shell.cpp       the CModule: listeners, the pipeline face, the Lua API
//
// Each surface paints itself, next to the state it paints from: the
// skeleton hands a SPaint (and the widgets an SFrame) to everything that
// draws rather than reaching into internals.
//
// Everything lives in NAwesome::Shell so no symbol can collide with
// another module's at dlopen time.

#include "core/busclient.hpp"
#include "core/canvas.hpp"
#include "core/config.hpp"
#include "core/hop.hpp"
#include "core/queries.hpp"
#include "core/supervisor.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
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
#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace NAwesome::Shell {

    // the render types are used unqualified in every shell unit (the old
    // bar's header did the same, for the same reason)
    using Render::ITexture;
    using Render::GL::g_pHyprOpenGL;

    // config reads, through the schema (the old cfg.* struct is gone):
    // `color(cfg().getColor("plugin:awesome:shell:col_bg"))` et al.
    using NAwesome::color;
    using NAwesome::cfg;

    // ---- util ----

    double      barHeight();
    void        damageBars(); // covers the menubar's prompt strip while it's open
    void        barChanged(); // warm + damage: the bar's content changed, event loop only
    std::string lower(std::string s);

    // Is this window a task of WS? By workspace ID, NEVER by pointer: while a
    // window closes, the monitor's active workspace and the windows' can
    // briefly be two different objects sharing one id, and a pointer test
    // then matches nothing — the whole tasklist blanked for that frame.
    bool isTaskOn(const PHLWINDOW& w, const PHLWORKSPACE& ws);

    // ---- the texture rule (strip.cpp explains the why) ----
    //
    // A texture cannot be painted by the frame that created it, and creating
    // one mid-draw silently swallows every later draw in the same element. So
    // textures are built ONLY by the warm pass, one frame ahead, from the
    // event loop — the state machine is the canvas's warm gate.
    inline NAwesome::CWarmGate& warmGate() {
        return NAwesome::Canvas::inst().gate();
    }

    // resolve-outside-warm permission (see CWarmGate::SToken)
    struct SWarmToken : NAwesome::CWarmGate::SToken {
        SWarmToken() : SToken(NAwesome::Canvas::inst().gate()) {}
    };

    // Build every texture the next frame will paint. Safe to call from
    // anywhere: it no-ops inside a render, which is exactly where building
    // would break. A monitor scopes the walk to it; a scoped warm marks what
    // it touches but never advances or evicts the cache generation.
    void warmBars(PHLMONITOR only = nullptr);
    void stripInit(); // register the strip's canvas layer (the module's init)
    void stripExit(); // clear the strip's caches (the module's teardown)

    // ---- icons.cpp ----

    SP<ITexture> loadPng(const std::string& path);
    SP<ITexture> loadPngBytes(const std::vector<uint8_t>& data); // dbusmenu icon-data blobs
    std::string  resolveIconPath(const std::string& name, const std::string& extraDir = "");
    void         buildIconDirs();
    void         iconsInit();
    SP<ITexture> appIcon(const std::string& klass);
    SP<ITexture> namedIcon(const std::string& name);
    SP<ITexture> trayIcon(const std::string& name, const std::string& theme, const std::string& id = "");
    void         iconsReload(); // config reload: re-probe the dirs, drop every resolved icon
    void         iconsExit();

    // ---- tray.cpp ----

    namespace Tray {
        inline constexpr const char* SNI = "org.kde.StatusNotifierItem";

        struct SItem {
            std::string                    service, path;
            std::unique_ptr<sdbus::IProxy> proxy;
            bool                           active = true;
            uint64_t                       propertyRequest = 0;
            uint64_t                       statusRequest   = 0;
            uint64_t                       iconRequest     = 0;
            std::string                    id, iconName, themePath;
            std::string                    menuPath; // dbusmenu object path, "" = none
            std::string                    status;   // SNI Status: Passive hides the icon, NeedsAttention swaps the icon set
            bool                           itemIsMenu = false;
            std::vector<uint8_t>           pixels; // premultiplied BGRA (DRM ARGB8888)
            int                            pw = 0, ph = 0;
            SP<ITexture>                   tex;
            bool                           dirty = true;
        };

        extern NAwesome::CBusLink bus; // the tray's session-bus link (menu.cpp borrows the connection)
        extern std::vector<SP<SItem>> items;

        void pollSoon(); // pull the next DBus poll tick close after a send
        void post(std::function<void()> fn); // defer send work out of input/render callbacks
        void init();
        void exit();
        void onServiceDropped(const std::string& service); // defined in menu.cpp
    }

    // ---- bell.cpp: the bell reads the notify MODEL, not a bus ----
    namespace Bell {
        void init(); // register the badge hook (Notify::Model::badgeChangedHook)
        void exit();
    }

    // ---- battery.cpp ----
    namespace Battery {
        void init(); // find the gauge, first read, arm the udev watch
        bool refresh();
        void alerts(); // battery-watch.sh folded in: edge-triggered plug/low/critical
        void exit();
    }

    // ---- clock.cpp ----
    namespace Clock {
        bool refresh(); // -> true when the text changed
        void exit();
    }

    // ---- the shell's half of the tasklist: the VIEW text ----
    // The state (minimized, the arrival order) lives in the windows module;
    // the markers are presentation, so the label builder is here.
    namespace Tasklist {
        // awesome's tasklist text: "⌃"/"+"/"✈" state markers, then the title;
        // fills a caller-owned buffer (it runs per task per frame)
        void label(const PHLWINDOW& w, std::string& out);
    }

    // ---- painting a surface (strip.cpp) ----
    //
    // Handed to each surface's own renderer so the strip, the menubar strip
    // and the menu panels paint themselves next to their state instead of
    // inside one function reaching into all three. The helpers are no-ops
    // during the warm pass, so ONE layout serves both modes — the texture
    // rule. (The canvas keeps its own SPaint for the card surface; the shell
    // needs the SHit-carrying context, so this is the strip's own copy.)
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
        void rect(const CBox& global, const CHyprColor& c, int round = 0, float rp = 2.f) const;
        void glass(const CBox& global, const CHyprColor& c, int round = 0, float rp = 2.f) const;          // opaque fast path or configured translucent blur
        void border(const CBox& global, const CHyprColor& c, int round, int sizePx, float rp = 2.f) const; // frame ring: one call, not four rects
        void tex(const SP<ITexture>& t, const CBox& physBox) const;                       // pre-computed physical box
        void texIn(const SP<ITexture>& t, const CBox& cell) const;                        // centered in a logical cell, native size
        void texFit(const SP<ITexture>& t, const CBox& cell) const;                       // aspect-preserving contain, centered in a logical cell
    };

    // Text -> cached GPU texture. Built ONLY by the warm pass; a miss during a
    // draw returns null rather than building (the texture rule).
    SP<ITexture> textTex(const std::string& text, const CHyprColor& col, int pt, int maxWidth = 0, const std::string& font = "");

    // ---- clickable regions, rebuilt each frame by strip.cpp ----

    struct IWidget;
    struct SHit {
        CBox            box;              // global logical
        IWidget*        widget = nullptr; // owns what this cell does
        int             tag    = 0;       // taglist: workspace id
        PHLWINDOWREF    window;           // tasklist
        WP<Tray::SItem> tray;             // tray
        double          anchorX = 0;      // menu anchor (cell-fixed)
        double          clickX  = 0;      // where the press landed (input.cpp fills it)
        double          clickY  = 0;      // screen-coordinate hint for SNI activation
        PHLMONITORREF   mon;
    };
    extern std::map<uint64_t, std::vector<SHit>> hitboxes; // per monitor id

    // ---- the widget model (awesome's wibar, compiled) ----
    //
    // The bar is a skeleton (strip.cpp: the strip, the texture machinery, the
    // canvas layer, ONE walk of the window list) hosting widgets in awesome's
    // align layout: a left slot, the tasklist filling the middle, a right
    // slot. Each widget lives in its own unit NEXT TO the state it paints
    // from, and owns what its cells do on click and scroll; the skeleton
    // owns geometry, damage and the texture rule.

    // what the skeleton's single window walk found, shared by every widget
    struct SFrame {
        PHLWORKSPACE                                       ws;    // the monitor's active workspace
        PHLWINDOW                                          focus; // frame-scoped strong refs: SFrame never outlives renderBar
        uint32_t                                           focusWs     = 0;    // focused window's numbered workspace, 0 = none
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
        // the pointer entered or left this widget's cells. Called on the
        // CHANGE only, never per motion event, and always paired — a leave
        // arrives before the next widget's enter. Must be async-safe: this
        // runs inside the motion emission (no workspace/focus changes).
        virtual void onHover(bool in) {}
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
    IWidget& bellWidget();      // bell.cpp
    IWidget& batteryWidget();   // battery.cpp
    IWidget& clockWidget();     // clock.cpp

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
            double                            width     = 0; // measured at warm; 0 = remeasure
            int                               widthPt   = 0;
            uint64_t                          loadRequest = 0;
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
            std::string name, lname, exec, lexec, icon, desktopId;
            int         category = -1; // index into CATEGORIES, -1 = none
            bool        terminal = false, dbusActivatable = false;
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

        void                       init();
        void                       open();
        void                       close();
        void                       toggleDeferred(); // hl.plugin.awesome.menubar, deferred out of the call
        void                       onKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info);
        void                       exit();
    }

    // ---- input.cpp (the CModule forwards the pipeline to it) ----

    void onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info);
    void onMouseAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info);
    void onMouseMove(const Vector2D& pos, Event::SCallbackInfo& info);
    void onInputBlocked(); // the pipeline head's reset: locked or natively captured
    void releasePointer();
    void inputInit(); // the hover self-heal timer
    void inputExit();

} // namespace NAwesome::Shell
