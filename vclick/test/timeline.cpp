// What each request does over time, headless: the steps of every verb, with
// a way there and at once. `make -C vclick test`.
#include "timeline.hpp"

#include <cstdio>
#include <string>

using namespace VClick;
using namespace VClick::Timeline;

static int pass = 0, fail = 0;

static void check(bool ok, const std::string& what) {
    if (ok)
        ++pass;
    else {
        ++fail;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

static const char* name(EStep k) {
    switch (k) {
        case EStep::WAY: return "way";
        case EStep::REST: return "rest";
        case EStep::TRAVEL: return "travel";
        case EStep::PLACE: return "place";
        case EStep::HOVER: return "hover";
        case EStep::BURST: return "burst";
        case EStep::WHEEL: return "wheel";
        case EStep::PACE: return "pace";
        case EStep::STROKE: return "stroke";
        case EStep::PRESS: return "press";
        case EStep::RELEASE: return "release";
    }
    return "?";
}

static std::string kinds(const std::vector<SStep>& steps) {
    std::string out;
    for (const auto& s : steps)
        out += (out.empty() ? "" : " ") + std::string{name(s.kind)};
    return out;
}

int main() {
    std::mt19937            rng{4242};
    const Profile::SProfile P;
    const Vector2D          BOX{1600, 900}, A{100, 100}, B{900, 500};

    const auto STEPS = [&](SRequest q) {
        q.pos    = B;
        q.bounds = BOX;
        return compose(q, P, rng);
    };
    const auto IS = [&](const SRequest& q, const std::string& want, const std::string& what) {
        const auto GOT = kinds(STEPS(q));
        check(GOT == want, what + ": got '" + GOT + "', want '" + want + "'");
    };

    IS({.verb = EVerb::CLICK}, "burst", "a click at once");
    IS({.verb = EVerb::CLICK, .from = A}, "way rest burst", "a click by a way");
    IS({.verb = EVerb::MOVE}, "place", "a move at once");
    IS({.verb = EVerb::MOVE, .from = A}, "way hover", "a move by a way has no rest");
    IS({.verb = EVerb::PRESS}, "press", "a press at once");
    IS({.verb = EVerb::PRESS, .from = A}, "way rest press", "a press by a way");
    IS({.verb = EVerb::PINCH}, "stroke", "a pinch at once");
    IS({.verb = EVerb::PINCH, .from = A}, "way rest stroke", "a pinch by a way");
    IS({.verb = EVerb::SCROLL, .px = 400}, "stroke", "a finger's scroll");
    IS({.verb = EVerb::SCROLL, .count = 1}, "burst", "one wheel step");
    IS({.verb = EVerb::SCROLL, .count = 5, .ms = 0}, "burst", "0ms: every step in one burst");
    IS({.verb = EVerb::SCROLL, .count = 3, .ms = 80}, "wheel pace wheel pace wheel", "a paced run");
    IS({.verb = EVerb::SCROLL, .from = A, .count = 2, .ms = 80}, "way rest wheel pace wheel", "a paced run by a way");
    IS({.verb = EVerb::DRAG}, "press travel rest release", "a drag at once");
    IS({.verb = EVerb::DRAG, .from = A}, "way rest press travel rest release", "a drag by a way");

    {
        const auto S = STEPS({.verb = EVerb::SCROLL, .count = 3, .ms = 80});
        check(S[1].ms == 80 && S[3].ms == 80, "the pace is the request's");
    }
    {
        const auto S = STEPS({.verb = EVerb::CLICK, .from = A});
        check(S[0].to == B && Path::sample(S[0].way, 1e9) == B && S[0].ms == Path::duration(S[0].way), "the way ends on the point, in its own time");
    }
    {
        const auto S = STEPS({.verb = EVerb::DRAG, .to = A});
        check(S[1].to == A && Path::sample(S[1].way, 1e9) == A && S[1].way.size() == 1, "a drag's way ends on its end, and never passes it");
    }
    {
        bool bounded = true;
        for (int i = 0; i < 500; ++i) {
            const auto S = STEPS({.verb = EVerb::CLICK, .from = A});
            bounded &= S[1].ms >= 0 && S[1].ms <= 600;
        }
        check(bounded, "the rest stays within 0-600 ms");
    }

    std::printf("%s: %d of %d checks passed\n", fail ? "FAIL" : "ok", pass, pass + fail);
    return fail ? 1 : 0;
}
