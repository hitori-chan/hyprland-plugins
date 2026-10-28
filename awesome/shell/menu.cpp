// awesome/shell/menu.cpp — the menu renderer: dbusmenu for tray items, local mode for the client list

#include "../core/icons.hpp" // iconDeferred: a budget-deferred icon is retried
#include "../core/glyph.hpp"
#include "../core/keys.hpp"
#include "../core/theme.hpp"
#include "shell/shell.hpp"

#include "../windows/state.hpp"

#include <hyprland/src/managers/input/InputManager.hpp>

namespace NAwesome::Shell {

    // ---- dbusmenu: a native menu renderer for tray items ----

    namespace Menu {
        constexpr const char*                 DBUSMENU = "com.canonical.dbusmenu";
        // A tray menu is untrusted D-Bus UI state. Keep one pathological item
        // from consuming the compositor's retained menu memory or building an
        // arbitrarily deep cascade.
        constexpr size_t                      MAX_LEVELS          = 16;
        constexpr size_t                      MAX_ENTRIES         = 512;
        constexpr size_t                      MAX_TOTAL_ENTRIES   = 4096;
        constexpr size_t                      MAX_PROPERTY_UPDATES = 1024;
        constexpr size_t                      MAX_LABEL_BYTES     = 4096;
        constexpr size_t                      MAX_ICON_BYTES      = 2u << 20;
        constexpr size_t                      MAX_INLINE_DECODES  = 64; // per layout reply

        bool                                  isOpen  = false;
        bool                                  isLocal = false;
        static SP<Tray::SItem>                item;
        static std::shared_ptr<sdbus::IProxy>  proxy;
        std::vector<SLevel>                   levels;
        double                                anchorX = 0;
        PHLMONITORREF                         mon;
        static uint64_t                       menuSession = 0;
        static uint64_t                       requestSerial = 0;

        static void                           menuEvent(int32_t id, const char* type); // "opened"/"closed" notifications

        static bool sessionActive(uint64_t session) {
            return isOpen && !isLocal && proxy && session == menuSession;
        }

        static SLevel* findLevel(int32_t parentId) {
            for (auto it = levels.rbegin(); it != levels.rend(); ++it)
                if (it->parentId == parentId)
                    return &*it;
            return nullptr;
        }

        // Logical panel height, the same layout math renderBar uses.
        double levelHeight(const SLevel& l) {
            double h = PAD * 2;
            for (const auto& E : l.entries)
                h += rowHeight(E);
            if (l.entries.empty())
                h += ROWH; // the placeholder row: loading, empty or unavailable
            return h;
        }

        void damageMenu() {
            // Menu state changed -> new labels/colours -> new textures. Build
            // them here in the event loop; warmBars() no-ops if a render is on
            // the stack (renderBar closes the menu when a window goes
            // fullscreen), which is exactly when building would break. The warm
            // also lays the panels out, so the boxes below are current.
            warmBars(mon.lock());
            damagePanels();
        }

        void damagePanels() {
            if (!g_pHyprRenderer)
                return;
            // the rim rides outside L.box and the drop reaches further:
            // damaging only L.box left both stranded when a panel closed.
            // Cover them on every panel, old and new.
            const double MARGIN = reach(mon.lock());

            static std::vector<CBox> last; // the previous panels: a close/resize must damage them too
            for (auto B : last)
                g_pHyprRenderer->damageBox(B.expand(MARGIN));
            last.clear();
            for (const auto& L : levels) {
                g_pHyprRenderer->damageBox(CBox{L.box}.expand(MARGIN));
                last.push_back(L.box);
            }
        }

        // ---- the hover-intent timer: GTK's menu popup delay ----
        // One shared 225ms delay both opens a hovered submenu row and closes
        // the cascade when settling on a sibling — delaying the close is what
        // lets a diagonal move into the cascade survive.

        static SP<CEventLoopTimer> intent;
        static bool                intentArmed = false;
        static size_t              intentLevel = 0;
        static int                 intentIdx   = -1; // >= 0: open that row's cascade; -1: close deeper levels

        static void                disarm() {
            intentArmed = false;
        }

        static void applyIntent() {
            if (!isOpen || !intentArmed)
                return;
            intentArmed = false;
            if (intentLevel >= levels.size())
                return;
            auto& L = levels[intentLevel];
            if (intentIdx >= 0) { // still resting on the row? open its cascade
                if (L.hover == intentIdx)
                    openSub(intentLevel, intentIdx);
            } else { // still resting on a sibling? the open cascade goes
                if (L.hover >= 0 && !(intentLevel + 1 < levels.size() && levels[intentLevel + 1].parentIdx == L.hover))
                    closeDeeperThan(intentLevel);
            }
        }

        static void arm(size_t level, int idx) {
            if (intentArmed && intentLevel == level && intentIdx == idx)
                return; // same target: let the running delay finish
            intentLevel = level;
            intentIdx   = idx;
            intentArmed = true;
            if (!intent) {
                intent = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { applyIntent(); }, nullptr);
                g_pEventLoopManager->addTimer(intent);
            }
            intent->updateTimeout(std::chrono::milliseconds(225)); // GTK's gtk-menu-popup-delay default
        }

        // ---- arrow-strip autoscroll: GTK's arrows scroll while rested on ----

        static SP<CEventLoopTimer> glide;
        static size_t              glideLevel = 0;
        static int                 glideDir   = 0; // ±1 row per tick, 0 = idle

        static void glideStop();

