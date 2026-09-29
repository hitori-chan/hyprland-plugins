// awesome/test/test_box.cpp
#include "../core/box.hpp"

#include "harness.hpp"

using namespace NAwesome;

bool test_box() {
    AW_CHECK(validBox({0, 0, 100, 50}));
    AW_CHECK(validBox({MIN_COORD, MIN_COORD, MAX_COORD, MAX_COORD}));
    AW_CHECK((!validBox({MAX_COORD + 1, 0, 1, 1})));
    AW_CHECK((!validBox({0, MIN_COORD - 1, 1, 1})));
    AW_CHECK((!validBox({0, 0, -1, 1})));
    AW_CHECK((!validBox({0, 0, 1, -1})));
    AW_CHECK((Box{1, 2, 3, 4} == Box{1, 2, 3, 4}));
    return true;
}
