// Awesome's tasklist VIEW: the state-marker labels and the widget filling
// the bar's middle. The STATE (minimized, the arrival order) lives in the
// windows module next to the maximize state it composes with; this unit
// only draws and routes.

#include "shell/shell.hpp"

#include "../windows/state.hpp"
#include "core/queries.hpp"
#include "core/theme.hpp"

namespace NAwesome::Shell {

    namespace WT = NAwesome::Windows::Tasklist;

    // awesome's tasklist text: state markers, then the title. The stock set
    // is ▪ sticky, ⌃ ontop, ▴ above, ▾ below, + maximized, ⬌/⬍ maximized
    // h/v, ✈ floating — crosschecked against what exists here:
    //   ⌃  a PINNED window. Hyprland's pin is ontop AND sticky at once, but
    //      it stands in for awesome's `c.ontop` toggle (Super+T, its
    //      tasklist marker: ⌃), so pin presents as ontop — ▪ would read as
    //      wrong.
    //   +  maximized (awesome drew it bold; plain here — markup would need
    //      escaping every title).
    //   ✈  floating is dropped: the floating-only rule floats every window,
    //      so the mark would sit on every task and say nothing.
    //   ▴/▾/⬌/⬍ have no Hyprland analog.
    void Tasklist::label(const PHLWINDOW& w, std::string& out) {
        out.clear();
        if (NAwesome::isPinned(w))
            out += "⌃";
        // maximized: the configured xdg state where it's honest (floating —
        // the windows module's per-window maximize speaks xdg only), the
        // fullscreen chain otherwise (tiled windows are told maximized as the
        // CSD lie). This runs per task per frame; the chain is a dozen
        // virtual calls.
        bool maximized;
        if (!w->backend().isX11() && w->isFloating() && NAwesome::xdgToplevel(w))
            maximized = NAwesome::toldMaximized(w);
        else
            maximized = Fullscreen::controller()->getFullscreenModes(w).internal == Fullscreen::FSMODE_MAXIMIZED;
        if (maximized)
            out += "+";
        out += w->metadata().title().empty() ? "<untitled>" : w->metadata().title();
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
                // awesome tasklist behavior: the windows split the WHOLE free
                // strip between taglist and tray — one window = one huge item
                constexpr double GAP = 4, ROW_H = 22, PADX = 10, ICON = ICON_CELL, ICON_GAP = 8;
                const double     N     = (double)F.tasks->size();
                const double     ITEMW = std::max(1.0, (box.w - GAP * (N - 1)) / N);
                // an unfocused task reads quieter than the bar's text: the
                // board's 62 % from its 86 %
                const CHyprColor QUIET = F.fg.modifyA(F.fg.a * 0.72f);

                double           x = box.x;
                for (const auto& [SEQ, W] : *F.tasks) {
                    const CBox CELL{x, box.y + (P.h - ROW_H) / 2, ITEMW, ROW_H};
                    // focus is accent text at medium, urgent red text — never
                    // a fill. Minimized wins over focus: a minimized window is
                    // never truly focused, but the compositor's focus fallback
                    // can leave focus on a hidden one — it must still read
                    // muted. A hovered task's text lifts a step (dim -> quiet
                    // -> the bar's text; the accent and the red toward white),
                    // never a ground; hover damages but never rewarms (crash
                    // class 4), so the warm builds both.
                    const bool MIN    = WT::isMinimized(W);
                    const bool FOCUS  = !MIN && W == F.focus;
                    const bool URGENT = !MIN && !FOCUS && (W->m_hints & Desktop::View::WINDOW_HINT_URGENT);
                    const auto inkOf  = [&](bool hover) -> CHyprColor {
                        if (FOCUS)
                            return hover ? F.activeHover : F.active;
                        if (URGENT)
                            return hover ? F.urgentHover : F.urgentFg;
                        if (MIN)
                            return hover ? QUIET : F.empty;
                        return hover ? F.fg : QUIET;
                    };
                    const bool HOVER = barHover.widget == this && barHover.window == W.get() && barHover.mon == P.mon->m_id;

                    // [10][icon 16][8][title][10]; a minimized task's icon fades with its text
                    double tx = x + PADX;
                    if (const auto ITEX = appIcon(W->metadata().appID()); ITEX && ITEX->m_texID != 0) {
                        auto PI = P;
                        if (MIN)
                            PI.alpha *= 0.5f;
                        PI.texFit(ITEX, CBox{tx, CELL.y + (ROW_H - ICON) / 2, ICON, ICON});
                    }
                    tx += ICON + ICON_GAP;

                    static std::string LBL; // reused; main thread only
                    Tasklist::label(W, LBL);
                    if (P.fp)
                        *P.fp = *P.fp * 1099511628211ULL + std::hash<std::string>{}(LBL);
                    const int MAXW =
                        std::max(1, (int)std::round((ITEMW - (tx - x) - PADX) * P.scale)); // floor >0: a non-positive width disables ellipsization and overflows the cell
                    const int WEIGHT = FOCUS ? 500 : 400;
                    if (P.warm)
                        textTex(LBL, inkOf(!HOVER), P.pt, MAXW, "", WEIGHT);
                    const auto TEX = textTex(LBL, inkOf(HOVER), P.pt, MAXW, "", WEIGHT);
                    if (TEX && TEX->m_texID != 0) {
                        const auto B = P.toPhys(CBox{tx, box.y, 1, P.h});
                        CBox       b{B.x, B.y + (B.h - TEX->m_size.y) / 2.0, TEX->m_size.x, TEX->m_size.y};
                        P.texAt(TEX, b.round());
                    }

                    SHit h;
                    h.box    = CBox{x, box.y, ITEMW + GAP, P.h};
                    h.widget = this;
                    h.window = W;
                    P.hits->push_back(h);
                    x += ITEMW + GAP;
                }
            }

            void onHit(const SHit& h, uint32_t bit, bool) override {
                if (bit == 2u) { // awesome: the all-clients menu, popped at the click
                    Menu::openClients(h.clickX, h.mon);
                    return;
                }
                if (bit != 1u)
                    return;
                const auto W = h.window.lock();
                if (!W || !W->mapped())
                    return;

                // awesome's tasklist button 1: clicking the focused task
                // minimizes it; clicking any other task (minimized included)
                // restores + focuses it. Test minimized FIRST: closing the
                // last visible window lands the compositor's focus fallback on
                // a hidden one, and "click the focused task to minimize" would
                // then no-op on it (minimize() bails on an already-hidden
                // window) — the click looks dead.
                if (WT::isMinimized(W)) {
                    WT::restore(W);
                    return;
                }
                if (W == (Desktop::focusState() ? Desktop::focusState()->window() : nullptr)) {
                    WT::minimize(W);
                    return;
                }

                WT::raiseAndFocus(W);
            }

            bool accumulatesScroll() const override {
                return true;
            }
            void onScrollSteps(int steps, PHLMONITOR mon) override {
                // focus.byidx ±steps, wrapping through this workspace's tasks
                // (the focusable ones: a minimized row is listed, not walked)
                const auto TASKS = NAwesome::winOrder().onWorkspace(mon->m_activeWorkspace, [](const PHLWINDOW& w) { return !WT::isMinimized(w); });
                const auto FOCUS = Desktop::focusState() ? Desktop::focusState()->window() : nullptr;
                if (const auto W = NAwesome::CArrivalOrder::step(TASKS, FOCUS, steps))
                    WT::raiseAndFocus(W);
            }
        };
    } // namespace

    IWidget& tasklistWidget() {
        static CTasklistWidget W;
        return W;
    }

} // namespace NAwesome::Shell
