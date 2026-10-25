// awesome/shell/bell.cpp — the notification bell. The monolith has no
// cross-module bus: the bell reads the notify MODEL directly and repaints off
// its badgeChangedHook (fired from the model's single change funnel). A click
// toggles the center — a function call, not a proxy. No count: SF Symbols'
// bell.badge, a green dot on the bell's shoulder while the center holds a
// card it has not shown (Model::unseen). Under Do Not Disturb the bell is the
// moon, as macOS shows a Focus, dotted while cards wait in the DND queue.

#include "shell/shell.hpp"

#include "core/glyph.hpp"
#include "notify/model.hpp"

#include <optional>

namespace NAwesome::Shell {

    // what the bell shows: the dot, whether DND holds, whether its center is open
    struct SBellState {
        bool dot  = false;
        bool dnd  = false;
        bool open = false;
        bool operator==(const SBellState&) const = default;
    };
    static SBellState bellState() {
        namespace M     = NAwesome::Notify::Model;
        const bool DND  = M::suspendedNow();
        const bool OPEN = NAwesome::Notify::centerVisible();
        // the open center is the place the dot points at: it never shows over
        // it; the bell lights instead, as macOS lights the menu-bar item whose
        // panel is open
        const bool DOT = !OPEN && (M::unseen() || (DND && M::heldCount() > 0));
        return {DOT, DND, OPEN};
    }

    namespace Bell {
        // what the bar last warmed for: the funnel fires on every model
        // change (each step of a volume sweep), and only the dot or a DND
        // flip changes what the bell draws
        static std::optional<SBellState> shown;

        void init() {
            // the model's funnel fires this on the event loop after every
            // change that can move the dot: the bell warms + damages the
            // strip. The hook is registered before any model event can be
            // dispatched (the bus link opens in the notify module's init,
            // which runs after this module's init returns).
            shown.reset();
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
            // the glyph's own 16 px grid: whole pixels at 1x (core/glyph_shapes.hpp)
            static constexpr double GLYPH = 16;

            double fit(const SPaint&, const SFrame&) override {
                return GLYPH;
            }

            void draw(const SPaint& P, const SFrame& F, const CBox& box) override {
                using NAwesome::Glyph::eGlyph;
                const auto ST = bellState();
                if (P.fp) // the state can move without a bar event
                    *P.fp = *P.fp * 1099511628211ULL + ST.dot * 4 + ST.dnd * 2 + ST.open;
                const int  PX   = (int)std::lround(GLYPH * P.scale);
                const auto BASE = ST.dnd ? (ST.dot ? eGlyph::MOON_BADGE : eGlyph::MOON) : (ST.dot ? eGlyph::BELL_BADGE : eGlyph::BELL);
                const CBox CELL{box.x, box.y, GLYPH, P.h};
                P.texIn(NAwesome::Glyph::tex(BASE, ST.open ? F.active : F.fg, PX), CELL);
                // the dot is its own glyph on the same grid, so the two
                // rasters register to the pixel in the same cell
                if (ST.dot)
                    P.texIn(NAwesome::Glyph::tex(eGlyph::BADGE_DOT, color(cfg().getColor("plugin:awesome:shell:col_dot")), PX), CELL);
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
