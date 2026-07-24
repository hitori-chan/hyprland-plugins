// hyprbar/taglist.cpp — awesome's taglist: the nine kanji tags and their widget

#include "hyprbar.hpp"

namespace NHyprbar {

    static const char* KANJI[9] = {"一", "二", "三", "四", "五", "六", "七", "八", "九"};

    namespace {
        class CTaglistWidget : public IWidget {
          public:
            // awesome's exact state matrix: the viewed tag gets the focus
            // colors, an urgent one the urgent colors, and everything else —
            // occupied or empty — the plain text color; occupancy shows as
            // the little corner square instead, filled when the tag holds
            // the focused window, hollow otherwise.
            struct SCell {
                CHyprColor fg, bg;
                bool       hasBg = false;
            };
            static SCell cell(const SFrame& F, int i) {
                SCell c{.fg = F.fg};
                if (F.ws && F.ws->m_id == i) {
                    c.bg    = F.activeBg;
                    c.hasBg = true;
                    c.fg    = F.active;
                } else if (F.urgent[i]) {
                    c.bg    = F.urgentBg;
                    c.hasBg = true;
                    c.fg    = F.urgentFg;
                }
                return c;
            }

            double fit(const SPaint& P, const SFrame& F) override {
                // awesome's tag button width (text + 12) — the warm pass
                // builds exactly the color variants the draw will paint, so
                // the measure walks the same state matrix
                double w = 0;
                for (int i = 1; i <= 9; i++) {
                    const auto TEX = textTex(KANJI[i - 1], cell(F, i).fg, P.pt);
                    w += (TEX ? TEX->m_size.x / P.scale : P.h) + 12;
                }
                return w;
            }

            void draw(const SPaint& P, const SFrame& F, const CBox& box) override {
                double     x  = box.x;
                const auto SQ = std::round(P.h * 4.0 / 19.0); // the 4px square of a 19px wibar, scaled
                for (int i = 1; i <= 9; i++) {
                    const auto   C   = cell(F, i);
                    const auto   TEX = textTex(KANJI[i - 1], C.fg, P.pt);
                    const double TW  = TEX ? TEX->m_size.x / P.scale : P.h;
                    const CBox   CELL{x, box.y, TW + 12, P.h};

                    if (C.hasBg)
                        P.rect(CELL, C.bg);

                    if (F.windows[i] > 0) {
                        if (F.focusWs == i)
                            P.rect(CBox{x, box.y, SQ, SQ}, F.squareSel);
                        else { // hollow
                            P.rect(CBox{x, box.y, SQ, 1}, F.squareUnsel);
                            P.rect(CBox{x, box.y + SQ - 1, SQ, 1}, F.squareUnsel);
                            P.rect(CBox{x, box.y, 1, SQ}, F.squareUnsel);
                            P.rect(CBox{x + SQ - 1, box.y, 1, SQ}, F.squareUnsel);
                        }
                    }

                    P.texIn(TEX, CELL);

                    SHit h;
                    h.box    = CELL;
                    h.widget = this;
                    h.tag    = i;
                    P.hits->push_back(h);
                    x += CELL.w;
                }
            }

            void onHit(const SHit& h, uint32_t bit, bool super) override {
                // awesome's taglist buttons: click views the tag, Mod+click
                // sends the focused window there without following. Right-click
                // (viewtoggle) and Mod+right (toggle_tag) have no analog — a
                // window sits on exactly one workspace here.
                if (bit != 1u)
                    return;
                if (super) {
                    auto ws = State::workspaceState()->query().id(h.tag).run();
                    if (!ws)
                        if (const auto M = Desktop::focusState() ? Desktop::focusState()->monitor() : nullptr)
                            ws = State::workspaceState()->create(h.tag, M->m_id);
                    if (ws)
                        std::ignore = Config::Actions::moveToWorkspace(ws, true); // silent — move_to_tag never followed
                } else
                    std::ignore = Config::Actions::changeWorkspace(std::to_string(h.tag));
            }

            bool accumulatesScroll() const override {
                return true;
            }
            void onScrollSteps(int steps, PHLMONITOR mon) override {
                // awful.tag.viewnext/viewprev, wrapping
                auto CUR = mon->m_activeWorkspace ? mon->m_activeWorkspace->m_id : 1;
                if (CUR < 1 || CUR > 9)
                    CUR = 1;
                const int T = (int)((CUR - 1 + (steps % 9) + 9) % 9) + 1;
                std::ignore = Config::Actions::changeWorkspace(std::to_string(T));
            }
        };
    } // namespace

    IWidget& taglistWidget() {
        static CTaglistWidget W;
        return W;
    }

} // namespace NHyprbar
