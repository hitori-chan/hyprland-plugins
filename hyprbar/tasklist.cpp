// hyprbar/tasklist.cpp — awesome's tasklist: arrival-order bookkeeping, the
// state-marker labels and the widget filling the bar's middle

#include "hyprbar.hpp"

namespace NHyprbar {

    // tasklist order: awesome lists clients in ARRIVAL order, stable across
    // raises — windowState()'s list is the Z-order, so the bar keeps its own
    // sequence
    static std::unordered_map<void*, uint64_t> winSeq;
    static uint64_t                            winSeqNext = 0;

    namespace Tasklist {
        uint64_t seqOf(void* w) {
            const auto [SEQ, NEW] = winSeq.try_emplace(w, winSeqNext);
            if (NEW)
                winSeqNext++;
            return SEQ->second;
        }

        void forget(void* w) {
            winSeq.erase(w);
        }

        void exit() {
            winSeq.clear();
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
            maximized = std::ranges::contains(w->m_xdgSurface->m_toplevel->m_pendingApply.states, XDG_TOPLEVEL_STATE_MAXIMIZED);
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
                // awesome tasklist behavior: the windows split the WHOLE free
                // strip between taglist and tray — one window = one huge item
                const double ITEMW = box.w / (double)F.tasks->size();

                double       x = box.x;
                for (const auto& [SEQ, W] : *F.tasks) {
                    const CBox CELL{x, box.y, ITEMW, P.h};
                    // the old theme: the focused task is cyan TEXT on the plain
                    // bar (tasklist_bg_focus = bg_normal, no box); urgent gets
                    // the urgent bg — and focus wins over urgent, like awesome
                    CHyprColor fg = F.fg;
                    if (W == F.focus)
                        fg = F.active;
                    else if (W->m_isUrgent) {
                        P.rect(CELL, F.urgentBg);
                        fg = F.urgentFg;
                    }

                    // [4][icon][4][title] — awesome's item margins, icon on
                    // the bar's 3px-inset rhythm
                    const double ICON = P.h - 6;
                    double       tx   = x + 4;
                    if (const auto ITEX = appIcon(W->m_class); ITEX && ITEX->m_texID != 0) {
                        const auto B = P.toPhys(CBox{tx, box.y + 3, ICON, ICON});
                        P.tex(ITEX, B);
                    } else
                        P.texIn(textTex(letterOf(W->m_class), F.active, P.pt), CBox{tx, box.y, ICON, P.h});
                    tx += ICON + 4;

                    static std::string LBL; // reused; main thread only
                    Tasklist::label(W, LBL);
                    if (P.fp)
                        *P.fp = *P.fp * 1099511628211ULL + std::hash<std::string>{}(LBL);
                    const auto TEX = textTex(LBL, fg, P.pt, (int)std::round((ITEMW - (tx - x) - 4) * P.scale));
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
                if (const auto W = h.window.lock(); W && W->m_isMapped) {
                    // Not Actions::focus(): that goes through FocusState with
                    // surface=nullptr, and its already-focused guard compares
                    // (window, surface) == (m_focusWindow, m_focusSurface).
                    // When a popup/layer that held the keyboard dies while the
                    // pointer sits on the bar (moves swallowed = FFM can't
                    // heal), m_focusSurface is left empty with m_focusWindow
                    // still set — nullptr == empty matches, the guard returns
                    // before the raise AND before keyboard focus, and the
                    // click looks dead until some other window gets focused.
                    // So: raise explicitly, then focus with the window's real
                    // surface — the guard can never match a half-focused
                    // window, and a focused-but-obscured one still raises.
                    Desktop::windowState()->raise(W);
                    Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_DISPATCH_FOCUSWINDOW, W->wlSurface()->resource());
                }
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
                    if (isTaskOn(W, WS))
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
                Desktop::windowState()->raise(W);
                Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_DISPATCH_FOCUSWINDOW, W->wlSurface()->resource());
            }
        };
    } // namespace

    IWidget& tasklistWidget() {
        static CTasklistWidget W;
        return W;
    }

} // namespace NHyprbar
