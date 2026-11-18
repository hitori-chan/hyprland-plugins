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

    std::expected<SProfile, std::string> read(const std::string& path) {
        std::ifstream f(path);
        if (path.empty() || !f)
            return std::unexpected(std::format("no profile at {}", path.empty() ? "(no path: neither XDG_STATE_HOME nor HOME is set)" : path));
        SProfile p = parse(f);
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