        static void glideTick() {
            // off the strip (or the strip gone with an update): stop for
            // real, so resting on it again restarts the glide (a stale
            // direction made hoverIntent skip the restart)
            if (!isOpen || glideDir == 0 || glideLevel >= levels.size() || !levels[glideLevel].overflow ||
                levels[glideLevel].hover != (glideDir < 0 ? SCROLL_UP : SCROLL_DOWN)) {
                glideStop();
                return;
            }
            auto& L = levels[glideLevel];
            const int NEXT = std::clamp(L.scrollTop + glideDir, 0, L.maxScroll);
            if (NEXT != L.scrollTop) {
                L.scrollTop = NEXT;
                closeDeeperThan(glideLevel); // its cascade anchored to rows that just moved
                damageMenu();
            }
            glide->updateTimeout(std::chrono::milliseconds(50)); // keep watching — a live update may extend the list
        }

        static void glideStop() {
            if (glideDir == 0)
                return;
            glideDir = 0;
            if (glide)
                glide->updateTimeout(std::nullopt);
        }

        // called from onMouseMove with the pointer's (level, row)
        void hoverIntent(size_t level, int row) {
            // resting on a ▴/▾ strip glides the list, like holding GTK's arrows
            if (const int DIR = row == SCROLL_UP ? -1 : row == SCROLL_DOWN ? 1 : 0; DIR != 0 && level < levels.size()) {
                if (glideDir != DIR || glideLevel != level) {
                    glideLevel = level;
                    glideDir   = DIR;
                    if (!glide) {
                        glide = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { glideTick(); }, nullptr);
                        g_pEventLoopManager->addTimer(glide);
                    }
                    glide->updateTimeout(std::chrono::milliseconds(300)); // settle first, then glide
                }
            } else
                glideStop();

