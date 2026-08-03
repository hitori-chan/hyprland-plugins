#include "hyprmax/geometry.hpp"

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
    using NHyprmax::Geometry::boundedRestore;

    const CBox workarea{100, 50, 1200, 750};
    expect(boundedRestore(CBox{200, 100, 800, 500}, workarea, std::nullopt, std::nullopt) == CBox{200, 100, 800, 500}, "ordinary restore unchanged");
    expect(boundedRestore(CBox{-500, -400, 2000, 1400}, workarea, std::nullopt, std::nullopt) == workarea, "hostile box constrained to workarea");
    expect(boundedRestore(CBox{1200, 700, 100, 100}, workarea, Vector2D{300, 200}, std::nullopt) == CBox{1000, 600, 300, 200}, "minimum size applied before bottom-right clamp");
    expect(boundedRestore(CBox{200, 100, 900, 600}, workarea, std::nullopt, Vector2D{500, 400}) == CBox{200, 100, 500, 400}, "client maximum constrains restore");
    expect(boundedRestore(CBox{1200, 700, 900, 600}, workarea, Vector2D{600, 500}, Vector2D{300, 200}) == CBox{700, 300, 600, 500}, "maximum below minimum cannot violate minimum");
    expect(boundedRestore(CBox{300, 200, 400, 300}, workarea, Vector2D{1400, 900}, std::nullopt) == CBox{100, 50, 1400, 900}, "minimum larger than workarea remains authoritative");

    if (failures != 0)
        return EXIT_FAILURE;
    std::cout << "hyprmax_geometry_test: all checks passed\n";
}
