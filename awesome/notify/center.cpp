// The shade: ONE list of live cards, newest first, drawn by the compositor.
// There are no lifecycle sections and no history view: what you can see is
// what the model holds, and a dismissed card is gone.
//
// This unit decides WHAT is shown and WHERE; card.cpp and stack.cpp draw
// it. The center is the banners' own column under a heading with Clear All
// and Do Not Disturb's moon on glass, the bell its door. It draws no
// backdrop: the desktop is never darkened.
//
// RANKING, without dividers (labels would fade over a bright wallpaper):
// critical, then conversations, then the rest; newest first inside each
// group, a wider gap where a group ends. An app's cards STACK under their
// newest once it has two, its chats with the rest — each chat keeps its own
// card, and the model merges that chat's messages into it. A stack stands
// where its newest card ranks, collapsed until a click spreads it into its
// header and a row per card, so paging reaches every one. The silent cards
// (low urgency, or silenced by the user) fold into one row at the bottom
// until a click spreads them.
//
// Above the cards, a status line says why arrivals are holding (Do Not
// Disturb, quiet automatically); Clear All hides the cards for an undo
// window (Model::clearLive); a card the center had not shown when it opened
// wears its time in the accent.
//
// THE EXPANSION BUDGET — cards open by DEFAULT, not on demand. The walk
// starts at the top of the page and opens each card while the panel still
// has room, so the center is readable the instant it appears instead of
// costing a click per card; it folds only what would overflow. Stacks are
// the exception: one spreads only when asked.
//
// This unit owns the shade's transient state (paging, every fold
// override); all of it resets when the shade closes.

#include "ui.hpp"

#include "../core/walltimer.hpp"
#include "../core/wheel.hpp"

#include <format>

namespace Awesome::Notify {

    // an app's notifications stack as soon as there are two
    inline constexpr size_t AUTOGROUP_AT = 2;

    static bool                  s_on    = false;
    static size_t                s_skip  = 0; // the page's top row (s_off px past its top)
    static size_t                s_items = 0; // rows the last layout had
    static std::set<uint32_t>    s_openedRow, s_foldedRow;     // user overrides of the budget
    static std::set<std::string> s_openedGroup, s_foldedGroup; // ditto, per app key
    static Time::steady_tp       s_openedAt;
    static bool                  s_animating = false;
    static const std::string CLEAR_WORD = "Clear All", UNDO_WORD = "Undo";

    // what the center had not shown when this visit opened: their times
    // wear the accent until it closes
    static std::set<uint32_t>    s_fresh;

    // the silent fold's identity in the fold state: no app key starts with
    // a control character
    static const std::string SILENT_KEY = "\x1e" "silent";

    // the status line as the last warm read it (the draws between reuse it)
    static struct {
        bool        on = false, dnd = false;
        bool        pending = false; // Clear All's hidden cards wait: the capsule says Undo
        std::string sub;
    } s_status;

    // keyboard navigation: an index into s_disp, -1 until an arrow key claims
    // the shade (so a mouse-only visit shows no selection at all). s_lastVis
    // is the last item the placement fitted — the paging keys work off it.
    static int      s_sel     = -1;
    static size_t   s_lastVis = 0;
    static bool     s_reveal  = false; // the keyboard moved, or a field armed or took a key: the next warm brings its row into view

    // ---- scrolling ----
    // The page's top is row s_skip, s_off px past that row's top (into the
    // row or the gap after it). A row keeps its form wherever the page
    // stands (the budget runs from the column's head), so a scroll only
    // moves rows: the frames between warms advance the motion and
    // translate, measuring nothing. The finger moves the page itself, 1:1;
    // its lift flings it; the wheel, the chips and the keyboard glide it to
    // a row's top.
    struct SPos {
        size_t row = 0;
        double off = 0;
    };
    enum class eMotion : uint8_t {
        NONE,
        GLIDE,
        FLING,
    };
    static double          s_off    = 0;
    static eMotion         s_motion = eMotion::NONE;
    static SPos            s_glideTo;          // GLIDE: its target
    static double          s_vel = 0;          // FLING: px/s, toward the older rows
    static Time::steady_tp s_movedAt;          // the motion's last advance
    static double          s_pageH = 0, s_gap = 0; // the last layout's page and gap: input moves the page between layouts
    static Wheel::CFling   s_finger;
    // The glide closes on its target exponentially (95 % in 3 of its time
    // constants), the fling coasts v·FLING_TAU, as a list's kinetic scroll
    // does; a lift slower than FLING_MIN px/s does not fling, and a fling
    // ends below FLING_STOP.
    inline constexpr double GLIDE_TAU = 0.05, FLING_TAU = 0.4, FLING_MIN = 150, FLING_STOP = 20, FLING_MAX = 8000;

    // the budget's verdict on one card it placed
    struct SPlacedCard {
        double h        = 0;
        bool   open     = false; // drawn open
        bool   more     = false; // its open form shows more than the folded one (a click unfolds)
        bool   measured = false; // the budget reached it: a draw ahead of its warm has nothing to place
    };

    // the warm-measured layout cache (see renderCenter); dropped on close so
    // no strong SNotif refs (and their textures) outlive the visit
    static std::vector<SDisp>                    s_disp;
    static std::vector<double>                   s_itemH;
    static std::vector<SPlacedCard>              s_card; // per item, the card it shows (a deck's: its top card)
    // resolved open state from the last warm, keyed by identity: the click
    // handlers flip THIS, so a toggle means the same thing the eye just saw
    static std::unordered_map<uint32_t, bool>    s_rowState;
    static std::unordered_map<std::string, bool> s_groupState;

    // ---- the page's geometry, over the rows the last warm measured ----

    // the room above row i: none at the column's head, the gap between
    // rows, and a wider one where a group of the ranking ends
    static double leadOf(size_t i) {
        if (i == 0 || i >= s_disp.size())
            return 0;
        return s_gap + (s_disp[i].tier != s_disp[i - 1].tier ? TIER_GAP : 0);
    }
    static bool known(size_t i) {
        return i < s_card.size() && s_card[i].measured;
    }
    // row i's top to the next row's top
    static double spanOf(size_t i) {
        return s_itemH[i] + leadOf(i + 1);
    }

