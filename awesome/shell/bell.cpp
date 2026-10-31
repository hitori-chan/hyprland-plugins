// awesome/shell/bell.cpp — the door into the notify column: the
// notification bell. The monolith has no cross-module bus: it reads the
// notify MODEL directly and repaints off its badgeChangedHook (fired from
// the model's single change funnel). A click toggles the center — a
// function call, not a proxy. The bell has no count: a dot on its shoulder
// while the center holds a card it has not shown (Model::unseen). Under Do
// Not Disturb the bell is the moon, dotted while cards wait in the DND
// queue. A right click opens the notification options, flat: quiet
// automatically (over a fullscreen app, while sharing the screen), and the
// apps and chats the card menus silenced, each a click from back on. Do Not
// Disturb's one home is the center's moon; the bell echoes it. The bell
// sits on a white pill while the center or its options menu is open. The
// accent stays text for where you are; the bell's dot is its one mark.

#include "shell/shell.hpp"

#include "core/glyph.hpp"
#include "core/theme.hpp"
#include "core/walltimer.hpp"
#include "notify/model.hpp"

#include <optional>

namespace NAwesome::Shell {

    // what the bell shows: the dot, whether DND holds, the center open
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
        // it; the bell sits on its pill instead
        const bool DOT = !OPEN && (M::unseen() || (DND && M::heldCount() > 0));
        return {DOT, DND, OPEN};
    }

    namespace Bell {
        // what the bar last warmed for: the funnel fires on every model
        // change (each step of a volume sweep), and only the dot, a DND
        // flip or the center opening changes what the bell draws
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

    // ---- the options menu ----

    namespace {
        namespace M = NAwesome::Notify::Model;
        namespace P = NAwesome::Notify::Policy;

        Menu::SEntry row(std::string label, std::function<void()> action) {
            Menu::SEntry e{.display = std::move(label)};
            e.action = std::move(action);
            return e;
        }

        Menu::SEntry separator() {
            return {.separator = true};
        }

        Menu::SEntry header(std::string label) {
            Menu::SEntry e{.display = std::move(label)};
            e.header = true;
            return e;
        }

        // The options, flat (Do Not Disturb is the center's moon): the
        // two switches that quiet the banners by themselves, checked while
        // on (both start off); then every switch the card menus turned off,
        // a row each — the app's icon, its name, what is off — a click from
        // back on (the hint says so while lit).
        std::vector<Menu::SEntry> optionRows() {
            namespace Q = NAwesome::Notify::Quiet;
            std::vector<Menu::SEntry> rows;
            rows.push_back(header("Quiet Automatically"));
            for (const auto& [LABEL, OPT] : {std::pair{"When an App Is Fullscreen", eOption::QUIET_FULLSCREEN}, {"While Sharing the Screen", eOption::QUIET_SHARING}}) {
                const bool ON = Q::on(OPT);
                auto       r  = row(LABEL, [OPT, ON]() { Q::set(OPT, !ON); });
                r.toggle      = Menu::TG_CHECK;
                r.toggleState = ON;
                rows.push_back(std::move(r));
            }
            rows.push_back(separator());
            rows.push_back(header("Silenced Apps"));
            const auto SILENCED = P::silenced();
            for (const auto& S : SILENCED) {
                auto r = row(S.label.empty() ? S.key : S.label, [S]() { P::restore(S); });
                r.detail = S.until > 0                                        ? "Muted until " + untilText(S.until) :
                    (S.flags & P::BANNERS_OFF) && (S.flags & P::SOUNDS_OFF) ? std::string{"Banners and sounds off"} :
                    (S.flags & P::BANNERS_OFF)                              ? std::string{"Banners off"} :
                                                                              std::string{"Sounds off"};
                r.hint   = S.until > 0 ? "Unmute" : "Turn On";
                // a chat's key leads with its app's (policy.cpp chatKey)
                r.iconRetry = S.key.substr(0, S.key.find('\x1f'));
                r.iconOfApp = true;
                rows.push_back(std::move(r));
            }
            if (SILENCED.empty()) {
                auto none    = row("None", {});
                none.enabled = false;
                rows.push_back(std::move(none));
            }
            return rows;
        }
    } // namespace

    // ---- the widget ----

    namespace {
        // the glyph's own 16 px grid: whole pixels at 1x (core/glyph_shapes.hpp)
        constexpr double GLYPH = 16;

        // the open door: the pill under the glyph, 4 px past it either side,
        // 22 tall on the strip's middle (a rect: nothing to warm)
        void doorPill(const SPaint& P, const CBox& box) {
            constexpr double W = 24, H = 22;
            P.rect(CBox{box.x + (GLYPH - W) / 2, box.y + (P.h - H) / 2, W, H}, NAwesome::tPill(), (int)std::lround(NAwesome::Theme::RAD_ROW * P.scale), 2.f);
        }

        class CBellWidget : public IWidget {
          public:

            double fit(const SPaint&, const SFrame&) override {
                return GLYPH;
            }

            void draw(const SPaint& P, const SFrame& F, const CBox& box) override {
                using NAwesome::Glyph::eGlyph;
                const auto ST = bellState();
                // the options menu is a door of its own bar's bell
                const bool OPEN = ST.open || (Menu::isOpen && Menu::opener == this && Menu::mon.lock() == P.mon);
                if (P.fp) // the state can move without a bar event
                    *P.fp = *P.fp * 1099511628211ULL + ST.dot * 4 + ST.dnd * 2 + OPEN;
                const int  PX   = (int)std::lround(GLYPH * P.scale);
                const auto BASE = ST.dnd ? (ST.dot ? eGlyph::MOON_BADGE : eGlyph::MOON) : (ST.dot ? eGlyph::BELL_BADGE : eGlyph::BELL);
                const CBox CELL{box.x, box.y, GLYPH, P.h};
                if (OPEN)
                    doorPill(P, box);
                P.texIn(NAwesome::Glyph::tex(BASE, F.fg, PX), CELL);
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
                if (bit == 2u) { // the options, built as they stand now
                    Menu::openLocal(optionRows(), hit.clickX, hit.mon, 0, this);
                    return;
                }
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
