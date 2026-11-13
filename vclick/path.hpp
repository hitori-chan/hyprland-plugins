// The glide's path, its timing and the user's pace: pure math with no
// compositor in it, so test/path.cpp checks it headless.
//
// A glide is one movement, or two when a long one overshoots: each a cubic
// Bézier whose inner control points sit off the straight line (a hand's
// arc), timed by Fitts's law, its speed bell-shaped as a hand's: zero at
// both ends, peaking where the user's does (a beta curve; centred, it is the
// minimum-jerk profile of a reaching arm).
#pragma once

#include <hyprutils/math/Vector2D.hpp>

#include <expected>
#include <iosfwd>
#include <random>
#include <string>
#include <vector>

namespace VClick::Path {
    using Hyprutils::Math::Vector2D;

    constexpr double TARGET_W     = 32;   // Fitts's W unless the request names it: vclick cannot see the target
    constexpr double MIN_TARGET_W = 4, MAX_TARGET_W = 1000;
    constexpr double MAX_MOVE_MS  = 1000; // one movement, before a correction

    // The pace: learn.py fits it from the user's own movements; these
    // defaults are a typical mouse user's.
    struct SProfile {
        double      aMs = 50, bMs = 120;                // Fitts: MT = a + b * log2(D / W + 1)
        double      bowMean = 0.06, bowSd = 0.04;       // the largest sideways distance, over the length
        double      peak       = 0.45;                  // where the speed peaks, 0-1 of the movement
        double      overshootP = 0.3, overshoot = 0.03; // of movements >= 300 px: how often, by how much of D
        double      dwellMs = 80;                       // still on the target before a click
        double      rateHz  = 125;                      // motion events a second
        int         moves   = 0;                        // how many movements it was fitted from
        std::string source  = "the built-in defaults";
    };

    struct SSeg {
        Vector2D            p0, p1, p2, p3;
        double              ms = 0;
        std::vector<double> progress; // the share of the way done, at WARP_N + 1 even times
    };
    constexpr int WARP_N = 128;

    // what a movement aims at
    struct SAim {
        double w = TARGET_W; // the target's size: Fitts's W
    };

    double            fittsMs(double distance, const SProfile& p, double w = TARGET_W);
    Vector2D          clampTo(const Vector2D& p, const Vector2D& bounds);

    // from and to in a box of size bounds (to inside it); empty when they
    // are under 2 px apart
    std::vector<SSeg> plan(const Vector2D& from, const Vector2D& to, const Vector2D& bounds, const SProfile& p, std::mt19937& rng, const SAim& aim = {});
    double            duration(const std::vector<SSeg>& segs);
    Vector2D          sample(const std::vector<SSeg>& segs, double ms); // segs not empty; past the end: the target

    // $XDG_STATE_HOME/vclick/profile, else ~/.local/state/vclick/profile
    std::string                         defaultProfilePath();
    // key=value lines; unknown keys and bad values are skipped, values
    // are clamped to sane bounds; values: how many were taken
    SProfile                            parseProfile(std::istream& in, int* values = nullptr);
    std::expected<SProfile, std::string> readProfile(const std::string& path);
    std::string                         describe(const SProfile& p);
}
