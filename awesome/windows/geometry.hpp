// awesome/windows/geometry.hpp — the window-state geometry math: the
// restore-box bounding (maximize) and the constrained snap slot (snap).
// One place for both — they share the client-hint clamping.
#pragma once

#include <hyprland/src/helpers/math/Math.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace NAwesome::Windows::Geometry {

    inline constexpr double DEFAULT_MIN_WINDOW_SIZE = 20.0;

    enum class EHorizontalAnchor {
        CENTER,
        LEFT,
        RIGHT,
    };
    enum class EVerticalAnchor {
        CENTER,
        TOP,
        BOTTOM,
    };

    inline double boundedMinimum(double value) {
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

    inline double anchoredPosition(double start, double extent, double size, EHorizontalAnchor anchor) {
        if (anchor == EHorizontalAnchor::RIGHT)
            return start + extent - size;
        if (anchor == EHorizontalAnchor::CENTER)
            return start + (extent - size) / 2.0;
        return start;
    }

    inline double anchoredPosition(double start, double extent, double size, EVerticalAnchor anchor) {
        if (anchor == EVerticalAnchor::BOTTOM)
            return start + extent - size;
        if (anchor == EVerticalAnchor::CENTER)
            return start + (extent - size) / 2.0;
        return start;
    }

    // slot and result are border boxes; client limits describe the surface
    // inside that border. A minimum larger than the workarea remains
    // authoritative, anchored to the edge the user selected.
    inline CBox constrainedSlot(const CBox& slot, std::optional<Vector2D> minSize, std::optional<Vector2D> maxSize, double border, EHorizontalAnchor horizontal,
                                EVerticalAnchor vertical) {
        border = std::max(0.0, std::isfinite(border) ? border : 0.0);

        const auto   MINRAW = minSize.value_or(Vector2D{DEFAULT_MIN_WINDOW_SIZE, DEFAULT_MIN_WINDOW_SIZE});
        const auto   MAXRAW = maxSize.value_or(Math::VECTOR2D_MAX);
        const double MINX   = boundedMinimum(MINRAW.x);
        const double MINY   = boundedMinimum(MINRAW.y);
        const double MAXX   = boundedMaximum(MAXRAW.x, MINX);
        const double MAXY   = boundedMaximum(MAXRAW.y, MINY);
        const double WANTW  = std::max(1.0, slot.w - 2.0 * border);
        const double WANTH  = std::max(1.0, slot.h - 2.0 * border);
        const double WIDTH  = std::clamp(WANTW, MINX, MAXX) + 2.0 * border;
        const double HEIGHT = std::clamp(WANTH, MINY, MAXY) + 2.0 * border;

        return {anchoredPosition(slot.x, slot.w, WIDTH, horizontal), anchoredPosition(slot.y, slot.h, HEIGHT, vertical), WIDTH, HEIGHT};
    }

} // namespace NAwesome::Windows::Geometry