    // How far the page at `p` can still move down: to the end stop (the
    // column's bottom at the page's bottom) when the measured rows reach
    // the column's end (`end`), else to where they end.
    static double roomBelow(const SPos& p, bool* end = nullptr) {
        double y = -p.off;
        size_t i = p.row;
        for (; i < s_disp.size() && known(i); i++)
            y += spanOf(i);
        if (end)
            *end = i == s_disp.size();
        return std::max(0.0, y - s_pageH);
    }

    // `p` moved d px (down: toward the older rows) through the measured
    // rows: it stops at the head, the end stop and an unmeasured row
    static SPos movedBy(SPos p, double d) {
        if (d > 0) {
            p.off += std::min(d, roomBelow(p));
            while (p.row + 1 < s_disp.size() && known(p.row) && p.off >= spanOf(p.row)) {
                p.off -= spanOf(p.row);
                ++p.row;
            }
        } else if (d < 0) {
            p.off += d;
            while (p.off < 0 && p.row > 0 && known(p.row - 1)) {
                --p.row;
                p.off += spanOf(p.row);
            }
            p.off = std::max(p.off, 0.0);
        }
        return p;
    }

    // b's distance below a, through measured rows (none: one between is not)
    static std::optional<double> distance(const SPos& a, const SPos& b) {
        if (b.row < a.row || (b.row == a.row && b.off < a.off)) {
            const auto D = distance(b, a);
            return D ? std::optional{-*D} : std::nullopt;
        }
        double d = b.off - a.off;
        for (size_t i = a.row; i < b.row; i++) {
            if (!known(i))
                return std::nullopt;
            d += spanOf(i);
        }
        return d;
    }

    // the page inside its bounds: an offset past its row's span carried
    // over, and a page past the end stop (rows went) back up to it
    static SPos clampPos(SPos p) {
        p.row = s_disp.empty() ? 0 : std::min(p.row, s_disp.size() - 1);
        p     = movedBy({p.row, 0}, p.off); // its offset held to the end stop
        bool end = false;
        roomBelow(p, &end);
        if (end) { // the column ends above the page's bottom: up to the end stop, the head the limit
            double y = -p.off;
            for (size_t i = p.row; i < s_disp.size(); i++)
                y += spanOf(i);
            if (y < s_pageH)
                p = movedBy(p, y - s_pageH);
        }
        return p;
    }

    // how far the page at `p` can move up through measured rows
    static double roomAbove(const SPos& p) {
        double up = p.off;
        for (size_t i = p.row; i-- > 0 && known(i);)
            up += spanOf(i);
        return up;
    }

    // rows the frames ahead may show that no warm measured: within half a
    // page of the page's edges
    static bool needsMeasure(const SPos& p) {
        bool end = false;
        if (roomBelow(p, &end) < s_pageH / 2 && !end)
            return true;
        size_t top = p.row;
        while (top > 0 && known(top - 1))
            --top;
        return top > 0 && roomAbove(p) < s_pageH / 2;
    }

    static void glideTo(SPos to) {
        if (s_motion != eMotion::GLIDE)
            s_movedAt = Time::steadyNow();
        s_motion  = eMotion::GLIDE;
        s_glideTo = to;
        s_vel     = 0;
    }

    // The draw's advance: where the glide or the fling has the page now.
    // Time, not frames, moves it; without animations a glide lands at once.
    // The measured edge holds it, and half a page short of that edge it
    // asks for the warm that measures on.
    static void stepMotion() {
        if (s_motion == eMotion::NONE)
            return;
        const auto   NOW = Time::steadyNow();
        const double DT  = std::chrono::duration<double>(NOW - s_movedAt).count();
        s_movedAt        = NOW;
        const SPos AT{s_skip, s_off};
        SPos       next = AT;
        bool       end  = false;
        const double ROOM = roomBelow(AT, &end);
        if (s_motion == eMotion::GLIDE) {
            // a target past rows no warm measured: toward the measured edge,
            // the warms measuring on ahead of the page
            const auto   D    = distance(AT, s_glideTo);
            const double DIST = std::min(D ? *D : s_glideTo.row > AT.row ? ROOM : -roomAbove(AT), ROOM); // past the end stop: the end stop
            const double STEP = animationsOn() && std::abs(DIST) > 0.5 ? DIST * (1.0 - std::exp(-DT / GLIDE_TAU)) : DIST;
            next              = movedBy(AT, STEP);
            if (D && (std::abs(*D - STEP) < 0.01 || (end && STEP == DIST))) // there, or held at the end stop
                s_motion = eMotion::NONE;
        } else {
            const double DECAY = std::exp(-DT / FLING_TAU);
            const double STEP  = s_vel * FLING_TAU * (1.0 - DECAY);
            s_vel *= DECAY;
            next = movedBy(AT, STEP);
            // the head or the end stop ends it; the measured edge only holds it
            const bool STOPPED = STEP > 0 ? STEP > ROOM && end : next.row == 0 && next.off <= 0;
            if (std::abs(s_vel) < FLING_STOP || STOPPED)
                s_motion = eMotion::NONE;
        }
        s_skip = next.row;
        s_off  = next.off;
        if (needsMeasure(next))
            notifChanged();
    }

    bool                                         centerVisible() {
        return s_on;
    }
    bool centerAnimating() {
        // by the clock, not only the draw: a draw that never runs (the
        // session locked mid-spring) would leave the flag up and the motion
        // tick damaging the cards for nothing
        if (s_animating && animT(s_openedAt, Theme::MOTION_SPATIAL) >= 1.f)
            s_animating = false;
        // likewise a glide or a fling no draw advanced for a while
        if (s_motion != eMotion::NONE && Time::steadyNow() - s_movedAt > std::chrono::milliseconds(250))
            s_motion = eMotion::NONE;
        return s_animating || s_motion != eMotion::NONE;
    }

