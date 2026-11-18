#include "profile.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <optional>
#include <string_view>

namespace VClick::Profile {
    double draw(double median, double sigma, std::mt19937& rng) {
        return median * std::exp(sigma * std::clamp(std::normal_distribution<double>{0, 1}(rng), -2.0, 2.0));
    }

    std::string defaultPath() {
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

    SProfile parse(std::istream& in, int* values) {
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
            const auto V = number(std::string_view{line}.substr(EQ + 1));
            if (!V)
                continue;
            const auto SET = [&](double& dst, double lo, double hi) {
                dst = std::clamp(*V, lo, hi);
                ++n;
            };
            if (K == "stroke_a_ms")
                SET(p.aMs, 0, 400);
            else if (K == "stroke_b_ms")
                SET(p.bMs, 10, 400);
            else if (K == "bell")
                SET(p.bell, 2, 24);
            else if (K == "peak")
                SET(p.peak, 0.2, 0.7);
            else if (K == "bow_median")
                SET(p.bowMedian, 0, 0.25);
            else if (K == "bow_sigma")
                SET(p.bowSigma, 0, 1.5);
            else if (K == "correct_p")
                SET(p.correctP, 0, 1);
            else if (K == "under_share")
                SET(p.underShare, 0, 1);
            else if (K == "correct_size")
                SET(p.correctSize, 0, 0.15);
            else if (K == "lead_p")
                SET(p.leadP, 0, 0.6);
            else if (K == "lead_ms")
                SET(p.leadMs, 100, 1000);
            else if (K == "lead_size")
                SET(p.leadSize, 0, 0.7);
            else if (K == "s_share")
                SET(p.sShare, 0, 0.6);
            else if (K == "tail_share")
                SET(p.tailShare, 0, 0.8);
            else if (K == "tail_p")
                SET(p.tailP, 0, 1);
            else if (K == "tail_p_slope")
                SET(p.tailPSlope, -1, 1);
            else if (K == "tail_size")
                SET(p.tailSize, 0, 0.2);
            else if (K == "rest_ms")
                SET(p.restMs, 0, 600);
            else if (K == "rest_sigma")
                SET(p.restSigma, 0, 1.5);
            else if (K == "hold_ms")
                SET(p.holdMs, 20, 250);
            else if (K == "hold_sigma")
                SET(p.holdSigma, 0, 1);
            else if (K == "double_gap_ms")
                SET(p.doubleGapMs, 20, 250);
            else if (K == "drag_lead_ms")
                SET(p.dragLeadMs, 0, 400);
            else if (K == "drop_rest_ms")
                SET(p.dropRestMs, 0, 400);
            else if (K == "wheel_ms")
                SET(p.wheelMs, 10, 200);
            else if (K == "rate_hz")
                SET(p.rateHz, 30, 1000);
            else if (K == "moves")
                p.moves = static_cast<int>(std::clamp(*V, 0.0, 1e9));
        }
        if (values)
            *values = n;
        return p;
    }

    std::expected<SProfile, std::string> read(const std::string& path) {
        std::ifstream f(path);
        if (path.empty() || !f)
            return std::unexpected(std::format("no profile at {}", path.empty() ? "(no path: neither XDG_STATE_HOME nor HOME is set)" : path));
        SProfile p = parse(f);
        p.source   = path;
        return p;
    }

    std::string describe(const SProfile& p) {
        return std::format("stroke a {:g} ms, b {:g} ms/bit (W {:g} px); bell {:g} peaking at {:g}; bow {:g} (sigma {:g}), {:g}% both ways; drift {:g}%, {:g} ms, {:g}% of the way; corrected {:g}%, {:g}% from short, by {:g}%; tail on {:g}% of moves at 300 px ({:+g} a doubling), {:g}% of the time, {:g}% of the way; "
                           "rest {:g} ms (sigma {:g}); hold {:g} ms (sigma {:g}), {:g} ms between clicks; drag lead {:g} ms, drop rest {:g} ms; wheel {:g} ms; {:g} Hz\nfrom {}{}",
                           p.aMs, p.bMs, TARGET_W, p.bell, p.peak, p.bowMedian, p.bowSigma, p.sShare * 100, p.leadP * 100, p.leadMs, p.leadSize * 100, p.correctP * 100, p.underShare * 100, p.correctSize * 100, p.tailP * 100, p.tailPSlope * 100, p.tailShare * 100, p.tailSize * 100, p.restMs, p.restSigma,
                           p.holdMs, p.holdSigma, p.doubleGapMs, p.dragLeadMs, p.dropRestMs, p.wheelMs, p.rateHz, p.source, p.moves ? std::format(" ({} movements)", p.moves) : "");
    }
}
