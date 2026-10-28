// awesome/core/text.cpp — see text.hpp.
#include "text.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace NAwesome {

    STextShadow textShadow(eShadow s, int pt) {
        switch (s) {
            // macOS's desktop text: r 3 px at 15 px type, 1 px down, black
            // at 0.8 — what still reads over a white patch of the wallpaper
            case eShadow::DESKTOP: return {.r = std::max(1, (int)std::lround(pt * 0.2)), .dy = std::max(1, (int)std::lround(pt * 0.07)), .a = 0.8};
            case eShadow::NONE: break;
        }
        return {};
    }

    void blurA8(unsigned char* px, int w, int h, int stride, int r) {
        std::vector<int> line(std::max(w, h));
        const auto       pass = [&](unsigned char* base, int n, int step) {
            for (int i = 0; i < n; i++)
                line[i] = base[i * step];
            int sum = 0;
            for (int i = -r; i <= r; i++)
                sum += line[std::clamp(i, 0, n - 1)];
            for (int i = 0; i < n; i++) {
                base[i * step] = (unsigned char)(sum / (2 * r + 1));
                sum += line[std::min(i + r + 1, n - 1)] - line[std::max(i - r, 0)];
            }
        };
        for (int it = 0; it < 3; it++) {
            for (int y = 0; y < h; y++)
                pass(px + y * stride, w, 1);
            for (int x = 0; x < w; x++)
                pass(px + x, h, stride);
        }
    }

    void showShadowedLayout(cairo_t* cr, PangoLayout* layout, int inkW, int inkH, const STextShadow& s, double r, double g, double b, double a) {
        const int PAD = s.pad();
        if (PAD > 0) {
            const int W = inkW + 2 * PAD, H = inkH + 2 * PAD;
            auto*     MASK = cairo_image_surface_create(CAIRO_FORMAT_A8, W, H);
            auto*     MCR  = cairo_create(MASK);
            cairo_move_to(MCR, PAD, PAD + s.dy);
            pango_cairo_show_layout(MCR, layout);
            cairo_destroy(MCR);
            cairo_surface_flush(MASK);
            if (cairo_surface_status(MASK) == CAIRO_STATUS_SUCCESS) {
                blurA8(cairo_image_surface_get_data(MASK), W, H, cairo_image_surface_get_stride(MASK), s.r);
                cairo_surface_mark_dirty(MASK);
                cairo_set_source_rgba(cr, 0, 0, 0, s.a);
                cairo_mask_surface(cr, MASK, 0, 0);
            }
            cairo_surface_destroy(MASK);
        }
        cairo_set_source_rgba(cr, r, g, b, a);
        cairo_move_to(cr, PAD, PAD);
        pango_cairo_show_layout(cr, layout);
    }

} // namespace NAwesome
