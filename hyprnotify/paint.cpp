// hyprnotify/paint.cpp — the paint context (glass, shadow, textures), the
// shared card recipes, the type scale and the motion curves. The config
// gates and color memos live in common/glass.hpp.

#include "ui.hpp"

#include <cairo/cairo.h>

#include <cmath>
#include <numbers>

namespace NHyprnotify {

    namespace {
        std::unordered_map<uint64_t, SP<ITexture>> controlIcons;

        uint64_t                                   controlIconKey(eControlIcon icon, int px, int glyphPx, const CHyprColor& color) {
            return ((uint64_t)icon << 56) ^ ((uint64_t)(uint32_t)px << 44) ^ ((uint64_t)(uint32_t)glyphPx << 32) ^ color.getAsHex();
        }

        void strokeIcon(cairo_t* cr, eControlIcon icon, double px) {
            const double S = px / 24.0;
            const double PI = std::numbers::pi;
            cairo_set_line_width(cr, std::max(1.0, 2.0 * S));
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

            // AOSP HEAD SystemUI's current 24dp OSD marks:
            // ic_brightness_full, ic_volume_media(_mute), ic_mic_26dp/off.
            const auto mediaVolume = [&]() {
                cairo_move_to(cr, 12.0 * S, 3.0 * S);
                cairo_line_to(cr, 12.01 * S, 13.55 * S);
                cairo_curve_to(cr, 11.42 * S, 13.21 * S, 10.74 * S, 13.0 * S, 10.01 * S, 13.0 * S);
                cairo_curve_to(cr, 7.79 * S, 13.0 * S, 6.0 * S, 14.79 * S, 6.0 * S, 17.0 * S);
                cairo_curve_to(cr, 6.0 * S, 19.21 * S, 7.79 * S, 21.0 * S, 10.01 * S, 21.0 * S);
                cairo_curve_to(cr, 12.22 * S, 21.0 * S, 14.0 * S, 19.21 * S, 14.0 * S, 17.0 * S);
                cairo_line_to(cr, 14.0 * S, 7.0 * S);
                cairo_line_to(cr, 18.0 * S, 7.0 * S);
                cairo_line_to(cr, 18.0 * S, 3.0 * S);
                cairo_close_path(cr);
                cairo_fill(cr);
            };
            const auto strike = [&]() {
                cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
                cairo_set_line_width(cr, std::max(2.0, 4.0 * S));
                cairo_move_to(cr, 3.0 * S, 3.0 * S);
                cairo_line_to(cr, 21.0 * S, 21.0 * S);
                cairo_stroke(cr);
                cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
                cairo_set_line_width(cr, std::max(1.0, 2.0 * S));
                cairo_move_to(cr, 2.8 * S, 2.8 * S);
                cairo_line_to(cr, 21.2 * S, 21.2 * S);
                cairo_stroke(cr);
            };
            const auto mic = [&]() {
                cairo_arc(cr, 12.0 * S, 5.0 * S, 3.0 * S, PI, 0);
                cairo_line_to(cr, 15.0 * S, 11.0 * S);
                cairo_arc(cr, 12.0 * S, 11.0 * S, 3.0 * S, 0, PI);
                cairo_close_path(cr);
                cairo_fill(cr);
                cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
                cairo_set_line_width(cr, std::max(1.0, 2.0 * S));
                cairo_move_to(cr, 12.0 * S, 5.0 * S);
                cairo_line_to(cr, 12.0 * S, 11.0 * S);
                cairo_stroke(cr);
                cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
                cairo_arc(cr, 12.0 * S, 11.0 * S, 6.0 * S, 0, PI);
                cairo_move_to(cr, 6.0 * S, 11.0 * S);
                cairo_curve_to(cr, 6.0 * S, 14.31 * S, 8.69 * S, 17.0 * S, 12.0 * S, 17.0 * S);
                cairo_curve_to(cr, 15.31 * S, 17.0 * S, 18.0 * S, 14.31 * S, 18.0 * S, 11.0 * S);
                cairo_move_to(cr, 12.0 * S, 17.0 * S);
                cairo_line_to(cr, 12.0 * S, 21.0 * S);
                cairo_move_to(cr, 9.0 * S, 21.0 * S);
                cairo_line_to(cr, 15.0 * S, 21.0 * S);
                cairo_stroke(cr);
            };
            const auto touchpad = [&]() {
                cairo_rectangle(cr, 4.0 * S, 4.0 * S, 16.0 * S, 16.0 * S);
                cairo_stroke(cr);
                cairo_move_to(cr, 4.0 * S, 15.5 * S);
                cairo_line_to(cr, 20.0 * S, 15.5 * S);
                cairo_stroke(cr);
            };
            const auto battery = [&]() {
                cairo_new_sub_path(cr);
                cairo_arc(cr, 17.0 * S, 7.0 * S, 2.0 * S, -PI / 2, 0);
                cairo_arc(cr, 17.0 * S, 17.0 * S, 2.0 * S, 0, PI / 2);
                cairo_arc(cr, 6.0 * S, 17.0 * S, 2.0 * S, PI / 2, PI);
                cairo_arc(cr, 6.0 * S, 7.0 * S, 2.0 * S, PI, 3 * PI / 2);
                cairo_close_path(cr);
                cairo_stroke(cr);
                cairo_rectangle(cr, 19.0 * S, 9.0 * S, 2.5 * S, 6.0 * S);
                cairo_fill(cr);
            };
            const auto dnd = [&]() {
                cairo_arc(cr, 12.0 * S, 12.0 * S, 8.0 * S, 0, 2 * PI);
                cairo_stroke(cr);
                cairo_move_to(cr, 7.0 * S, 12.0 * S);
                cairo_line_to(cr, 17.0 * S, 12.0 * S);
                cairo_stroke(cr);
            };
            const auto snooze = [&]() {
                // SystemUI's drawable/ic_snooze.xml: the lower clock ring,
                // two top motion marks, and the square Z are kept in one
                // cached 24dp recipe so font fallback cannot alter it.
                cairo_arc(cr, 12.0 * S, 13.0 * S, 8.0 * S, 0, 2 * PI);
                cairo_stroke(cr);

                cairo_move_to(cr, 9.0 * S, 11.0 * S);
                cairo_line_to(cr, 12.63 * S, 11.0 * S);
                cairo_line_to(cr, 9.0 * S, 15.2 * S);
                cairo_line_to(cr, 9.0 * S, 17.0 * S);
                cairo_line_to(cr, 15.0 * S, 17.0 * S);
                cairo_line_to(cr, 15.0 * S, 15.0 * S);
                cairo_line_to(cr, 11.37 * S, 15.0 * S);
                cairo_line_to(cr, 15.0 * S, 10.8 * S);
                cairo_line_to(cr, 15.0 * S, 9.0 * S);
                cairo_line_to(cr, 9.0 * S, 9.0 * S);
                cairo_close_path(cr);
                cairo_fill(cr);

                cairo_move_to(cr, 16.056 * S, 3.346 * S);
                cairo_line_to(cr, 17.338 * S, 1.811 * S);
                cairo_line_to(cr, 21.945 * S, 5.661 * S);
                cairo_line_to(cr, 20.665 * S, 7.201 * S);
                cairo_close_path(cr);
                cairo_fill(cr);
                cairo_move_to(cr, 3.336 * S, 7.19 * S);
                cairo_line_to(cr, 2.056 * S, 5.654 * S);
                cairo_line_to(cr, 6.662 * S, 1.81 * S);
                cairo_line_to(cr, 7.942 * S, 3.346 * S);
                cairo_close_path(cr);
                cairo_fill(cr);
            };
            const auto alert = [&]() {
                cairo_move_to(cr, 7.0 * S, 17.0 * S);
                cairo_line_to(cr, 7.0 * S, 11.0 * S);
                cairo_curve_to(cr, 7.0 * S, 8.0 * S, 8.75 * S, 6.0 * S, 12.0 * S, 6.0 * S);
                cairo_curve_to(cr, 15.25 * S, 6.0 * S, 17.0 * S, 8.0 * S, 17.0 * S, 11.0 * S);
                cairo_line_to(cr, 17.0 * S, 17.0 * S);
                cairo_move_to(cr, 5.0 * S, 18.0 * S);
                cairo_line_to(cr, 19.0 * S, 18.0 * S);
                cairo_move_to(cr, 10.0 * S, 20.5 * S);
                cairo_curve_to(cr, 10.5 * S, 22.0 * S, 13.5 * S, 22.0 * S, 14.0 * S, 20.5 * S);
                cairo_stroke(cr);
            };
            const auto priority = [&]() {
                cairo_move_to(cr, 15.0 * S, 5.0 * S);
                cairo_line_to(cr, 3.0 * S, 5.0 * S);
                cairo_line_to(cr, 7.5 * S, 12.0 * S);
                cairo_line_to(cr, 3.0 * S, 19.0 * S);
                cairo_line_to(cr, 15.0 * S, 19.0 * S);
                cairo_curve_to(cr, 15.65 * S, 19.0 * S, 16.26 * S, 18.69 * S, 16.63 * S, 18.16 * S);
                cairo_line_to(cr, 21.0 * S, 12.0 * S);
                cairo_line_to(cr, 16.63 * S, 5.84 * S);
                cairo_curve_to(cr, 16.26 * S, 5.31 * S, 15.65 * S, 5.0 * S, 15.0 * S, 5.0 * S);
                cairo_close_path(cr);
                cairo_fill(cr);

                cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
                cairo_move_to(cr, 6.5 * S, 7.0 * S);
                cairo_line_to(cr, 15.0 * S, 7.0 * S);
                cairo_line_to(cr, 18.5 * S, 12.0 * S);
                cairo_line_to(cr, 15.0 * S, 17.0 * S);
                cairo_line_to(cr, 6.5 * S, 17.0 * S);
                cairo_line_to(cr, 10.0 * S, 12.0 * S);
                cairo_close_path(cr);
                cairo_fill(cr);
                cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
            };

            switch (icon) {
                case eControlIcon::APPS:
                    for (int y = 0; y < 3; y++)
                        for (int x = 0; x < 3; x++)
                            cairo_rectangle(cr, (5.0 + x * 6.0) * S, (5.0 + y * 6.0) * S, 3.0 * S, 3.0 * S);
                    cairo_fill(cr);
                    break;
                case eControlIcon::BATTERY:
                    battery();
                    break;
                case eControlIcon::BRIGHTNESS:
                    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
                    cairo_move_to(cr, 18.667 * S, 9.241 * S);
                    cairo_line_to(cr, 18.667 * S, 5.333 * S);
                    cairo_line_to(cr, 14.759 * S, 5.333 * S);
                    cairo_line_to(cr, 12.0 * S, 2.575 * S);
                    cairo_line_to(cr, 9.242 * S, 5.333 * S);
                    cairo_line_to(cr, 5.334 * S, 5.333 * S);
                    cairo_line_to(cr, 5.334 * S, 9.241 * S);
                    cairo_line_to(cr, 2.575 * S, 12.0 * S);
                    cairo_line_to(cr, 5.334 * S, 14.758 * S);
                    cairo_line_to(cr, 5.334 * S, 18.666 * S);
                    cairo_line_to(cr, 9.242 * S, 18.666 * S);
                    cairo_line_to(cr, 12.0 * S, 21.425 * S);
                    cairo_line_to(cr, 14.759 * S, 18.666 * S);
                    cairo_line_to(cr, 18.667 * S, 18.666 * S);
                    cairo_line_to(cr, 18.667 * S, 14.758 * S);
                    cairo_line_to(cr, 21.425 * S, 12.0 * S);
                    cairo_close_path(cr);
                    cairo_move_to(cr, 17.0 * S, 14.066 * S);
                    cairo_line_to(cr, 17.0 * S, 17.0 * S);
                    cairo_line_to(cr, 14.067 * S, 17.0 * S);
                    cairo_line_to(cr, 12.0 * S, 19.066 * S);
                    cairo_line_to(cr, 9.934 * S, 17.0 * S);
                    cairo_line_to(cr, 7.0 * S, 17.0 * S);
                    cairo_line_to(cr, 7.0 * S, 14.066 * S);
                    cairo_line_to(cr, 4.934 * S, 12.0 * S);
                    cairo_line_to(cr, 7.0 * S, 9.933 * S);
                    cairo_line_to(cr, 7.0 * S, 7.0 * S);
                    cairo_line_to(cr, 9.934 * S, 7.0 * S);
                    cairo_line_to(cr, 12.0 * S, 4.933 * S);
                    cairo_line_to(cr, 14.067 * S, 7.0 * S);
                    cairo_line_to(cr, 17.0 * S, 7.0 * S);
                    cairo_line_to(cr, 17.0 * S, 9.933 * S);
                    cairo_line_to(cr, 19.067 * S, 12.0 * S);
                    cairo_close_path(cr);
                    cairo_fill(cr);
                    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
                    cairo_arc(cr, 12.0 * S, 12.0 * S, 3.25 * S, 0, 2 * PI);
                    cairo_fill(cr);
                    break;
                case eControlIcon::VOLUME:
                    mediaVolume();
                    break;
                case eControlIcon::VOLUME_MUTED:
                    mediaVolume();
                    strike();
                    break;
                case eControlIcon::MICROPHONE:
                    mic();
                    break;
                case eControlIcon::MICROPHONE_MUTED:
                    mic();
                    strike();
                    break;
                case eControlIcon::TOUCHPAD:
                    touchpad();
                    break;
                case eControlIcon::TOUCHPAD_DISABLED:
                    touchpad();
                    cairo_move_to(cr, 4.0 * S, 4.0 * S);
                    cairo_line_to(cr, 20.0 * S, 20.0 * S);
                    cairo_stroke(cr);
                    break;
                case eControlIcon::DO_NOT_DISTURB:
                    dnd();
                    break;
                case eControlIcon::SNOOZE:
                    snooze();
                    break;
                case eControlIcon::PRIORITY: priority(); break;
                case eControlIcon::NOTIFICATION_ALERT: alert(); break;
                case eControlIcon::NOTIFICATION_SILENT:
                    alert();
                    strike();
                    break;
                case eControlIcon::EXPAND_MORE:
                    cairo_move_to(cr, 7.0 * S, 9.0 * S);
                    cairo_line_to(cr, 12.0 * S, 14.0 * S);
                    cairo_line_to(cr, 17.0 * S, 9.0 * S);
                    cairo_stroke(cr);
                    break;
                case eControlIcon::EXPAND_LESS:
                    cairo_move_to(cr, 7.0 * S, 15.0 * S);
                    cairo_line_to(cr, 12.0 * S, 10.0 * S);
                    cairo_line_to(cr, 17.0 * S, 15.0 * S);
                    cairo_stroke(cr);
                    break;
            }
        }
    }

