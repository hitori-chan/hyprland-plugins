// The user's pace: what learn.py fits from their own movements, and how it
// is read back. Pure: no compositor in it, so test/path.cpp checks it headless.
#pragma once

#include <expected>
#include <iosfwd>
#include <string>

namespace VClick::Profile {
    constexpr double TARGET_W = 32; // Fitts's W unless the request names it: vclick cannot see the target

    // these defaults are a typical mouse user's
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

    // $XDG_STATE_HOME/vclick/profile, else ~/.local/state/vclick/profile
    std::string defaultPath();
    // key=value lines; unknown keys and bad values are skipped, values
    // are clamped to sane bounds; values: how many were taken
    SProfile                             parse(std::istream& in, int* values = nullptr);
    std::expected<SProfile, std::string> read(const std::string& path);
    std::string                          describe(const SProfile& p);
}