            if (!isOpen || level >= levels.size() || row < 0 || (size_t)row >= levels[level].entries.size()) {
                disarm();
                return;
            }
            const auto& E        = levels[level].entries[row];
            const bool  OPENHERE = level + 1 < levels.size() && levels[level + 1].parentIdx == row;
            if (E.submenu && E.enabled && !OPENHERE)
                arm(level, row); // open this row's cascade
            else if (!E.submenu && !E.separator && !E.header && level + 1 < levels.size())
                arm(level, -1); // resting on a sibling: close the cascade
            else
                disarm();
        }

        void closeDeeperThan(size_t level) {
            if (level + 1 >= levels.size())
                return;
            for (size_t i = level + 1; i < levels.size(); i++)
                menuEvent(levels[i].parentId, "closed");
            levels.resize(level + 1);
            damageMenu();
        }

        // The proxy's destructor sends its RemoveMatch calls: never from the
        // input or render pass that closed the menu — the tray link's own
        // turn drops it (its teardown clears the queue before the
        // connection; a refused post drops it here, as before).
        static void releaseProxy() {
            if (!proxy)
                return;
            auto P = std::move(proxy);
            proxy.reset();
            Tray::post([P]() mutable { P.reset(); });
        }

        void close() {
            ++menuSession; // invalidate every reply from the old owner/session
            if (!isOpen)
            {
                // a level appended under a close that ran mid-openSub (its
                // warm closed the menu: the lock, a fullscreen) must not
                // open the next menu as a phantom "…" root
                levels.clear();
                releaseProxy();
                item.reset();
                return;
            }
            isOpen  = false;
            isLocal = false;
            disarm();
            glideStop();
            for (const auto& L : levels)
                menuEvent(L.parentId, "closed");
            levels.clear();
            damageMenu();
            releaseProxy(); // also drops the LayoutUpdated/ItemsPropertiesUpdated subscriptions
            item.reset();
        }

        // Esc closes an open menu, its press swallowed (the release passes:
        // invariant 3). No other key is the menu's: it has no keyboard
        // navigation, and the prompt and the apps keep theirs; a modified
        // chord (Super+Esc) is a user bind passing through, as in the shade.
        bool onKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info) {
            if (!isOpen || info.cancelled || e.state != WL_KEYBOARD_KEY_STATE_PRESSED || NAwesome::sessionLocked() || NAwesome::nativeInputCaptureActive() ||
                NAwesome::Keys::exclusiveLayer())
                return false;
            const auto KB = NAwesome::Keys::keyboardFor(e.keycode);
            if (!KB || !KB->m_xkbState || xkb_state_key_get_one_sym(KB->m_xkbState, e.keycode + 8) != XKB_KEY_Escape)
                return false;
            for (const char* M : {XKB_MOD_NAME_CTRL, XKB_MOD_NAME_ALT, XKB_MOD_NAME_LOGO})
                if (xkb_state_mod_name_is_active(KB->m_xkbState, M, XKB_STATE_MODS_EFFECTIVE) > 0)
                    return false;
            close();
            info.cancelled = true;
            return true;
        }

        void exit() {
            close();
            for (auto* T : {&intent, &glide}) {
                if (*T && g_pEventLoopManager)
                    g_pEventLoopManager->removeTimer(*T);
                T->reset();
            }
        }

        // dbusmenu labels: "__" is an escaped literal underscore, a single
        // "_" marks the GTK mnemonic character (dropped — no keyboard here)
        static std::string cleanLabel(const std::string& s) {
            std::string out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size(); i++) {
                if (s[i] == '_') {
                    if (i + 1 < s.size() && s[i + 1] == '_') {
                        out += '_';
                        i++;
                    }
                    continue;
                }
                out += s[i];
            }
            return out;
        }

        // the spec's row notifications — apps may lazy-build on "opened"
        // rather than AboutToShow, and clean up on "closed"
        static void menuEvent(int32_t id, const char* type) {
            if (!proxy)
                return;
            const auto P    = proxy;
            const auto TYPE = std::string{type};
            Tray::post([P, id, TYPE]() {
                if (!P)
                    return;
                try {
                    P->callMethodAsync("Event")
                        .onInterface(DBUSMENU)
                        .withArguments(id, TYPE, sdbus::Variant{(int32_t)0}, (uint32_t)0)
                        .uponReplyInvoke([](std::optional<sdbus::Error>) {});
                    Tray::pollSoon();
                } catch (...) {} // dying bus: teardown is already pending
            });
        }

        static void loadLevel(int32_t parentId, uint64_t session);

        static void loadLevelNow(int32_t parentId, uint64_t session) {
            if (!sessionActive(session))
                return;
            using LayoutItem = sdbus::Struct<int32_t, std::map<std::string, sdbus::Variant>, std::vector<sdbus::Variant>>;
            auto* const level = findLevel(parentId);
            if (!level)
                return;
            if (level->loading) {
                level->reloadAgain = true;
                return;
            }
            const uint64_t REQUEST = ++requestSerial;
            level->loadRequest = REQUEST;
            level->loading     = true;
            try {
                proxy->callMethodAsync("GetLayout")
                    .onInterface(DBUSMENU)
                    .withArguments(parentId, (int32_t)1, std::vector<std::string>{})
                    .uponReplyInvoke([parentId, session, REQUEST](std::optional<sdbus::Error> e, uint32_t, LayoutItem root) {
                        if (!sessionActive(session))
                            return;
                        auto* const lvl = findLevel(parentId);
                        if (!lvl || lvl->loadRequest != REQUEST)
                            return; // that cascade closed while the reply was in flight
                        lvl->loading = false;
                        if (std::exchange(lvl->reloadAgain, false))
                            loadLevel(parentId, session); // the updates that came in meanwhile
                        if (e) {
                            // a level already shown keeps its rows; an empty
                            // one stops promising a load that never comes
                            if (lvl->entries.empty() && lvl->load != SLevel::LOAD_FAILED) {
                                lvl->load = SLevel::LOAD_FAILED;
                                damageMenu();
                            }
                            return;
                        }
                        lvl->load = SLevel::LOAD_DONE;
                        // the hovered row survives the update by id (nm-applet
                        // re-lays its menu every few seconds: the pill vanished
                        // under a resting pointer and its cascade never opened);
                        // a ▴/▾ strip stays hovered. scrollTop stays too: live
                        // updates must not yank a browsed list to its top. Once
                        // laid out, the pointer settles it: a re-sorted list
                        // (nm-applet's scan) moves the row away from it.
                        const int                    HOVER = lvl->hover;
                        const std::optional<int32_t> HOVER_ID =
                            HOVER >= 0 && (size_t)HOVER < lvl->entries.size() ? std::optional{lvl->entries[HOVER].id} : std::nullopt;
                        lvl->entries.clear();
                        lvl->hover = HOVER == SCROLL_UP || HOVER == SCROLL_DOWN ? HOVER : -1;
                        SWarmToken WARM; // we resolve icon-name textures here, in a DBus reply — not a render
                        size_t retained = 0;
                        for (const auto& L : levels)
                            if (&L != lvl)
                                retained += L.entries.size();
                        const size_t entryLimit = retained >= MAX_TOTAL_ENTRIES ? 0 : std::min(MAX_ENTRIES, MAX_TOTAL_ENTRIES - retained);
                        // inline icons decode on this thread: a reply buys
                        // a bounded number of them, the rest go iconless
                        size_t inlineDecodes = 0;
                        for (auto& CV : std::get<2>(root)) {
                            if (lvl->entries.size() >= entryLimit)
                                break;
                            try {
                                auto  c = CV.get<LayoutItem>();
                                auto& P = std::get<1>(c);

                                if (auto it = P.find("visible"); it != P.end() && !it->second.get<bool>())
                                    continue;

                                SEntry en;
                                en.id = std::get<0>(c);
                                if (auto it = P.find("type"); it != P.end() && it->second.get<std::string>() == "separator")
                                    en.separator = true;
                                if (auto it = P.find("label"); it != P.end()) {
                                    const auto LABEL = it->second.get<std::string>();
                                    if (LABEL.size() > MAX_LABEL_BYTES)
                                        continue;
                                    en.display = cleanLabel(LABEL); // a submenu's ▸ has its own column (render)
                                }
                                if (auto it = P.find("enabled"); it != P.end())
                                    en.enabled = it->second.get<bool>();
                                if (auto it = P.find("children-display"); it != P.end() && it->second.get<std::string>() == "submenu")
                                    en.submenu = true;
                                if (auto it = P.find("toggle-type"); it != P.end())
                                    en.toggle = it->second.get<std::string>() == "checkmark" ? TG_CHECK : it->second.get<std::string>() == "radio" ? TG_RADIO : TG_NONE;
                                if (auto it = P.find("toggle-state"); it != P.end())
                                    en.toggleState = it->second.get<int32_t>();
                                if (auto it = P.find("disposition"); it != P.end())
                                    en.alert = it->second.get<std::string>() == "warning" || it->second.get<std::string>() == "alert";
                                std::string iconName;
                                bool        deferred = false;
                                if (auto it = P.find("icon-name"); it != P.end()) {
                                    iconName = it->second.get<std::string>();
                                    if (iconName.size() <= MAX_LABEL_BYTES) {
                                        NAwesome::iconDeferred() = false;
                                        en.icon                  = namedIcon(iconName);
                                        deferred                 = !en.icon && NAwesome::iconDeferred();
                                    }
                                }
                                if (!en.icon && inlineDecodes < MAX_INLINE_DECODES) // nm-applet ships its signal-strength icons inline
                                    if (auto it = P.find("icon-data"); it != P.end())
                                        if (const auto DATA = it->second.get<std::vector<uint8_t>>(); DATA.size() <= MAX_ICON_BYTES) {
                                            inlineDecodes++;
                                            en.icon = loadPngBytes(DATA);
                                        }
                                if (!en.icon && deferred)
                                    en.iconRetry = std::move(iconName);
                                if (en.icon && en.icon->m_texID != 0 && en.toggle != TG_NONE && en.toggleState == 1)
                                    en.display = (en.toggle == TG_RADIO ? "● " : "✓ ") + en.display;
                                lvl->entries.push_back(std::move(en));
                            } catch (...) { continue; }
                        }
                        // dbusmenu providers can end a level on a separator or
                        // emit them in a row: nm-applet via appindicator appends
                        // one after "Edit Connections…" and its About row only
                        // exists in the GtkStatusIcon fallback, so the live menu
                        // ends on a line with nothing below it. The panel must
                        // never draw a leading, doubled, or trailing separator.
                        {
                            std::vector<SEntry> CLEAN;
                            CLEAN.reserve(lvl->entries.size());
                            bool pendingSep = false;
                            for (auto& E : lvl->entries) {
                                if (E.separator) {
                                    if (!CLEAN.empty())
                                        pendingSep = true; // a run collapses to one, emitted lazily
                                    continue;
                                }
                                if (pendingSep) {
                                    SEntry S{};
                                    S.separator = true;
                                    CLEAN.push_back(std::move(S));
                                    pendingSep = false;
                                }
                                CLEAN.push_back(std::move(E));
                            }
                            lvl->entries = std::move(CLEAN); // a pendingSep with no row after it dies here
                        }
                        lvl->width = 0;
                        const size_t K = (size_t)(lvl - levels.data());
                        if (HOVER_ID)
                            if (const auto IT = std::ranges::find_if(lvl->entries, [&](const SEntry& E) { return E.id == *HOVER_ID && !E.separator; }); IT != lvl->entries.end())
                                lvl->hover = (int)(IT - lvl->entries.begin());
                        // a pending cascade intent named the row by index
                        if (intentArmed && intentLevel == K && intentIdx >= 0) {
                            if (intentIdx == HOVER && lvl->hover >= 0)
                                intentIdx = lvl->hover;
                            else
                                disarm();
                        }
                        // the open cascade hangs off its parent row by INDEX:
                        // re-find that row by id (rows above it may have come
                        // or gone), or close the cascade with it
                        if (K + 1 < levels.size()) {
                            auto&      NEXT = levels[K + 1];
                            const auto IT   = std::ranges::find_if(lvl->entries, [&](const SEntry& E) { return E.id == NEXT.parentId && E.submenu; });
                            if (IT == lvl->entries.end())
                                closeDeeperThan(K);
                            else
                                NEXT.parentIdx = (int)(IT - lvl->entries.begin());
                        }
                        damageMenu();
                        if (g_pInputManager)
                            menuHoverAt(g_pInputManager->getMouseCoordsInternal());
                    });
                Tray::pollSoon(); // don't leave the layout reply waiting on a poll tick
            } catch (...) {} // dying bus: teardown is already pending
        }

        static void loadLevel(int32_t parentId, uint64_t session) {
            if (!sessionActive(session))
                return;
            Tray::post([parentId, session]() { loadLevelNow(parentId, session); });
        }

        static void aboutToShow(int32_t id, uint64_t session) {
            if (!sessionActive(session))
                return;
            Tray::post([id, session]() {
                if (!sessionActive(session))
                    return;
                try {
                    proxy->callMethodAsync("AboutToShow")
                        .onInterface(DBUSMENU)
                        .withArguments(id)
                        .uponReplyInvoke([id, session](std::optional<sdbus::Error>, bool) { loadLevel(id, session); });
                    Tray::pollSoon();
                } catch (...) {} // dying bus: teardown is already pending
            });
        }

        void openFor(SP<Tray::SItem> it, double ax, PHLMONITORREF m) {
            if (!it || !it->active || !it->proxy || it->menuPath.empty() || !Tray::bus.conn())
                return;
            close();
            const uint64_t SESSION = ++menuSession;
            item    = it;
            anchorX = ax;
            mon     = m;
            isOpen  = true;
            levels.emplace_back(); // the root panel (parentId 0)
            try {
                proxy.reset(sdbus::createProxy(*Tray::bus.conn(), sdbus::ServiceName{it->service}, sdbus::ObjectPath{it->menuPath}).release());
            } catch (...) {
                close();
                return;
            }

            // AboutToShow lets lazy apps (nm-applet) populate; load either way.
            menuEvent(0, "opened");
            aboutToShow(0, SESSION);
            loadLevel(0, SESSION);
            damageMenu();
        }

        // hover (after the popup delay) or a click on a submenu row: cascade
        // its children out into a new panel beside the parent, like the GTK
        // menus these were under X11.
        void openSub(size_t level, int entryIdx) {
            if (!isOpen || (!isLocal && !proxy) || level >= levels.size())
                return;
            if (levels.size() >= MAX_LEVELS)
                return;
            auto& L = levels[level];
            if (entryIdx < 0 || (size_t)entryIdx >= L.entries.size() || !L.entries[entryIdx].submenu || !L.entries[entryIdx].enabled)
                return;
            if (level + 1 < levels.size() && levels[level + 1].parentIdx == entryIdx)
                return;                                // already open
            const int32_t ID = L.entries[entryIdx].id; // before emplace_back can move the vector
            auto           CHILDREN = isLocal ? L.entries[entryIdx].children : std::vector<SEntry>{};
            const uint64_t SESSION = menuSession;
            disarm();
            closeDeeperThan(level);
            if (!isOpen || menuSession != SESSION)
                return; // its warm closed the menu (the lock, a fullscreen)
            auto& N     = levels.emplace_back();
            N.parentId  = ID;
            N.parentIdx = entryIdx;
            if (isLocal) { // its rows came with it
                N.entries = std::move(CHILDREN);
                N.load    = SLevel::LOAD_DONE;
                damageMenu();
                return;
            }
            menuEvent(ID, "opened");
            aboutToShow(ID, SESSION);
            loadLevel(ID, SESSION);
            damageMenu();
        }

        // a client-list row's jump: view its workspace, then focus + raise
        // (nothing when the window closed under the open menu)
        static void jumpTo(const PHLWINDOW& w) {
            if (!w || !w->mapped())
                return;
            if (w->m_workspace && !w->m_workspace->visible())
                std::ignore = Config::Actions::changeWorkspace(w->m_workspace);
            // a minimized target must be un-minimized, not focused while
            // hidden — restore() unhides, re-tiles, raises, focuses and
            // re-enters any fullscreen it held
            if (NAwesome::Windows::Tasklist::isMinimized(w))
                NAwesome::Windows::Tasklist::restore(w);
            else {
                Desktop::windowState()->raise(w);
                Desktop::focusState()->fullWindowFocus(w, Desktop::FOCUS_REASON_DISPATCH_FOCUSWINDOW, w->wlSurface()->resource());
            }
        }

        // right-click on a task, awesome's awful.menu.client_list: every mapped
        // window on every workspace, in the same stable arrival order; a click
        // jumps to it (view its workspace, then focus + raise).
        void openClients(double ax, PHLMONITORREF m) {
            close();
            static std::vector<std::pair<uint64_t, PHLWINDOW>> ws; // reused; main thread only
            ws.clear();
            for (const auto& W : Desktop::windowState()->windows()) {
                // minimized windows stay listed (awesome's client_list un-minimizes) —
                // mirror isTaskOn; only genuinely-hidden (swallowed) ones drop out
                if (NAwesome::isClient(W) && (!W->isHidden() || NAwesome::Windows::Tasklist::isMinimized(W)))
                    ws.emplace_back(NAwesome::winOrder().seqOf(W.get()), W);
            }
            if (ws.empty())
                return;
            std::sort(ws.begin(), ws.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            std::vector<SEntry> rows;
            SWarmToken WARM; // appIcon builds a texture, and we are in a deferred click, not a render
            const auto FOCUS = Desktop::focusState() ? Desktop::focusState()->window() : nullptr;
            for (const auto& [SEQ, W] : ws) {
                std::string lbl;
                Tasklist::label(W, lbl);
                NAwesome::iconDeferred() = false;
                const auto& APP          = W->metadata().appID();
                auto        ICON         = appIcon(APP);
                const bool  DEFERRED     = !ICON && NAwesome::iconDeferred();
                // a row reads as its task does: minimized wins over focus
                // (the focus fallback can rest on a hidden window)
                const bool MIN = NAwesome::Windows::Tasklist::isMinimized(W);
                rows.push_back({.display   = std::move(lbl),
                                .alert     = !MIN && W != FOCUS && (W->m_hints & Desktop::View::WINDOW_HINT_URGENT),
                                .tag       = tagName(W->m_workspace),
                                .current   = !MIN && W == FOCUS,
                                .dim       = MIN,
                                .icon      = std::move(ICON),
                                .iconRetry = DEFERRED ? APP : "",
                                .iconOfApp = true,
                                .action    = [REF = PHLWINDOWREF{W}]() { jumpTo(REF.lock()); }});
            }
            ws.clear(); // drop the strong refs now — rows hold weak ones
            openLocal(std::move(rows), ax, m, 250);
        }

        void openLocal(std::vector<SEntry> rows, double ax, PHLMONITORREF m, double width) {
            close();
            if (rows.empty())
                return;
            auto& L      = levels.emplace_back();
            L.entries    = std::move(rows);
            L.fixedWidth = width;
            L.load       = SLevel::LOAD_DONE;
            anchorX      = ax;
            mon          = m;
            isOpen       = true;
            isLocal      = true;
            damageMenu();
        }

        // leaf rows only — submenu rows cascade via openSub (hover or click)
        void activate(const SEntry& en) {
            if (isLocal) { // the row's own action, once the menu is gone
                const auto ACTION = en.action;
                close();
                if (ACTION)
                    ACTION();
                return;
            }
            if (!proxy)
                return;
            const auto P = proxy;
            Tray::post([P, id = en.id]() {
                if (!P)
                    return;
                try {
                    P->callMethodAsync("Event")
                        .onInterface(DBUSMENU)
                        .withArguments(id, std::string{"clicked"}, sdbus::Variant{(int32_t)0}, (uint32_t)0)
                        .uponReplyInvoke([](std::optional<sdbus::Error>) {});
                    Tray::pollSoon();
                } catch (...) {} // dying bus: teardown is already pending
            });
            close();
        }

        // The open panels, cascade by cascade.
        void render(const SPaint& PAINT) {
            if (!isOpen || mon.lock() != PAINT.mon)
                return;

            // one palette fetch per render: color() memoizes but still hashes per call
            const CHyprColor COLMENU = color(cfg().getColor("plugin:awesome:theme:col_glass")), COLFG = color(cfg().getColor("plugin:awesome:shell:col_fg")),
                             COLEMPTY = color(cfg().getColor("plugin:awesome:shell:col_empty")), COLURGENT = color(cfg().getColor("plugin:awesome:shell:col_urgent")),
                             COLFOCUS = color(cfg().getColor("plugin:awesome:shell:col_focus")), COLFRAME = color(cfg().getColor("plugin:awesome:theme:col_rim"));

            // the Frost menu: the glass at the menu radius, the row under the
            // pointer on the pill, hairline separators, glyph checks and
            // chevrons — the card menu's look (notify/menu.cpp)
            namespace Th       = NAwesome::Theme;
            namespace G        = NAwesome::Glyph;
            const int   RPANEL = (int)std::lround(Th::RAD_MENU * PAINT.scale);
            const float RP     = (float)Th::ROUNDING_POWER;
            const int   GLYPHPX = (int)std::lround(14 * PAINT.scale);

            const double ROWH = Menu::ROWH, SEPH = Menu::SEPH, PAD = Menu::PAD;
            // the pill under the row the pointer is on: 4 px off the panel's sides
            constexpr double PILL_INSET = 4;
            const int        PILL_R     = (int)std::lround(NAwesome::Theme::RAD_ROW * PAINT.scale);
            const double MTOP = PAINT.mb.y + PAINT.h + Menu::GAP, MBOT = PAINT.mb.y + PAINT.mb.h - 8;

            for (size_t li = 0; li < Menu::levels.size(); li++) {
                auto& L = Menu::levels[li];
                L.rows.clear();

                // an icon a budget deferred at build time: the warm the
                // budget's retry triggers asks again, until it lands or is
                // a true miss
                if (PAINT.warm)
                    for (auto& E : L.entries)
                        if (!E.icon && !E.iconRetry.empty()) {
                            NAwesome::iconDeferred() = false;
                            E.icon                   = E.iconOfApp ? appIcon(E.iconRetry) : namedIcon(E.iconRetry);
                            if (E.icon || !NAwesome::iconDeferred())
                                E.iconRetry.clear();
                        }

                // labels share one leading column when any row in this level
                // has an icon or a check/radio state, ragged otherwise — how
                // GTK's own menus aligned
                bool anyIcon = false, anyToggle = false;
                for (const auto& E : L.entries) {
                    anyIcon   = anyIcon || (E.icon && E.icon->m_texID != 0);
                    anyToggle = anyToggle || E.toggle != Menu::TG_NONE;
                }

                const double IS   = ICON_CELL;
                const double COLW = anyIcon ? IS : anyToggle ? 14 : 0;
                const double LX   = COLW > 0 ? 10 + COLW + 8 : 12;

                // a submenu row's chevron sits in a right-hand column of its
                // own: as the label's tail it was measured out of the panel
                // and ellipsized away
                constexpr double ARW = 18;
                const auto       capPx = [&](const Menu::SEntry& E) { return (int)std::lround((380.0 - LX - 12 - (E.submenu ? ARW : 0)) * PAINT.scale); };
                // the client list's tags: a right-hand column one size down
                // in the quiet ink, or the row's own colour when it has one
                const bool       anyTag = std::ranges::any_of(L.entries, [](const Menu::SEntry& E) { return !E.tag.empty(); });
                const double     TAGW   = anyTag ? 26 : 0;
                const int        TAGPT  = std::max(1, PAINT.pt - (int)std::lround(PAINT.scale));
                // a section header starts where the leading column does
                const double     HX     = COLW > 0 ? 10 : 12;
                const auto       headTex = [&](const Menu::SEntry& E) { return textTex(E.display, NAwesome::tSub(), TAGPT, capPx(E), "", 600); };
                // a row's right-hand words: both faces every pass (the warm
                // builds the lit one too, so the label's room — and so its
                // raster's key — is the same in the warm and the draw)
                constexpr double DETAIL_GAP = 16;
                const auto       detailW = [&](const Menu::SEntry& E) {
                    if (E.detail.empty() && E.hint.empty())
                        return 0.0;
                    const double D = E.detail.empty() ? 0 : textInkW(textTex(E.detail, NAwesome::tSub(), TAGPT), PAINT.scale);
                    const double H = E.hint.empty() ? 0 : textInkW(textTex(E.hint, COLFG, TAGPT), PAINT.scale);
                    return std::max(D, H) + DETAIL_GAP;
                };

                double           mw = L.fixedWidth;
                if (mw <= 0) {
                    // the widest row as drawn (label, its arrow column), at
                    // most the panel's cap: an app's 4 KiB label would raster
                    // at full length, and the walk stops once a row reaches
                    // it. The columns before it are added per render: an
                    // icon landing later moves them.
                    double textW = 0;
                    if (L.width > 0 && L.widthPt == PAINT.pt)
                        textW = L.width;
                    else {
                        for (const auto& E : L.entries) {
                            if (E.separator)
                                continue;
                            if (E.header) {
                                if (const auto T = headTex(E); T)
                                    textW = std::max(textW, textInkW(T, PAINT.scale) - (LX - HX));
                                continue;
                            }
                            if (const auto T = textTex(E.display, COLFG, PAINT.pt, capPx(E)); T)
                                textW = std::max(textW, textInkW(T, PAINT.scale) + (E.submenu ? ARW : 0) + detailW(E));
                            if (textW + LX + 12 >= 380.0)
                                break;
                        }
                        if (PAINT.warm) { // a draw can hit texture misses -> a short measure
                            L.width   = textW;
                            L.widthPt = PAINT.pt;
                        }
                    }
                    mw = std::clamp(textW + LX + 12, 200.0, 380.0);
                }

                // A narrow output must shrink the panel before anchoring it;
                // clamping only the x coordinate leaves the right edge past
                // the output and steals the tasklist's input region.
                mw = std::min(mw, std::max(1.0, PAINT.mb.w - 16.0));

                // level 0 opens under the bar at the click; a cascade sits
                // beside its parent panel, top-aligned with the row it hangs
                // off, flipped to the left when the right edge won't fit
                double mx, my0;
                if (li == 0) {
                    // not std::clamp: lo > hi (UB) once a panel is wider than the monitor
                    mx  = std::max(PAINT.mb.x + 8.0, std::min(Menu::anchorX - mw / 2, PAINT.mb.x + PAINT.mb.w - mw - 8));
                    my0 = MTOP;
                } else {
                    const auto& P = Menu::levels[li - 1];
                    mx            = P.box.x + P.box.w + 2;
                    if (mx + mw > PAINT.mb.x + PAINT.mb.w - 8)
                        mx = std::max(P.box.x - mw - 2, PAINT.mb.x + 8.0);
                    my0 = P.box.y;
                    for (const auto& [RB, RI] : P.rows)
                        if (RI == L.parentIdx) {
                            my0 = RB.y - PAD;
                            break;
                        }
                }

                // taller than the screen (nm-applet's wifi list): clamp and
                // scroll between chevron strips, as GTK did under X11;
                // shorter panels shift up instead until they fit
                const double fullH  = Menu::levelHeight(L);
                const double mh     = std::min(fullH, std::max(1.0, MBOT - MTOP));
                const bool   SCROLL = mh < fullH;
                L.overflow          = SCROLL;
                my0                 = std::clamp(my0, MTOP, std::max(MTOP, MBOT - mh));
                L.box               = CBox{mx, my0, mw, mh};

                PAINT.frost(L.box, COLMENU, COLFRAME, RPANEL, RP, NAwesome::SPaint::eDepth::MENU);

                double my    = my0 + PAD;
                double myEnd = my0 + mh - PAD;
                if (SCROLL) {
                    my += Menu::ARROWH;
                    myEnd -= Menu::ARROWH;
                    // the last scroll position still fills the window: clamp to
                    // the smallest top index whose tail fits whole
                    int    minTop = (int)L.entries.size() - 1;
                    double acc    = 0;
                    for (int i = (int)L.entries.size() - 1; i >= 0; i--) {
                        acc += Menu::rowHeight(L.entries[i]);
                        if (acc > myEnd - my + 0.5)
                            break;
                        minTop = i;
                    }
                    L.scrollTop = std::clamp(L.scrollTop, 0, minTop);
                    L.maxScroll = minTop;
                } else {
                    L.scrollTop = 0;
                    L.maxScroll = 0;
                }

                if (L.entries.empty())
                    PAINT.texIn(textTex(L.load == Menu::SLevel::LOAD_PENDING ? "…" : L.load == Menu::SLevel::LOAD_FAILED ? "Unavailable" : "Empty", COLEMPTY, PAINT.pt),
                                CBox{mx, my, mw, ROWH});

                const bool FITS      = L.width > 0 && mw + 0.5 >= L.width + LX + 12;
                bool       moreBelow = false;
                for (size_t i = L.scrollTop; i < L.entries.size(); i++) {
                    const auto& E = L.entries[i];
                    if (my + Menu::rowHeight(E) > myEnd + 0.5) {
                        moreBelow = true;
                        break;
                    }
                    if (E.separator) {
                        PAINT.rect(CBox{mx + 12, my + SEPH / 2, mw - 24, 1.0 / PAINT.scale}, NAwesome::tHairline());
                        my += SEPH;
                        continue;
                    }
                    if (E.header) { // sits a pixel low: it belongs to the rows under it
                        if (const auto T = headTex(E); T && T->m_texID != 0) {
                            const auto P = PAINT.toPhys(CBox{mx, my + 1, mw, Menu::HEADH});
                            PAINT.texAt(T, CBox{P.x + std::round(HX * PAINT.scale), P.y + (P.h - T->m_size.y) / 2.0, T->m_size.x, T->m_size.y}.round());
                        }
                        my += Menu::HEADH;
                        continue;
                    }
                    const CBox ROW{mx, my, mw, ROWH};
                    // the hovered row, and the row a cascade hangs off while
                    // the cascade is open, sit on the pill in their own ink
                    // (a dim one lifts); the current row (the focused window)
                    // wears the accent text: where you are; disposition
                    // warning/alert rows take the urgent colour
                    const bool OPENSUB = li + 1 < Menu::levels.size() && Menu::levels[li + 1].parentIdx == (int)i;
                    const bool LIT     = ((int)i == L.hover || OPENSUB) && E.enabled;
                    if (LIT)
                        PAINT.rect(CBox{mx + PILL_INSET, my, mw - 2 * PILL_INSET, ROWH}, NAwesome::tPill(), PILL_R, 2.f);
                    CHyprColor fg = !E.enabled ? COLEMPTY : E.current ? COLFOCUS : E.alert ? COLURGENT : E.dim && !LIT ? COLEMPTY : COLFG;

                    if (E.icon && E.icon->m_texID != 0) {
                        if (E.dim && !LIT) { // a minimized window's icon fades with its row
                            auto FADED = PAINT;
                            FADED.alpha *= 0.5f;
                            FADED.texFit(E.icon, CBox{mx + 10, my + (ROWH - IS) / 2, IS, IS});
                        } else
                            PAINT.texFit(E.icon, CBox{mx + 10, my + (ROWH - IS) / 2, IS, IS});
                    }
                    else if (E.toggle == Menu::TG_CHECK && E.toggleState == 1)
                        PAINT.texIn(G::tex(G::eGlyph::CHECK, fg, GLYPHPX), CBox{mx + 10, my, COLW, ROWH});
                    else if (E.toggle == Menu::TG_RADIO && E.toggleState == 1) {
                        constexpr double DOT = 6;
                        PAINT.rect(CBox{mx + 10 + (COLW - DOT) / 2, my + (ROWH - DOT) / 2, DOT, DOT}, fg, (int)std::lround(DOT / 2 * PAINT.scale), 2.f);
                    } else if (E.toggle != Menu::TG_NONE && E.toggleState != 0)
                        PAINT.texIn(textTex("–", fg, PAINT.pt), CBox{mx + 10, my, COLW, ROWH}); // indeterminate

                    // the measure's own raster while the panel holds its widest
                    // row (a narrow output shrinks it below that)
                    const double DETW   = detailW(E);
                    const int    ROOMPX = (int)std::round((mw - LX - 12 - TAGW - (E.submenu ? ARW : 0) - DETW) * PAINT.scale);
                    const auto T      = textTex(E.display, fg, PAINT.pt, L.fixedWidth > 0 ? ROOMPX : FITS ? capPx(E) : ROOMPX);
                    if (T && T->m_texID != 0) {
                        const auto P = PAINT.toPhys(ROW);
                        CBox       b{P.x + std::round(LX * PAINT.scale), P.y + (P.h - T->m_size.y) / 2.0, T->m_size.x, T->m_size.y};
                        PAINT.texAt(T, b.round());
                    }
                    if (E.submenu)
                        PAINT.texIn(G::tex(G::eGlyph::CHEVRON_RIGHT, fg, GLYPHPX), CBox{mx + mw - 8 - ARW, my, ARW, ROWH});
                    if (DETW > 0) {
                        const bool HINT = LIT && !E.hint.empty();
                        if (const auto DT = HINT ? textTex(E.hint, COLFG, TAGPT) : E.detail.empty() ? nullptr : textTex(E.detail, NAwesome::tSub(), TAGPT); DT && DT->m_texID != 0) {
                            const double W = textInkW(DT, PAINT.scale);
                            PAINT.texIn(DT, CBox{mx + mw - 12 - W, my, W, ROWH});
                        }
                    }
                    if (!E.tag.empty()) {
                        const bool PLAIN = !LIT && !E.current && !E.alert && !E.dim;
                        if (const auto TT = textTex(E.tag, PLAIN ? NAwesome::tSub() : fg, TAGPT); TT && TT->m_texID != 0)
                            PAINT.texIn(TT, CBox{mx + mw - 12 - TAGW + 8, my, TAGW - 8, ROWH});
                    }
                    L.rows.push_back({ROW, (int)i});
                    my += ROWH;
                }

                if (SCROLL) {
                    // GTK's scroll strips, dimmed at their end of the list;
                    // a click steps like a wheel notch
                    const auto arrow = [&](const CBox& B, int id, bool on, G::eGlyph glyph) {
                        if (on && L.hover == id)
                            PAINT.rect(CBox{B.x + PILL_INSET, B.y, B.w - 2 * PILL_INSET, B.h}, NAwesome::tPill(), PILL_R, 2.f);
                        const CHyprColor fg = !on ? COLEMPTY : COLFG;
                        PAINT.texIn(G::tex(glyph, fg, GLYPHPX), B);
                        L.rows.push_back({B, id});
                    };
                    arrow(CBox{mx, my0 + PAD, mw, Menu::ARROWH}, Menu::SCROLL_UP, L.scrollTop > 0, G::eGlyph::CHEVRON_UP);
                    arrow(CBox{mx, my0 + mh - PAD - Menu::ARROWH, mw, Menu::ARROWH}, Menu::SCROLL_DOWN, moreBelow, G::eGlyph::CHEVRON_DOWN);
                }
            }
        }
    }

    // A dbusmenu signal from any client, through the tray connection's one
    // async match (tray.cpp): only the open menu's own sender and path
    // count. Apps mutate their menu while it is up (scans, state flips):
    // reload any open level the update touches. LayoutUpdated names the
    // parent of the changed subtree: its open level and every cascade
    // below it reload (parent 0, the whole layout, reloads them all).
    void Tray::onMenuSignal(sdbus::Message& m) {
        using namespace Menu;
        const uint64_t SESSION = menuSession;
        if (!sessionActive(SESSION) || !item)
            return;
        const char* SENDER = m.getSender();
        const char* PATH   = m.getPath();
        const char* MEMBER = m.getMemberName();
        if (!SENDER || !PATH || !MEMBER || item->owner != SENDER || item->menuPath != PATH)
            return;
        const std::string_view NAME{MEMBER};
        try {
            if (NAME == "LayoutUpdated") {
                uint32_t revision = 0;
                int32_t  parent   = 0;
                m >> revision >> parent;
                std::vector<int32_t> reload;
                for (const auto& L : levels)
                    if (!reload.empty() || L.parentId == parent)
                        reload.push_back(L.parentId);
                for (const auto P : reload)
                    loadLevel(P, SESSION);
            } else if (NAME == "ItemsPropertiesUpdated") {
                std::vector<sdbus::Struct<int32_t, std::map<std::string, sdbus::Variant>>> changed;
                std::vector<sdbus::Struct<int32_t, std::vector<std::string>>>             removed;
                m >> changed >> removed;
                std::unordered_set<int32_t> ids;
                for (const auto& C : changed) {
                    if (ids.size() >= MAX_PROPERTY_UPDATES)
                        break;
                    ids.insert(std::get<0>(C));
                }
                for (const auto& R : removed) {
                    if (ids.size() >= MAX_PROPERTY_UPDATES)
                        break;
                    ids.insert(std::get<0>(R));
                }
                std::unordered_set<int32_t> reload; // affected levels, deduped
                for (const auto& L : levels)
                    for (const auto& E : L.entries)
                        if (ids.contains(E.id)) {
                            reload.insert(L.parentId);
                            break;
                        }
                for (const auto PARENT : reload)
                    loadLevel(PARENT, SESSION);
            }
        } catch (...) {} // a malformed signal is ignored
    }

    void Tray::onServiceDropped(const std::string& service) {
        if (Menu::item && Menu::item->service == service)
            Menu::close();
    }

} // namespace NAwesome::Shell
