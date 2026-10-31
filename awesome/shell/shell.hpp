// awesome/shell/shell.hpp — shared declarations between the shell module's
// translation units. The module: the strip and its widgets, the menubar
// prompt, the dbusmenu renderer.
//
//   strip.cpp       the strip's SKELETON: the texture cache, the paint
//                   context (paintFor), the canvas layer, one window walk (SFrame)
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
#include "core/text.hpp"

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
    // `color(cfg().getColor("plugin:awesome:theme:col_glass"))` et al.
    using NAwesome::color;
    using NAwesome::cfg;

    // ---- util ----

    double      barHeight();
    void        damageBars(); // covers the menubar's prompt strip while it's open
    void        barChanged(); // warm + damage: the bar's content changed, event loop only
    void        titleChanged(); // a visible title: the strip alone, rate-limited

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
    void warmPopups(PHLMONITOR only = nullptr); // the menus, the launcher's list, the battery note: synchronous
    void promptChanged(); // the launcher's prompt band only (see strip.cpp)
    void menuHoverChanged(); // the open menu's panels only (see strip.cpp)
    // the menu's hover and cascade intent from a pointer position: motion,
    // and a layout reply that moved the rows under a resting pointer
    void menuHoverAt(const Vector2D& pos);
    void stripInit(); // register the strip's canvas layer (the module's init)
    void stripExit(); // clear the strip's caches (the module's teardown)

    // ---- icons.cpp ----

    // Every icon cell — tasks, tray, menu rows, the launcher — is this many
    // logical px; icons.cpp rasterizes at its device size (iconDevicePx), so
    // a texture lands 1:1 instead of being shrunk by the GPU (aliased).
    inline constexpr double ICON_CELL = 16;
    int                     iconDevicePx(); // ICON_CELL at the densest monitor's scale

    SP<ITexture> loadPng(const std::string& path);
    SP<ITexture> loadPngBytes(const std::vector<uint8_t>& data); // dbusmenu icon-data blobs
    std::string  resolveIconPath(const std::string& name, const std::string& extraDir = "");
    void         iconsInit();
    void         dropStaleTint(); // the bar's warm: a new tint or device size empties the icon caches
    SP<ITexture> appIcon(const std::string& klass);
    SP<ITexture> namedIcon(const std::string& name);
    SP<ITexture> trayIcon(const std::string& name, const std::string& theme, const std::string& id = "");
    bool         iconsReload(); // config reload: true = the icon theme switched, every resolved icon dropped
    void         iconsExit();

    // ---- tray.cpp ----

    namespace Tray {
        inline constexpr const char* SNI = "org.kde.StatusNotifierItem";

        struct SItem {
            std::string                    service, path;
            std::string                    owner; // the unique bus name its signals come from
            std::unique_ptr<sdbus::IProxy> proxy;
            bool                           active = true;
            uint64_t                       propertyRequest = 0;
            uint64_t                       statusRequest   = 0;
            uint64_t                       iconRequest     = 0;
            bool                           iconFetching = false, iconAgain = false; // one icon fetch in flight; a storm coalesces
            bool                           themeFetching = false, themeAgain = false; // the same for NewIconThemePath
            bool                           menuFetching = false, menuAgain = false;   // and NewMenu
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
        void onMenuSignal(sdbus::Message& msg);             // defined in menu.cpp: a dbusmenu signal from any client
    }

    // ---- bell.cpp: the bell reads the notify MODEL, not a bus ----
    namespace Bell {
        void init(); // register the badge hook (Notify::Model::badgeChangedHook)
        void exit();
    }

    // ---- battery.cpp ----
    struct SBarHover;
    namespace Battery {
        void init(); // find the gauge, first read, arm the udev watch
        bool refresh();
        void alerts(); // battery-watch.sh folded in: edge-triggered plug/low/critical
        void exit();
        // the note: the pill's state in words after the pointer rests on it
        void                hover(const SBarHover& h);            // the bar's hover moved (input.cpp)
        bool                noteWanted(const PHLMONITOR& mon);    // it shows on this monitor (strip.cpp adds the bar's own conditions)
        std::optional<CBox> noteReach(const PHLMONITOR& mon);     // what it can cover while it shows, global logical
        void                noteLaidOut(const PHLMONITOR& mon, bool on); // the popup warm's verdict: a note leaving damages its box
        void                noteChanged();                        // a sample landed: its words follow
        std::string         noteDescribe(); // the gate's: the words showing, "" none (defined in its build only)
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
        // awesome's tasklist text: "⌃"/"+" state markers, then the title;
        // fills a caller-owned buffer (it runs per task per frame)
        void label(const PHLWINDOW& w, std::string& out);
    }

    // ---- painting a surface ----
    //
    // Handed to each surface's own renderer so the strip, the menubar strip
    // and the menu panels paint themselves next to their state instead of
    // inside one function reaching into all three. The core context's
    // primitives no-op during the warm pass, so ONE layout serves both
    // modes — the texture rule. The shell adds its hit cells and metrics.
    struct SHit;
    struct SPaint : NAwesome::SPaint {
        std::vector<SHit>* hits = nullptr; // clickable regions, appended as we go
        double             h    = 0;       // bar height
        int                pt   = 0;       // text size in pt, already scaled
        size_t*            fp   = nullptr; // frame fingerprint: widgets whose drawn content
                                           // can change without damage fold a hash in here
    };
    // the bar's metrics on this monitor; null rctx = a warm
    SPaint paintFor(PHLMONITOR mon, Render::CRenderContext* rctx);
    namespace Battery {
        void renderNote(const SPaint& P); // the popup layer's: composes it (warm) and paints it
    }

    // Text -> cached GPU texture. Built ONLY by the warm pass; a miss during a
    // draw returns null rather than building (the texture rule).
    SP<ITexture> textTex(const std::string& text, const CHyprColor& col, int pt, int maxWidth = 0, const std::string& font = "", int weight = 400);
    // a label's ink width, logical px
    inline double textInkW(const SP<ITexture>& t, double scale) {
        return t ? t->m_size.x / scale : 0;
    }

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
    void forgetMonitor(uint64_t id); // its hit cells and layout memos (strip.cpp)

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
        bool                                               urgent[10]  = {}; // workspaces 1..9
        int                                                windows[10] = {};
        const std::vector<std::pair<uint64_t, PHLWINDOW>>* tasks       = nullptr; // this workspace's tasks, arrival order
        // the frame palette, fetched once — color() memoizes but still
        // hashes per call, and the taglist alone makes dozens
        CHyprColor fg, active, urgentFg, empty;
        CHyprColor activeHover, urgentHover; // lifted(): the accent and red under the pointer
    };

    // The bar cell under the pointer (input.cpp tracks it): its widget
    // paints a hover ground there. Damage only — a ground is a rect, so the
    // warmed texture set never changes with it (crash class 4).
    struct SBarHover {
        MONITORID      mon    = MONITOR_INVALID;
        const IWidget* widget = nullptr;
        int            tag    = 0;
        const void*    window = nullptr;
        CBox           box; // global logical, for the damage
        bool           operator==(const SBarHover& o) const {
            return mon == o.mon && widget == o.widget && tag == o.tag && window == o.window;
        }
    };
    extern SBarHover barHover;

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
        // workspace/focus change inside the emission. steps: whole notches,
        // positive = up
        virtual void onScroll(const SHit& h, int steps) {}
    };

    // the widget instances, one per unit (function-local statics: no
    // cross-TU construction order, and a same-map reload re-enters clean)
    IWidget& taglistWidget();   // taglist.cpp
    std::string tagName(const PHLWORKSPACE& ws); // taglist.cpp: a workspace's tag ("一".."九", a number past them, "" special)
    IWidget& tasklistWidget();  // tasklist.cpp
    IWidget& trayWidget();      // tray.cpp
    IWidget& bellWidget();      // bell.cpp
    IWidget& controlsWidget();  // bell.cpp: the control center's door
    IWidget& batteryWidget();   // battery.cpp
    IWidget& clockWidget();     // clock.cpp

    // ---- menu.cpp ----

    namespace Menu {
        void render(const SPaint& P); // menu.cpp — the open panels, cascade by cascade

        // the Frost menu: 5 px of glass around 28 px rows, 9 px separators,
        // 22 px section headers
        inline constexpr double ROWH = 28, SEPH = 9, HEADH = 22, PAD = 5, ARROWH = 18, GAP = 4; // GAP: under the bar

        // how far a panel paints past its box: the frost drop, or the blur
        // its glass samples — every damage and bound grows by it
        inline double reach(const PHLMONITOR& m) {
            return (m ? std::ceil(m->m_scale) : 1.0) + 1.0 + std::max(NAwesome::blurRadius(), NAwesome::SPaint::frostReach(NAwesome::SPaint::eDepth::MENU));
        }

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
            std::string  display; // the label + toggle decorations, fixed at load
            bool         enabled = true, separator = false, submenu = false;
            uint8_t      toggle      = TG_NONE;
            int32_t      toggleState = 0;     // 0 off, 1 on, anything else indeterminate
            bool         alert       = false; // disposition warning/alert
            std::string  tag;                 // client-list mode: its workspace's tag, a right-hand column
            // a local menu's section title: quiet, never lit, never hit
            bool         header = false;
            // a local menu's right-hand words: what the row is (the quiet
            // ink), and what a click on it does, in the accent while lit
            std::string  detail, hint;
            bool         current = false;     // client-list mode: the focused window (accent, as its task)
            bool         dim     = false;     // client-list mode: minimized (dim, as its task)
            SP<ITexture> icon;                // client-list app icon / dbusmenu icon-name or icon-data
            // an icon a budget deferred (a theme walk, a decode): the name
            // (or the app id, iconOfApp) the menu's warm asks for again
            std::string iconRetry;
            bool        iconOfApp = false;
            // a local menu's row (no dbusmenu behind it): what a click runs
            // once the menu closed, or the rows its cascade shows
            std::function<void()> action;
            std::vector<SEntry>   children;
        };

        inline double rowHeight(const SEntry& e) {
            return e.separator ? SEPH : e.header ? HEADH : ROWH;
        }

        // one open panel; a submenu cascades out as the next level, GTK-style
        struct SLevel {
            int32_t                           parentId  = 0;  // dbusmenu id whose children this shows (0 = root)
            int                               parentIdx = -1; // the row in the previous level this cascades from
            std::vector<SEntry>               entries;
            double                            width     = 0; // measured at warm; 0 = remeasure
            int                               widthPt   = 0;
            uint64_t                          loadRequest = 0;
            // one GetLayout in flight per level: an update storm (a scan
            // flipping rows every few ms) re-requests once when it lands
            bool                              loading     = false;
            bool                              reloadAgain = false;
            // what an empty level's one row says: "…" until a reply lands,
            // then "(empty)" — or "(unavailable)" when GetLayout failed
            enum eLoad : uint8_t {
                LOAD_PENDING,
                LOAD_DONE,
                LOAD_FAILED,
            } load = LOAD_PENDING;
            int                               hover     = -1;
            double                            fixedWidth = 0;     // a local list's own width (the client list's); 0 = measured
            int                               scrollTop = 0;     // first visible entry when overflowing
            int                               maxScroll = 0;     // set at render: the last scrollTop that still fills the panel
            bool                              overflow  = false; // set at render: taller than the screen, scrolls
            CBox                              box;               // global logical, set at render
            std::vector<std::pair<CBox, int>> rows;              // row box -> entry index, set at render
        };

        extern bool                isOpen;
        extern bool                isLocal; // built by the plugin (the client list, the bell's options): no dbus behind it
        extern const void*         opener;  // the bar widget a local menu hangs off (its door sits on the pill while open), or null
        extern std::vector<SLevel> levels;
        extern double              anchorX;
        extern PHLMONITORREF       mon;

        double                     levelHeight(const SLevel& l);
        void                       damageMenu();
        void                       damagePanels(); // the open panels, frame ring included, and the ones they replaced
        void                       close();
        void                       exit(); // close + tear the hover timer out of the event loop
        void                       openFor(SP<Tray::SItem> it, double ax, PHLMONITORREF m);
        void                       openClients(double ax, PHLMONITORREF m);
        // a menu the plugin builds: rows with actions and cascades, at the
        // click; width 0 measures the rows; by: the door it hangs off
        void                       openLocal(std::vector<SEntry> rows, double ax, PHLMONITORREF m, double width = 0, const void* by = nullptr);
        void                       openSub(size_t level, int entryIdx); // cascade that row's children
        void                       closeDeeperThan(size_t level);
        void                       hoverIntent(size_t level, int row); // GTK's popup delay: open/close cascades
        void                       activate(const SEntry& en);         // leaf rows only
        bool                       onKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info); // Esc closes; true = taken
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
    void inputExit();

} // namespace NAwesome::Shell
