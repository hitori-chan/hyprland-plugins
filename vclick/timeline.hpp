// What a request does over time, as a bounded list of steps: pure, with no
// compositor in it, so test/timeline.cpp checks it headless. run.cpp runs
// the steps.
#pragma once

#include "path.hpp"

#include <cstdint>
#include <optional>
#include <random>
#include <vector>

namespace VClick {
    enum class EVerb : uint8_t {
        CLICK,
        SCROLL,
        MOVE,  // only go there, and stay (with a held button: a drag's way)
        PRESS, // go there and hold a button down
        DRAG,  // press there, glide to `to`, let go
        PINCH, // two fingers spread to `scale` and turn `deg` there
    };

    enum class EStyle : uint8_t {
        HUMAN,       // at the user's own pace
        PERFORMANCE, // as fast as apps still read it right
    };
}

namespace VClick::Timeline {
    using Path::Vector2D;

    constexpr double FAST_DRAG_MS = 48; // enough motions for an app to read a drag
    // press to press: apps read two clicks as a double click up to 400-500 ms
    constexpr double MAX_CLICK_MS = 280;

    enum class EStep : uint8_t {
        WAY,     // the pointer goes there, on its way
        REST,    // the pointer stays, ms long
        TRAVEL,  // a held button's way
        PLACE,   // the pointer is there at once
        HOVER,   // the way's end: the app keeps the hover
        BURST,   // the input in one burst: clicks, or wheel steps
        WHEEL,   // one wheel step of a paced run
        PACE,    // between two wheel steps, ms long
        STROKE,  // a finger's scroll or a pinch, over time
        PRESS,   // the button goes down
        HOLD,    // the button stays down, ms long
        RELEASE, // the button goes up
    };

    struct SStep {
        EStep                   kind = EStep::REST;
        double                  ms   = 0;
        std::vector<Path::SSeg> way;
        Vector2D                to; // where a WAY or a TRAVEL ends
    };

    struct SRequest {
        EVerb                   verb  = EVerb::CLICK;
        EStyle                  style = EStyle::HUMAN;
        Vector2D                pos, to; // the point; a drag's end
        Vector2D                bounds;  // the window's size
        std::optional<Vector2D> from;    // where a way there starts; none: at once
        double                  size  = Path::TARGET_W;
        int                     count = 1;  // wheel steps
        int                     ms    = 50; // between them
        double                  px    = 0;  // a finger's scroll
        bool                    hold  = false; // a click may hold its button (the drag-and-drop guard is in)
    };

    std::vector<SStep> compose(const SRequest& q, const Profile::SProfile& p, std::mt19937& rng);
}
