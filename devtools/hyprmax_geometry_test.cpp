#include "hyprmax/geometry.hpp"
#include "test.hpp"

#include <optional>

int main() {
    using NHyprmax::Geometry::boundedRestore;
    NHyprTest::CSuite suite{"hyprmax_geometry_test"};

    const CBox        workarea{100, 50, 1200, 750};
    suite.expect(boundedRestore(CBox{200, 100, 800, 500}, workarea, std::nullopt, std::nullopt) == CBox{200, 100, 800, 500}, "ordinary restore unchanged");
    suite.expect(boundedRestore(CBox{-500, -400, 2000, 1400}, workarea, std::nullopt, std::nullopt) == workarea, "hostile box constrained to workarea");
    suite.expect(boundedRestore(CBox{1200, 700, 100, 100}, workarea, Vector2D{300, 200}, std::nullopt) == CBox{1000, 600, 300, 200},
                 "minimum size applied before bottom-right clamp");
    suite.expect(boundedRestore(CBox{200, 100, 900, 600}, workarea, std::nullopt, Vector2D{500, 400}) == CBox{200, 100, 500, 400}, "client maximum constrains restore");
    suite.expect(boundedRestore(CBox{1200, 700, 900, 600}, workarea, Vector2D{600, 500}, Vector2D{300, 200}) == CBox{700, 300, 600, 500},
                 "maximum below minimum cannot violate minimum");
    suite.expect(boundedRestore(CBox{300, 200, 400, 300}, workarea, Vector2D{1400, 900}, std::nullopt) == CBox{100, 50, 1400, 900},
                 "minimum larger than workarea remains authoritative");

    return suite.finish();
}
