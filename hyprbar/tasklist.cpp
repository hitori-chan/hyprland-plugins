// hyprbar/tasklist.cpp — awesome's tasklist: arrival-order bookkeeping, the
// state-marker labels and the widget filling the bar's middle

#include "common/lifecycle.hpp"
#include "common/queries.hpp"

#include "hyprbar.hpp"

namespace NHyprbar {

    // tasklist order: awesome lists clients in ARRIVAL order, stable across
    // raises — windowState()'s list is the Z-order, so the bar keeps its own
    // sequence
    static std::unordered_map<void*, uint64_t> winSeq;
    static uint64_t                            winSeqNext = 0;

    // awesome's client.minimized, which the compositor has no flag for. The
    // window's raw pointer is the identity key (like winSeq), dropped in
    // forget() on destroy before the pointer can be reused. `tiled` records
    // whether restore must re-add a layout slot: a floating window (the
    // floating-only rule = all of them today) reserves none, so hiding alone
    // suffices and its box stays untouched — routing it through the layout
    // would risk the float-recenter (see hyprmax). `fs` is the compositor
    // fullscreen/maximize mode held at minimize: a hidden window can't keep
    // the workspace's one FS slot without stranding it, so the mode is
    // dropped before hiding and re-entered on restore — awesome keeps a
    // minimized client's fullscreen flag, this reproduces it. hyprmax's
    // told-maximize is a plain floating window (internal FSMODE_NONE), holds
    // no slot, and passes through here untouched.
    struct SMinimized {
        PHLWINDOWREF                w;
        void*                       key   = nullptr;
        bool                        tiled = false;
        Fullscreen::SFullscreenMode fs{};
    };
    static std::vector<SMinimized> minStack; // most-recently minimized last

    // After hiding a window, commit focus to the next task. Two compositor
    // quirks shape this: setHidden's own focus reset only fires for the swallow
    // path (the swallowee is never focused), so minimizing the FOCUSED window
    // leaves focus on it; and a plain fullWindowFocus won't move focus off a
    // just-hidden window here. refocus() at the successor's own center is the
    // authoritative input path and lands right regardless of where the real
    // cursor sits (a click leaves it on the bar). m_windows is bottom-first, so
    // walk it reversed to pick the topmost task.
    static void focusNextAfterMinimize(const PHLWINDOW& gone, const PHLWORKSPACE& ws) {
        if (!ws || !g_pInputManager)
            return;
        const auto& WINS = Desktop::windowState()->windows();
        for (size_t i = WINS.size(); i-- > 0;) {
            const auto& W = WINS[i];
            if (W == gone || !W || !W->m_isMapped || W->isHidden() || !W->m_workspace || W->m_workspace->m_id != ws->m_id)
                continue;
            g_pInputManager->refocus(W->middle());
            return;
        }
        // nothing left on the workspace: clear focus so it isn't stranded on the
        // hidden window.
        if (Desktop::focusState())
            Desktop::focusState()->fullWindowFocus(nullptr, Desktop::FOCUS_REASON_DISPATCH_FOCUSWINDOW);
    }

    // awesome honored a client minimizing ITSELF — X11's WM_CHANGE_STATE ->
    // IconicState, and on Wayland a CSD minimize button's xdg set_minimized.
    // Hyprland's onUpdateState ignores requestsMinimize, so the button is dead;
    // hyprbar routes it into client.minimized. The request is a per-window
    // stateChanged signal carrying a VOLATILE requestsMinimize (the compositor
    // resets it right after the emit), so it's read synchronously in the signal
    // and the client.minimized change is deferred out of the request.
    static std::unordered_map<void*, CHyprSignalListener> minReqListeners;
    static std::vector<std::pair<PHLWINDOWREF, bool>>     minReqQueue; // (window, minimize?)
    static bool                                           minReqQueued = false;
    static NHyprCommon::CHop                              pendingMinReq;

