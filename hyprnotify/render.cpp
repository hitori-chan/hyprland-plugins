// hyprnotify/render.cpp — the cards, their textures, the pass element, damage

#include "hyprnotify.hpp"

namespace NHyprnotify {

    bool               warming = false, texStale = false; // the texture rule — see hyprnotify.hpp

    std::vector<SCard> cards;
    PHLMONITORREF      cardsMon;

    // spacing is design freedom (the parity contract binds colors/structure)
    static constexpr double          PADX = 10, PADY = 8, FRAME = 1, TITLE_GAP = 2, PROGRESS_H = 8, PROGRESS_GAP = 6;

    static CBox                      lastBox;            // last damaged card column, global logical
    static double                    lastStackH     = 0; // the column's height from the last layout, for the pass bounding box
    static bool                      inRenderNotifs = false;

    static UP<SEventLoopDoLaterLock> pendingWarm, pendingRewarm;

    CHyprColor                       color(const SP<Config::Values::CColorValue>& v) {
        struct SMemo {
            uint64_t   raw = 0;
            bool       set = false;
            CHyprColor col;
        };
        static std::unordered_map<const void*, SMemo> memo; // main thread only
        auto&                                         M = memo[v.get()];
        if (!M.set || M.raw != (uint64_t)v->value()) {
            M.raw = (uint64_t)v->value();
            M.set = true;
            M.col = CHyprColor{M.raw};
        }
        return M.col;
    }

    static PHLMONITOR focusedMon() {
        return Desktop::focusState() ? Desktop::focusState()->monitor() : nullptr;
    }

    // ---- text textures ----

    // renderText word-wraps nothing (maxWidth only ellipsizes), so the body
    // rasters through its own pango layout: wrapped to the text column, height-
    // capped with the tail line ellipsized — then the same premultiplied-ARGB32
    // cairo -> createTexture path renderText itself uses.
    static SP<ITexture> buildBodyTex(const std::string& text, const CHyprColor& col, int pt, int maxWidthPx, int maxHeightPx) {
        PangoFontMap*         fontMap = pango_cairo_font_map_get_default();
        PangoContext*         context = pango_font_map_create_context(fontMap);
        PangoLayout*          layout  = pango_layout_new(context);
        PangoFontDescription* fd      = pango_font_description_new();
        g_object_unref(context);

        const auto FAMILY = cfg.font->value();
        pango_font_description_set_family_static(fd, FAMILY.c_str());
        pango_font_description_set_absolute_size(fd, pt * PANGO_SCALE);
        pango_layout_set_font_description(layout, fd);
        pango_layout_set_text(layout, text.c_str(), -1);
        pango_layout_set_width(layout, std::max(maxWidthPx, 1) * PANGO_SCALE);
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_height(layout, std::max(maxHeightPx, pt) * PANGO_SCALE);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

        PangoRectangle ink = {}, log = {};
        pango_layout_get_pixel_extents(layout, &ink, &log);
        const int W = std::max(log.width, ink.x + ink.width), H = std::max(log.height, ink.y + ink.height);
        if (W <= 0 || H <= 0) {
            pango_font_description_free(fd);
            g_object_unref(layout);
            return nullptr;
        }

        auto* SURF = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
        auto* CR   = cairo_create(SURF);
        cairo_set_source_rgba(CR, col.r, col.g, col.b, col.a);
        cairo_move_to(CR, 0, 0);
        pango_cairo_show_layout(CR, layout);

        pango_font_description_free(fd);
        g_object_unref(layout);
        cairo_surface_flush(SURF);

        auto tex = g_pHyprRenderer->createTexture(SURF);
        cairo_destroy(CR);
        cairo_surface_destroy(SURF);
        return tex;
    }

    // ---- painting ----

    struct SPaint {
        PHLMONITOR mon;
        bool       warm  = false;
        double     scale = 1.0;

        CBox       toPhys(const CBox& global) const { // global logical -> monitor physical
            return CBox{global}.translate(-mon->m_position).scale(scale).round();
        }
        void rect(const CBox& global, const CHyprColor& c) const {
            if (warm)
                return;
            g_pHyprOpenGL->renderRect(toPhys(global), c, {});
        }
        // text: native pixel size at a logical position, never scaled
        void tex(const SP<ITexture>& t, double gx, double gy) const {
            if (warm || !t || t->m_texID == 0)
                return;
            const auto P = toPhys(CBox{gx, gy, 1, 1});
            g_pHyprOpenGL->renderTexture(t, CBox{(double)P.x, (double)P.y, t->m_size.x, t->m_size.y}, {});
        }
        // images: scaled into a logical box
        void texFit(const SP<ITexture>& t, const CBox& cell) const {
            if (warm || !t || t->m_texID == 0)
                return;
            g_pHyprOpenGL->renderTexture(t, toPhys(cell), {});
        }
    };

