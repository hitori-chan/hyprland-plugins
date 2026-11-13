#include "path.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <optional>
#include <string_view>
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

    // The speed: t^a (1-t)^b, a + b = 4, peaking at a / 4 = peak; at 0.5 it
    // is 30 t^2 (1-t)^2, the minimum-jerk speed. Its integral, tabled.
    static std::vector<double> progressFor(double peak) {
        const double        A = 4 * peak, B = 4 * (1 - peak);
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

    std::vector<SSeg> plan(const Vector2D& from, const Vector2D& to, const Vector2D& bounds, const SProfile& p, std::mt19937& rng, const SAim& aim) {
        const double D = from.distance(to);
        if (D < 2)
            return {};
        auto uni = [&](double a, double b) { return std::uniform_real_distribution<double>{a, b}(rng); };

        // the main movement ends on the target, or past it
        Vector2D end = to;
        if (D >= 300 && p.overshoot > 0 && uni(0, 1) < p.overshootP) {
            const double OVER = std::clamp(D * p.overshoot * uni(0.6, 1.4), 3.0, 40.0);
            end               = clampTo(to + (to - from) / D * OVER, bounds);
            if (end.distance(to) < 2)
                end = to;
        }

        // Control points at a third and two thirds of the line keep the
        // progress along it the speed profile's; their sideways offsets h
        // bow the curve by at most 0.75 h. The curve stays in the convex
        // hull of its control points: kept in the window, so is the path.
        // A bow the window's edge would cut takes the other side.
        const double   DE  = from.distance(end);
        const Vector2D N   = Vector2D{-(end - from).y, (end - from).x} / DE;
        const double   BOW = std::clamp(std::abs(std::normal_distribution<double>{p.bowMean, p.bowSd}(rng)), 0.0, 0.25);
        const double   H1 = BOW * DE / 0.75 * uni(0.85, 1.15), H2 = BOW * DE / 0.75 * uni(0.85, 1.15);
        const auto     CONTROLS = [&](double side) {
            return std::pair{from + (end - from) / 3 + N * (side * H1), from + (end - from) * (2.0 / 3) + N * (side * H2)};
        };
        const auto CUT = [&](const std::pair<Vector2D, Vector2D>& c) {
            return clampTo(c.first, bounds).distance(c.first) + clampTo(c.second, bounds).distance(c.second);
        };
        const double SIDE = uni(0, 1) < 0.5 ? -1 : 1;
        auto         c    = CONTROLS(SIDE);
        if (const auto OTHER = CONTROLS(-SIDE); CUT(OTHER) < CUT(c))
            c = OTHER;

        const auto        PROGRESS = progressFor(p.peak);
        std::vector<SSeg> segs;
        segs.push_back({from, clampTo(c.first, bounds), clampTo(c.second, bounds), end, std::clamp(fittsMs(DE, p, aim.w) * (end == to ? 1.0 : 0.85), 40.0, MAX_MOVE_MS),
                        PROGRESS});
        if (end != to) // the correction: short and straight
            segs.push_back({end, end + (to - end) / 3, end + (to - end) * (2.0 / 3), to, std::clamp(fittsMs(end.distance(to), p, aim.w) * 0.8, 60.0, 300.0), PROGRESS});
        return segs;
    }

    double duration(const std::vector<SSeg>& segs) {
        double ms = 0;
        for (const auto& s : segs)
            ms += s.ms;
        return ms;
    }

    Vector2D sample(const std::vector<SSeg>& segs, double ms) {
        for (const auto& s : segs) {
            if (ms < s.ms)
                return bezier(s, progress(s, ms / s.ms));
            ms -= s.ms;
        }
        return segs.back().p3;
    }

    std::string defaultProfilePath() {
        if (const char* s = std::getenv("XDG_STATE_HOME"); s && *s)
            return std::string{s} + "/vclick/profile";
        if (const char* h = std::getenv("HOME"); h && *h)
            return std::string{h} + "/.local/state/vclick/profile";
        return "";
    }

    static std::optional<double> number(std::string_view s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\r'))
            s.remove_suffix(1);
        while (!s.empty() && s.front() == ' ')
            s.remove_prefix(1);
        double     v{};
        const auto END        = s.data() + s.size();
        const auto [ptr, err] = std::from_chars(s.data(), END, v);
        if (err != std::errc{} || ptr != END || !std::isfinite(v))
            return std::nullopt;
        return v;
    }

    SProfile parseProfile(std::istream& in, int* values) {
        SProfile    p;
        int         n = 0;
        std::string line;
        while (std::getline(in, line)) {
            const auto EQ = line.find('=');
            if (line.empty() || line[0] == '#' || EQ == std::string::npos)
                continue;
            std::string_view K{line.data(), EQ};
            while (!K.empty() && K.back() == ' ')
                K.remove_suffix(1);
            while (!K.empty() && K.front() == ' ')
                K.remove_prefix(1);
            const auto             V = number(std::string_view{line}.substr(EQ + 1));
            if (!V)
                continue;
            const auto SET = [&](double& dst, double lo, double hi) {
                dst = std::clamp(*V, lo, hi);
                ++n;
            };
            if (K == "fitts_a_ms")
                SET(p.aMs, 0, 400);
            else if (K == "fitts_b_ms")
                SET(p.bMs, 30, 400);
            else if (K == "bow_mean")
                SET(p.bowMean, 0, 0.25);
            else if (K == "bow_sd")
                SET(p.bowSd, 0, 0.2);
            else if (K == "peak")
                SET(p.peak, 0.3, 0.7);
            else if (K == "overshoot_p")
                SET(p.overshootP, 0, 1);
            else if (K == "overshoot")
                SET(p.overshoot, 0, 0.15);
            else if (K == "dwell_ms")
                SET(p.dwellMs, 0, 400);
            else if (K == "rate_hz")
                SET(p.rateHz, 30, 1000);
            else if (K == "moves")
                p.moves = static_cast<int>(std::clamp(*V, 0.0, 1e9));
        }
        if (values)
            *values = n;
        return p;
    }

    std::expected<SProfile, std::string> readProfile(const std::string& path) {
        std::ifstream f(path);
        if (path.empty() || !f)
            return std::unexpected(std::format("no profile at {}", path.empty() ? "(no path: neither XDG_STATE_HOME nor HOME is set)" : path));
        SProfile p = parseProfile(f);
        p.source   = path;
        return p;
    }

    std::string describe(const SProfile& p) {
        return std::format("fitts a {:g} ms, b {:g} ms/bit (W {:g} px); bow {:g} +- {:g}; speed peak at {:g}; overshoot {:g}% of moves >= 300 px, by {:g}%; "
                           "dwell {:g} ms; {:g} Hz\nfrom {}{}",
                           p.aMs, p.bMs, TARGET_W, p.bowMean, p.bowSd, p.peak, p.overshootP * 100, p.overshoot * 100, p.dwellMs, p.rateHz, p.source,
                           p.moves ? std::format(" ({} movements)", p.moves) : "");
    }
}