    static std::optional<bool>                            minimizeRequestOf(const PHLWINDOW& w) {
        if (w->m_xdgSurface && w->m_xdgSurface->m_toplevel)
            return w->m_xdgSurface->m_toplevel->m_state.requestsMinimize;
        return std::nullopt; // XWayland self-minimize would need XSurface.hpp
    }

    namespace Tasklist {
        uint64_t seqOf(void* w) {
            const auto [SEQ, NEW] = winSeq.try_emplace(w, winSeqNext);
            if (NEW)
                winSeqNext++;
            return SEQ->second;
        }

        bool isMinimized(const PHLWINDOW& w) {
            if (!w)
                return false;
            for (const auto& M : minStack)
                if (M.key == w.get())
                    return true;
            return false;
        }

        void minimize(const PHLWINDOW& w) {
            if (!w || !w->m_isMapped || w->isHidden())
                return; // already hidden — minimized, or swallowed
            // Only the focused window pulls focus to a neighbor on hide; an app
            // minimizing a BACKGROUND window (its own set_minimized) must not
            // yank focus from wherever it currently sits.
            const bool WASFOCUSED = Desktop::focusState() && Desktop::focusState()->window() == w;
            // Drop the workspace's single FS slot before hiding: a fullscreen
            // or compositor-maximized window that goes hidden strands the slot
            // (black workspace, bar gone). Remember the mode; restore()
            // re-enters it. hyprmax's told-maximize reports internal
            // FSMODE_NONE and is left alone — its box and told-state survive
            // the hide untouched.
            const auto FS = Fullscreen::controller()->getFullscreenModes(w);
            if (FS.internal != Fullscreen::FSMODE_NONE)
                Fullscreen::controller()->setFullscreenMode(w, Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE);
            const bool TILED = !w->m_isFloating;
            const auto WS    = w->m_workspace;
            if (TILED && g_layoutManager)
                g_layoutManager->removeTarget(w->layoutTarget());
            w->setHidden(true); // unrendered, xdg-suspended, no frame callbacks
            // setHidden issues NO damage. A tiled window's removeTarget reflow
            // repaints the vacated area for free, but a floating window leaves
            // its last frame stale on screen until something else damages it —
            // so its box flickers back under the cursor's motion damage. Force
            // the vacated area to repaint once here.
            if (g_pHyprRenderer)
                g_pHyprRenderer->damageWindow(w, true);
            if (WASFOCUSED)
                focusNextAfterMinimize(w, WS);
            minStack.push_back({PHLWINDOWREF{w}, w.get(), TILED, FS});
            barChanged();
        }

        // awesome's check_focus (awful.permissions): focus must never rest on a
        // minimized — invisible — window. In X11 a minimized client is unmapped
        // and simply can't take focus; Hyprland's focus fallback (e.g. closing
        // the last visible window) doesn't know a hidden window is "minimized"
        // and lands focus on it. Bounce it to the most-recent visible window on
        // the workspace (or clear focus), like awful.focus.history.get skipping
        // minimized. Called deferred from the window.active hook.
        void focusAwayFromHidden(const PHLWINDOW& w) {
            if (!w || !w->isHidden() || !isMinimized(w) || !w->m_workspace)
                return;
            if (Desktop::focusState() && Desktop::focusState()->window() == w)
                focusNextAfterMinimize(w, w->m_workspace);
        }

        // Raise, then focus with the window's REAL surface — not
        // Actions::focus(): that goes through FocusState with surface=nullptr,
        // and its already-focused guard compares (window, surface) ==
        // (m_focusWindow, m_focusSurface). When a popup/layer that held the
        // keyboard dies while the pointer sits on the bar (moves swallowed =
        // FFM can't heal), m_focusSurface is left empty with m_focusWindow
        // still set — nullptr == empty matches, the guard returns before the
        // raise AND before keyboard focus, and the click looks dead until some
        // other window gets focused. With the real surface the guard can never
        // match a half-focused window, and a focused-but-obscured one still
        // raises.
        static void raiseAndFocus(const PHLWINDOW& w) {
            Desktop::windowState()->raise(w);
            if (Desktop::focusState())
                Desktop::focusState()->fullWindowFocus(w, Desktop::FOCUS_REASON_DISPATCH_FOCUSWINDOW, w->wlSurface()->resource());
        }

