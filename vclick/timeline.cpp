#include "timeline.hpp"

#include <algorithm>
#include <utility>

namespace VClick::Timeline {
    constexpr double MAX_REST_MS = 600;

    // the rest counts from arriving near the point: the way's own end is in it
    static double restAfter(const std::vector<Path::SSeg>& way, const Profile::SProfile& p, std::mt19937& rng) {
        const double REST = Profile::draw(p.restMs, p.restSigma, rng);
        return std::clamp(REST - Path::settleMs(way, Profile::NEAR_PX), 0.0, MAX_REST_MS);
    }

    // a hand's click: the button held, then the gap to the next one's press
    static void clicks(std::vector<SStep>& out, int count, const Profile::SProfile& p, std::mt19937& rng) {
        for (int i = 0; i < count; ++i) {
            const double HOLD = std::clamp(Profile::draw(p.holdMs, p.holdSigma, rng), 20.0, MAX_CLICK_MS - 20);
            out.push_back({.kind = EStep::PRESS});
            out.push_back({.kind = EStep::HOLD, .ms = HOLD});
            out.push_back({.kind = EStep::RELEASE});
            if (i + 1 < count)
                out.push_back({.kind = EStep::REST, .ms = std::clamp(Profile::draw(p.doubleGapMs, p.holdSigma, rng), 20.0, MAX_CLICK_MS - HOLD)});
        }
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
            case EVerb::CLICK:
                if (q.from && q.hold)
                    clicks(out, q.count, p, rng);
                else
                    out.push_back({.kind = EStep::BURST});
                break;
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
                    auto         way  = Path::plan(q.pos, q.to, q.bounds, p, rng, {.w = q.size, .overshoot = false});
                    const double LEAD = Profile::draw(p.dragLeadMs, p.restSigma, rng) - Path::leaveMs(way, Profile::NEAR_PX);
                    const double DROP = Profile::draw(p.dropRestMs, p.restSigma, rng) - Path::settleMs(way, Profile::NEAR_PX);
                    out.push_back({.kind = EStep::HOLD, .ms = std::clamp(LEAD, 0.0, MAX_REST_MS)});
                    walk(out, EStep::TRAVEL, std::move(way), q.to);
                    out.push_back({.kind = EStep::REST, .ms = std::clamp(DROP, 0.0, MAX_REST_MS)});
                }
                out.push_back({.kind = EStep::RELEASE});
                break;
            }
        }
        return out;
    }
}