    // Everything one visit accumulates. The close resets it (paging and
    // every fold start over), and so must PLUGIN_EXIT — s_disp holds STRONG
    // card refs, so a session that exits with the shade open would otherwise
    // carry them past Model::exit and destroy them, textures and all, at
    // static-destruction time with the renderer already gone.
    static void resetVisit() {
        s_skip = s_items = 0;
        s_fresh.clear();
        s_openedRow.clear();
        s_foldedRow.clear();
        s_openedGroup.clear();
        s_foldedGroup.clear();
        s_rowState.clear();
        s_groupState.clear();
        s_animating = false;
        s_sel       = -1;
        s_lastVis   = 0;
        s_reveal    = false;
        s_off       = 0;
        s_motion    = eMotion::NONE;
        s_finger.clear();
        s_disp.clear();
        s_itemH.clear();
        s_card.clear();
    }

    void centerExit() {
        s_on = false;
        resetVisit();
    }

    void centerToggleRow(uint32_t id) {
        const auto IT = s_rowState.find(id);
        if (IT != s_rowState.end() && IT->second) {
            s_openedRow.erase(id);
            s_foldedRow.insert(id);
            if (replyArmedOn(id))
                replyClose(); // its field folds away with the row
        } else {
            s_foldedRow.erase(id);
            s_openedRow.insert(id);
        }
        notifChanged();
    }

    // a row that stands for several cards: Enter and Delete act on all
    static bool isGroup(const SDisp& d) {
        return d.kind == SDisp::DECK || d.kind == SDisp::HEAD || d.kind == SDisp::FOLD;
    }

    // a stack's right-click or Delete: exactly the cards it drew
    void centerDismissStack(const std::string& key) {
        if (const auto IT = std::ranges::find_if(s_disp, [&](const SDisp& D) { return (D.kind == SDisp::DECK || D.kind == SDisp::HEAD) && D.key == key; }); IT != s_disp.end())
            Model::dismissCards(IT->items);
    }

    void centerToggleGroup(const std::string& appKey) {
        const auto IT = s_groupState.find(appKey);
        if (IT != s_groupState.end() && IT->second) {
            s_openedGroup.erase(appKey);
            s_foldedGroup.insert(appKey);
        } else {
            s_foldedGroup.erase(appKey);
            s_openedGroup.insert(appKey);
        }
        notifChanged();
    }

    // Row by row from where the glide was going (else from the page's
    // top): up from inside a row reaches that row's top first, and a target
    // past the end stop is the end stop, so the way back starts there.
    void centerPage(int dir) {
        if (s_items <= 1 || dir == 0)
            return;
        const SPos AT{s_skip, s_off};
        const SPos FROM = s_motion == eMotion::GLIDE ? s_glideTo : AT;
        const auto ROW  = (int64_t)FROM.row + dir + (dir < 0 && FROM.off > 0.5 ? 1 : 0);
        SPos       to{(size_t)std::clamp<int64_t>(ROW, 0, (int64_t)s_items - 1), 0};
        bool       end  = false;
        const auto ROOM = roomBelow(AT, &end);
        if (const auto D = distance(AT, to); end && D && *D > ROOM)
            to = movedBy(AT, ROOM);
        glideTo(to);
        // a pointer page leaves the keyboard's row behind: the next arrow
        // enters the new page (Enter/Delete must never act off screen)
        s_sel = -1;
        notifChanged(); // the warm measures what the glide crosses
    }

    void centerScroll(double px, uint32_t ms, bool lift) {
        if (s_disp.empty())
            return;
        if (lift) {
            const double V = s_finger.lift(ms);
            if (animationsOn() && std::abs(V) >= FLING_MIN) {
                s_motion  = eMotion::FLING;
                s_vel     = std::clamp(V, -FLING_MAX, FLING_MAX);
                s_movedAt = Time::steadyNow();
                notifMoved();
            }
            return;
        }
        s_motion = eMotion::NONE;
        s_finger.add(ms, px);
        const SPos NEXT = movedBy({s_skip, s_off}, px);
        s_skip          = NEXT.row;
        s_off           = NEXT.off;
        s_sel           = -1;
        if (needsMeasure(NEXT))
            notifChanged();
        else
            notifMoved();
    }

    void centerReveal() {
        s_reveal = true;
    }

    void centerScrollHalt() {
        s_motion = eMotion::NONE;
        s_finger.clear();
    }

    void centerDeselect() {
        if (s_sel < 0)
            return;
        s_sel = -1;
        notifChanged();
    }

    // a screenful, keeping the edge row in view: the paging chips' click
    void centerPageScreen(int dir) {
        centerPage(dir * (int)std::max<size_t>(1, s_lastVis > s_skip ? s_lastVis - s_skip : 1));
    }

    void centerSelectMove(int dir) {
        if (s_disp.empty()) {
            s_sel = -1;
            return;
        }
        const int LAST = (int)s_disp.size() - 1;
        if (s_sel < 0) // the first arrow enters the page, it does not jump
            s_sel = dir > 0 ? std::min((int)s_skip, LAST) : std::min((int)s_lastVis, LAST);
        else
            s_sel = std::clamp(s_sel + dir, 0, LAST);
        s_reveal = true; // the warm glides it into view, its rows measured
        notifChanged();
    }

    bool centerHasRows() {
        return !s_disp.empty();
    }

    bool centerSelection(uint32_t& id, std::string& group) {
        if (s_sel < 0 || (size_t)s_sel >= s_disp.size())
            return false;
        const auto& D = s_disp[s_sel];
        group         = isGroup(D) ? D.key : "";
        id            = D.items.front()->id;
        return true;
    }