    // One layout, two modes: WARM builds every texture and paints nothing,
    // DRAW paints and must never build (the texture rule — a texture cannot be
    // painted by the frame that created it, and the miss swallows every later
    // draw in the element).
    static void renderCards(PHLMONITOR mon, bool warm) {
        if (!mon)
            return;

        const SPaint P{mon, warm, mon->m_scale};
        const int    PT      = std::max(1, (int)std::lround(cfg.fontSize->value() * P.scale));
        const auto   MB      = mon->logicalBox();
        const double W       = std::max((double)cfg.width->value(), 60.0);
        const double MAXH    = std::max((double)cfg.maxHeight->value(), 40.0);
        const double GAP     = std::max((double)cfg.margin->value(), 0.0);
        const double MAXICON = std::clamp((double)cfg.maxIcon->value(), 8.0, MAXH - 2 * PADY);

        const auto   COLBG = color(cfg.colBg), COLFG = color(cfg.colFg), COLFRAME = color(cfg.colFrame), COLURGENT = color(cfg.colUrgent), COLHL = color(cfg.colHighlight);

        cards.clear(); // capacity retained: no per-frame allocations
        cardsMon = mon;

        const double X = MB.x + MB.w - GAP - W;
        double       y = MB.y + (double)cfg.offsetY->value();

        for (const auto& N : notifs) {
            if (y + 2 * PADY + FRAME * 2 > MB.y + MB.h)
                break; // no room: the tail of the stack waits off-screen, timeouts still running

            if (warm)
                ensureIconTex(*N, (int)std::lround(MAXICON * P.scale));

            // icon fit, logical (image-data pixmaps arrive uncapped)
            double iw = 0, ih = 0;
            if (N->iconTex) {
                const auto   TS = N->iconTex->m_size / P.scale;
                const double F  = std::min(1.0, std::min(MAXICON / TS.x, MAXICON / TS.y));
                iw              = TS.x * F;
                ih              = TS.y * F;
            }

            const double TEXTW   = W - 2 * PADX - (iw > 0 ? iw + PADX : 0);
            const int    TEXTWPX = std::max(1, (int)std::floor(TEXTW * P.scale));

            if (warm) {
                // width/size/color are part of what the textures ARE — a replace
                // that adds an icon narrows the column, a config reload recolors
                if (N->builtPt != PT || N->builtFg != (uint64_t)COLFG.getAsHex() || N->builtTextW != TEXTWPX) {
                    N->titleTex.reset();
                    N->bodyTex.reset();
                    N->titleFor.clear();
                    N->bodyFor.clear();
                    N->builtPt    = PT;
                    N->builtFg    = (uint64_t)COLFG.getAsHex();
                    N->builtTextW = TEXTWPX;
                }
                if (N->summary.empty())
                    N->titleTex.reset();
                else if (!N->titleTex || N->titleFor != N->summary) {
                    N->titleTex = g_pHyprRenderer->renderText(N->summary, COLFG, PT, false, cfg.font->value(), TEXTWPX, 700);
                    N->titleFor = N->summary;
                }
                if (N->body.empty())
                    N->bodyTex.reset();
                else if (!N->bodyTex || N->bodyFor != N->body) {
                    const int CAP = (int)std::floor((MAXH - 2 * PADY) * P.scale) - (N->titleTex ? (int)(N->titleTex->m_size.y + TITLE_GAP * P.scale) : 0) -
                        (N->progress >= 0 ? (int)((PROGRESS_H + PROGRESS_GAP) * P.scale) : 0);
                    N->bodyTex = buildBodyTex(N->body, COLFG, PT, TEXTWPX, CAP);
                    N->bodyFor = N->body;
                }
            } else if ((!N->titleTex && !N->summary.empty()) || (!N->bodyTex && !N->body.empty()))
                texStale = true; // a draw ran ahead of the warm; rebuild + repaint after this frame

            const double TITLEH = N->titleTex ? N->titleTex->m_size.y / P.scale : 0;
            const double BODYH  = N->bodyTex ? N->bodyTex->m_size.y / P.scale : 0;
            double       th     = TITLEH + (TITLEH > 0 && BODYH > 0 ? TITLE_GAP : 0) + BODYH;
            if (N->progress >= 0)
                th += (th > 0 ? PROGRESS_GAP : 0) + PROGRESS_H;

            const double CH       = std::min(MAXH, std::max(ih, th) + 2 * PADY);
            const bool   CRITICAL = N->urgency >= 2;

            // frame: four 1px rects around the fill (the house idiom)
            const auto& FC = CRITICAL ? COLURGENT : COLFRAME;
            P.rect(CBox{X, y, W, FRAME}, FC);
            P.rect(CBox{X, y + CH - FRAME, W, FRAME}, FC);
            P.rect(CBox{X, y + FRAME, FRAME, CH - 2 * FRAME}, FC);
            P.rect(CBox{X + W - FRAME, y + FRAME, FRAME, CH - 2 * FRAME}, FC);
            P.rect(CBox{X + FRAME, y + FRAME, W - 2 * FRAME, CH - 2 * FRAME}, COLBG);

            if (N->iconTex && iw > 0)
                P.texFit(N->iconTex, CBox{X + PADX, y + (CH - ih) / 2, iw, ih});

            const double TX = X + PADX + (iw > 0 ? iw + PADX : 0);
            double       ty = y + std::max(PADY, (CH - th) / 2); // the text block centers, like the old boxes
            if (N->titleTex) {
                P.tex(N->titleTex, TX, ty);
                ty += TITLEH + (BODYH > 0 ? TITLE_GAP : 0);
            }
            if (N->bodyTex) {
                P.tex(N->bodyTex, TX, ty);
                ty += BODYH;
            }
            if (N->progress >= 0) {
                ty += th > 0 ? PROGRESS_GAP : 0;
                P.rect(CBox{TX, ty, TEXTW, PROGRESS_H}, COLFRAME);
                if (N->progress > 0)
                    P.rect(CBox{TX, ty, TEXTW * N->progress / 100.0, PROGRESS_H}, CRITICAL ? COLURGENT : COLHL);
            }

            cards.push_back({CBox{X, y, W, CH}, N->id});
            y += CH + GAP;
        }

        lastStackH = std::max(0.0, y - GAP - (MB.y + (double)cfg.offsetY->value()));
    }

