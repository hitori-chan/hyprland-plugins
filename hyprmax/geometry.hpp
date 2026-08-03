#pragma once

#include <hyprland/src/helpers/math/Math.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace NHyprmax::Geometry {

    inline constexpr double DEFAULT_MIN_WINDOW_SIZE = 20.0;

    inline double           boundedMinimum(double value) {
        return std::max(1.0, std::isfinite(value) ? value : 1.0);
    }

    inline double boundedMaximum(double value, double minimum) {
        return std::max(minimum, std::isfinite(value) ? value : std::numeric_limits<double>::max());
    }

    // Client minimums remain authoritative when larger than the workarea. In
    // every other case a remembered box is constrained by both the current
    // client hints and usable output before its position is clamped.
    inline CBox boundedRestore(CBox box, const CBox& workarea, std::optional<Vector2D> minSize, std::optional<Vector2D> maxSize) {
        const auto   MINRAW = minSize.value_or(Vector2D{DEFAULT_MIN_WINDOW_SIZE, DEFAULT_MIN_WINDOW_SIZE});
        const auto   MAXRAW = maxSize.value_or(Math::VECTOR2D_MAX);
        const double MINX   = boundedMinimum(MINRAW.x);
        const double MINY   = boundedMinimum(MINRAW.y);
        const double MAXX   = std::max(MINX, std::min(boundedMaximum(MAXRAW.x, MINX), std::max(1.0, workarea.w)));
        const double MAXY   = std::max(MINY, std::min(boundedMaximum(MAXRAW.y, MINY), std::max(1.0, workarea.h)));

        box.w = std::clamp(box.w, MINX, MAXX);
        box.h = std::clamp(box.h, MINY, MAXY);
        box.x = std::clamp(box.x, workarea.x, std::max(workarea.x, workarea.x + workarea.w - box.w));
        box.y = std::clamp(box.y, workarea.y, std::max(workarea.y, workarea.y + workarea.h - box.h));
        return box;
    }

} // namespace NHyprmax::Geometry
