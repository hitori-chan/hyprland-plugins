#include "path.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace VClick::Path {
    double fittsMs(double distance, const SProfile& p, double w) {
        return p.aMs + p.bMs * std::log2(distance / w + 1.0);
    }

    Vector2D clampTo(const Vector2D& p, const Vector2D& bounds) {
        return {std::clamp(p.x, 0.0, std::max(0.0, bounds.x - 1)), std::clamp(p.y, 0.0, std::max(0.0, bounds.y - 1))};
    }

    static Vector2D bezier(const SSeg& s, double u) {
        const double V = 1 - u;
        return s.p0 * (V * V * V) + s.p1 * (3 * V * V * u) + s.p2 * (3 * V * u * u) + s.p3 * (u * u * u);
    }

    // The speed: t^A (1-t)^B, A + B = bell, peaking at A / bell = peak; a bell
    // of 4 peaking at 0.5 is 30 t^2 (1-t)^2, the minimum-jerk speed. Its
    // integral, tabled.
    static std::vector<double> progressFor(const SProfile& p) {
        const double        A = p.bell * p.peak, B = p.bell * (1 - p.peak);
        std::vector<double> c(WARP_N + 1, 0.0);
        for (int i = 1; i <= WARP_N; ++i) {
            const double T = (i - 0.5) / WARP_N;
            c[i]           = c[i - 1] + std::pow(T, A) * std::pow(1 - T, B);
        }
        for (auto& v : c)
            v /= c[WARP_N];
        return c;
    }

    static double progress(const SSeg& s, double t) {
        const double X = std::clamp(t, 0.0, 1.0) * WARP_N;
        const int    I = std::min(static_cast<int>(X), WARP_N - 1);
        return s.progress[I] + (s.progress[I + 1] - s.progress[I]) * (X - I);
    }

    // control points on the line, at a third and two thirds: the point moves
    // along it exactly as the progress says
    static SSeg straight(const Vector2D& from, const Vector2D& to, double ms, std::vector<double> progress) {
        return {from, from + (to - from) / 3, from + (to - from) * (2.0 / 3), to, ms, std::move(progress)};
    }

    double tailAt(double distance, const SProfile& p) {
        return p.tailShare * std::clamp(std::log2(std::max(distance, 1.0) / NEAR_TAIL_PX) / std::log2(FAR_TAIL_PX / NEAR_TAIL_PX), 0.0, 1.0);
    }

    std::vector<SSeg> plan(const Vector2D& from, const Vector2D& to, const Vector2D& bounds, const SProfile& p, std::mt19937& rng, const SAim& aim) {
        const double D = from.distance(to);
        if (D < 2)
            return {};
        auto           uni   = [&](double a, double b) { return std::uniform_real_distribution<double>{a, b}(rng); };
        const Vector2D U     = (to - from) / D;
        const double   FITTS = std::clamp(fittsMs(D, p, aim.w), 40.0, MAX_MOVE_MS);
        double         total = FITTS;

        // where the first stroke lands, and its share of the time: short of
        // the target or past it, for a correction to mend; else, on a long
        // move, short of it for the slow approach
        Vector2D end   = to;
        double   share = 1;
        if (D >= MIN_CORRECT_PX && p.correctSize > 0 && uni(0, 1) < p.correctP) {
            const bool SHORT = uni(0, 1) < p.underShare;
            if (SHORT || aim.overshoot) {
                const double OFF = std::clamp(D * p.correctSize * uni(0.6, 1.4), 3.0, SHORT ? 100.0 : 40.0);
                end              = clampTo(to + U * (SHORT ? -OFF : OFF), bounds);
                share            = CORRECT_SHARE;
            }
        }
        if (end == to && p.tailSize > 0 && tailAt(D, p) > 0) {
            end   = to - U * (D * p.tailSize);
            share = 1 - tailAt(D, p);
            total = FITTS / share; // the first stroke keeps Fitts's time: the slow end comes on top
        }
        if (end.distance(to) < 2) {
            end   = to;
            share = 1;
        }

        // Control points at a third and two thirds of the line keep the
        // progress along it the speed profile's; their sideways offsets h
        // bow the curve by at most 0.75 h. The curve stays in the convex
        // hull of its control points: kept in the window, so is the path.
        // A bow the window's edge would cut takes the other side.
        const double   DE  = from.distance(end);
        const Vector2D N   = Vector2D{-(end - from).y, (end - from).x} / DE;
        const double   BOW = std::clamp(Profile::draw(p.bowMedian, p.bowSigma, rng), 0.0, 0.25);
        const double   H1 = std::min(BOW * uni(0.85, 1.15), 0.25) * DE / 0.75, H2 = std::min(BOW * uni(0.85, 1.15), 0.25) * DE / 0.75;
        const auto     CONTROLS = [&](double side) { return std::pair{from + (end - from) / 3 + N * (side * H1), from + (end - from) * (2.0 / 3) + N * (side * H2)}; };
        const auto     CUT      = [&](const std::pair<Vector2D, Vector2D>& c) { return clampTo(c.first, bounds).distance(c.first) + clampTo(c.second, bounds).distance(c.second); };
        const double   SIDE     = uni(0, 1) < 0.5 ? -1 : 1;
        auto           c        = CONTROLS(SIDE);
        if (const auto OTHER = CONTROLS(-SIDE); CUT(OTHER) < CUT(c))
            c = OTHER;

        const auto        PROGRESS = progressFor(p);
        std::vector<SSeg> segs;
        segs.push_back({from, clampTo(c.first, bounds), clampTo(c.second, bounds), end, total * share, PROGRESS});
        segs[0].bounds = bounds;
        if (end != to) { // about as the first stops: a little before it, or just after
            const double START = segs[0].ms * uni(0.95, 1.1);
            auto         mend  = straight(end, to, std::max(total - START, MIN_MEND_MS), PROGRESS);
            mend.start         = START;
            segs.push_back(std::move(mend));
        }
        return segs;
    }

    SSeg stroke(double px, const SProfile& p) {
        return straight({0, 0}, {1, 0}, std::clamp(fittsMs(px, p), MIN_STROKE_MS, MAX_MOVE_MS), progressFor(p));
    }

    SSeg line(const Vector2D& from, const Vector2D& to, double ms) {
        std::vector<double> even(WARP_N + 1);
        for (int i = 0; i <= WARP_N; ++i)
            even[i] = static_cast<double>(i) / WARP_N;
        return straight(from, to, ms, std::move(even));
    }

    double duration(const std::vector<SSeg>& segs) {
        double ms = 0;
        for (const auto& s : segs)
            ms = std::max(ms, s.start + s.ms);
        return ms;
    }

    double leaveMs(const std::vector<SSeg>& segs, double px) {
        if (segs.empty())
            return 0;
        const double MS = duration(segs);
        for (double t = 0; t < MS; t += 2)
            if (sample(segs, t).distance(segs.front().p0) > px)
                return t;
        return MS;
    }

    double settleMs(const std::vector<SSeg>& segs, double px) {
        if (segs.empty())
            return 0;
        const double MS = duration(segs);
        for (double t = MS; t > 0; t -= 2)
            if (sample(segs, t).distance(segs.back().p3) > px)
                return MS - t;
        return MS;
    }

    Vector2D along(const SSeg& s, double ms) {
        return ms < s.ms ? bezier(s, progress(s, ms / s.ms)) : s.p3;
    }

    Vector2D sample(const std::vector<SSeg>& segs, double ms) {
        if (ms >= duration(segs))
            return segs.back().p3;
        if (segs.size() == 1)
            return along(segs[0], ms); // the control points' hull keeps it in the window
        Vector2D at = segs.front().p0;
        for (const auto& s : segs)
            at = at + (along(s, ms - s.start) - s.p0);
        return clampTo(at, segs.front().bounds);
    }
}