    void setCenter(bool on, bool repop) {
        if (on == s_on)
            return;
        CardMenu::close(); // the layout it was opened over is going
        if (on) {
            for (const auto& N : notifs)
                if (!N->seen && !N->waiting && !Model::offShade(N))
                    s_fresh.insert(N->id);
        } else
            Model::commitClear(); // the undo window ends with the visit
        // what the center listed while open, and lists as it opens, has been
        // seen: the bell's dot goes out (the funnel below repaints it)
        Model::markSeen();
        s_on = on;
        if (!on) {
            resetVisit();
            replyExit(); // a field cannot outlive the panel it was drawn in
            Model::repopAbsorbed(repop); // an explicit close returns the parked stack
        } else {
            // Opening absorbs the popped stack — the banners stand down into
            // parked shade rows; an explicit close re-pops them (the user
            // must not lose sight of what the shade swallowed).
            Model::absorbPopped();
            if (animationsOn()) {
                s_openedAt  = Time::steadyNow();
                s_animating = true;
            }
        }
        notifChanged();
    }

    // The ranking: urgent things, then the people, then everything else;
    // the quiet ones fold below.
    static uint8_t tier(const SP<SNotif>& n) {
        if (n->urgency >= 2)
            return 0;
        return n->conversation ? 1 : 2;
    }
    inline constexpr uint8_t TIER_SILENT = 3;

    // Asked for no banner (low urgency), or silenced by the user (its app's
    // banners off, its chat muted), read live: the fold at the bottom.
    // Quiet automatically holds banners a while and is no silence; critical
    // never folds.
    static bool silent(const SNotif& n) {
        return n.urgency == 0 || Policy::silenced(n);
    }

    // How many stackable cards one (app, declared group) holds, and where its
    // stack landed. Different x-notify-group-key values (an app's
    // threads) never merge: each owns its own stack and fold state.
    struct SOwner {
        std::string        key;
        size_t             count = 0;
        size_t             first = (size_t)-1; // index in out, once one is placed
    };

    // `src` ranked, an app's cards stacked under its newest
    static void appendStacked(std::vector<SDisp>& out, const std::vector<SP<SNotif>>& src, bool quiet) {
        static std::vector<SOwner> owners; // reused; main thread only
        // key -> owner: a linear scan per card would be quadratic in distinct
        // keys (a sender rotating its app name)
        static std::unordered_map<std::string, size_t> ownerIdx;   // ditto
        static std::vector<size_t>                     ownerOfSrc; // src[i]'s owner, -1: never stacks
        owners.clear();
        ownerIdx.clear();
        ownerOfSrc.clear();
        for (const auto& N : src) {
            if (!Model::stackable(N)) {
                ownerOfSrc.push_back((size_t)-1);
                continue;
            }
            const auto [IT, NEW] = ownerIdx.try_emplace(Model::groupKeyOf(N), owners.size());
            if (NEW)
                owners.push_back(SOwner{.key = IT->first});
            owners[IT->second].count++;
            ownerOfSrc.push_back(IT->second);
        }

        for (size_t i = 0; i < src.size(); i++) {
            const auto& N = src[i];
            if (ownerOfSrc[i] != (size_t)-1) {
                auto& O = owners[ownerOfSrc[i]];
                if (O.count >= AUTOGROUP_AT) {
                    if (O.first != (size_t)-1) {
                        out[O.first].items.push_back(N);
                        out[O.first].kind = SDisp::DECK;
                        continue;
                    }
                    O.first = out.size();
                }
            }
            // the key rides the fold state, hover, selection and dismissal —
            // a stack carries the (app, group) identity, singles the app; the
            // silent section's stacks key apart from the same app's ranked one
            const auto KEY = ownerOfSrc[i] != (size_t)-1 ? owners[ownerOfSrc[i]].key : Model::groupKeyOf(N);
            out.push_back(SDisp{.items = {N}, .key = quiet ? SILENT_KEY + "\x1f" + KEY : KEY, .tier = quiet ? TIER_SILENT : tier(N)});
        }
    }

    // spread by a click, or around a field being typed into
    static bool spread(const std::string& key, const std::vector<SP<SNotif>>& items) {
        return (s_openedGroup.contains(key) && !s_foldedGroup.contains(key)) || std::ranges::any_of(items, [](const auto& N) { return replyArmedOn(N->id); });
    }

    static void buildDisplay(std::vector<SDisp>& out) {
        out.clear();
        s_groupState.clear();
        static std::vector<SP<SNotif>> src, quiet; // reused; main thread only
        static std::vector<SDisp>      staged;     // ditto
        staged.clear();
        for (const auto& N : notifs)
            if (!N->waiting && !Model::offShade(N))
                (silent(*N) ? quiet : src).push_back(N);
        // notifs is newest-first; a STABLE sort by tier keeps that inside each
        std::ranges::stable_sort(src, [](const auto& a, const auto& b) { return tier(a) < tier(b); });
        appendStacked(staged, src, false);
        if (!quiet.empty()) {
            if (spread(SILENT_KEY, quiet))
                appendStacked(staged, quiet, true);
            else {
                staged.push_back(SDisp{.items = quiet, .key = SILENT_KEY, .tier = TIER_SILENT, .kind = SDisp::FOLD});
                s_groupState[SILENT_KEY] = false;
            }
        }
        for (auto& D : staged) {
            if (D.kind != SDisp::DECK) {
                out.push_back(std::move(D));
                continue;
            }
            const bool SPREAD   = spread(D.key, D.items);
            s_groupState[D.key] = SPREAD;
            if (!SPREAD) {
                out.push_back(std::move(D));
                continue;
            }
            out.push_back(SDisp{.items = D.items, .key = D.key, .tier = D.tier, .kind = SDisp::HEAD});
            for (const auto& N : D.items)
                out.push_back(SDisp{.items = {N}, .key = D.key, .tier = D.tier, .kind = SDisp::MEMBER});
        }
        // the buffers are reused, their references are not: a dismissed card
        // (and its textures) must not live on until the shade next opens
        src.clear();
        quiet.clear();
        staged.clear();
    }