        void restore(const PHLWINDOW& w) {
            if (!w)
                return;
            bool                        tiled = false, found = false;
            Fullscreen::SFullscreenMode fs{};
            for (auto it = minStack.begin(); it != minStack.end(); ++it)
                if (it->key == w.get()) {
                    tiled = it->tiled;
                    fs    = it->fs;
                    minStack.erase(it);
                    found = true;
                    break;
                }
            if (!found || !w->m_isMapped)
                return;
            w->setHidden(false);
            if (tiled && g_layoutManager && w->m_workspace)
                g_layoutManager->newTarget(w->layoutTarget(), w->m_workspace->m_space);
            raiseAndFocus(w);
            // re-enter the fullscreen/maximize the window held when minimized,
            // after focus — the compositor fullscreens the active window.
            if (fs.internal != Fullscreen::FSMODE_NONE)
                Fullscreen::controller()->setFullscreenMode(w, fs.internal, fs.client);
            barChanged();
        }

        // Attach the self-minimize listener to a freshly-opened window (from
        // window.open). The listener is dropped in forget() on destroy / exit().
        void watchMinimize(const PHLWINDOW& w) {
            if (!w || !w->m_xdgSurface || !w->m_xdgSurface->m_toplevel)
                return;
            minReqListeners[w.get()] = w->m_xdgSurface->m_toplevel->m_events.stateChanged.listen([wr = PHLWINDOWREF{w}]() {
                const auto W = wr.lock();
                if (!W)
                    return;
                const auto REQ = minimizeRequestOf(W);
                if (!REQ.has_value())
                    return; // this stateChanged carried a fs/maximize change, not a minimize
                minReqQueue.emplace_back(wr, *REQ);
                if (minReqQueued)
                    return; // one drain coalesces a burst — overwriting the lock would cancel it
                minReqQueued = true;
                pendingMinReq.arm([]() {
                    minReqQueued = false;
                    const auto Q = std::move(minReqQueue);
                    minReqQueue.clear();
                    if (NHyprCommon::sessionLocked())
                        return; // never hide/reorder windows under the lockscreen
                    for (const auto& [WR, MIN] : Q) {
                        const auto WW = WR.lock();
                        if (!WW)
                            continue;
                        if (MIN)
                            minimize(WW);
                        else
                            restore(WW);
                    }
                });
            });
        }

        void minimizeFocused() {
            if (const auto W = Desktop::focusState() ? Desktop::focusState()->window() : nullptr)
                minimize(W);
        }

        void restoreLast() {
            // awful.client.restore: the most-recently minimized window whose tag
            // is currently viewed (isVisible), so it returns where you are.
            for (size_t i = minStack.size(); i-- > 0;) {
                const auto W = minStack[i].w.lock();
                if (W && W->m_isMapped && W->m_workspace && W->m_workspace->isVisible()) {
                    restore(W);
                    return;
                }
            }
        }

        void forget(void* w) {
            winSeq.erase(w);
            minReqListeners.erase(w);
            std::erase_if(minStack, [w](const SMinimized& m) { return m.key == w || m.w.expired(); });
        }

        void exit() {
            winSeq.clear();
            minStack.clear();
            pendingMinReq.reset();
            minReqQueued = false;
            minReqQueue.clear();
            minReqListeners.clear();
        }
    } // namespace Tasklist

