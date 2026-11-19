// The real planner's moves for test/fit.py: `plan PROFILE SEED`, then
// "x0 y0 x1 y1" lines on stdin; out, one sample every 4 ms: "id ms x y".
#include "path.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>

using namespace VClick;

int main(int argc, char** argv) {
    if (argc < 3)
        return 2;
    const auto P = Profile::read(argv[1]);
    if (!P) {
        std::fprintf(stderr, "plan: %s\n", P.error().c_str());
        return 3;
    }
    std::mt19937       rng{static_cast<unsigned>(std::atoi(argv[2]))};
    const Path::Vector2D BOX{9000, 9000};
    double             a, b, c, d;
    int                id = 0;
    while (std::cin >> a >> b >> c >> d) {
        const Path::Vector2D F{a + 3000, b + 3000}, T{c + 3000, d + 3000};
        const auto           S = Path::plan(F, T, BOX, *P, rng);
        if (!S.empty()) {
            const double MS = Path::duration(S);
            for (double t = 0; t < MS + 4; t += 4) {
                const auto Q = Path::sample(S, t);
                std::printf("%d %.1f %.4f %.4f\n", id, t, Q.x - 3000, Q.y - 3000);
            }
        }
        ++id;
    }
}
