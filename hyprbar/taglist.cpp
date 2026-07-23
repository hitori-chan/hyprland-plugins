// hyprbar/taglist.cpp — the nine kanji tags in the left island.
//
// State matrix (the compact-islands contract): the viewed tag = accent text
// on an accent-dim fill (radius 8); an urgent tag = the kanji in the urgent
// color and NOTHING else (no dot, no fill); occupied = full ink; empty =
// muted. Viewing a tag clears its urgency, like Android's badge — the
// compositor only clears m_isUrgent on focus, so a suppression set carries
// the "seen" state for windows the view didn't focus.

#include "hyprbar.hpp"

namespace NHyprbar {

    static const char* KANJI[9] = {"一", "二", "三", "四", "五", "六", "七", "八", "九"};

    static std::unordered_set<void*> urgentSeen; // windows whose urgency the user has viewed

    namespace Taglist {
        void noteViewed(const PHLWORKSPACE& ws) {
            if (!ws)
                return;
            bool changed = false;
            for (const auto& W : Desktop::windowState()->windows())
                if (W->m_isMapped && W->m_isUrgent && W->m_workspace && W->m_workspace->m_id == ws->m_id)
                    changed = urgentSeen.insert(W.get()).second || changed;
            if (changed)
                barChanged();
        }
        void forget(void* w) {
            urgentSeen.erase(w);
        }
        void exit() {
            urgentSeen.clear();
        }
        bool seen(void* w) {
            return urgentSeen.contains(w);
        }
    } // namespace Taglist

    namespace {
        class CTaglistWidget : public IWidget {
          public:
            struct SCell {
                CHyprColor fg;
                bool       fill = false; // the active tag's accent-dim pill->rect
            };
            static SCell cell(const SFrame& F, int i) {
                if (F.ws && F.ws->m_id == i)
                    return {F.active, true};
                if (F.urgent[i])
                    return {F.urgentFg, false}; // the urgent marker is the color alone
                if (F.windows[i] > 0)
                    return {F.fg, false};
                return {color(cfg.colEmpty), false};
            }

            double fit(const SPaint& P, const SFrame& F) override {
                // min-w 26 cells, gap 2, 6px island padding each side — the
                // warm builds exactly the color variants the draw will paint
                double w = 6;
                for (int i = 1; i <= 9; i++) {
                    const auto   TEX = textTex(KANJI[i - 1], cell(F, i).fg, P.pt);
                    const double TW  = TEX ? TEX->m_size.x / P.scale : P.h;
                    w += std::max(26.0, TW + 8) + (i < 9 ? 2 : 0);
                }
                return w + 6;
            }

            void draw(const SPaint& P, const SFrame& F, const CBox& box) override {
                double     x     = box.x + 6;
                const auto RCELL = (int)std::lround(8 * P.scale);
                for (int i = 1; i <= 9; i++) {
                    const auto   C   = cell(F, i);
                    const auto   TEX = textTex(KANJI[i - 1], C.fg, P.pt);
                    const double TW  = TEX ? TEX->m_size.x / P.scale : P.h;
                    const CBox   CELL{x, box.y + (box.h - 24) / 2, std::max(26.0, TW + 8), 24};

                    if (C.fill)
                        P.rect(CELL, F.activeBg, RCELL);
                    else if (barHover.widget == this && barHover.tag == i)
                        P.rect(CELL, tFill2(), RCELL);

                    P.texIn(TEX, CELL);

                    SHit h;
                    h.box    = CELL;
                    h.widget = this;
                    h.tag    = i;
                    P.hits->push_back(h);
                    x += CELL.w + 2;
                }
            }

            void onHit(const SHit& h, uint32_t bit, bool super) override {
                // click views the tag, Mod+click sends the focused window
                // there without following
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
                // view next/previous, wrapping
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