    // awesome's tasklist text: state markers, then the title. The stock set
    // is ▪ sticky, ⌃ ontop, ▴ above, ▾ below, + maximized, ⬌/⬍ maximized
    // h/v, ✈ floating — crosschecked against what exists here:
    //   ⌃  a PINNED window. Hyprland's pin is ontop AND sticky at once, but
    //      the user's Super+T was awesome's `c.ontop` toggle (its tasklist
    //      marker: ⌃) and nothing in their old config ever set sticky, so
    //      pin presents as the ontop it replaces — ▪ would read as wrong.
    //   +  maximized (awesome drew it bold; plain here — markup would need
    //      escaping every title).
    //   ✈  floating, in maximized's else exactly like awesome. The
    //      floating-only rule floats every window, so today every
    //      unmaximized task carries it; it starts discriminating when
    //      other layouts arrive.
    //   ▴/▾/⬌/⬍ have no Hyprland analog.
    void Tasklist::label(const PHLWINDOW& w, std::string& out) {
        out.clear();
        if (w->m_pinned)
            out += "⌃";
        // maximized: the configured xdg state where it's honest (floating —
        // hyprmax's per-window maximize speaks xdg only), the fullscreen
        // chain otherwise (tiled windows are told maximized as the CSD lie).
        // This runs per task per frame; the chain is a dozen virtual calls.
        bool maximized;
        if (!w->m_isX11 && w->m_isFloating && w->m_xdgSurface && w->m_xdgSurface->m_toplevel)
            maximized = NHyprCommon::toldMaximized(w);
        else
            maximized = Fullscreen::controller()->getFullscreenModes(w).internal == Fullscreen::FSMODE_MAXIMIZED;
        if (maximized)
            out += "+";
        else if (w->m_isFloating)
            out += "✈";
        out += w->m_title.empty() ? "<untitled>" : w->m_title;
    }

    namespace {
        class CTasklistWidget : public IWidget {
          public:
            double fit(const SPaint&, const SFrame&) override {
                return 0; // the middle widget: the skeleton hands it the leftover strip
            }

            void draw(const SPaint& P, const SFrame& F, const CBox& box) override {
                if (!F.tasks || F.tasks->empty() || box.w < 40)
                    return;
                // the compact islands: one chip per task — h 24, pad-x 10,
                // gap 6, max-w 220, 15px themed app icon; the focused chip
                // fills accent-dim, urgent fills its own tint, minimized dims
                constexpr double CHIP_H = 24, CHIP_PADX = 10, CHIP_GAP = 6, CHIP_MAXW = 220, ICON = 15, ICON_GAP = 5;
                const int        RCHIP = (int)std::lround(CHIP_H / 2 * P.scale);
                const double     CY    = box.y + (box.h - CHIP_H) / 2;

                // chips shrink together when the strip runs out of room
                const double N     = (double)F.tasks->size();
                const double AVAIL = (box.w - (N - 1) * CHIP_GAP) / N;
                const double CAPW  = std::clamp(AVAIL, 48.0, CHIP_MAXW);

                double x = box.x;
                for (const auto& [SEQ, W] : *F.tasks) {
                    const bool MINIMIZED = Tasklist::isMinimized(W);
                    const bool FOCUSED   = !MINIMIZED && W == F.focus;
                    CHyprColor fg        = MINIMIZED ? F.minimized : FOCUSED ? F.active : F.fg;
                    if (!MINIMIZED && !FOCUSED && W->m_isUrgent)
                        fg = F.urgentFg;

                    static std::string LBL; // reused; main thread only
                    Tasklist::label(W, LBL);
                    if (P.fp)
                        *P.fp = *P.fp * 1099511628211ULL + std::hash<std::string>{}(LBL);

                    const auto   TEX   = textTex(LBL, fg, P.pt, std::max(1, (int)std::round((CAPW - 2 * CHIP_PADX - ICON - ICON_GAP) * P.scale)));
                    const double LW    = TEX ? TEX->m_size.x / P.scale : 0;
                    const double CHIPW = std::min(CAPW, 2 * CHIP_PADX + ICON + ICON_GAP + LW);
                    const CBox   CELL{x, CY, CHIPW, CHIP_H};
                    if (CELL.x + CELL.w > box.x + box.w + 0.5)
                        break; // out of strip: the tail waits for a wider monitor

                    const bool HOV = barHover.widget == this && barHover.win == W.get();
                    if (FOCUSED)
                        P.rect(CELL, F.activeBg, RCHIP);
                    else if (!MINIMIZED && W->m_isUrgent)
                        P.rect(CELL, F.urgentBg, RCHIP);
                    else
                        P.rect(CELL, (HOV ? tFill2() : tFill()), RCHIP);

                    double tx = x + CHIP_PADX;
                    if (const auto ITEX = appIcon(W->m_class); ITEX && ITEX->m_texID != 0) {
                        if (!P.warm) {
                            // 75% at rest, full ink on the focused chip
                            g_pHyprOpenGL->renderTexture(ITEX, P.toPhys(CBox{tx, CY + (CHIP_H - ICON) / 2, ICON, ICON}),
                                                         {.a = FOCUSED ? 1.f : 0.75f, .round = (int)std::lround(4 * P.scale)});
                        }
                    } else
                        P.texIn(textTex(letterOf(W->m_class), F.active, P.pt), CBox{tx, CY, ICON, CHIP_H});
                    tx += ICON + ICON_GAP;

                    if (TEX && TEX->m_texID != 0) {
                        const auto B = P.toPhys(CBox{tx, CY, 1, CHIP_H});
                        CBox       b{B.x, B.y + (B.h - TEX->m_size.y) / 2.0, TEX->m_size.x, TEX->m_size.y};
                        P.tex(TEX, b.round());
                    }

                    SHit h;
                    h.box    = CELL;
                    h.widget = this;
                    h.window = W;
                    P.hits->push_back(h);
                    x += CHIPW + CHIP_GAP;
                }
            }

