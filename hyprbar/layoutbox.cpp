// hyprbar/layoutbox.cpp — awesome's layoutbox: the per-tag layout registry
// and its widget

#include "hyprbar.hpp"

namespace NHyprbar {

    // awesome's awful.layout: the ordered registry (order matters, like
    // awful.layout.layouts) and each workspace's index — per tag, exactly
    // awesome's model. Future layouts append here and take real effect
    // wherever they get implemented; the bar carries the state and the icon
    // (~/.config/hypr/icons/<name>.png).
    static const std::vector<const char*>          LAYOUTS = {"floating"};
    static std::unordered_map<WORKSPACEID, size_t> wsLayout;
    // keyed by the LAYOUTS literals themselves — pointer identity, no
    // per-frame string
    static std::unordered_map<const char*, SP<ITexture>> layoutTexs;
    static std::unordered_set<const char*>               layoutTexTried;

    static const char*                                   currentLayout(WORKSPACEID ws) {
        const auto IT = wsLayout.find(ws);
        return LAYOUTS[(IT == wsLayout.end() ? 0 : IT->second) % LAYOUTS.size()];
    }

    void layoutInc(int dir) {
        const auto MON = Desktop::focusState() ? Desktop::focusState()->monitor() : nullptr;
        if (!MON || !MON->m_activeWorkspace)
            return;
        const int64_t N   = (int64_t)LAYOUTS.size();
        auto&         IDX = wsLayout[MON->m_activeWorkspace->m_id];
        IDX               = (size_t)(((int64_t)IDX + dir % N + N) % N);
        barChanged();
    }

    void layoutboxExit() {
        wsLayout.clear();
        layoutTexs.clear();
        layoutTexTried.clear();
    }

    namespace {
        class CLayoutboxWidget : public IWidget {
          public:
            double fit(const SPaint& P, const SFrame&) override {
                return P.h;
            }
            void draw(const SPaint& P, const SFrame& F, const CBox& box) override {
                // the active workspace's layout icon; click/wheel cycles the
                // registry — with its single entry it is still the static
                // floating indicator it always was
                const char* NAME = currentLayout(F.ws ? F.ws->m_id : WORKSPACE_INVALID);
                auto&       TEX  = layoutTexs[NAME];
                if (!TEX && !layoutTexTried.contains(NAME)) {
                    if (!warming)
                        texStale = true; // an icon is a texture too: warm builds it
                    else {
                        layoutTexTried.insert(NAME);
                        if (const char* HOME = std::getenv("HOME"))
                            TEX = loadPng(std::string{HOME} + "/.config/hypr/icons/" + NAME + ".png");
                    }
                }
                if (TEX && TEX->m_texID != 0) {
                    // 3px inset, the bar's icon rhythm
                    const double S = std::round((P.h - 6) * P.scale);
                    const auto   B = P.toPhys(box);
                    CBox         b{B.x + (B.w - S) / 2.0, B.y + (B.h - S) / 2.0, S, S};
                    P.tex(TEX, b.round());
                }
                SHit h;
                h.box    = box;
                h.widget = this;
                P.hits->push_back(h);
            }
            void onHit(const SHit&, uint32_t bit, bool) override {
                // awesome's layoutbox buttons: left = next, right = previous
                if (bit == 1u || bit == 2u)
                    layoutInc(bit == 2u ? -1 : 1);
            }
            bool accumulatesScroll() const override {
                return true;
            }
            void onScrollSteps(int steps, PHLMONITOR) override {
                layoutInc(steps);
            }
        };
    } // namespace

    IWidget& layoutboxWidget() {
        static CLayoutboxWidget W;
        return W;
    }

} // namespace NHyprbar
