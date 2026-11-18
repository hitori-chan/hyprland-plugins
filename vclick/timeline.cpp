#include "timeline.hpp"

#include <algorithm>
#include <utility>

namespace VClick::Timeline {
    constexpr double MAX_REST_MS = 600;

    // the rest counts from arriving near the point: the way's own end is in it
    static double restAfter(const std::vector<Path::SSeg>& way, const Profile::SProfile& p, std::mt19937& rng) {
        const double REST = Profile::draw(p.restMs, p.restSigma, rng);
        return std::clamp(way.empty() ? REST : REST - Path::settleMs(way, Profile::NEAR_PX), 0.0, MAX_REST_MS);
    }

    static void walk(std::vector<SStep>& out, EStep kind, std::vector<Path::SSeg> way, const Vector2D& to) {
        const double MS = Path::duration(way);
        out.push_back({.kind = kind, .ms = MS, .way = std::move(way), .to = to});
    }

    std::vector<SStep> compose(const SRequest& q, const Profile::SProfile& p, std::mt19937& rng) {
        std::vector<SStep> out;
        if (q.from) {
            walk(out, EStep::WAY, Path::plan(*q.from, q.pos, q.bounds, p, rng, {.w = q.size}), q.pos);
            if (q.verb != EVerb::MOVE)
                out.push_back({.kind = EStep::REST, .ms = restAfter(out.back().way, p, rng)});
        }

        switch (q.verb) {
            case EVerb::MOVE: out.push_back({.kind = q.from ? EStep::HOVER : EStep::PLACE}); break;
            case EVerb::CLICK: out.push_back({.kind = EStep::BURST}); break;
            case EVerb::PINCH: out.push_back({.kind = EStep::STROKE}); break;
            case EVerb::PRESS: out.push_back({.kind = EStep::PRESS}); break;
            case EVerb::SCROLL:
                if (q.px > 0)
                    out.push_back({.kind = EStep::STROKE});
                else if (q.count == 1 || q.ms == 0)
                    out.push_back({.kind = EStep::BURST});
                else
                    for (int i = 0; i < q.count; ++i) {
                        if (i)
                            out.push_back({.kind = EStep::PACE, .ms = static_cast<double>(q.ms)});
                        out.push_back({.kind = EStep::WHEEL});
                    }
                break;
            case EVerb::DRAG: {
                // as a hand drags: the user's pace, never past the target (a
                // drop target, a slider's stop), a rest on it before letting go
                out.push_back({.kind = EStep::PRESS});
                if (q.style == EStyle::PERFORMANCE)
                    walk(out, EStep::TRAVEL, {Path::line(q.pos, q.to, FAST_DRAG_MS)}, q.to);
                else {
                    walk(out, EStep::TRAVEL, Path::plan(q.pos, q.to, q.bounds, p, rng, {.w = q.size, .overshoot = false}), q.to);
                    out.push_back({.kind = EStep::REST, .ms = restAfter(out.back().way, p, rng)});
                }
                out.push_back({.kind = EStep::RELEASE});
                break;
            }
        }
        return out;
    }
}
