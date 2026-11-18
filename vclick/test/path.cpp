// The glide's path, headless: every path ends exactly on its target, stays
// in the window, is timed by Fitts's law, moves forward without jumps, and
// bows and overshoots as the profile says. `make -C vclick test`.
#include "path.hpp"

#include <algorithm>
#include <vector>
#include "profile.hpp"

#include <cmath>
#include <cstdio>
#include <sstream>

using namespace VClick::Path;

static int pass = 0, fail = 0;

static void check(bool ok, const char* what) {
    if (ok)
        ++pass;
    else {
        ++fail;
        std::printf("  FAIL  %s\n", what);
    }
}

int main() {
    std::mt19937   rng{12345};
    SProfile       P;
    P.correctP = 0.3, P.underShare = 0, P.correctSize = 0.03; // every correction overshoots, and no tail
    P.tailShare = 0;
    const Vector2D BOX{1555, 830};

    check(plan({100, 100}, {101, 100}, BOX, P, rng).empty(), "under 2 px: no glide");

    // many random paths in the box
    int    over = 0, longs = 0;
    double maxStep = 0;
    bool   ends = true, inside = true, forward = true, timed = true, bowed = true;
    for (int i = 0; i < 4000; ++i) {
        std::uniform_real_distribution<double> X{0, BOX.x - 1}, Y{0, BOX.y - 1};
        const Vector2D                         FROM{X(rng), Y(rng)}, TO{X(rng), Y(rng)};
        const auto                             SEGS = plan(FROM, TO, BOX, P, rng);
        const double                           D    = FROM.distance(TO);
        if (D < 2)
            continue;
        const double MS = duration(SEGS);
        ends &= sample(SEGS, MS + 1) == TO && sample(SEGS, 1e9) == TO;
        ends &= sample(SEGS, 0).distance(FROM) < 1e-9;
        if (D >= 300) {
            ++longs;
            over += SEGS.size() == 2;
        }
        // Fitts: the main movement within its clamp of a + b log2(D/W + 1)
        const double FITTS = std::clamp(fittsMs(D, P), 40.0, MAX_MOVE_MS);
        timed &= MS >= std::max(FITTS * 0.81, 40.0) - 1e-6 && MS <= std::min(FITTS * 1.23, MAX_MOVE_MS) + MIN_MEND_MS;
        double   farthest = 0;
        Vector2D prev     = FROM;
        // 8 ms ticks, as at 125 Hz
        for (double t = 0; t <= MS + 8; t += 8) {
            const auto Q = sample(SEGS, t);
            inside &= clampTo(Q, BOX).distance(Q) < 1e-9;
            // against the first stroke's average step: a bell's peak is under 2x, a bow adds a little
            maxStep = std::max(maxStep, Q.distance(prev) / (D * 8 / SEGS[0].ms));
            // the distance still to go never grows much: a forward path, a correction allowed
            farthest = std::max(farthest, FROM.distance(Q));
            prev     = Q;
        }
        forward &= farthest <= D + 41;
        // the bow: the largest sideways distance stays under 25% of D (the clamp)
        const Vector2D U    = (TO - FROM) / D;
        double         side = 0;
        for (double t = 0; t <= MS; t += 4) {
            const auto Q = sample(SEGS, t) - FROM;
            side         = std::max(side, std::abs(-Q.x * U.y + Q.y * U.x));
        }
        bowed &= side <= D * 0.26 + 1;
    }
    check(ends, "every path starts on its start and ends exactly on its target");
    check(inside, "every path stays in the window");
    check(timed, "a path's time is Fitts's law within two PACE_SD, a correction's included (at most MIN_MEND_MS more)");
    check(forward, "no path runs past the target by more than the overshoot bound");
    check(bowed, "no path bows more than 25% of its length sideways");
    char steps[96];
    std::snprintf(steps, sizeof steps, "no tick moves over 3.5x the first stroke's average step (worst %.2fx)", maxStep);
    check(maxStep < 3.5, steps);
    check(longs > 500 && over > longs * 0.2 && over < longs * 0.4, "about correct_p (30%) of the long moves overshoot");

    // the speed peaks where the profile says
    for (const double PEAK : {0.35, 0.45, 0.6}) {
        SProfile Q   = P;
        Q.peak       = PEAK;
        Q.correctP  = 0;
        Q.bowMedian = 0;
        const auto   SEGS   = plan({0, 0}, {800, 0}, {1600, 900}, Q, rng);
        const double MS     = duration(SEGS);
        double       best = 0, at = 0;
        for (double t = 1; t < MS; t += 1) {
            const double V = sample(SEGS, t).x - sample(SEGS, t - 1).x;
            if (V > best)
                best = V, at = t / MS;
        }
        char what[96];
        std::snprintf(what, sizeof what, "peak %.2f: the speed peaks at %.2f", PEAK, at);
        check(std::abs(at - PEAK) < 0.03, what);
    }

    // the target's size is Fitts's W: a wider target is reached sooner
    {
        SProfile Q       = P;
        Q.correctP       = 0;
        const auto TYPICAL = [&](double w) {
            std::vector<double> ms;
            for (int i = 0; i < 401; ++i)
                ms.push_back(duration(plan({0, 0}, {600, 0}, {1600, 900}, Q, rng, {.w = w})));
            std::ranges::nth_element(ms, ms.begin() + 200);
            return ms[200];
        };
        const auto SMALL = TYPICAL(32), WIDE = TYPICAL(128);
        check(std::abs(SMALL / fittsMs(600, Q) - 1) < 0.03 && std::abs(WIDE / fittsMs(600, Q, 128) - 1) < 0.03 && WIDE < SMALL - 50,
              "a target's size is Fitts's W (128 px: sooner than 32 px)");
    }

    // a drag's way never overshoots, whatever the profile
    {
        SProfile Q = P;
        Q.correctP = 1;
        bool once    = true;
        for (int i = 0; i < 200; ++i)
            once &= plan({0, 0}, {900, 300}, {1600, 900}, Q, rng, {.overshoot = false}).size() == 1;
        check(once && plan({0, 0}, {900, 300}, {1600, 900}, Q, rng).size() == 2, "no overshoot when the aim forbids it (correct_p 1)");
    }

    // a finger's stroke: Fitts's time within its bounds, from none to all
    // of the way, never back, the bell's ends slow
    {
        check(stroke(1, P).ms == MIN_STROKE_MS && stroke(1e12, P).ms == MAX_MOVE_MS && std::abs(stroke(400, P).ms - fittsMs(400, P)) < 1e-9,
              "a stroke takes Fitts's time for its length, 100-1000 ms");
        const auto S       = stroke(400, P);
        bool       forward = along(S, 0).x == 0 && along(S, S.ms) == Vector2D{1, 0} && along(S, 1e9) == Vector2D{1, 0};
        double     prev = 0, first = 0, last = 0, peak = 0;
        for (double t = 8; t < S.ms; t += 8) {
            const auto Q = along(S, t);
            forward &= Q.y == 0 && Q.x >= prev;
            if (!first)
                first = Q.x - prev;
            last = Q.x - prev;
            peak = std::max(peak, Q.x - prev);
            prev = Q.x;
        }
        check(forward, "a stroke goes from none to all of the way, straight, never back");
        check(first < peak / 4 && last < peak / 4, "  slow at both ends, as a finger's");
    }

    // a sharper bell peaks higher over the same time
    {
        SProfile Q  = P;
        Q.correctP  = 0;
        Q.bowMedian = 0;
        const auto PEAK = [&](double bell) {
            Q.bell            = bell;
            const auto   SEGS = plan({0, 0}, {800, 0}, {1600, 900}, Q, rng);
            double       best = 0;
            for (double t = 1; t < duration(SEGS); t += 1)
                best = std::max(best, sample(SEGS, t).x - sample(SEGS, t - 1).x);
            return best * duration(SEGS); // over the move's own time, which varies
        };
        check(PEAK(12) > PEAK(4) * 1.4, "a bell of 12 peaks well above a bell of 4");
    }

    // a correction: from short of the target or past it, overlapping the
    // first stroke, and the sum ends exactly on the target
    {
        SProfile Q      = P;
        Q.correctP      = 1;
        Q.correctSize   = 0.1;
        Q.bowMedian     = 0;
        const Vector2D FROM{100, 100}, TO{700, 100};
        bool           shortOf = true, past = true, exact = true, overlapped = false, near = true;
        for (int i = 0; i < 300; ++i) {
            Q.underShare = 1;
            auto S       = plan(FROM, TO, {1600, 900}, Q, rng);
            shortOf &= S.size() == 2 && S[0].p3.x < TO.x && duration(S) >= fittsMs(600, Q) * 0.81 - 1e-6 && duration(S) <= fittsMs(600, Q) * 1.23 + MIN_MEND_MS;
            overlapped |= S[1].start < S[0].ms - 1;
            exact &= sample(S, duration(S)) == TO && sample(S, duration(S) - 1e-6).distance(TO) < 0.01;
            Q.underShare = 0;
            S            = plan(FROM, TO, {1600, 900}, Q, rng);
            past &= S.size() == 2 && S[0].p3.x > TO.x && S[0].p3.x <= TO.x + 40;
        }
        check(shortOf, "a correction from short: the first stroke lands short, in Fitts's time for the whole");
        check(past, "a correction from past the target: by at most 40 px");
        check(overlapped, "  it can begin before the first stroke has stopped");
        check(exact, "  the sum ends exactly on the target");
        Q.underShare = 1;
        for (int i = 0; i < 200; ++i)
            near &= plan({100, 100}, {180, 100}, {1600, 900}, Q, rng).size() == 1;
        check(near, "a move under 100 px lands in one stroke");
    }

    // a slow end: on top of the stroke's time, on as many moves as the profile
    // says, and the window's edge still holds the sum
    {
        SProfile Q   = P;
        Q.correctP   = 0;
        Q.bowMedian  = 0;
        const auto PEAK = [&](double tail) {
            Q.tailShare = tail, Q.tailSize = 0.05;
            const auto S    = plan({0, 0}, {1000, 0}, {1600, 900}, Q, rng);
            double     best = 0;
            for (double t = 1; t < duration(S); t += 1)
                best = std::max(best, sample(S, t).x - sample(S, t - 1).x);
            return best * S[0].ms; // over the first stroke's own time, which varies
        };
        const double ALONE = PEAK(0), SLOWED = PEAK(0.5);
        check(SLOWED > ALONE * 0.8 && SLOWED < ALONE * 1.1, "a slow end leaves the first stroke's speed alone");
        const auto TAILED = plan({0, 0}, {1000, 0}, {1600, 900}, Q, rng);
        check(std::abs(duration(TAILED) - TAILED[0].ms / 0.5) < 1e-6, "  and comes on top of its time");
        Q.tailShare = 0.8;
        Q.aMs = 400, Q.bMs = 400;
        check(duration(plan({0, 0}, {1000, 0}, {1600, 900}, Q, rng)) <= MAX_WHOLE_MS + 1e-6, "  within MAX_WHOLE_MS");
        Q.aMs = P.aMs, Q.bMs = P.bMs, Q.tailShare = 0.5;
        Q.tailP = 0.5, Q.tailPSlope = 0.1;
        check(std::abs(tailChance(300, Q) - 0.5) < 1e-9 && std::abs(tailChance(600, Q) - 0.6) < 1e-9 && tailChance(1e12, Q) == 1 && tailChance(1e-3, Q) == 0, "how often grows with each doubling of the distance, within 0-1");
        int slowed = 0;
        for (int i = 0; i < 2000; ++i)
            slowed += plan({0, 0}, {300, 0}, {1600, 900}, Q, rng).size() == 2;
        check(slowed > 850 && slowed < 1150, "  about tail_p of the moves end slowly");
        Q.tailP = 1, Q.tailPSlope = 0;
        bool inside = true;
        Q.bowMedian = 0.2, Q.correctP = 1, Q.underShare = 0, Q.correctSize = 0.15;
        for (int i = 0; i < 500; ++i) {
            const auto S = plan({10, 10}, {1590, 890}, {1600, 900}, Q, rng);
            for (double t = 0; t < duration(S); t += 4) {
                const auto AT = sample(S, t);
                inside &= AT.x >= 0 && AT.y >= 0 && AT.x <= 1599 && AT.y <= 899;
            }
        }
        check(inside, "  the sum of two strokes stays in the window");
    }

    // a long move may drift first: its stroke starts from where it drifted to
    {
        SProfile Q = P;
        Q.correctP = 0, Q.leadP = 1, Q.leadMs = 300, Q.leadSize = 0.2, Q.bowMedian = 0;
        const Vector2D FROM{100, 100}, TO{1100, 100};
        bool           drifts = true, exact = true, joined = true;
        for (int i = 0; i < 300; ++i) {
            const auto S = plan(FROM, TO, {1600, 900}, Q, rng);
            drifts &= S.size() >= 2 && S[0].ms >= 210 - 1e-6 && S[0].ms <= 390 + 1e-6 && S[0].p3.x >= 100 + 1000 * 0.12 - 1e-6 && S[0].p3.x <= 100 + 1000 * 0.28 + 1e-6;
            joined &= S[1].start == S[0].ms && S[1].p0 == S[0].p3;
            exact &= sample(S, duration(S)) == TO && sample(S, 0).distance(FROM) < 1e-9;
        }
        check(drifts, "a drift takes about lead_ms over about lead_size of the way");
        check(joined, "  and the stroke starts where it ends");
        check(exact, "  the way still ends exactly on the target");
        check(plan({100, 100}, {399, 100}, {1600, 900}, Q, rng).size() == 1, "  a move under 300 px never drifts");
        check(plan(FROM, TO, {1600, 900}, Q, rng, {.overshoot = false}).size() == 1, "  nor does a drag");
    }

    // the bow grows with the distance as the profile says
    {
        SProfile Q = P;
        Q.correctP = 0, Q.sShare = 0, Q.bowSigma = 0, Q.bowMedian = 0.04, Q.bowSlope = 0.5;
        const auto BOW = [&](double d) {
            double most = 0;
            for (int i = 0; i < 40; ++i) {
                const auto S = plan(Vector2D{100.0, 400.0}, Vector2D{100.0 + d, 400.0}, {1600, 900}, Q, rng);
                for (double t = 0; t <= duration(S); t += 2)
                    most = std::max(most, std::abs(sample(S, t).y - 400) / d);
            }
            return most;
        };
        check(std::abs(BOW(1200) / BOW(300) - 2) < 0.3, "a bow of slope 0.5 doubles from 300 to 1200 px");
    }

    // each move draws its own time
    {
        SProfile Q = P;
        Q.correctP = 0, Q.sShare = 0;
        double sum = 0, sq = 0, low = 1e9, high = 0;
        for (int i = 0; i < 4000; ++i) {
            const double R = duration(plan({0, 0}, {600, 0}, {1600, 900}, Q, rng)) / fittsMs(600, Q);
            sum += std::log(R), sq += std::log(R) * std::log(R), low = std::min(low, R), high = std::max(high, R);
        }
        const double SD = std::sqrt(sq / 4000 - sum / 4000 * (sum / 4000));
        check(SD > 0.07 && SD < 0.11, "a move's time varies by about PACE_SD");
        check(low >= 0.81 && high <= 1.23, "  within two sigma");
    }

    // s_share of the moves from 100 px up bend both ways, as far as a plain bow
    {
        SProfile Q   = P;
        Q.correctP   = 0, Q.tailShare = 0, Q.bowSigma = 0, Q.bowMedian = 0.1, Q.sShare = 0.5;
        const auto LOBES = [&](const std::vector<SSeg>& S, double D) {
            double hi = 0, lo = 0;
            for (double t = 0; t <= duration(S); t += 2) {
                const double SIDE = (sample(S, t).y - 400) / D;
                hi = std::max(hi, SIDE), lo = std::min(lo, SIDE);
            }
            return std::pair{hi, lo};
        };
        int  both = 0;
        bool even = true, near = true;
        for (int i = 0; i < 1000; ++i) {
            const auto [HI, LO] = LOBES(plan({100, 400}, {900, 400}, {1600, 900}, Q, rng), 800);
            if (std::min(HI, -LO) > 0.02) {
                ++both;
                even &= std::max(HI, -LO) > 0.07 && std::max(HI, -LO) < 0.14;
            }
            const auto [H2, L2] = LOBES(plan({100, 400}, {180, 400}, {1600, 900}, Q, rng), 80);
            near &= std::min(H2, -L2) < 0.005;
        }
        check(both > 420 && both < 580, "about s_share of the moves bend both ways");
        check(even, "  as far as a plain bow");
        check(near, "  and none under 100 px");
    }

    // the time the way's end spends near the target, and the rest's draw
    {
        SProfile Q      = P;
        Q.correctP      = 0;
        Q.bowMedian     = 0;
        const auto SEGS = plan({0, 0}, {800, 0}, {1600, 900}, Q, rng);
        const auto NEAR = settleMs(SEGS, 3);
        check(NEAR > 0 && NEAR < duration(SEGS) / 2 && sample(SEGS, duration(SEGS) - NEAR - 4).distance({800, 0}) > 3, "settleMs: the end of the way spent within 3 px");
        bool   bounded = true;
        int    under   = 0;
        for (int i = 0; i < 4000; ++i) {
            const double V = VClick::Profile::draw(100, 0.5, rng);
            bounded &= V >= 100 * std::exp(-1.0) - 1e-9 && V <= 100 * std::exp(1.0) + 1e-9;
            under += V < 100;
        }
        check(bounded && under > 1800 && under < 2200, "a draw stays within two sigma of its median, half on each side");
    }

    // the profile file
    std::istringstream in{"# a comment\nstroke_a_ms=70\nstroke_b_ms = 140 \npeak=0.9\nrate_hz=1000\nnonsense\nbow_median=x\nmoves=321\n"};
    int                n = 0;
    const auto         R = VClick::Profile::parse(in, &n);
    check(R.aMs == 70 && R.bMs == 140, "profile: the Fitts constants");
    check(R.peak == 0.7, "profile: an out-of-range peak is clamped (0.9 -> 0.7)");
    check(R.rateHz == 1000 && R.moves == 321, "profile: the rate and the count");
    check(R.bowMedian == SProfile{}.bowMedian && n == 4, "profile: a bad value is skipped (4 values taken)");
    check(!VClick::Profile::read("/nonexistent/vclick/profile"), "profile: a missing file is an error");
    std::istringstream old{"fitts_a_ms=114\nfitts_b_ms=75\n"};
    const auto         O = VClick::Profile::parse(old, &n);
    check(O.aMs == SProfile{}.aMs && O.bMs == SProfile{}.bMs, "profile: the old whole-move keys mean nothing now");

    std::printf("%s: %d of %d checks passed\n", fail ? "FAIL" : "ok", pass, pass + fail);
    return fail ? 1 : 0;
}
