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
                // min-w 26 cells; islands pad 6 each side with gap-2 pills,
                // strip runs contiguous full-height cells from the corner —
                // the warm builds exactly the color variants the draw will paint
                const bool STRIP = stripMode();
                double     w     = STRIP ? 0 : 6;
                for (int i = 1; i <= 9; i++) {
                    const auto   TEX = textTex(KANJI[i - 1], cell(F, i).fg, P.pt);
                    const double TW  = TEX ? TEX->m_size.x / P.scale : P.h;
                    w += std::max(26.0, TW + 8) + (!STRIP && i < 9 ? 2 : 0);
                }
                return w + (STRIP ? 0 : 6);
            }

            void draw(const SPaint& P, const SFrame& F, const CBox& box) override {
                const bool STRIP = stripMode();
                double     x     = box.x + (STRIP ? 0 : 6);
                const auto RCELL = STRIP ? 0 : (int)std::lround(8 * P.scale);
                for (int i = 1; i <= 9; i++) {
                    const auto   C   = cell(F, i);
                    const auto   TEX = textTex(KANJI[i - 1], C.fg, P.pt);
                    const double TW  = TEX ? TEX->m_size.x / P.scale : P.h;
                    const double CW  = std::max(26.0, TW + 8);
                    const CBox   CELL = STRIP ? CBox{x, box.y, CW, box.h} : CBox{x, box.y + (box.h - 24) / 2, CW, 24};

                    if (C.fill) {
                        P.rect(CELL, F.activeBg, RCELL);
                        if (STRIP) // the active pill retires: full-height wash + a 2px accent baseline
                            P.rect(CBox{CELL.x, CELL.y + CELL.h - 2, CELL.w, 2}, F.active);
                    } else if (barHover.widget == this && barHover.tag == i)
                        P.rect(CELL, tFill2(), RCELL);

                    P.texIn(TEX, CELL);

                    SHit h;
                    h.box    = CELL;
                    h.widget = this;
                    h.tag    = i;
                    P.hits->push_back(h);
                    x += CELL.w + (STRIP ? 0 : 2);
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
