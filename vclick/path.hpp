// The glide's path and its timing: pure math with no
// compositor in it, so test/path.cpp checks it headless.
//
// A glide is a first stroke, and sometimes a second that overlaps its end:
// a small correction, or a long move's slow approach. Their displacements add,
// so the sum ends exactly on the target. The first is a cubic Bézier whose
// inner control points sit off the straight line (a hand's arc), the whole
// timed by Fitts's law, each stroke's speed bell-shaped as a hand's: zero at
// both ends, as sharp as the user's and peaking where theirs does (a beta
// curve; of 4, centred, it is the minimum-jerk profile of a reaching arm).
#pragma once

#include "profile.hpp"

#include <hyprutils/math/Vector2D.hpp>

#include <random>
#include <vector>

namespace VClick::Path {
    using Hyprutils::Math::Vector2D;

    using Profile::SProfile;
    using Profile::TARGET_W;

    constexpr double MIN_TARGET_W = 4, MAX_TARGET_W = 1000;
    constexpr double MAX_MOVE_MS = 1000;    // one movement
    constexpr double MAX_WHOLE_MS = 2000;   // a stroke and its slow end
    constexpr double MIN_LEAD_PX  = 300;    // a shorter move never drifts first
    constexpr double BOW_AT_PX    = 300;    // where bow_median holds
    constexpr double PACE_SD     = 0.10;    // a move's own time, log-normal: not learned, a recording's spread is mostly its targets' sizes
    constexpr double S_BOW       = 2.4;     // control points on either side bow 0.29 h, not 0.75 h: about the same bow, and within the 25% bound
    constexpr double MIN_CORRECT_PX = 100;  // a shorter move lands in one stroke
    constexpr double CORRECT_SHARE  = 0.9;  // of a corrected move's time, the first stroke's
    constexpr double MIN_MEND_MS    = 40;   // a second stroke is never a twitch
    constexpr double TAIL_AT_PX = 300; // where tail_p holds

    struct SSeg {
        Vector2D            p0, p1, p2, p3;
        double              ms = 0;
        std::vector<double> progress; // the share of the way done, at WARP_N + 1 even times
        double              start = 0; // when it begins, in the movement's time
        Vector2D            bounds;    // the first stroke's: the box the sum stays in
    };
    constexpr int WARP_N = 128;

    // what a movement aims at
    struct SAim {
        double w         = TARGET_W; // the target's size: Fitts's W
        bool   overshoot = true;     // a move may pass the target, as the profile says (a drag's never does)
    };

    double   fittsMs(double distance, const SProfile& p, double w = TARGET_W);
    double   tailChance(double distance, const SProfile& p); // how likely a move of this length ends slowly
    Vector2D clampTo(const Vector2D& p, const Vector2D& bounds);

    // from and to in a box of size bounds (to inside it); empty when they
    // are under 2 px apart
    std::vector<SSeg> plan(const Vector2D& from, const Vector2D& to, const Vector2D& bounds, const SProfile& p, std::mt19937& rng, const SAim& aim = {});
    double            duration(const std::vector<SSeg>& segs);
    Vector2D          sample(const std::vector<SSeg>& segs, double ms); // segs not empty; past the end: the target
    double            settleMs(const std::vector<SSeg>& segs, double px); // how long the end of the way spends within px of the target
    double            leaveMs(const std::vector<SSeg>& segs, double px);  // how long the way takes to get px from its start
    Vector2D          along(const SSeg& s, double ms);                  // one segment's point; past its end: its end

    // A finger's stroke on a touchpad: one straight movement, its x the
    // share done (0 to 1), its speed the profile's bell; in Fitts's time
    // for its length (W 32 px), 100-1000 ms
    constexpr double MIN_STROKE_MS = 100;
    SSeg             stroke(double px, const SProfile& p);

    // straight, at one speed, ms long
    SSeg line(const Vector2D& from, const Vector2D& to, double ms);
}
