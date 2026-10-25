// awesome/shell/bell.cpp — the notification bell + badge. The monolith has
// no cross-module bus: the bell reads the notify MODEL's badge counts
// directly and repaints off its badgeChangedHook (fired from the model's
// single change funnel). A click toggles the center — a function call, not a
// proxy. The glyph is the Frost outline bell (core/glyph); the count rides
// beside it in an accent pill. Under Do Not Disturb the bell is struck
// through and the count is plain text — it includes the held queue, what
// waits for the resume.

#include "shell/shell.hpp"

#include "core/glyph.hpp"
#include "notify/model.hpp"

namespace NAwesome::Shell {

    // what the bell shows: the unread count, and whether DND holds
    struct SBellState {
        uint32_t count = 0;
        bool     dnd   = false;
        bool     operator==(const SBellState&) const = default;
    };
    static SBellState bellState() {
        namespace M             = NAwesome::Notify::Model;
        const auto [LIVE, KEPT] = M::badgeCounts();
        const bool DND          = M::suspendedNow();
        return {LIVE + KEPT + (DND ? M::heldCount() : 0), DND};
    }

    namespace Bell {
        // what the bar last warmed for: the funnel fires on every model
        // change (each step of a volume sweep), and only a moved count or
        // a DND flip changes what the bell draws
        static SBellState shown{UINT32_MAX, false};

        void init() {
            // the model's funnel fires this on the event loop after every
            // change that can move the badge: the bell warms + damages the
            // strip (a count change re-keys the badge texture). The hook is
            // registered before any model event can be dispatched (the bus
            // link opens in the notify module's init, which runs after this
            // module's init returns).
            shown                                       = {UINT32_MAX, false};
            NAwesome::Notify::Model::badgeChangedHook() = []() {
                const auto NOW = bellState();
                if (NOW == shown)
                    return;
                shown = NOW;
                barChanged();
            };
        }

        void exit() {
            NAwesome::Notify::Model::badgeChangedHook() = {};
        }
    } // namespace Bell

    // ---- the widget ----

    namespace {
        class CBellWidget : public IWidget {
          public:
            static constexpr double GLYPH = 15, GAP = 5, PILL_H = 16, PILL_PADX = 5;

            struct SLayout {
                SBellState   st;
                SP<ITexture> glyph, count;
                double       countW = 0, w = 0;
            };
            static SLayout layout(const SPaint& P, const SFrame& F) {
                SLayout L;
                L.st    = bellState();
                L.glyph = NAwesome::Glyph::tex(L.st.dnd ? NAwesome::Glyph::eGlyph::BELL_OFF : NAwesome::Glyph::eGlyph::BELL, F.fg, (int)std::lround(GLYPH * P.scale));
                L.w     = GLYPH;
                if (L.st.count == 0)
                    return L;
                const auto TXT = L.st.count > 99 ? std::string{"99+"} : std::to_string(L.st.count);
                if (L.st.dnd) { // held, not alerting: plain and quiet
                    L.count  = textTex(TXT, F.empty, P.pt, 0, "", 500);
                    L.countW = L.count ? L.count->m_size.x / P.scale : 0;
                } else {
                    L.count  = textTex(TXT, color(cfg().getColor("plugin:awesome:shell:col_on_active")), (int)std::lround(10.5 * P.scale), 0, "", 700);
                    L.countW = std::max(PILL_H, (L.count ? L.count->m_size.x / P.scale : 0) + 2 * PILL_PADX);
                }
                L.w += GAP + L.countW;
                return L;
            }

            double fit(const SPaint& P, const SFrame& F) override {
                return layout(P, F).w;
            }

            void draw(const SPaint& P, const SFrame& F, const CBox& box) override {
                const auto L = layout(P, F);
                if (P.fp) // the count can move without a bar event
                    *P.fp = *P.fp * 1099511628211ULL + L.st.count * 2 + L.st.dnd;
                P.texIn(L.glyph, CBox{box.x, box.y, GLYPH, P.h});
                if (L.st.count > 0) {
                    const CBox CB{box.x + GLYPH + GAP, box.y + (P.h - PILL_H) / 2, L.countW, PILL_H};
                    if (!L.st.dnd) // the one accent fill in the shell: the unread count
                        P.rect(CB, color(cfg().getColor("plugin:awesome:shell:col_active")), (int)std::lround(PILL_H / 2 * P.scale), 2.f);
                    P.texIn(L.count, CB);
                }
                SHit h;
                h.box    = box;
                h.widget = this;
                P.hits->push_back(h);
            }

            void onHit(const SHit& hit, uint32_t bit, bool) override {
                if (bit != 1u)
                    return;
                // the shared click door: deferred (the monolith's center is a
                // function call, not a proxy round trip); the shade opens on
                // the clicked bar's monitor — the focus hasn't moved there
                // yet (the strip swallowed the press before the compositor
                // saw it)
                NAwesome::Notify::queueCenterToggle(hit.mon.lock());
            }
        };
    } // namespace

    IWidget& bellWidget() {
        static CBellWidget W;
        return W;
    }

} // namespace NAwesome::Shell
