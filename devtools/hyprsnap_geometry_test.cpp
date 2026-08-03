#include "hyprsnap/geometry.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>

namespace {

    int  failures = 0;

    void expect(bool condition, std::string_view name) {
        if (condition)
            return;
        std::cerr << "FAIL: " << name << '\n';
        ++failures;
    }

} // namespace

int main() {
    using namespace NHyprsnap::Geometry;

    const CBox left{0, 20, 600, 780};
    expect(constrainedSlot(left, std::nullopt, std::nullopt, 2, EHorizontalAnchor::LEFT, EVerticalAnchor::CENTER) == left, "unconstrained left slot unchanged");

    const auto maxLeft = constrainedSlot(left, std::nullopt, Vector2D{400, 500}, 2, EHorizontalAnchor::LEFT, EVerticalAnchor::CENTER);
    expect(maxLeft == CBox{0, 158, 404, 504}, "maximum size remains left anchored and vertically centered");

    const CBox right{600, 20, 600, 780};
    const auto maxRight = constrainedSlot(right, std::nullopt, Vector2D{400, 500}, 2, EHorizontalAnchor::RIGHT, EVerticalAnchor::CENTER);
    expect(maxRight == CBox{796, 158, 404, 504}, "maximum size remains right anchored");

    const CBox top{0, 20, 1200, 390};
    const auto maxTop = constrainedSlot(top, std::nullopt, Vector2D{800, 300}, 2, EHorizontalAnchor::CENTER, EVerticalAnchor::TOP);
    expect(maxTop == CBox{198, 20, 804, 304}, "top slot remains top anchored and horizontally centered");

    const CBox corner{0, 20, 600, 390};
    const auto fixedCorner = constrainedSlot(corner, Vector2D{320, 240}, Vector2D{320, 240}, 2, EHorizontalAnchor::LEFT, EVerticalAnchor::TOP);
    expect(fixedCorner == CBox{0, 20, 324, 244}, "fixed-size corner preserves selected corner");

    const auto oversizedMin = constrainedSlot(right, Vector2D{1400, 900}, std::nullopt, 2, EHorizontalAnchor::RIGHT, EVerticalAnchor::CENTER);
    expect(oversizedMin == CBox{-204, -42, 1404, 904}, "oversized minimum remains authoritative and edge anchored");

    const auto badLimits = constrainedSlot(left, Vector2D{300, 200}, Vector2D{100, 100}, 2, EHorizontalAnchor::LEFT, EVerticalAnchor::CENTER);
    expect(badLimits == CBox{0, 308, 304, 204}, "maximum below minimum cannot violate minimum");

    if (failures != 0)
        return EXIT_FAILURE;
    std::cout << "hyprsnap_geometry_test: all checks passed\n";
}