    // ---- damage ----

    static CBox columnBox() {
        if (cards.empty())
            return {};
        double x0 = cards.front().box.x, y0 = cards.front().box.y, x1 = x0, y1 = y0;
        for (const auto& C : cards) {
            x0 = std::min(x0, C.box.x);
            y0 = std::min(y0, C.box.y);
            x1 = std::max(x1, C.box.x + C.box.w);
            y1 = std::max(y1, C.box.y + C.box.h);
        }
        return CBox{x0, y0, x1 - x0, y1 - y0};
    }

    void damageNotifs() {
        if (!g_pHyprRenderer)
            return;
        const auto CUR = columnBox();
        if (lastBox.w > 0)
            g_pHyprRenderer->damageBox(lastBox);
        if (CUR.w > 0)
            g_pHyprRenderer->damageBox(CUR);
        lastBox = CUR;
    }

    // ---- warm ----

    void warmNotifs() {
        if (warming || inRenderNotifs || !g_pCompositor)
            return;
        warming = true;
        if (notifs.empty()) {
            cards.clear();
            lastStackH = 0;
        } else
            renderCards(focusedMon(), true);
        warming  = false;
        texStale = false;
    }

    void notifChanged() {
        if (!g_pEventLoopManager)
            return;
        // one lock: bursts (an OSD volume sweep, a batch of closes) coalesce
        pendingWarm = g_pEventLoopManager->doLaterLock([]() {
            warmNotifs();
            damageNotifs();
            refreshPointerOwnership();
        });
    }

    // Back out to the event loop to build what a draw found missing, then
    // repaint. Deferred because we are inside the render when we notice.
    static void scheduleWarmRepaint() {
        if (!g_pEventLoopManager)
            return;
        pendingRewarm = g_pEventLoopManager->doLaterLock([]() {
            warmNotifs();
            damageNotifs();
        });
    }

    // ---- pass element ----

    class CNotifyPassElement : public IPassElement {
      public:
        CNotifyPassElement(PHLMONITOR mon) : m_mon(mon) {}
        virtual ~CNotifyPassElement() = default;

        virtual std::vector<UP<IPassElement>> draw() override {
            inRenderNotifs = true;
            renderCards(m_mon.lock(), false);
            inRenderNotifs = false;
            if (texStale)
                scheduleWarmRepaint();
            return {};
        }
        virtual bool needsLiveBlur() override {
            return false;
        }
        virtual bool needsPrecomputeBlur() override {
            return false;
        }
        virtual std::optional<CBox> boundingBox() override {
            const auto MON = m_mon.lock();
            if (!MON)
                return std::nullopt;
            const auto   MB = MON->logicalBox();
            const double W  = std::max((double)cfg.width->value(), 60.0) + std::max((double)cfg.margin->value(), 0.0);
            // monitor-local LOGICAL px — the pass scales by m_scale itself
            return CBox{MB.w - W - 1, (double)cfg.offsetY->value() - 1, W + 2, std::max(lastStackH, 0.0) + 2};
        }
        virtual const char* passName() override {
            return "CNotifyPassElement";
        }
        virtual ePassElementType type() override {
            return EK_CUSTOM;
        }

      private:
        PHLMONITORREF m_mon;
    };

    void onRenderStage(eRenderStage stage) {
        if (stage != RENDER_POST_WINDOWS || notifs.empty())
            return;
        // never above the lockscreen (the built-in overlay leaks there; these
        // are the user's notifications). No unlock watcher needed: textures
        // stay warm through the lock, and the lock surface's unmap damages the
        // whole output — that IS the survivors' repaint.
        if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked())
            return;
        const auto MON = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!MON || MON != focusedMon())
            return;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CNotifyPassElement>(MON));
    }

    void renderExit() {
        pendingWarm.reset();
        pendingRewarm.reset();
        if (lastBox.w > 0 && g_pHyprRenderer)
            g_pHyprRenderer->damageBox(lastBox);
        lastBox = {};
        cards.clear();
        cardsMon.reset();
        lastStackH = 0;
        warming    = false;
        texStale   = false;
    }

} // namespace NHyprnotify