    // one card's verdict: `extra` is what must still fit under it (a deck's
    // depth), `force` the open it must take (an armed field, the user's
    // unfold) or must not (the user's fold)
    static SPlacedCard budgetCard(const SPaint& P, const SType& T, const SP<SNotif>& N, double w, double room, double extra, bool top, bool deck) {
        const bool  ARMED = replyArmedOn(N->id); // a field shows only on the open card
        const bool  OPEN  = ARMED || s_openedRow.contains(N->id), FOLD = !ARMED && s_foldedRow.contains(N->id);
        const bool  FRESH = s_fresh.contains(N->id);
        SPlacedCard c;
        c.h    = measureCard(P, T, N, w, SCardForm{.where = SCardForm::CENTER, .open = false, .deck = deck, .fresh = FRESH});
        c.more = true;
        if (!FOLD && (OPEN || top || c.h + extra < room)) {
            const double OH = measureCard(P, T, N, w, SCardForm{.where = SCardForm::CENTER, .open = true, .deck = deck, .fresh = FRESH});
            c.more          = OH > c.h + 0.5;
            c.open          = c.more && (OPEN || top || OH + extra <= room);
            if (c.open)
                c.h = OH;
        }
        return c;
    }

    static void measureRow(const SPaint& P, const SType& T, size_t i, double contentW, double room, bool top) {
        const auto& D = s_disp[i];
        auto&       C = s_card[i];
        switch (D.kind) {
            case SDisp::FOLD: s_itemH[i] = FOLD_H; break;
            case SDisp::HEAD: s_itemH[i] = SHEAD_H; break;
            case SDisp::DECK: {
                const double DEPTH = deckDepth(D.items.size());
                C                  = budgetCard(P, T, D.items.front(), contentW, room, DEPTH, top, true);
                s_itemH[i]         = C.h + DEPTH;
                break;
            }
            case SDisp::CARD:
            case SDisp::MEMBER:
                C                               = budgetCard(P, T, D.items.front(), contentW, room, 0, top, false);
                s_itemH[i]                      = C.h;
                s_rowState[D.items.front()->id] = C.open;
                break;
        }
        C.measured = true;
    }

    // The expansion budget walks the column from its head, opening what
    // its first page fits: the top card always opens, a user override wins
    // both ways, and every row past the first page comes folded (a click
    // or Space opens it) — a row's form never hangs on where the page
    // stands. Measured are the first page and the rows around the page (a
    // page above it, two below it, and around a glide's target): what the
    // frames until the next warm can show. Folded rows past them are not
    // measured — that would raster texts and queue icon decodes for rows
    // no frame shows.
    static void runBudget(const SPaint& P, const SType& T, double contentW, double bodyCap) {
        double used = 0;
        for (size_t i = 0; i < s_disp.size() && used <= bodyCap; i++) {
            const double LEAD = leadOf(i);
            measureRow(P, T, i, contentW, bodyCap - used - LEAD, i == 0);
            used += LEAD + s_itemH[i];
        }
        const auto AROUND = [&](const SPos& at) {
            double up = at.off;
            for (size_t i = std::min(at.row, s_disp.size()); i-- > 0 && up < bodyCap;) {
                if (!known(i))
                    measureRow(P, T, i, contentW, -1, false);
                up += spanOf(i);
            }
            double down = -at.off;
            for (size_t i = at.row; i < s_disp.size() && down < 3 * bodyCap; i++) {
                if (!known(i))
                    measureRow(P, T, i, contentW, -1, false);
                down += spanOf(i);
            }
        };
        AROUND({s_skip, s_off});
        if (s_motion == eMotion::GLIDE)
            AROUND(s_glideTo);
    }

    // a row's identity across a rebuild: its kind and its first card
    struct SRowKey {
        SDisp::eKind kind = SDisp::CARD;
        uint32_t     id   = 0;
        bool         operator==(const SRowKey&) const = default;
    };
    static SRowKey rowKey(size_t i) {
        return i < s_disp.size() ? SRowKey{s_disp[i].kind, s_disp[i].items.front()->id} : SRowKey{};
    }
    static std::optional<size_t> findRow(const SRowKey& key) {
        for (size_t i = 0; i < s_disp.size(); i++)
            if (rowKey(i) == key)
                return i;
        return std::nullopt;
    }

    // The row the keyboard works on: an armed reply field's, else the
    // selection; -1 for none.
    static int keyboardRow() {
        for (size_t i = 0; i < s_disp.size(); i++)
            if (s_disp[i].kind != SDisp::HEAD && std::ranges::any_of(s_disp[i].items, [](const auto& N) { return replyArmedOn(N->id); }))
                return (int)i;
        return s_sel;
    }

