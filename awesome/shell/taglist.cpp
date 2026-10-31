// Awesome's taglist: the nine kanji tags and their widget

#include "shell/shell.hpp"

#include "core/theme.hpp"

#include <array>

namespace NAwesome::Shell {

    static const char* KANJI[9] = {"一", "二", "三", "四", "五", "六", "七", "八", "九"};

    namespace {
        class CTaglistWidget : public IWidget {
          public:
            // The viewed tag stands on awesome's bg_focus: a flat cell the
            // bar's full height (white 8 %, square), its text accent at
            // semibold. The rest lives in the text alone: an urgent tag
            // red, an occupied one the bar's text, an empty one dim — no
            // occupancy marks. A hovered tag's text lifts a step (dim ->
            // the bar's text -> white; the accent and the red toward
            // white), never a ground. Hover damages but never rewarms
            // (crash class 4), so the warm builds both variants of every
            // tag.
            static constexpr double CELL_W = 26, CELL_H = 22, GAP = 2;
            static constexpr int    WEIGHT = 400, WEIGHT_VIEWED = 600;

            static bool             viewed(const SFrame& F, int i) {
                return F.ws && F.ws->numberedID() == i;
            }
            static CHyprColor ink(const SFrame& F, int i, bool hover) {
                if (viewed(F, i))
                    return hover ? F.activeHover : F.active;
                if (F.urgent[i])
                    return hover ? F.urgentHover : F.urgentFg;
                if (F.windows[i] > 0)
                    return hover ? NAwesome::tTitle() : F.fg;
                return hover ? F.fg : F.empty;
            }
            static SP<ITexture> label(const SPaint& P, const SFrame& F, int i, bool hover) {
                return textTex(KANJI[i - 1], ink(F, i, hover), P.pt, 0, "", viewed(F, i) ? WEIGHT_VIEWED : WEIGHT);
            }

            double fit(const SPaint& P, const SFrame& F) override {
                // the warm builds exactly the variants the draw paints (a
                // draw's lookups would only repeat the draw's own)
                if (P.warm)
                    for (int i = 1; i <= 9; i++) {
                        label(P, F, i, false);
                        label(P, F, i, true);
                    }
                return 9 * CELL_W + 8 * GAP;
            }

            void draw(const SPaint& P, const SFrame& F, const CBox& box) override {
                double x = box.x;
                for (int i = 1; i <= 9; i++) {
                    const CBox CELL{x, box.y + (P.h - CELL_H) / 2, CELL_W, CELL_H};
                    const bool HOVER = barHover.widget == this && barHover.tag == i && barHover.mon == P.mon->m_id;
                    if (viewed(F, i))
                        P.rect(CBox{x, box.y, CELL_W, P.h}, NAwesome::tCell());
                    P.texIn(label(P, F, i, HOVER), CELL);

                    // the hit spans the bar's height and the gap: no dead
                    // pixel between two tags
                    SHit h;
                    h.box    = CBox{x, box.y, CELL_W + (i < 9 ? GAP : 0), P.h};
                    h.widget = this;
                    h.tag    = i;
                    P.hits->push_back(h);
                    x += CELL_W + GAP;
                }
            }

            void onHit(const SHit& h, uint32_t bit, bool super) override {
                // awesome's taglist buttons: click views the tag, Mod+click
                // sends the focused window there without following. Right-click
                // (viewtoggle) and Mod+right (toggle_tag) have no analog — a
                // window sits on exactly one workspace here.
                if (bit != 1u)
                    return;
                const auto MON = h.mon.lock();
                if (!MON)
                    return;

                // Workspace IDs are global in Hyprland. Create a missing tag
                // on the monitor whose bar was clicked, but never create a
                // second workspace for an ID already owned elsewhere. A new
                // one is ready (workspace.created) once the switch or the
                // move landed, as the native string actions make it.
                auto ws = State::workspaceState()->find(State::Workspace::STarget{.id = Workspace::SWorkspaceNumberedID{(uint32_t)h.tag}, .address = std::to_string(h.tag)});
                if (!ws)
                    ws = State::workspaceState()->createNumbered(Workspace::SWorkspaceNumberedID{(uint32_t)h.tag}, MON, {}, !super);
                if (!ws)
                    return;
                if (super) {
                    std::ignore = Config::Actions::moveToWorkspace(ws, true); // silent — move_to_tag never followed
                    ws->ready();
                    return;
                }

                // A tag owned by another monitor is not a local target. Do
                // not redirect the click through changeWorkspace(), which
                // would focus and warp to that monitor unexpectedly.
                if (ws->m_monitor.lock() != MON || MON->m_activeWorkspace == ws)
                    return;
                if (Desktop::focusState() && Desktop::focusState()->monitor() == MON)
                    std::ignore = Config::Actions::changeWorkspace(ws);
                else
                    MON->changeWorkspace(ws, false, true, true); // switch the background output without stealing focus
                ws->ready();
            }

            bool accumulatesScroll() const override {
                return true;
            }
            void onScrollSteps(int steps, PHLMONITOR mon) override {
                if (!mon)
                    return;
                // awful.tag.viewnext/viewprev, wrapping through THIS screen's
                // tags. Tags are global here: one another monitor shows or
                // holds is not this screen's, so the wheel skips it instead
                // of stopping there (a missing tag is created here, so it
                // counts).
                auto CUR = mon->m_activeWorkspace ? mon->m_activeWorkspace->numberedID().value_or(1) : 1;
                if (CUR < 1 || CUR > 9)
                    CUR = 1;
                const auto FIND = [](int t) {
                    return State::workspaceState()->find(State::Workspace::STarget{.id = Workspace::SWorkspaceNumberedID{(uint32_t)t}, .address = std::to_string(t)});
                };
                std::array<int, 9> local{};
                long               count = 0, at = 0;
                for (int t = 1; t <= 9; ++t) {
                    if (const auto W = FIND(t); W && W->m_monitor.lock() != mon)
                        continue;
                    if ((uint32_t)t == CUR)
                        at = count;
                    local[count++] = t;
                }
                if (count == 0)
                    return;
                const int T  = local[((at + steps) % count + count) % count];
                auto      ws = FIND(T);
                if (!ws)
                    ws = State::workspaceState()->createNumbered(Workspace::SWorkspaceNumberedID{(uint32_t)T}, mon);
                if (!ws || ws->m_monitor.lock() != mon || mon->m_activeWorkspace == ws)
                    return;
                if (Desktop::focusState() && Desktop::focusState()->monitor() == mon)
                    std::ignore = Config::Actions::changeWorkspace(ws);
                else
                    mon->changeWorkspace(ws, false, true, true);
                ws->ready();
            }
        };
    } // namespace

    IWidget& taglistWidget() {
        static CTaglistWidget W;
        return W;
    }

    std::string tagName(const PHLWORKSPACE& ws) {
        const auto N = ws ? ws->numberedID() : std::nullopt; // none for a special or named workspace
        if (!N || *N == 0)
            return {};
        return *N <= 9 ? KANJI[*N - 1] : std::to_string(*N);
    }

} // namespace NAwesome::Shell
