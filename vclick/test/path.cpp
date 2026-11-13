// The glide's path, headless: every path ends exactly on its target, stays
// in the window, is timed by Fitts's law, moves forward without jumps, and
// bows and overshoots as the profile says. `make -C vclick test`.
#include "path.hpp"

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
        if (D >= 300)
            ++longs;
        if (SEGS.size() == 2)
            ++over;
        // Fitts: the main movement within its clamp of a + b log2(D/W + 1)
        const double FITTS = std::clamp(fittsMs(D, P), 40.0, MAX_MOVE_MS);
        timed &= SEGS.size() == 2 ? MS >= FITTS * 0.6 && MS <= FITTS * 1.2 + 300 : std::abs(MS - FITTS) < 1e-6;
        double   farthest = 0;
        Vector2D prev     = FROM;
        // 8 ms ticks, as at 125 Hz
        for (double t = 0; t <= MS + 8; t += 8) {
            const auto Q = sample(SEGS, t);
            inside &= clampTo(Q, BOX).distance(Q) < 1e-9;
            // against the average step: a bell's peak is under 2x, a bow and an overshoot add a little
            maxStep = std::max(maxStep, Q.distance(prev) / (D * 8 / MS));
            // the distance still to go never grows much: a forward path, a correction allowed
            farthest = std::max(farthest, FROM.distance(Q));
            prev     = Q;
        }
        forward &= farthest <= D + 41;
        // the bow: the largest sideways distance stays under 25% of D (the clamp)
        const Vector2D U = (TO - FROM) / D;
        double         side = 0;
        for (double t = 0; t <= MS; t += 4) {
            const auto Q = sample(SEGS, t) - FROM;
            side         = std::max(side, std::abs(-Q.x * U.y + Q.y * U.x));
        }
        bowed &= side <= D * 0.26 + 1;
    }
    check(ends, "every path starts on its start and ends exactly on its target");
    check(inside, "every path stays in the window");
    check(timed, "a path's time is Fitts's law (a correction adds at most 300 ms)");
    check(forward, "no path runs past the target by more than the overshoot bound");
    check(bowed, "no path bows more than 25% of its length sideways");
    char steps[96];
    std::snprintf(steps, sizeof steps, "no tick moves over 3x the average step (worst %.2fx)", maxStep);
    check(maxStep < 3, steps);
    check(longs > 500 && over > longs * 0.2 && over < longs * 0.4, "about overshoot_p (30%) of the long moves overshoot");

    // the speed peaks where the profile says
    for (const double PEAK : {0.35, 0.45, 0.6}) {
        SProfile Q = P;
        Q.peak       = PEAK;
        Q.overshootP = 0;
        Q.bowMean = Q.bowSd = 0;
        const auto SEGS     = plan({0, 0}, {800, 0}, {1600, 900}, Q, rng);
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
        SProfile Q   = P;
        Q.overshootP = 0;
        const auto   SMALL = duration(plan({0, 0}, {600, 0}, {1600, 900}, Q, rng));
        const auto   WIDE  = duration(plan({0, 0}, {600, 0}, {1600, 900}, Q, rng, {.w = 128}));
        check(std::abs(SMALL - fittsMs(600, Q)) < 1e-6 && std::abs(WIDE - fittsMs(600, Q, 128)) < 1e-6 && WIDE < SMALL - 100,
              "a target's size is Fitts's W (128 px: sooner than 32 px)");
    }

    // a drag's way never overshoots, whatever the profile
    {
        SProfile Q   = P;
        Q.overshootP = 1;
        bool once    = true;
        for (int i = 0; i < 200; ++i)
            once &= plan({0, 0}, {900, 300}, {1600, 900}, Q, rng, {.overshoot = false}).size() == 1;
        check(once && plan({0, 0}, {900, 300}, {1600, 900}, Q, rng).size() == 2, "no overshoot when the aim forbids it (overshoot_p 1)");
    }

    // the profile file
    std::istringstream in{"# a comment\nfitts_a_ms=70\nfitts_b_ms = 140 \npeak=0.9\nrate_hz=1000\nnonsense\nbow_mean=x\nmoves=321\n"};
    int                n = 0;
    const auto         R = parseProfile(in, &n);
    check(R.aMs == 70 && R.bMs == 140, "profile: the Fitts constants");
    check(R.peak == 0.7, "profile: an out-of-range peak is clamped (0.9 -> 0.7)");
    check(R.rateHz == 1000 && R.moves == 321, "profile: the rate and the count");
    check(R.bowMean == SProfile{}.bowMean && n == 4, "profile: a bad value is skipped (4 values taken)");
    check(!readProfile("/nonexistent/vclick/profile"), "profile: a missing file is an error");

    std::printf("%s: %d of %d checks passed\n", fail ? "FAIL" : "ok", pass, pass + fail);
    return fail ? 1 : 0;
}