    void renderCenter(const SPaint& PIN, const SType& T) {
        SPaint P = PIN;
        if (s_animating) { // the open spring: fade + a 6 px rise
            const float AT = animT(s_openedAt, Theme::MOTION_SPATIAL);
            if (AT >= 1.f)
                s_animating = false;
            else {
                P.alpha *= easeOutCubic(AT);
                P.dy -= (1.0 - easeOutBack(AT)) * 6.0;
            }
        }

        const auto  MB = P.mon->logicalBox();
        const float RP = rPow();
        const auto  COLTITLE = color(cfg().getColor("plugin:awesome:notify:col_title")), COLSUB = color(cfg().getColor("plugin:awesome:notify:col_kicker")),
                   COLACC = color(cfg().getColor("plugin:awesome:notify:col_highlight")), COLBG = color(cfg().getColor("plugin:awesome:theme:col_glass")),
                   COLRIM = color(cfg().getColor("plugin:awesome:theme:col_rim"));

        // why arrivals are holding, read once per warm: Do Not Disturb, else
        // quiet automatically (its reason)
        if (P.warm) {
            s_status.pending = Model::clearPending();
            const auto WHY   = Quiet::why();
            s_status.dnd   = Model::suspendedNow();
            s_status.on    = s_status.dnd || WHY != Quiet::eWhy::NONE;
            s_status.sub.clear();
            if (s_status.dnd) {
                const auto END = Model::suspendEnd();
                s_status.sub   = END > 0 ? "until " + untilText(END) : "until turned off";
                if (const auto HELD = Model::heldCount(); HELD > 0)
                    s_status.sub += " · " + std::to_string(HELD) + " waiting";
            } else if (s_status.on)
                s_status.sub = WHY == Quiet::eWhy::SHARING ? "while you share the screen" : "while an app is fullscreen";
        }

        const double W   = std::max((double)cfg().getI("plugin:awesome:notify:width"), 120.0);
        const double GAP = std::max((double)cfg().getI("plugin:awesome:notify:margin"), 0.0);
        const double X    = MB.x + MB.w - EDGE - W;
        const double TOP  = cardsTop(P.mon);
        const double Y0   = MB.y + TOP;
        const double HY   = Y0;                         // the heading's row heads the column
        const double SY   = HY + HEADER_H + HEADER_GAP; // the status line's top
        const double CY0  = SY + (s_status.on ? STATUS_H + HEADER_GAP : 0); // the cards' top

        // The column runs to what the monitor leaves below offset_y (a margin
        // of air), and the expansion budget spends exactly this. Overflow
        // becomes wheel paging, never off-screen bleed; a card caps its body
        // (card.cpp), so no single card can exceed the cap and the
        // always-place-the-first rule can't spill.
        const double AVAILH  = MB.h - TOP - cardsBottomReserved(P.mon) - GAP;
        const double BODYCAP = std::max(64.0, AVAILH - (CY0 - Y0));

        // The display list, every height AND every fold verdict are decided
        // once per WARM and reused by the draws between warms: hover fills
        // change nothing they depend on, and every model/fold change warms
        // first (notifChanged). The draw side lays out without measuring twice.
        if (P.warm) {
            // the selection follows its card (or stack) across the rebuild:
            // a card landing above must not shift Enter/Delete onto a
            // neighbor. One whose card left keeps its INDEX — the card that
            // took the dismissed one's place, as a list does after a delete.
            // A field that died (its card left, its sender left the bus, a
            // replace moved it to another chat) goes now, not at the next
            // key, and takes the selection with it: a key typed for it must
            // not act on a card.
            if (replyHeld() && !replyArmed())
                s_sel = -1;
            uint32_t    selId = 0;
            std::string selGroup;
            const bool  SELECTED = centerSelection(selId, selGroup);
            // the page keeps its rows across the rebuild: a card landing
            // above moves neither what it shows nor where a glide goes
            const auto ANCHOR = rowKey(s_skip), TARGET = rowKey(s_glideTo.row);
            buildDisplay(s_disp);
            if (const auto AT = findRow(ANCHOR))
                s_skip = *AT;
            else
                s_off = 0;
            if (const auto TO = findRow(TARGET); TO && s_motion == eMotion::GLIDE)
                s_glideTo.row = *TO;
            if (SELECTED)
                for (size_t i = 0; i < s_disp.size(); i++) {
                    const auto& D = s_disp[i];
                    if (selGroup.empty() ? D.kind != SDisp::HEAD && std::ranges::any_of(D.items, [&](const auto& N) { return N->id == selId; }) : isGroup(D) && D.key == selGroup) {
                        s_sel = (int)i;
                        break;
                    }
                }
            s_skip = s_disp.empty() ? 0 : std::min(s_skip, s_disp.size() - 1);
            if (s_sel >= (int)s_disp.size())
                s_sel = s_disp.empty() ? -1 : (int)s_disp.size() - 1;
            s_gap   = GAP;
            s_pageH = BODYCAP;
            s_itemH.assign(s_disp.size(), 0.0);
            s_card.assign(s_disp.size(), {});
            s_rowState.clear();
            runBudget(P, T, W, BODYCAP);
            const SPos AT = clampPos({s_skip, s_off});
            s_skip        = AT.row;
            s_off         = AT.off;
            if (s_motion == eMotion::GLIDE)
                s_glideTo = clampPos(s_glideTo);
            // the keyboard's row into view, by the shortest glide: its top
            // to the page's top, else its bottom to the page's bottom
            if (const int K = keyboardRow(); s_reveal && K >= 0 && known((size_t)K)) {
                const SPos TOP{(size_t)K, 0};
                if (const auto D = distance(AT, TOP); D && (*D < 0 || *D + s_itemH[K] > BODYCAP))
                    glideTo(*D < 0 || s_itemH[K] >= BODYCAP ? TOP : movedBy(TOP, s_itemH[K] - BODYCAP));
            }
            s_reveal = false;
        }
        const auto& disp = s_disp;
        s_items          = disp.size();
        s_skip           = disp.empty() ? 0 : std::min(s_skip, disp.size() - 1);
        if (!P.warm)
            stepMotion();

        // The page's rows: from its top row, s_off above the page, to the
        // first that starts past its bottom or that no warm has measured.
        // The offset in whole device pixels: a fractional one would sample
        // every glyph anew each frame.
        struct SPlaced {
            size_t idx;
            double y, h;
        };
        static std::vector<SPlaced> placed; // reused
        placed.clear();
        const double PAGE_END = CY0 + BODYCAP;
        double       rowY     = CY0 - std::round(s_off * P.scale) / P.scale;
        for (size_t i = s_skip; i < disp.size() && known(i); i++) {
            rowY += i > s_skip ? leadOf(i) : 0;
            if (rowY >= PAGE_END)
                break;
            placed.push_back({i, rowY, s_itemH[i]});
            rowY += s_itemH[i];
        }
        s_lastVis           = placed.empty() ? s_skip : placed.back().idx;
        const bool   EMPTY  = disp.empty();
        const bool   CUT_T  = !placed.empty() && placed.front().y < CY0 - 0.01;
        const bool   CUT_B  = !placed.empty() && placed.back().y + placed.back().h > PAGE_END + 0.01;
        const double SHOWNH = placed.empty() ? 0 : std::min(PAGE_END, placed.back().y + placed.back().h) - CY0;

        // the column swallows its clicks and owns the wheel, gaps included
        {
            SCard pc;
            pc.kind = SCard::PANEL;
            pc.box  = CBox{X, Y0, W, CY0 - Y0 + (EMPTY ? EMPTY_H : SHOWNH)};
            cards.push_back(pc);
        }

        // ---- the heading's row: "Notifications", Clear All, the moon ----
        // No backdrop darkens the desktop: Clear All and the moon sit on
        // glass of their own (as a stack's Show Less), and the heading bare
        // on the desktop as its stack headers do, a soft shadow keeping it
        // legible over any wallpaper or window. The moon holds the row's end
        // (Clear All comes and goes beside it, the moon never moves): Do Not
        // Disturb's one home. Off, its glyph on the glass, the accent under
        // the pointer (both faces warm: a hover only damages); on, the
        // white disc.
        {
            if (const auto HEAD = cachedText("Notifications", COLTITLE, T.heading, (int)std::lround(W * P.scale), -1, 0, false, 600, nullptr, false, eShadow::DESKTOP))
                texAt(P, HEAD, X + 4, HY + (HEADER_H - texH(HEAD, P.scale)) / 2);

            const bool DND = s_status.dnd; // the warm's
            const CBox MOON{X + W - CAP_H, HY + (HEADER_H - CAP_H) / 2, CAP_H, CAP_H};
            const int  GPX = (int)std::lround(MOON_GLYPH * P.scale);
            if (DND)
                P.rect(MOON, COLTITLE, (int)std::lround(CAP_H / 2 * P.scale), 2.f);
            else
                P.frost(MOON, COLBG, COLRIM, (int)std::lround(CAP_H / 2 * P.scale), 2.f);
            const auto REST = Glyph::tex(Glyph::eGlyph::MOON, DND ? tLitInk() : COLTITLE, GPX);
            const auto LIT  = DND ? REST : Glyph::tex(Glyph::eGlyph::MOON, COLACC, GPX);
            if (const auto G = hovered.kind == SCard::BTN_DND ? LIT : REST)
                P.texIn(G, MOON);
            SCard mc;
            mc.kind = SCard::BTN_DND;
            mc.box  = MOON;
            mc.id   = DND; // what the click saw: on ends it, off opens the durations
            cards.push_back(mc);

            // Clear All, or Undo while what it hid waits on its window;
            // gone with nothing to clear. The pointer turns its word the
            // accent (both faces warm: a hover only damages).
            const bool PENDING = s_status.pending; // the warm's: the draw asks for the faces it built
            if (PENDING || !EMPTY) {
                const std::string& WORD   = PENDING ? UNDO_WORD : CLEAR_WORD;
                const auto         CLEAR  = cachedText(WORD, COLTITLE, T.meta, (int)std::lround(W * P.scale), -1, 0, false, 600);
                const auto         LIT    = cachedText(WORD, COLACC, T.meta, (int)std::lround(W * P.scale), -1, 0, false, 600);
                const double       CW     = texW(CLEAR, P.scale) + 2 * CAP_PADX;
                const CBox         CB{MOON.x - HEAD_GAP - CW, HY + (HEADER_H - CAP_H) / 2, CW, CAP_H};
                P.frost(CB, COLBG, COLRIM, (int)std::lround(CAP_H / 2 * P.scale), 2.f);
                texCentered(P, hovered.kind == SCard::BTN_CLEAR ? LIT : CLEAR, CB);
                SCard cc;
                cc.kind = SCard::BTN_CLEAR;
                cc.box  = CB;
                cc.id   = PENDING; // what the click saw: a window that ended since must not turn Undo into a clear
                cards.push_back(cc);
            }
        }

        // ---- the status line: why arrivals are holding ----
        if (s_status.on) {
            const CBox SB{X, SY, W, STATUS_H};
            P.frost(SB, COLBG, COLRIM, (int)std::lround(STATUS_H / 2 * P.scale), 2.f);
            const CBox DISC{SB.x + 6, SB.y + (STATUS_H - 24) / 2, 24, 24};
            P.rect(DISC, COLTITLE, (int)std::lround(12 * P.scale), 2.f);
            if (const auto G = Glyph::tex(Glyph::eGlyph::MOON, tLitInk(), (int)std::lround(12 * P.scale)))
                P.texIn(G, DISC);
            double tx = DISC.x + DISC.w + 8;
            if (const auto TTL = cachedText(s_status.dnd ? "Do Not Disturb" : "Quiet", COLTITLE, T.title, (int)std::lround(W * P.scale), -1, 0, false, 600)) {
                texAt(P, TTL, tx, SB.y + (STATUS_H - texH(TTL, P.scale)) / 2);
                tx += texW(TTL, P.scale) + 8;
            }
            if (const auto SUB = cachedText(s_status.sub, COLSUB, T.meta, std::max(1, (int)std::floor((SB.x + W - 14 - tx) * P.scale)), -1, 0, false, 400))
                texAt(P, SUB, tx, SB.y + (STATUS_H - texH(SUB, P.scale)) / 2);
        }

        if (EMPTY) {
            const CBox EB{X, CY0, W, EMPTY_H};
            P.frost(EB, COLBG, COLRIM, rCard(P.scale), RP);
            texCentered(P, cachedText("No Notifications", COLSUB, T.body, (int)std::lround(W * P.scale), -1, 0, false, 500), EB);
        }

        // A row the page's edge cuts paints inside the page alone and
        // answers there alone; with none cut the column paints whole (its
        // drops and rims past the page's box), as at rest.
        const size_t                 ROW_HITS = cards.size();
        std::optional<SPaint::CClip> clip;
        if (CUT_T || CUT_B) {
            const double REACH = SPaint::frostReach(), TOPY = CUT_T ? CY0 : MB.y, ENDY = CUT_B ? PAGE_END : MB.y + MB.h;
            clip.emplace(P, CBox{X - REACH, TOPY, W + 2 * REACH, ENDY - TOPY});
        }
        for (const auto& [IDX, RY, IH] : placed) {
            const auto& D = disp[IDX];
            const CBox  ROW{X, RY, W, IH};
            // the keyboard's selection: the accent around the whole row, so
            // it never reads as a hover
            const bool SEL = (int)IDX == s_sel;
            if (D.kind == SDisp::FOLD || D.kind == SDisp::HEAD) {
                if (D.kind == SDisp::FOLD)
                    paintSilentFold(P, T, D, ROW);
                else
                    paintStackHead(P, T, D, ROW);
                if (SEL)
                    P.ring(ROW, COLACC, (int)std::lround(IH / 2 * P.scale), 2.f, 1.5);
                continue;
            }
            if (IDX >= s_card.size() || !s_card[IDX].measured) // a draw ahead of its warm: nothing measured to place
                continue;
            const auto& C    = s_card[IDX];
            const auto& N    = D.items.front();
            const bool  DECK = D.kind == SDisp::DECK;
            CBox        ring = ROW;
            if (DECK) { // the deck first: the top card's glass covers all but its edges
                ring.h -= deckDepth(D.items.size());
                paintDeck(P, ring, D.items.size());
            }
            SCard card = recycledCard();
            renderCard(P, T, N, CBox{X, ROW.y, W, 0}, SCardForm{.where = SCardForm::CENTER, .open = C.open, .deck = DECK, .fresh = s_fresh.contains(N->id)}, card);
            if (DECK)
                card.box.h += deckDepth(D.items.size()); // the deck answers for the stack too
            else
                card.folded = !C.open && C.more;
            if (D.kind != SDisp::CARD)
                card.group = D.key;
            cards.push_back(std::move(card));
            if (SEL)
                P.ring(ring, COLACC, rCard(P.scale), RP, 1.5);
        }
        clip.reset();
        if (CUT_T || CUT_B)
            for (size_t k = ROW_HITS; k < cards.size(); k++) {
                auto&        B  = cards[k].box;
                const double LO = std::max(B.y, CY0), HI = std::min(B.y + B.h, PAGE_END);
                B               = HI > LO ? CBox{B.x, LO, B.w, HI - LO} : CBox{};
            }

        // Paging cues: a wheel-scroll is invisible otherwise. An up chevron
        // when cards sit above the fold, a "chevron N" chip when N sit below.
        // Drawn over the edge cards, they take their own clicks (a page, like
        // the wheel) — pushed last, they are hit first.
        if (!EMPTY) {
            size_t below = 0; // the cards not shown whole under the page: a cut row's with them
            for (size_t i = placed.empty() ? 0 : placed.back().idx + (CUT_B ? 0 : 1); i < disp.size(); i++)
                below += disp[i].kind == SDisp::HEAD ? 0 : disp[i].items.size(); // its cards count as its rows
            const double CHIP_H = 24;
            if (s_skip > 0 || s_off > 0.5) {
                const CBox UP{X + (W - 44) / 2, CY0 - CHIP_H / 2, 44, CHIP_H};
                P.frost(UP, COLBG, COLRIM, (int)std::lround(CHIP_H / 2 * P.scale), 2.f);
                if (const auto G = Glyph::tex(Glyph::eGlyph::CHEVRON_UP, COLTITLE, (int)std::lround(14 * P.scale)))
                    P.texIn(G, UP);
                SCard c;
                c.kind = SCard::PAGE_UP;
                c.box  = UP;
                cards.push_back(c);
            }
            if (below > 0) {
                const auto   NUM = cachedText(std::to_string(below), COLTITLE, T.small, 128, -1, 0, false, 600);
                const double CW  = 14 + 4 + texW(NUM, P.scale) + 24;
                const CBox   DN{X + (W - CW) / 2, std::min(CY0 + SHOWNH - CHIP_H / 2, PAGE_END - CHIP_H), CW, CHIP_H};
                P.frost(DN, COLBG, COLRIM, (int)std::lround(CHIP_H / 2 * P.scale), 2.f);
                if (const auto G = Glyph::tex(Glyph::eGlyph::CHEVRON_DOWN, COLTITLE, (int)std::lround(14 * P.scale)))
                    P.texIn(G, CBox{DN.x + 12, DN.y, 14, CHIP_H});
                if (NUM && NUM->tex)
                    texAt(P, NUM, DN.x + 12 + 14 + 4, DN.y + (CHIP_H - texH(NUM, P.scale)) / 2);
                SCard c;
                c.kind = SCard::PAGE_DOWN;
                c.box  = DN;
                cards.push_back(c);
            }
        }
    }

#ifdef AWESOME_GATE
    // a row as the gate names it: its group's mark (! critical, @ chat,
    // - app, ~ spread silent) and its newest title, "+N" for a stack (a
    // spread one's header), "." and the title for a spread stack's card,
    // "silent:N" for the fold
    static void describeRow(std::string& out, const SDisp& D) {
        out += ' ';
        if (D.kind == SDisp::FOLD) {
            out += "silent:" + std::to_string(D.items.size());
            return;
        }
        out += D.kind == SDisp::MEMBER ? '.' : "!@-~"[std::min<size_t>(D.tier, 3)];
        out += D.items.front()->summary;
        if (D.items.size() > 1)
            out += "+" + std::to_string(D.items.size() - 1);
    }

