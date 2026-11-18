// The user's pace: what learn.py fits from their own movements, and how it
// is read back. Pure: no compositor in it, so test/path.cpp checks it headless.
#pragma once

#include <expected>
#include <iosfwd>
#include <random>
#include <string>

namespace VClick::Profile {
    constexpr double TARGET_W = 32; // Fitts's W unless the request names it: vclick cannot see the target

    // these defaults are a typical mouse user's
    struct SProfile {
        double      aMs = 50, bMs = 120;                 // Fitts: MT = a + b * log2(D / W + 1)
        double      bell = 4, peak = 0.45;               // the speed t^A (1-t)^B: A + B, and where it peaks, 0-1 of the movement
        double      bowMedian = 0.05, bowSigma = 0.8;    // the largest sideways distance over the length: log-normal
        double      bowSlope = 0;                        // the bow grows as (D / 300 px) to this power
        double      leadP = 0, leadMs = 300, leadSize = 0.15; // moves from 300 px up that drift first: how many, how long, how far of the way
        double      sShare = 0.15;                       // of the moves from 100 px up, those that bend both ways
        double      correctP = 0.05, underShare = 0.6;   // how often a small correction follows, how often from short of the target
        double      correctSize = 0.05;                  // how far off the first stroke lands, of D
        double      tailP = 1, tailPSlope = 0, tailShare = 0, tailSize = 0.03;   // a long move's slow end: its share of the time, and of the way
        double      restMs = 120, restSigma = 0.6;       // from arriving within NEAR_PX to a click: log-normal
        double      holdMs = 100, holdSigma = 0.25;      // a click's button held down: log-normal
        double      doubleGapMs = 120;                   // from a release to the next click's press
        double      dragLeadMs = 100, dropRestMs = 80;   // a drag: from the press to NEAR_PX away; from NEAR_PX of the drop to the release
        double      wheelMs = 50;                        // between the steps of a wheel run
        double      rateHz = 125;                        // motion events a second
        int         moves  = 0;                          // how many movements it was fitted from
        std::string source = "the built-in defaults";
    };

    constexpr double NEAR_PX = 3; // arrived: what a recording can tell from a hand at rest

    // a draw of a log-normal, within two sigma of its median
    double draw(double median, double sigma, std::mt19937& rng);

    // $XDG_STATE_HOME/vclick/profile, else ~/.local/state/vclick/profile
    std::string defaultPath();
    // key=value lines; unknown keys and bad values are skipped, values
    // are clamped to sane bounds; values: how many were taken
    SProfile                             parse(std::istream& in, int* values = nullptr);
    std::expected<SProfile, std::string> read(const std::string& path);
    std::string                          describe(const SProfile& p);
}