    SP<ITexture> controlIcon(eControlIcon icon, int physicalPx, const CHyprColor& color, double glyphRatio) {
        const int  PX  = std::clamp(physicalPx, 8, 256);
        const int  GPX = std::clamp((int)std::lround(PX * std::clamp(glyphRatio, 0.1, 1.0)), 8, PX);
        const auto KEY = controlIconKey(icon, PX, GPX, color);
        if (const auto IT = controlIcons.find(KEY); IT != controlIcons.end())
            return IT->second;
        if (!warmGate.mayBuild() || !g_pHyprRenderer)
            return {};

        auto* SURF = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, PX, PX);
        auto* CR   = cairo_create(SURF);
        cairo_set_source_rgba(CR, color.r, color.g, color.b, color.a);
        cairo_translate(CR, (PX - GPX) / 2.0, (PX - GPX) / 2.0);
        strokeIcon(CR, icon, GPX);
        cairo_destroy(CR);
        cairo_surface_flush(SURF);
        auto TEX = g_pHyprRenderer->createTexture(SURF);
        cairo_surface_destroy(SURF);
        if (!TEX)
            return {};
        if (controlIcons.size() >= 128)
            controlIcons.erase(controlIcons.begin());
        controlIcons.emplace(KEY, TEX);
        return TEX;
    }

    void controlIconCacheClear() {
        controlIcons.clear();
    }

    double damageMargin(PHLMONITOR m) {
        // hairlines ride outside boxes, glass grows by the blur radius, and
        // shadows reach further still (their range covers the no-blur case)
        return (m ? std::ceil(m->m_scale) : 1.0) + 1.0 + std::max(liveBlurNeeded() ? blurRadius() : 0.0, 26.0);
    }

    // ---- the radius family ----

    float rPow() {
        return (float)cfg.roundingPower->value();
    }
    int rPanel(double scale) {
        return (int)std::lround(PIXEL_SHADE_RADIUS * scale);
    }
    int rRow(double scale) {
        return std::max(0, (int)std::lround(std::max(0, (int)cfg.rounding->value()) * scale));
    }
    int rJoint(double scale) {
        return std::max(0, (int)std::lround(PIXEL_INTERNAL_RADIUS * scale));
    }

    // ---- motion ----

    float easeOutCubic(float t) {
        const float U = 1 - t;
        return 1 - U * U * U;
    }
    float easeOutBack(float t) { // the spatial overshoot (damping ~.75)
        const float U = t - 1;
        return 1 + 2.2f * U * U * U + 1.2f * U * U;
    }
    float animT(const Time::steady_tp& since, int ms) {
        const auto EL = std::chrono::duration_cast<std::chrono::milliseconds>(Time::steadyNow() - since).count();
        return std::clamp((float)EL / (float)ms, 0.f, 1.f);
    }

    // ---- the type scale (physical pt) ----

    SType typeScale(double scale) {
        const double FS = (double)cfg.fontSize->value();
        const auto   PT = [&](double logical) { return std::max(1, (int)std::lround(logical * scale)); };
        // the spec's roles off the 12px base: header 11, title 13.5,
        // body 12.5, small 10.5, actions/bar 12.5
        return SType{PT(FS - 1), PT(FS + 1.5), PT(FS + 0.5), PT(FS - 1.5), PT(FS + 0.5), PT(FS + 0.5)};
    }

    // ---- the paint context ----

    CBox SPaint::toPhys(const CBox& global) const {
        return CBox{global}.translate(Vector2D{-monPos.x, -monPos.y + dy}).scale(scale).round();
    }

    void SPaint::rect(const CBox& global, const CHyprColor& c, int round, float rp) const {
        if (warm)
            return;
        const auto B = toPhys(global);
        if (B.w <= 0 || B.h <= 0)
            return;
        g_pHyprOpenGL->renderRect(B, c.modifyA(c.a * alpha), {.round = round, .roundingPower = rp});
    }

    void SPaint::glass(const CBox& global, const CHyprColor& c, int round, float rp) const {
        if (warm)
            return;
        const auto B = toPhys(global);
        if (B.w <= 0 || B.h <= 0)
            return;
        g_pHyprOpenGL->renderRect(B, c.modifyA(c.a * alpha), {.round = round, .roundingPower = rp, .blur = blurOn() && c.a < 1.f, .blurA = alpha});
    }

    void SPaint::shadow(const CBox& global, int round, float rp, int range) const {
        if (warm)
            return;
        const auto B = toPhys(global);
        if (B.w <= 0 || B.h <= 0)
            return;
        static Config::CGradientValueData GRAD{CHyprColor{Theme::SHADOW}};
        g_pHyprOpenGL->renderRoundedShadow(B, round, rp, (int)std::lround(range * scale), GRAD, alpha);
    }

    void SPaint::ring(const CBox& global, const CHyprColor& c, int round, float rp, double px) const {
        if (warm)
            return;
        const auto B = toPhys(global);
        if (B.w <= 0 || B.h <= 0)
            return;
        // the gradient ctor heap-allocates and OkLab-converts — memoize per color
        static std::unordered_map<uint64_t, Config::CGradientValueData> grads;
        const auto                                                      KEY = c.getAsHex();
        auto                                                            IT  = grads.find(KEY);
        if (IT == grads.end())
            IT = grads.emplace(KEY, Config::CGradientValueData{c}).first;
        g_pHyprOpenGL->renderBorder(B, IT->second, {.round = round, .roundingPower = rp, .borderSize = std::max(1, (int)std::lround(px * scale)), .a = alpha});
    }

    void SPaint::tex(const SP<ITexture>& t, double gx, double gy) const {
        if (warm || !t || t->m_texID == 0)
            return;
        const auto P = toPhys(CBox{gx, gy, 1, 1});
        g_pHyprOpenGL->renderTexture(t, CBox{(double)P.x, (double)P.y, t->m_size.x, t->m_size.y}, {.a = alpha});
    }

    void SPaint::texClipped(const SP<ITexture>& t, double gx, double gy, const CBox& clip) const {
        if (warm || !t || t->m_texID == 0 || clip.empty())
            return;
        const auto P = toPhys(CBox{gx, gy, 1, 1});
        g_pHyprOpenGL->renderTexture(t, CBox{(double)P.x, (double)P.y, t->m_size.x, t->m_size.y}, {.a = alpha, .clipRegion = CRegion{toPhys(clip)}});
    }

    void SPaint::texFit(const SP<ITexture>& t, const CBox& cell, int round, float rp) const {
        if (warm || !t || t->m_texID == 0)
            return;
        const auto   B  = toPhys(cell);
        const double TW = t->m_size.x, TH = t->m_size.y;
        if (TW <= 0 || TH <= 0 || B.w <= 0 || B.h <= 0)
            return;
        const double S = std::min(B.w / TW, B.h / TH);
        const double W = TW * S, H = TH * S;
        CBox         F{B.x + (B.w - W) / 2.0, B.y + (B.h - H) / 2.0, W, H};
        const int    R = round > 0 ? std::min(round, (int)std::lround(std::min(W, H) / 2.0)) : 0;
        g_pHyprOpenGL->renderTexture(t, F.round(), {.a = alpha, .round = R, .roundingPower = rp});
    }

    void SPaint::texCover(const SP<ITexture>& t, const CBox& cell, int round, float rp) const {
        if (warm || !t || t->m_texID == 0)
            return;
        const auto   B  = toPhys(cell);
        const double TW = t->m_size.x, TH = t->m_size.y;
        if (TW <= 0 || TH <= 0 || B.w <= 0 || B.h <= 0)
            return;

        Vector2D     uv0{0, 0}, uv1{1, 1};
        const double TEX_AR = TW / TH, CELL_AR = B.w / B.h;
        if (TEX_AR > CELL_AR) {
            const double SPAN = CELL_AR / TEX_AR;
            uv0.x             = (1.0 - SPAN) / 2.0;
            uv1.x             = 1.0 - uv0.x;
        } else if (TEX_AR < CELL_AR) {
            const double SPAN = TEX_AR / CELL_AR;
            uv0.y             = (1.0 - SPAN) / 2.0;
            uv1.y             = 1.0 - uv0.y;
        }
        g_pHyprOpenGL->renderTexture(t, B,
                                     {.a = alpha, .round = round, .roundingPower = rp, .allowCustomUV = true, .primarySurfaceUVTopLeft = uv0, .primarySurfaceUVBottomRight = uv1});
    }

    // ---- shared card recipes (popups and center rows drifted apart once —
    //      the badge and the progress pill draw from ONE place now) ----

    static bool ready(const SP<ITexture>& texture) {
        return texture && texture->m_texID != 0;
    }

    static bool hasFacePile(const SNotif& n) {
        return n.conversation && n.conversationKind == "group" && n.conversationIcon.empty() && n.participants.size() >= 2 && ready(n.participants[0].avatarTex) &&
            ready(n.participants[1].avatarTex);
    }

    static SP<ITexture> conversationLead(const SNotif& n) {
        if (!n.conversation)
            return {};
        if (ready(n.conversationTex))
            return n.conversationTex;
        if (!n.participants.empty() && !n.participants.front().icon.empty() && ready(n.participants.front().avatarTex))
            return n.participants.front().avatarTex;
        if (ready(n.iconTex))
            return n.iconTex;
        if (!n.participants.empty() && ready(n.participants.front().avatarTex))
            return n.participants.front().avatarTex;
        return {};
    }

    bool hasLeadIcon(const SNotif& n) {
        return hasFacePile(n) || ready(conversationLead(n)) || ready(n.identTex);
    }

    void paintProgress(const SPaint& P, double x, double y, double w, int pct, bool critical) {
        const int PR = (int)std::lround(PROGRESS_H / 2 * P.scale);
        P.rect(CBox{x, y, w, PROGRESS_H}, surfaceHigh(), PR);
        if (pct > 0)
            P.rect(CBox{x, y, std::max(w * pct / 100.0, PROGRESS_H), PROGRESS_H}, critical ? color(cfg.colUrgent) : color(cfg.colHighlight), PR);
    }

    // Android's conversation icon container: content is the sender AVATAR and
    // the app IDENTITY rides its bottom-right corner as a badge. Ordinary
    // notifications use only their unbadged application identity.
    void paintIconColumn(const SPaint& P, const SNotif& n, const CBox& cell, bool withBadge, float rp) {
        const bool HASIDENT  = ready(n.identTex);
        const bool FACEPILE  = hasFacePile(n);
        const auto AVATARTEX = conversationLead(n);
        const bool AVATAR    = FACEPILE || ready(AVATARTEX);
        const auto LEAD      = AVATAR ? AVATARTEX : n.identTex;
        if (!FACEPILE && !ready(LEAD))
            return;

        // Faces are round and app icons are squircles. The control geometry is
        // supplied by the warm-pass AOSP recipes, independent of this radius.
        const bool   ROUNDFACE = AVATAR && n.conversation;
        const double R         = ROUNDFACE ? cell.w / 2 : cell.w * 10.0 / 44.0;
        const float  LRP       = ROUNDFACE ? 2.f : rp;
        if (FACEPILE) {
            const double D = cell.w * 24.0 / 40.0;
            const CBox   BACK{cell.x, cell.y, D, D};
            const CBox   FRONT{cell.x + cell.w - D, cell.y + cell.h - D, D, D};
            P.texCover(n.participants[1].avatarTex, BACK, (int)std::lround(D / 2 * P.scale), 2.f);
            P.rect(CBox{FRONT.x - 2, FRONT.y - 2, FRONT.w + 4, FRONT.h + 4}, surface(), (int)std::lround((D + 4) / 2 * P.scale), 2.f);
            P.texCover(n.participants[0].avatarTex, FRONT, (int)std::lround(D / 2 * P.scale), 2.f);
        } else if (ROUNDFACE)
            P.texCover(LEAD, cell, (int)std::lround(R * P.scale), LRP);
        else
            P.texFit(LEAD, cell, (int)std::lround(R * P.scale), LRP);

        // renderBorder grows OUTWARD from the box it is given, so every ring
        // here is drawn on a box inset by its own stroke: the band then lands
        // inside the shape's edge instead of making a marked icon bigger than
        // an unmarked one.
        const double RINGPX = cell.w * BADGE_D * BADGE_INSET;
        const auto   INSET  = [&](const CBox& b) { return CBox{b.x + RINGPX, b.y + RINGPX, b.w - 2 * RINGPX, b.h - 2 * RINGPX}; };

        if (!withBadge || !AVATAR || !HASIDENT) {
            // no badge to ring: a marked chat with no face wears it on the
            // icon it does have
            if (n.priority)
                P.ring(INSET(cell), color(cfg.colHighlight), (int)std::lround((R - RINGPX) * P.scale), LRP, RINGPX);
            return;
        }
        const double D = cell.w * BADGE_D, IN = D * BADGE_INSET;
        const CBox   BB{cell.x + cell.w * (1 + BADGE_PROT) - D, cell.y + cell.h * (1 + BADGE_PROT) - D, D, D};
        // The rim's job is to cut the app glyph free of the avatar it sits on.
        // AOSP tints conversation_badge_background (a white oval in the
        // drawable) to the notification's own background colour, so on a phone
        // the rim reads as a gap; our card is glass over whatever is beneath
        // it, and has no one colour to borrow — a near-white disc separates the
        // glyph against a light avatar and a dark one alike.
        P.rect(BB, CHyprColor{Theme::BADGE_RIM}, (int)std::lround(D / 2 * P.scale), 2.f);
        P.texFit(n.identTex, CBox{BB.x + IN, BB.y + IN, D - 2 * IN, D - 2 * IN}, (int)std::lround((D / 2 - IN) * P.scale), 2.f);
        // conversation_icon_badge_ring — the one AOSP ships with
        // visibility=gone and shows only once you mark the conversation. Its
        // stroke IS the rim's width at the rim's place (importance_ring_size
        // is the badge's own 20dp), so marking a chat recolours that band
        // rather than hanging a second circle off the outside of it.
        if (n.priority)
            P.ring(INSET(BB), color(cfg.colHighlight), (int)std::lround((D / 2 - RINGPX) * P.scale), 2.f, RINGPX);
    }

    void paintParticipantAvatar(const SPaint& P, const SNotif& n, const std::string_view participant, const CBox& cell) {
        const auto IT = std::ranges::find_if(n.participants, [&](const auto& item) { return item.key == participant; });
        if (IT == n.participants.end() || !ready(IT->avatarTex))
            return;
        P.texCover(IT->avatarTex, cell, (int)std::lround(cell.w / 2 * P.scale), 2.f);
    }

} // namespace NHyprnotify
