// awesome/shell/bell.cpp — the notification bell. The monolith has no
// cross-module bus: the bell reads the notify MODEL directly and repaints off
// its badgeChangedHook (fired from the model's single change funnel). A click
// toggles the center — a function call, not a proxy. No count: SF Symbols'
// bell.badge, a green dot on the bell's shoulder while the center holds a
// card it has not shown (Model::unseen). Under Do Not Disturb the bell is the
// moon, as macOS shows a Focus, dotted while cards wait in the DND queue.
// A right click opens the notification options: Do Not Disturb for a while,
// quiet automatically (over a fullscreen app, while sharing the screen), and
// the apps and chats the card menus silenced, each a click from back on.

#include "shell/shell.hpp"

#include "core/glyph.hpp"
#include "notify/model.hpp"

#include <ctime>
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

    // ---- the options menu ----

    namespace {
        namespace M = NAwesome::Notify::Model;
        namespace P = NAwesome::Notify::Policy;

        // the local date's day number: noon keeps a DST shift off the edge
        int dayOf(time_t t) {
            std::tm d{};
            localtime_r(&t, &d);
            d.tm_hour  = 12;
            d.tm_min   = 0;
            d.tm_sec   = 0;
            d.tm_isdst = -1;
            return (int)(std::mktime(&d) / 86400);
        }

        // when something ends, as the bar's clock writes time: "14:30"
        // today, "Tomorrow 08:00", the weekday within a week, else the date
        std::string untilText(int64_t end) {
            const time_t T = (time_t)end;
            std::tm      at{};
            localtime_r(&T, &at);
            const int   DAYS = dayOf(T) - dayOf(std::time(nullptr));
            const char* FMT  = DAYS <= 0 ? "%H:%M" : DAYS == 1 ? "Tomorrow %H:%M" : DAYS < 7 ? "%A %H:%M" : "%b %-d %H:%M";
            char        buf[64];
            return std::strftime(buf, sizeof buf, FMT, &at) ? buf : "";
        }

        Menu::SEntry row(std::string label, std::function<void()> action) {
            Menu::SEntry e{.label = label, .display = std::move(label)};
            e.action = std::move(action);
            return e;
        }

        Menu::SEntry separator() {
            return {.separator = true};
        }

        // Do Not Disturb ▸: on, it says until when and offers Turn Off
        // first; the lengths start it anew from now
        Menu::SEntry dndRow() {
            const bool ON  = M::suspendedNow();
            const auto END = M::suspendEnd();
            auto       e   = row(END > 0 ? "Do Not Disturb Until " + untilText(END) : "Do Not Disturb", {});
            e.submenu      = true;
            e.toggle       = Menu::TG_CHECK;
            e.toggleState  = ON;
            if (ON) {
                e.children.push_back(row("Turn Off", []() { M::setSuspend(0); }));
                e.children.push_back(separator());
            }
            for (const auto& [LABEL, MINUTES] : {std::pair{"For 30 Minutes", 30}, {"For 1 Hour", 60}, {"For 3 Hours", 180}})
                e.children.push_back(row(LABEL, [MINUTES]() { M::setSuspend((int64_t)std::time(nullptr) + MINUTES * 60); }));
            auto forever        = row("Until Turned Off", []() { M::setSuspend(-1); });
            forever.toggle      = Menu::TG_CHECK;
            forever.toggleState = END == -1;
            e.children.push_back(std::move(forever));
            return e;
        }

        // Quiet Automatically ▸: its two switches, checked while on (both
        // start off)
        Menu::SEntry quietRow() {
            namespace Q = NAwesome::Notify::Quiet;
            auto e      = row("Quiet Automatically", {});
            e.submenu   = true;
            for (const auto& [LABEL, OPT] : {std::pair{"When an App Is Fullscreen", eOption::QUIET_FULLSCREEN}, {"While Sharing the Screen", eOption::QUIET_SHARING}}) {
                const bool ON = Q::on(OPT);
                auto       r  = row(LABEL, [OPT, ON]() { Q::set(OPT, !ON); });
                r.toggle      = Menu::TG_CHECK;
                r.toggleState = ON;
                e.children.push_back(std::move(r));
            }
            return e;
        }

        // Silenced Apps ▸: every switch the card menus turned off, each row
        // a click from back on; dim with nothing silenced
        Menu::SEntry silencedRow() {
            auto e = row("Silenced Apps", {});
            for (const auto& S : P::silenced()) {
                const auto NAME = S.label.empty() ? S.key : S.label;
                const auto WHAT = S.until > 0                               ? "Muted Until " + untilText(S.until) :
                    (S.flags & P::BANNERS_OFF) && (S.flags & P::SOUNDS_OFF) ? std::string{"Banners and Sounds Off"} :
                    (S.flags & P::BANNERS_OFF)                              ? std::string{"Banners Off"} :
                                                                              std::string{"Sounds Off"};
                e.children.push_back(row(NAME + " — " + WHAT, [S]() { P::restore(S); }));
            }
            e.submenu = !e.children.empty();
            e.enabled = e.submenu;
            return e;
        }
    } // namespace

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
                if (bit == 2u) { // the options, built as they stand now
                    Menu::openLocal({dndRow(), quietRow(), silencedRow()}, hit.clickX, hit.mon);
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