            void onHit(const SHit& h, uint32_t bit, bool) override {
                const auto W = h.window.lock();
                if (!W || !W->m_isMapped)
                    return;
                if (bit == 4u) { // middle closes the client (the map's contract)
                    std::ignore = Config::Actions::closeWindow(W);
                    return;
                }
                if (bit != 1u)
                    return;

                // left: the focused chip minimizes, any other (minimized
                // included) restores + focuses. Test minimized FIRST: closing
                // the last visible window lands the compositor's focus
                // fallback on a hidden one, and "click the focused task to
                // minimize" would then no-op on it — the click looks dead.
                if (Tasklist::isMinimized(W)) {
                    Tasklist::restore(W);
                    return;
                }
                if (W == (Desktop::focusState() ? Desktop::focusState()->window() : nullptr)) {
                    Tasklist::minimize(W);
                    return;
                }

                Tasklist::raiseAndFocus(W);
            }

            bool accumulatesScroll() const override {
                return true;
            }
            void onScrollSteps(int steps, PHLMONITOR mon) override {
                // focus.byidx ±steps, wrapping through this workspace's tasks
                const auto WS = mon->m_activeWorkspace;
                if (!WS)
                    return;
                static std::vector<std::pair<uint64_t, PHLWINDOW>> tasks; // reused; main thread only
                tasks.clear();
                for (const auto& W : Desktop::windowState()->windows()) {
                    if (isTaskOn(W, WS) && !Tasklist::isMinimized(W)) // scroll walks focusable tasks, not minimized
                        if (const auto SEQ = winSeq.find(W.get()); SEQ != winSeq.end())
                            tasks.emplace_back(SEQ->second, W);
                }
                if (tasks.empty())
                    return;
                std::sort(tasks.begin(), tasks.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                const auto FOCUS = Desktop::focusState() ? Desktop::focusState()->window() : nullptr;
                int        idx   = 0;
                for (int i = 0; i < (int)tasks.size(); i++)
                    if (tasks[i].second == FOCUS)
                        idx = i;
                const int  N = (int)tasks.size();
                const auto W = tasks[((idx + steps) % N + N) % N].second;
                tasks.clear(); // don't keep strong window refs across scrolls
                Tasklist::raiseAndFocus(W);
            }
        };
    } // namespace

    IWidget& tasklistWidget() {
        static CTasklistWidget W;
        return W;
    }

} // namespace NHyprbar
