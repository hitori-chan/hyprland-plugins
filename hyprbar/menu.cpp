// hyprbar/menu.cpp — the menu renderer: dbusmenu for tray items, local mode for the client list

#include "hyprbar.hpp"

namespace NHyprbar {

    // ---- dbusmenu: a native menu renderer for tray items ----

    namespace Menu {
        constexpr const char*                 DBUSMENU = "com.canonical.dbusmenu";

        bool                                  isOpen  = false;
        bool                                  isLocal = false;
        static SP<Tray::SItem>                item;
        static std::unique_ptr<sdbus::IProxy> proxy;
        std::vector<SLevel>                   levels;
        double                                anchorX = 0;
        PHLMONITORREF                         mon;

        static void                           menuEvent(int32_t id, const char* type); // "opened"/"closed" notifications

        // Logical panel height, the same layout math renderBar uses.
        double levelHeight(const SLevel& l) {
            double h = PAD * 2;
            for (const auto& E : l.entries)
                h += E.separator ? SEPH : ROWH;
            if (l.entries.empty())
                h += ROWH; // the "…" loading row
            return h;
        }

        void damageMenu() {
            // Menu state changed -> new labels/colours -> new textures. Build
            // them here in the event loop; warmBars() no-ops if a render is on
            // the stack (renderBar closes the menu when a window goes
            // fullscreen), which is exactly when building would break. The warm
            // also lays the panels out, so the boxes below are current.
            warmBars(mon.lock());
            if (!g_pHyprRenderer)
                return;
            static std::vector<CBox> last; // the previous panels: a close/resize must damage them too
            for (const auto& B : last)
                g_pHyprRenderer->damageBox(B);
            last.clear();
            for (const auto& L : levels) {
                g_pHyprRenderer->damageBox(L.box);
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

        static void                glideTick() {
            if (!isOpen || glideDir == 0 || glideLevel >= levels.size())
                return;
            auto& L = levels[glideLevel];
            if (L.hover != (glideDir < 0 ? SCROLL_UP : SCROLL_DOWN))
                return;
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

            if (!isOpen || isLocal || level >= levels.size() || row < 0 || (size_t)row >= levels[level].entries.size()) {
                disarm();
                return;
            }
            const auto& E        = levels[level].entries[row];
            const bool  OPENHERE = level + 1 < levels.size() && levels[level + 1].parentIdx == row;
            if (E.submenu && E.enabled && !OPENHERE)
                arm(level, row); // open this row's cascade
            else if (!E.submenu && !E.separator && level + 1 < levels.size())
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

        void close() {
            if (!isOpen)
                return;
            isOpen  = false;
            isLocal = false;
            disarm();
            glideStop();
            for (const auto& L : levels)
                menuEvent(L.parentId, "closed");
            levels.clear();
            damageMenu();
            proxy.reset(); // also drops the LayoutUpdated/ItemsPropertiesUpdated subscriptions
            item.reset();
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
            proxy->callMethodAsync("Event")
                .onInterface(DBUSMENU)
                .withArguments(id, std::string{type}, sdbus::Variant{(int32_t)0}, (uint32_t)0)
                .uponReplyInvoke([](std::optional<sdbus::Error>) {});
        }

        static void loadLevel(int32_t parentId) {
            if (!proxy)
                return;
            using LayoutItem = sdbus::Struct<int32_t, std::map<std::string, sdbus::Variant>, std::vector<sdbus::Variant>>;
            proxy->callMethodAsync("GetLayout")
                .onInterface(DBUSMENU)
                .withArguments(parentId, (int32_t)1, std::vector<std::string>{})
                .uponReplyInvoke([parentId](std::optional<sdbus::Error> e, uint32_t, LayoutItem root) {
                    if (e || !isOpen)
                        return;
                    SLevel* lvl = nullptr; // deepest match wins
                    for (auto it = levels.rbegin(); it != levels.rend(); ++it)
                        if (it->parentId == parentId) {
                            lvl = &*it;
                            break;
                        }
                    if (!lvl)
                        return; // that cascade closed while the reply was in flight
                    lvl->entries.clear();
                    lvl->hover = -1; // scrollTop stays: live updates must not yank a browsed list to its top
                    SWarmToken WARM; // we resolve icon-name textures here, in a DBus reply — not a render
                    for (auto& CV : std::get<2>(root)) {
                        try {
                            auto  c = CV.get<LayoutItem>();
                            auto& P = std::get<1>(c);

                            if (auto it = P.find("visible"); it != P.end() && !it->second.get<bool>())
                                continue;

                            SEntry en;
                            en.id = std::get<0>(c);
                            if (auto it = P.find("type"); it != P.end() && it->second.get<std::string>() == "separator")
                                en.separator = true;
                            if (auto it = P.find("label"); it != P.end())
                                en.label = cleanLabel(it->second.get<std::string>());
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
                            if (auto it = P.find("icon-name"); it != P.end())
                                en.icon = namedIcon(it->second.get<std::string>());
                            if (!en.icon) // nm-applet ships its signal-strength icons inline
                                if (auto it = P.find("icon-data"); it != P.end())
                                    en.icon = loadPngBytes(it->second.get<std::vector<uint8_t>>());
                            en.display = en.label + (en.submenu ? "  ▸" : "");
                            if (en.icon && en.icon->m_texID != 0 && en.toggle != TG_NONE && en.toggleState == 1)
                                en.display = (en.toggle == TG_RADIO ? "● " : "✓ ") + en.display;
                            lvl->entries.push_back(std::move(en));
                        } catch (...) { continue; }
                    }
                    lvl->width = 0;
                    damageMenu();
                });
            Tray::pollSoon(); // don't leave the layout reply waiting on a poll tick
        }

        void openFor(SP<Tray::SItem> it, double ax, PHLMONITORREF m) {
            if (!it || it->menuPath.empty() || !Tray::conn)
                return;
            close();
            item    = it;
            anchorX = ax;
            mon     = m;
            isOpen  = true;
            levels.emplace_back(); // the root panel (parentId 0)
            proxy = sdbus::createProxy(*Tray::conn, sdbus::ServiceName{it->service}, sdbus::ObjectPath{it->menuPath});

            // apps mutate their menu while it's up (scans, state flips) —
            // reload any open level the update signals touch
            proxy->uponSignal("LayoutUpdated").onInterface(DBUSMENU).call([](uint32_t, int32_t parent) {
                if (!isOpen)
                    return;
                for (const auto& L : levels)
                    if (L.parentId == parent) {
                        loadLevel(parent);
                        break;
                    }
            });
            proxy->uponSignal("ItemsPropertiesUpdated")
                .onInterface(DBUSMENU)
                .call(
                    [](std::vector<sdbus::Struct<int32_t, std::map<std::string, sdbus::Variant>>> changed, std::vector<sdbus::Struct<int32_t, std::vector<std::string>>> removed) {
                        if (!isOpen)
                            return;
                        std::unordered_set<int32_t> ids;
                        for (const auto& C : changed)
                            ids.insert(std::get<0>(C));
                        for (const auto& R : removed)
                            ids.insert(std::get<0>(R));
                        std::unordered_set<int32_t> reload; // affected levels, deduped
                        for (const auto& L : levels)
                            for (const auto& E : L.entries)
                                if (ids.contains(E.id)) {
                                    reload.insert(L.parentId);
                                    break;
                                }
                        for (const auto P : reload)
                            loadLevel(P);
                    });

            // AboutToShow lets lazy apps (nm-applet) populate; load either way.
            menuEvent(0, "opened");
            proxy->callMethodAsync("AboutToShow").onInterface(DBUSMENU).withArguments((int32_t)0).uponReplyInvoke([](std::optional<sdbus::Error>, bool) { loadLevel(0); });
            loadLevel(0);
            Tray::pollSoon();
            damageMenu();
        }

        // hover (after the popup delay) or a click on a submenu row: cascade
        // its children out into a new panel beside the parent, like the GTK
        // menus these were under X11.
        void openSub(size_t level, int entryIdx) {
            if (!isOpen || isLocal || !proxy || level >= levels.size())
                return;
            auto& L = levels[level];
            if (entryIdx < 0 || (size_t)entryIdx >= L.entries.size() || !L.entries[entryIdx].submenu || !L.entries[entryIdx].enabled)
                return;
            if (level + 1 < levels.size() && levels[level + 1].parentIdx == entryIdx)
                return;                                // already open
            const int32_t ID = L.entries[entryIdx].id; // before emplace_back can move the vector
            disarm();
            closeDeeperThan(level);
            auto& N     = levels.emplace_back();
            N.parentId  = ID;
            N.parentIdx = entryIdx;
            menuEvent(ID, "opened");
            proxy->callMethodAsync("AboutToShow").onInterface(DBUSMENU).withArguments(ID).uponReplyInvoke([ID](std::optional<sdbus::Error>, bool) { loadLevel(ID); });
            loadLevel(ID);
            Tray::pollSoon();
            damageMenu();
        }

        // right-click on a task, awesome's awful.menu.client_list: every mapped
        // window on every workspace, in the same stable arrival order; a click
        // jumps to it (view its workspace, then focus + raise).
        void openClients(double ax, PHLMONITORREF m) {
            close();
            static std::vector<std::pair<uint64_t, PHLWINDOW>> ws; // reused; main thread only
            ws.clear();
            for (const auto& W : Desktop::windowState()->windows()) {
                if (W->m_isMapped && !W->isHidden()) {
                    const auto [SEQ, NEW] = winSeq.try_emplace(W.get(), winSeqNext);
                    if (NEW)
                        winSeqNext++;
                    ws.emplace_back(SEQ->second, W);
                }
            }
            if (ws.empty())
                return;
            std::sort(ws.begin(), ws.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            auto&      L = levels.emplace_back();
            SWarmToken WARM; // appIcon builds a texture, and we are in a deferred click, not a render
            for (const auto& [SEQ, W] : ws) {
                std::string lbl;
                taskLabel(W, lbl);
                L.entries.push_back({.label = lbl, .display = std::move(lbl), .win = W, .icon = appIcon(W->m_class)});
            }
            ws.clear(); // drop the strong refs now — entries hold weak ones
            anchorX = ax;
            mon     = m;
            isOpen  = true;
            isLocal = true;
            damageMenu();
        }

        // leaf rows only — submenu rows cascade via openSub (hover or click)
        void activate(const SEntry& en) {
            if (const auto W = en.win.lock(); W && W->m_isMapped) { // client-list row: jump to it
                if (W->m_workspace && !W->m_workspace->isVisible())
                    std::ignore = Config::Actions::changeWorkspace(W->m_workspace);
                Desktop::windowState()->raise(W);
                Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_DISPATCH_FOCUSWINDOW, W->wlSurface()->resource());
                close();
                return;
            }
            if (!proxy)
                return;
            proxy->callMethodAsync("Event")
                .onInterface(DBUSMENU)
                .withArguments(en.id, std::string{"clicked"}, sdbus::Variant{(int32_t)0}, (uint32_t)0)
                .uponReplyInvoke([](std::optional<sdbus::Error>) {});
            Tray::pollSoon();
            close();
        }

        // The open panels, cascade by cascade.
        void render(const SPaint& PAINT) {
            if (!isOpen || mon.lock() != PAINT.mon)
                return;

            // one palette fetch per render: color() memoizes but still hashes per call
            const CHyprColor COLBG = color(cfg.colBg), COLACTIVEBG = color(cfg.colActiveBg), COLFG = color(cfg.colFg), COLEMPTY = color(cfg.colEmpty),
                             COLURGENT = color(cfg.colUrgent), COLFOCUS = color(cfg.colFocus), COLFRAME = color(cfg.colFrame);

            // the overlay language: 1px rounding on the panel and its rows
            const int ROUND = std::max(0, (int)std::lround(PAINT.scale));

            const double     ROWH = Menu::ROWH, SEPH = Menu::SEPH, PAD = Menu::PAD;
            const double     MTOP = PAINT.mb.y + PAINT.h, MBOT = PAINT.mb.y + PAINT.mb.h - 2;

            for (size_t li = 0; li < Menu::levels.size(); li++) {
                auto& L = Menu::levels[li];
                L.rows.clear();

                double mw = 250;
                if (!Menu::isLocal) {
                    if (L.width > 0 && L.widthPt == PAINT.pt)
                        mw = L.width;
                    else {
                        mw = 180;
                        for (const auto& E : L.entries) {
                            if (E.separator)
                                continue;
                            if (const auto T = textTex(E.label, COLFG, PAINT.pt); T)
                                mw = std::max(mw, T->m_size.x / PAINT.scale + 48);
                        }
                        mw = std::min(mw, 380.0);
                        if (PAINT.warm) { // a draw can hit texture misses -> a short measure
                            L.width   = mw;
                            L.widthPt = PAINT.pt;
                        }
                    }
                }

                // level 0 opens under the bar at the click; a cascade sits
                // beside its parent panel, top-aligned with the row it hangs
                // off, flipped to the left when the right edge won't fit
                double mx, my0;
                if (li == 0) {
                    mx  = std::clamp(Menu::anchorX - mw / 2, PAINT.mb.x + 2.0, PAINT.mb.x + PAINT.mb.w - mw - 2);
                    my0 = MTOP;
                } else {
                    const auto& P = Menu::levels[li - 1];
                    mx            = P.box.x + P.box.w - 1; // share the 1px frame
                    if (mx + mw > PAINT.mb.x + PAINT.mb.w - 2)
                        mx = std::max(P.box.x - mw + 1, PAINT.mb.x + 2.0);
                    my0 = P.box.y;
                    for (const auto& [RB, RI] : P.rows)
                        if (RI == L.parentIdx) {
                            my0 = RB.y - PAD;
                            break;
                        }
                }

                // taller than the screen (nm-applet's wifi list): clamp and
                // scroll between ▴/▾ arrow strips, as GTK did under X11;
                // shorter panels shift up instead until they fit
                const double fullH  = Menu::levelHeight(L);
                const double mh     = std::min(fullH, MBOT - MTOP);
                const bool   SCROLL = mh < fullH;
                L.overflow          = SCROLL;
                my0                 = std::clamp(my0, MTOP, MBOT - mh);
                L.box               = CBox{mx, my0, mw, mh};

                // fill under the whole panel, frame ring over its edge: no
                // corner seam, and 5 rects are 2 calls
                PAINT.rect(L.box, COLBG, ROUND);
                PAINT.border(L.box, COLFRAME, ROUND, std::max(1, (int)std::lround(PAINT.scale)));

                // labels share one leading column when any row in this level
                // has an icon or a check/radio state, ragged otherwise — how
                // GTK's own menus aligned
                bool anyIcon = false, anyToggle = false;
                for (const auto& E : L.entries) {
                    anyIcon   = anyIcon || (E.icon && E.icon->m_texID != 0);
                    anyToggle = anyToggle || E.toggle != Menu::TG_NONE;
                }

                const double IS   = ROWH - 8;
                const double COLW = anyIcon ? IS : anyToggle ? 14 : 0;
                const double LX   = COLW > 0 ? 8 + COLW + 6 : 12;

                double       my    = my0 + PAD;
                double       myEnd = my0 + mh - PAD;
                if (SCROLL) {
                    my += Menu::ARROWH;
                    myEnd -= Menu::ARROWH;
                    // the last scroll position still fills the window: clamp to
                    // the smallest top index whose tail fits whole
                    int    minTop = (int)L.entries.size() - 1;
                    double acc    = 0;
                    for (int i = (int)L.entries.size() - 1; i >= 0; i--) {
                        acc += L.entries[i].separator ? SEPH : ROWH;
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
                    PAINT.texIn(textTex("…", COLEMPTY, PAINT.pt), CBox{mx, my, mw, ROWH});

                bool moreBelow = false;
                for (size_t i = L.scrollTop; i < L.entries.size(); i++) {
                    const auto& E = L.entries[i];
                    if (my + (E.separator ? SEPH : ROWH) > myEnd + 0.5) {
                        moreBelow = true;
                        break;
                    }
                    if (E.separator) {
                        PAINT.rect(CBox{mx + 8, my + SEPH / 2, mw - 16, 1}, COLACTIVEBG);
                        my += SEPH;
                        continue;
                    }
                    const CBox ROW{mx, my, mw, ROWH};
                    // awful.menu item_enter: fg_focus on bg_focus; the row a
                    // cascade hangs off stays lit while the cascade is open;
                    // disposition warning/alert rows take the urgent color
                    const bool OPENSUB = li + 1 < Menu::levels.size() && Menu::levels[li + 1].parentIdx == (int)i;
                    CHyprColor fg      = !E.enabled ? COLEMPTY : E.alert ? COLURGENT : COLFG;
                    if (((int)i == L.hover || OPENSUB) && E.enabled) {
                        // the hover row floats off the frame: 4px inset, softened corners
                        PAINT.rect(CBox{ROW.x + 4, ROW.y, ROW.w - 8, ROW.h}, COLACTIVEBG, ROUND);
                        fg = COLFOCUS;
                    }

                    if (E.icon && E.icon->m_texID != 0) {
                        auto IP = PAINT.toPhys(CBox{mx + 8, my + 4, IS, IS});
                        PAINT.tex(E.icon, IP.round());
                    } else if (E.toggle != Menu::TG_NONE) {
                        // the leading column: ✓/– for checkmarks, ●/○ for radios
                        const char* G = E.toggle == Menu::TG_RADIO ? (E.toggleState == 1     ? "●" :
                                                                          E.toggleState == 0 ? "○" :
                                                                                               "–") :
                                                                     (E.toggleState == 1     ? "✓" :
                                                                          E.toggleState == 0 ? "" :
                                                                                               "–");
                        if (*G)
                            PAINT.texIn(textTex(G, fg, PAINT.pt), CBox{mx + 8, my, COLW, ROWH});
                    }

                    const auto T = textTex(E.display, fg, PAINT.pt, (int)std::round((mw - LX - 12) * PAINT.scale));
                    if (T && T->m_texID != 0) {
                        const auto P = PAINT.toPhys(ROW);
                        CBox       b{P.x + std::round(LX * PAINT.scale), P.y + (P.h - T->m_size.y) / 2.0, T->m_size.x, T->m_size.y};
                        PAINT.tex(T, b.round());
                    }
                    L.rows.push_back({ROW, (int)i});
                    my += ROWH;
                }

                if (SCROLL) {
                    // GTK's scroll arrows: full-width strips, dimmed at their
                    // end of the list, click steps like a wheel notch
                    const auto arrow = [&](const CBox& B, int id, bool on, const char* glyph) {
                        CHyprColor fg = on ? COLFG : COLEMPTY;
                        if (L.hover == id && on) {
                            PAINT.rect(CBox{B.x + 4, B.y, B.w - 8, B.h}, COLACTIVEBG, ROUND);
                            fg = COLFOCUS;
                        }
                        PAINT.texIn(textTex(glyph, fg, PAINT.pt), B);
                        L.rows.push_back({B, id});
                    };
                    arrow(CBox{mx, my0 + PAD, mw, Menu::ARROWH}, Menu::SCROLL_UP, L.scrollTop > 0, "▴");
                    arrow(CBox{mx, my0 + mh - PAD - Menu::ARROWH, mw, Menu::ARROWH}, Menu::SCROLL_DOWN, moreBelow, "▾");
                }
            }
        }
    }

    void Tray::onServiceDropped(const std::string& service) {
        if (Menu::item && Menu::item->service == service)
            Menu::close();
    }

} // namespace NHyprbar
