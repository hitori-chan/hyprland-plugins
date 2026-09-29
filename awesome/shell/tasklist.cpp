// awesome/shell/tasklist.cpp — awesome's tasklist VIEW: the state-marker
// labels and the widget filling the bar's middle. The STATE (minimized, the
// arrival order) lives in the windows module next to the maximize state it
// composes with; this unit only draws and routes.

#include "shell/shell.hpp"

#include "../windows/state.hpp"
#include "core/queries.hpp"

namespace NAwesome::Shell {

    namespace WT = NAwesome::Windows::Tasklist;

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
        else if (w->isFloating())
            out += "✈";
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
                const double ITEMW = box.w / (double)F.tasks->size();

                double       x = box.x;
                for (const auto& [SEQ, W] : *F.tasks) {
                    const CBox CELL{x, box.y, ITEMW, P.h};
                    // The focused task is primary TEXT on the plain bar
                    // (tasklist_bg_focus = bg_normal, no box); urgent gets the
                    // error container — focus wins over urgent, like awesome.
                    CHyprColor fg = F.fg;
                    // minimized wins over focus: a minimized window is never
                    // truly focused, but the compositor's focus fallback can
                    // leave focus on a hidden one — it must still read muted.
                    if (WT::isMinimized(W))
                        fg = F.minimized; // awesome's fg_minimize: muted, no bg
                    else if (W == F.focus)
                        fg = F.active;
                    else if (W->m_hints & Desktop::View::WINDOW_HINT_URGENT) {
                        P.rect(CELL, F.urgentBg);
                        fg = F.urgentFg;
                    }

                    // [4][icon][4][title] — awesome's item margins, icon on
                    // the bar's 3px-inset rhythm
                    const double ICON = P.h - 6;
                    double       tx   = x + 4;
                    if (const auto ITEX = appIcon(W->metadata().appID()); ITEX && ITEX->m_texID != 0)
                        P.texFit(ITEX, CBox{tx, box.y + 3, ICON, ICON});
                    tx += ICON + 4;

                    static std::string LBL; // reused; main thread only
                    Tasklist::label(W, LBL);
                    if (P.fp)
                        *P.fp = *P.fp * 1099511628211ULL + std::hash<std::string>{}(LBL);
                    const auto TEX = textTex(LBL, fg, P.pt, std::max(1, (int)std::round((ITEMW - (tx - x) - 4) * P.scale))); // floor >0: a non-positive width disables ellipsization and overflows the cell
                    if (TEX && TEX->m_texID != 0) {
                        const auto B = P.toPhys(CBox{tx, box.y, 1, P.h});
                        CBox       b{B.x, B.y + (B.h - TEX->m_size.y) / 2.0, TEX->m_size.x, TEX->m_size.y};
                        P.tex(TEX, b.round());
                    }

                    SHit h;
                    h.box    = CELL;
                    h.widget = this;
                    h.window = W;
                    P.hits->push_back(h);
                    x += ITEMW;
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
