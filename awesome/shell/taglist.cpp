// awesome/shell/taglist.cpp — awesome's taglist: the nine kanji tags and their widget

#include "shell/shell.hpp"

#include "core/theme.hpp"

#include <array>

namespace NAwesome::Shell {

    static const char* KANJI[9] = {"一", "二", "三", "四", "五", "六", "七", "八", "九"};

    namespace {
        class CTaglistWidget : public IWidget {
          public:
            // The Frost state matrix: the viewed tag is accent text on a
            // dark ground; an urgent one red text; an occupied one the bar's
            // text, an empty one dim. No occupancy marks: the text's
            // strength says it. A hovered tag gets a light ground.
            static constexpr double CELL_W = 26, CELL_H = 22, GAP = 2;

            static CHyprColor ink(const SFrame& F, int i) {
                if (F.ws && F.ws->numberedID() == i)
                    return F.active;
                if (F.urgent[i])
                    return F.urgentFg;
                return F.windows[i] > 0 ? F.fg : F.empty;
            }

            double fit(const SPaint& P, const SFrame& F) override {
                // the warm builds exactly the variants the draw paints
                for (int i = 1; i <= 9; i++)
                    textTex(KANJI[i - 1], ink(F, i), P.pt, 0, "", 500);
                return 9 * CELL_W + 8 * GAP;
            }

            void draw(const SPaint& P, const SFrame& F, const CBox& box) override {
                const int   R  = (int)std::lround(NAwesome::Theme::RAD_ROW * P.scale);
                const float RP = (float)NAwesome::Theme::ROUNDING_POWER;
                double      x  = box.x;
                for (int i = 1; i <= 9; i++) {
                    const CBox CELL{x, box.y + (P.h - CELL_H) / 2, CELL_W, CELL_H};
                    if (F.ws && F.ws->numberedID() == i)
                        P.rect(CELL, F.activeBg, R, RP);
                    else if (barHover.widget == this && barHover.tag == i && barHover.mon == P.mon->m_id)
                        P.rect(CELL, NAwesome::tGroundHover(), R, RP);
                    P.texIn(textTex(KANJI[i - 1], ink(F, i), P.pt, 0, "", 500), CELL);

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
                // second workspace for an ID already owned elsewhere.
                auto ws = State::workspaceState()->find(State::Workspace::STarget{.id = Workspace::SWorkspaceNumberedID{(uint32_t)h.tag}, .address = std::to_string(h.tag)});
                if (!ws)
                    ws = State::workspaceState()->createNumbered(Workspace::SWorkspaceNumberedID{(uint32_t)h.tag}, MON);
                if (!ws)
                    return;
                if (super) {
                    std::ignore = Config::Actions::moveToWorkspace(ws, true); // silent — move_to_tag never followed
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
            }
        };
    } // namespace

    IWidget& taglistWidget() {
        static CTaglistWidget W;
        return W;
    }

} // namespace NAwesome::Shell