    // "<status> [undo] | <rows>": the status line ("-", or "dnd:"/"quiet:"
    // and what it says), then every row the last warm built
    std::string centerDescribe() {
        std::string out = !s_status.on ? "-" : (s_status.dnd ? "dnd:" : "quiet:") + s_status.sub;
        if (Model::clearPending())
            out += " undo";
        out += " |";
        for (const auto& D : s_disp)
            describeRow(out, D);
        return out;
    }

    // "<px> <row>+<px> <motion>": how far the page stands below the
    // column's head ("?" past rows no warm measured), its top row and how
    // far into it, and what moves it
    std::string centerScrollDescribe() {
        const auto PX = distance({0, 0}, {s_skip, s_off});
        return std::format("{} {}+{} {}", PX ? std::to_string((int)std::lround(*PX)) : "?", s_skip, (int)std::lround(s_off),
                           s_motion == eMotion::GLIDE ? "glide" : s_motion == eMotion::FLING ? "fling" : "none");
    }

    // the rows the last draw placed on the page, whole or in part
    std::string centerPageDescribe() {
        std::string out;
        for (size_t i = s_skip; i <= s_lastVis && i < s_disp.size(); i++)
            describeRow(out, s_disp[i]);
        return out.empty() ? out : out.substr(1);
    }
#endif

} // namespace Awesome::Notify
