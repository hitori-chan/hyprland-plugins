// hyprnotify/render.cpp — the cards, their textures, the pass element, damage

#include "hyprnotify.hpp"

#include <format>

namespace NHyprnotify {

    bool               warming = false, texStale = false; // the texture rule — see hyprnotify.hpp

    std::vector<SCard> cards;
    PHLMONITORREF      cardsMon;

    // spacing is design freedom (the parity contract binds colors/structure)
    static constexpr double          PADX = 14, PADY = 12, FRAME = 1, ICON_GAP = 12, KICKER_GAP = 3, TITLE_GAP = 4, PROGRESS_H = 4, PROGRESS_GAP = 8;
    static constexpr double          HERO_TEXT_MIN = 60;                                                          // a hero never starves the text: ~kicker + title + a body line
    static constexpr double          BTN_H = 24, BTN_PADX = 10, BTN_GAP = 6, BTN_ROW_GAP = 8, BTN_ICON = 15, BTN_ICON_GAP = 5; // action-button metrics
    static constexpr double          BODYIMG_H = 96, IMG_GAP = 6, IMG_ROW_GAP = 8;                                             // body <img> thumbnail metrics

    static CBox                      lastBox;            // last damaged card column, global logical
    static double                    lastStackH     = 0; // the column's height from the last layout, for the pass bounding box
    static bool                      inRenderNotifs = false;
    static uint32_t                  hoveredId      = 0;
    static int                       hoveredBtn     = -1; // action button under the pointer within hoveredId, -1 = the frame

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

    // the kicker line renders the app name, uppercased ASCII-only (UTF-8
    // continuation bytes pass through untouched)
    static std::string kickerCase(std::string s) {
        for (auto& c : s)
            c = (char)std::toupper((unsigned char)c);
        return s;
    }

    // ---- hyperlinks (<a href>) ----

    static int cpToUtf8(uint32_t c, char buf[4]) {
        if (c < 0x80) {
            buf[0] = (char)c;
            return 1;
        }
        if (c < 0x800) {
            buf[0] = (char)(0xC0 | (c >> 6));
            buf[1] = (char)(0x80 | (c & 0x3F));
            return 2;
        }
        if (c < 0x10000) {
            buf[0] = (char)(0xE0 | (c >> 12));
            buf[1] = (char)(0x80 | ((c >> 6) & 0x3F));
            buf[2] = (char)(0x80 | (c & 0x3F));
            return 3;
        }
        buf[0] = (char)(0xF0 | (c >> 18));
        buf[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (c & 0x3F));
        return 4;
    }

    static uint32_t parseCp(const std::string& e) { // "#960" or "#x3C0"
        if (e.size() < 2 || e[0] != '#')
            return 0;
        return e[1] == 'x' || e[1] == 'X' ? (uint32_t)std::strtol(e.c_str() + 2, nullptr, 16) : (uint32_t)std::strtol(e.c_str() + 1, nullptr, 10);
    }

    // Byte length an entity decodes to — must match Pango's stripping so link
    // offsets into the stripped text stay aligned.
    static int entityBytes(const std::string& e) {
        if (e == "amp" || e == "lt" || e == "gt" || e == "quot" || e == "apos")
            return 1;
        if (e.size() > 1 && e[0] == '#') {
            const uint32_t C = parseCp(e);
            char           b[4];
            if (C == 0 || C > 0x10FFFF || (C >= 0xD800 && C <= 0xDFFF))
                return 0;
            return cpToUtf8(C, b);
        }
        return 0;
    }

    static std::string decodeEntities(const std::string& s) { // for the href handed to xdg-open
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size();) {
            if (s[i] == '&') {
                const auto END = s.find(';', i);
                if (END != std::string::npos && END - i <= 10) {
                    const auto E = s.substr(i + 1, END - i - 1);
                    if (E == "amp" || E == "lt" || E == "gt" || E == "quot" || E == "apos") {
                        out += E == "amp" ? '&' : E == "lt" ? '<' : E == "gt" ? '>' : E == "quot" ? '"' : '\'';
                        i = END + 1;
                        continue;
                    }
                    if (E.size() > 1 && E[0] == '#') {
                        char           b[4];
                        const uint32_t C = parseCp(E);
                        if (C > 0 && C <= 0x10FFFF && !(C >= 0xD800 && C <= 0xDFFF)) {
                            out.append(b, cpToUtf8(C, b));
                            i = END + 1;
                            continue;
                        }
                    }
                }
            }
            out += s[i];
            i++;
        }
        return out;
    }

    // The clean fallback when markup won't parse (a client's unbalanced or
    // malformed tags): drop every tag, decode the entities. Never leaks tag
    // syntax to the user the way set_text on the raw markup would.
    static std::string stripMarkupTags(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size();) {
            if (s[i] == '<') {
                if (const auto END = s.find('>', i); END != std::string::npos) {
                    i = END + 1;
                    continue;
                }
            }
            out += s[i++];
        }
        return decodeEntities(out);
    }

    struct SLinkSpan {
        std::string href;
        int         start = 0, len = 0; // byte range in the STRIPPED text
    };

    // Rewrite the sanitizer's live <a href> into a styled <span> Pango renders,
    // tracking each link's byte span in the stripped text for later hit-testing.
    // The input is already sanitized, so every '<' opens a whitelisted tag and
    // every '&' is a valid entity.
    static std::string convertLinks(const std::string& in, const std::string& colHex, std::vector<SLinkSpan>& out) {
        std::string md;
        md.reserve(in.size() + 32);
        int       plain = 0;
        SLinkSpan cur;
        bool      inLink = false;
        for (size_t i = 0; i < in.size();) {
            if (in[i] == '<') {
                const auto END = in.find('>', i);
                if (END == std::string::npos) {
                    md += in[i++];
                    continue;
                }
                size_t j = i + 1;
                bool   close = false;
                if (j < END && in[j] == '/') {
                    close = true;
                    j++;
                }
                size_t ns = j;
                while (j < END && std::isalpha((unsigned char)in[j]))
                    j++;
                std::string name = in.substr(ns, j - ns);
                std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });
                if (name == "a") {
                    if (close) {
                        md += "</span>";
                        if (inLink) {
                            cur.len = plain - cur.start;
                            out.push_back(cur);
                            inLink = false;
                        }
                    } else {
                        std::string href;
                        const auto  TAG = in.substr(i, END - i + 1);
                        std::string tl  = TAG; // case-insensitive attr search, same offsets as TAG
                        std::ranges::transform(tl, tl.begin(), [](unsigned char c) { return std::tolower(c); });
                        if (const auto HP = tl.find("href"); HP != std::string::npos)
                            if (const auto Q = TAG.find_first_of("\"'", HP); Q != std::string::npos)
                                if (const auto Q2 = TAG.find(TAG[Q], Q + 1); Q2 != std::string::npos)
                                    href = decodeEntities(TAG.substr(Q + 1, Q2 - Q - 1));
                        md += "<span foreground=\"" + colHex + "\" underline=\"single\">";
                        cur    = SLinkSpan{href, plain, 0};
                        inLink = true;
                    }
                } else
                    md += in.substr(i, END - i + 1); // other tag verbatim, 0 plain bytes
                i = END + 1;
                continue;
            }
            if (in[i] == '&') {
                const auto END = in.find(';', i);
                if (END != std::string::npos) {
                    md += in.substr(i, END - i + 1);
                    plain += entityBytes(in.substr(i + 1, END - i - 1));
                    i = END + 1;
                    continue;
                }
            }
            md += in[i];
            plain++;
            i++;
        }
        return md;
    }

    // renderText word-wraps nothing (maxWidth only ellipsizes), so the body
    // and the kicker raster through their own pango layout — then the same
    // premultiplied-ARGB32 cairo -> createTexture path renderText uses.
    // maxHeightPx > 0 caps pixels (tail line ellipsized); < 0 caps lines
    // (pango's negative-height convention — the kicker passes -1).
    // outLinks (body only): the <a href> hit rects, physical px relative to the
    // text origin.
    static SP<ITexture> buildText(const std::string& text, const CHyprColor& col, int pt, int maxWidthPx, int maxHeightPx, double letterSpacingEm, float lineSpacing, bool markup = false,
                                  int weight = 400, const CHyprColor* linkCol = nullptr, std::vector<std::pair<std::string, CBox>>* outLinks = nullptr) {
        PangoFontMap*         fontMap = pango_cairo_font_map_get_default();
        PangoContext*         context = pango_font_map_create_context(fontMap);
        PangoLayout*          layout  = pango_layout_new(context);
        PangoFontDescription* fd      = pango_font_description_new();
        g_object_unref(context);

        const auto FAMILY = cfg.font->value();
        pango_font_description_set_family_static(fd, FAMILY.c_str());
        pango_font_description_set_absolute_size(fd, pt * PANGO_SCALE);
        if (weight != 400)
            pango_font_description_set_weight(fd, (PangoWeight)weight);
        pango_layout_set_font_description(layout, fd);

        // markup: the body/title carry the whitelisted Pango tags (bus sanitizes
        // them). parse_markup splits attrs from text and validates; a malformed
        // run fails soft to the raw string instead of erroring the whole card.
        // For the body, <a href> is first rewritten to a styled <span> and its
        // stripped-text byte span recorded for hit-testing.
        PangoAttrList*         attrs = nullptr;
        std::vector<SLinkSpan> linkSpans;
        if (markup) {
            std::string md = text;
            if (outLinks && linkCol) {
                const auto HEX = std::format("#{:02x}{:02x}{:02x}", (int)std::lround(linkCol->r * 255), (int)std::lround(linkCol->g * 255), (int)std::lround(linkCol->b * 255));
                md             = convertLinks(text, HEX, linkSpans);
            }
            char*   stripped = nullptr;
            GError* err      = nullptr;
            if (pango_parse_markup(md.c_str(), -1, 0, &attrs, &stripped, nullptr, &err)) {
                pango_layout_set_text(layout, stripped, -1);
                g_free(stripped);
            } else {
                if (err)
                    g_error_free(err);
                const std::string PLAIN = stripMarkupTags(text); // never show raw <span>/<a> syntax
                pango_layout_set_text(layout, PLAIN.c_str(), -1);
                linkSpans.clear(); // offsets are meaningless if the markup didn't parse
            }
        } else
            pango_layout_set_text(layout, text.c_str(), -1);

        pango_layout_set_width(layout, std::max(maxWidthPx, 1) * PANGO_SCALE);
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_height(layout, maxHeightPx < 0 ? maxHeightPx : std::max(maxHeightPx, pt) * PANGO_SCALE);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        if (letterSpacingEm > 0) {
            if (!attrs)
                attrs = pango_attr_list_new();
            pango_attr_list_insert(attrs, pango_attr_letter_spacing_new((int)std::lround(letterSpacingEm * pt * PANGO_SCALE)));
        }
        if (attrs) {
            pango_layout_set_attributes(layout, attrs);
            pango_attr_list_unref(attrs);
        }
        if (lineSpacing > 0)
            pango_layout_set_line_spacing(layout, lineSpacing);

        PangoRectangle ink = {}, log = {};
        pango_layout_get_pixel_extents(layout, &ink, &log);
        const int W = std::max(log.width, ink.x + ink.width), H = std::max(log.height, ink.y + ink.height);
        if (W <= 0 || H <= 0) {
            pango_font_description_free(fd);
            g_object_unref(layout);
            return nullptr;
        }

        // probe the laid-out layout for each link's hit rect (physical px)
        if (outLinks && !linkSpans.empty())
            for (const auto& L : linkSpans) {
                if (L.len <= 0)
                    continue;
                PangoRectangle a, b;
                pango_layout_index_to_pos(layout, L.start, &a);
                pango_layout_index_to_pos(layout, L.start + L.len, &b);
                const double X0 = a.x / (double)PANGO_SCALE, Y0 = a.y / (double)PANGO_SCALE;
                double       x1 = b.x / (double)PANGO_SCALE, h = a.height / (double)PANGO_SCALE;
                if (b.y != a.y) { // the link wrapped a line: cover to the right edge and down
                    x1 = (double)W;
                    h  = (b.y + b.height) / (double)PANGO_SCALE - Y0;
                }
                outLinks->push_back({L.href, CBox{std::min(X0, x1), Y0, std::abs(x1 - X0), h}});
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
        void rect(const CBox& global, const CHyprColor& c, int round = 0) const {
            if (warm)
                return;
            g_pHyprOpenGL->renderRect(toPhys(global), c, {.round = round});
        }
        // the frame: one border call instead of four rects. The gradient is
        // memoized — its ctor heap-allocates and OkLab-converts, and this ran
        // per card per frame for colors that only move on config reload
        void border(const CBox& global, const CHyprColor& c, int round, int sizePx) const {
            if (warm)
                return;
            static std::unordered_map<uint64_t, Config::CGradientValueData> grads;
            const auto                                                      KEY = c.getAsHex();
            auto                                                            IT  = grads.find(KEY);
            if (IT == grads.end())
                IT = grads.emplace(KEY, Config::CGradientValueData{c}).first;
            g_pHyprOpenGL->renderBorder(toPhys(global), IT->second, {.round = round, .borderSize = sizePx});
        }
        // text: native pixel size at a logical position, never scaled
        void tex(const SP<ITexture>& t, double gx, double gy) const {
            if (warm || !t || t->m_texID == 0)
                return;
            const auto P = toPhys(CBox{gx, gy, 1, 1});
            g_pHyprOpenGL->renderTexture(t, CBox{(double)P.x, (double)P.y, t->m_size.x, t->m_size.y}, {});
        }
        // images: scaled into a logical box
        void texFit(const SP<ITexture>& t, const CBox& cell, int round = 0) const {
            if (warm || !t || t->m_texID == 0)
                return;
            g_pHyprOpenGL->renderTexture(t, toPhys(cell), {.round = round});
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
        const int    PT     = std::max(1, (int)std::lround(cfg.fontSize->value() * P.scale));
        const int    KPT    = std::max(1, (int)std::lround((cfg.fontSize->value() - 2) * P.scale)); // the kicker's 10px against the body's 12
        const int    TPT    = std::max(1, (int)std::lround((cfg.fontSize->value() + 1) * P.scale)); // the title's 13
        const int    ROUNDP = std::max(0, (int)std::lround(cfg.rounding->value() * P.scale));
        const int    FRAMEP = std::max(1, (int)std::lround(FRAME * P.scale));
        const auto   MB     = mon->logicalBox();
        const double W       = std::max((double)cfg.width->value(), 60.0);
        const double MAXH    = std::max((double)cfg.maxHeight->value(), 40.0);
        const double GAP     = std::max((double)cfg.margin->value(), 0.0);
        const double MAXICON = std::clamp((double)cfg.maxIcon->value(), 8.0, MAXH - 2 * PADY);
        const double HEROCAP = std::max(16.0, MAXH - 2 * PADY - HERO_TEXT_MIN);

        const auto   COLBG = color(cfg.colBg), COLFG = color(cfg.colFg), COLTITLE = color(cfg.colTitle), COLKICKER = color(cfg.colKicker), COLFRAME = color(cfg.colFrame),
                   COLURGENT = color(cfg.colUrgent), COLHL = color(cfg.colHighlight), COLLINK = color(cfg.colLink);
        // any text color moving under a config reload re-keys every raster
        const uint64_t FGKEY = COLFG.getAsHex() ^ (COLTITLE.getAsHex() * 0x9E3779B97F4A7C15ULL) ^ (COLKICKER.getAsHex() * 0xC2B2AE3D27D4EB4FULL) ^
            (COLURGENT.getAsHex() * 0xD6E8FEB86659FD93ULL) ^ (COLLINK.getAsHex() * 0x8CB92BA72F3D8DD7ULL); // critical kicker + link color re-key the body

        // pack action buttons into rows within maxW (logical); fills `out` with
        // each button box relative to the button area's top-left and returns the
        // block height. A button is icon+label+padding, capped at the full width.
        auto layoutButtons = [&P](const SNotif& n, double maxW, std::vector<CBox>& out) -> double {
            out.clear();
            double x = 0, rowY = 0;
            for (const auto& A : n.actions) {
                const double LW    = A.tex ? A.tex->m_size.x / P.scale : 0;
                const double ICONW = (n.actionIcons && A.iconTex) ? BTN_ICON + BTN_ICON_GAP : 0;
                const double BW    = std::min(maxW, ICONW + LW + 2 * BTN_PADX);
                if (x > 0 && x + BW > maxW + 0.5) { // wrap to a new row
                    x = 0;
                    rowY += BTN_H + BTN_GAP;
                }
                out.push_back(CBox{x, rowY, BW, BTN_H});
                x += BW + BTN_GAP;
            }
            return out.empty() ? 0 : rowY + BTN_H;
        };

        // same row packing for body <img> thumbnails, each scaled to BODYIMG_H
        auto layoutImages = [&P](const SNotif& n, double maxW, std::vector<CBox>& out) -> double {
            out.clear();
            double x = 0, rowY = 0;
            for (const auto& IM : n.bodyImages) {
                if (!IM.tex)
                    continue;
                const double AR = IM.tex->m_size.y > 0 ? IM.tex->m_size.x / IM.tex->m_size.y : 1.0;
                const double WD = std::min(maxW, AR * BODYIMG_H);
                if (x > 0 && x + WD > maxW + 0.5) {
                    x = 0;
                    rowY += BODYIMG_H + IMG_GAP;
                }
                out.push_back(CBox{x, rowY, WD, BODYIMG_H});
                x += WD + IMG_GAP;
            }
            return out.empty() ? 0 : rowY + BODYIMG_H;
        };

        cards.clear(); // capacity retained: no per-frame allocations
        cardsMon = mon;

        const double X = MB.x + MB.w - GAP - W;
        double       y = MB.y + (double)cfg.offsetY->value();

        for (const auto& N : notifs) {
            if (N->waiting)
                continue; // DND queue: collected, not shown
            if (y + 2 * PADY + FRAME * 2 > MB.y + MB.h)
                break; // no room: the tail of the stack waits off-screen, timeouts still running

            if (warm)
                ensureIconTex(*N, (int)std::lround(MAXICON * P.scale), (int)std::lround((W - 2 * FRAME) * P.scale), (int)std::lround(HEROCAP * P.scale));

            const bool   CRITICAL = N->urgency >= 2;
            const bool   HERO     = N->iconTex && N->heroTex;
            const double HEROH    = HERO ? N->iconTex->m_size.y / P.scale : 0;

            // icon fit, logical (image-data pixmaps can arrive uncapped)
            double iw = 0, ih = 0;
            if (N->iconTex && !HERO) {
                const auto   TS = N->iconTex->m_size / P.scale;
                const double F  = std::min(1.0, std::min(MAXICON / TS.x, MAXICON / TS.y));
                iw              = TS.x * F;
                ih              = TS.y * F;
            }

            const double TEXTW   = W - 2 * PADX - (iw > 0 ? iw + ICON_GAP : 0);
            const int    TEXTWPX = std::max(1, (int)std::floor(TEXTW * P.scale));

            // action-button label (+ icon) textures — built one step ahead of the
            // body so the body's height cap can leave room for the button row
            if (warm) {
                const bool BRESET = N->builtPt != PT || N->builtFg != FGKEY || N->builtTextW != TEXTWPX;
                const int  BLW    = std::max(1, (int)std::floor((TEXTW - 2 * BTN_PADX) * P.scale));
                for (auto& A : N->actions) {
                    if (BRESET || A.label.empty()) {
                        A.tex.reset();
                        A.builtFor.clear();
                    }
                    if (!A.label.empty() && A.builtFor != A.label) {
                        A.tex      = buildText(A.label, COLTITLE, PT, BLW, -1, 0, 0);
                        A.builtFor = A.label;
                    }
                    ensureActionIcon(*N, A, (int)std::lround(BTN_ICON * P.scale));
                }
            }
            std::vector<CBox> btnBoxes;
            const double      BUTTONSH  = N->actions.empty() ? 0 : layoutButtons(*N, TEXTW, btnBoxes);
            const double      BTN_BLOCK = BUTTONSH > 0 ? BTN_ROW_GAP + BUTTONSH : 0;

            // body <img> thumbnails — same pre-body treatment so the body cap
            // leaves room for them too
            if (warm)
                for (auto& IM : N->bodyImages)
                    ensureBodyImage(IM, (int)std::lround(BODYIMG_H * P.scale));
            std::vector<CBox> imgBoxes;
            const double      IMAGESH   = N->bodyImages.empty() ? 0 : layoutImages(*N, TEXTW, imgBoxes);
            const double      IMG_BLOCK = IMAGESH > 0 ? IMG_ROW_GAP + IMAGESH : 0;

            if (warm) {
                // width/size/color are part of what the textures ARE — a replace
                // that adds an icon narrows the column, a config reload recolors
                if (N->builtPt != PT || N->builtFg != FGKEY || N->builtTextW != TEXTWPX) {
                    N->kickerTex.reset();
                    N->titleTex.reset();
                    N->bodyTex.reset();
                    N->kickerFor.clear();
                    N->titleFor.clear();
                    N->bodyFor.clear();
                    N->builtPt    = PT;
                    N->builtFg    = FGKEY;
                    N->builtTextW = TEXTWPX;
                }
                if (N->builtCrit != CRITICAL) { // criticality recolors the kicker
                    N->kickerTex.reset();
                    N->kickerFor.clear();
                    N->builtCrit = CRITICAL;
                }
                // staleness keys on the For fields ALONE, never texture
                // presence: text that rasters to zero (a "\n" body) records a
                // failed build as done — keying on the null texture retried
                // every warm and the draw side re-flagged it, a repaint loop
                // at frame rate for the card's whole life
                if (N->appName.empty()) {
                    N->kickerTex.reset();
                    N->kickerFor.clear();
                } else if (N->kickerFor != N->appName) {
                    N->kickerTex = buildText(kickerCase(N->appName), CRITICAL ? COLURGENT : COLKICKER, KPT, TEXTWPX, -1, 0.08, 0);
                    N->kickerFor = N->appName;
                }
                if (N->summary.empty()) {
                    N->titleTex.reset();
                    N->titleFor.clear();
                } else if (N->titleFor != N->summary) {
                    N->titleTex = buildText(N->summary, COLTITLE, TPT, TEXTWPX, -1, 0, 0, true, 700);
                    N->titleFor = N->summary;
                }
                if (N->body.empty()) {
                    N->bodyTex.reset();
                    N->bodyFor.clear();
                    N->links.clear();
                } else if (N->bodyFor != N->body) {
                    const int CAP = (int)std::floor((MAXH - 2 * PADY - HEROH) * P.scale) - (N->kickerTex ? (int)(N->kickerTex->m_size.y + KICKER_GAP * P.scale) : 0) -
                        (N->titleTex ? (int)(N->titleTex->m_size.y + TITLE_GAP * P.scale) : 0) - (N->progress >= 0 ? (int)((PROGRESS_H + PROGRESS_GAP) * P.scale) : 0) -
                        (int)std::floor((BTN_BLOCK + IMG_BLOCK) * P.scale); // buttons + images claim their room; the body ellipsizes to fit
                    // 1.1 x pango's natural line ~= the design's 1.35em body leading.
                    // clamp CAP >0: a tiny max_height makes it negative, which pango
                    // reads as a LINE count (kicker's -1 convention) and overflows the card
                    std::vector<std::pair<std::string, CBox>> lrects;
                    N->bodyTex = buildText(N->body, COLFG, PT, TEXTWPX, std::max(CAP, PT), 0, 1.1f, true, 400, &COLLINK, &lrects);
                    N->bodyFor = N->body;
                    N->links.clear();
                    for (auto& [HREF, R] : lrects)
                        N->links.push_back({HREF, CBox{R.x / P.scale, R.y / P.scale, R.w / P.scale, R.h / P.scale}}); // physical -> logical, relative to body origin
                }
            } else {
                // a draw ran ahead of the warm: flag it if ANY texture the card
                // paints is unbuilt (text, an action label, or a body image),
                // so the rewarm+repaint catches a replace that changed only
                // actions/images, not just text
                bool stale = N->kickerFor != N->appName || N->titleFor != N->summary || N->bodyFor != N->body;
                for (const auto& A : N->actions)
                    stale = stale || (!A.label.empty() && A.builtFor != A.label);
                for (const auto& IM : N->bodyImages)
                    stale = stale || (!IM.src.empty() && IM.builtFor != IM.src);
                if (stale)
                    texStale = true;
            }

            const double KICKERH = N->kickerTex ? N->kickerTex->m_size.y / P.scale : 0;
            const double TITLEH  = N->titleTex ? N->titleTex->m_size.y / P.scale : 0;
            const double BODYH   = N->bodyTex ? N->bodyTex->m_size.y / P.scale : 0;
            double       th      = KICKERH + (KICKERH > 0 && (TITLEH > 0 || BODYH > 0) ? KICKER_GAP : 0) + TITLEH + (TITLEH > 0 && BODYH > 0 ? TITLE_GAP : 0) + BODYH;
            th += IMG_BLOCK;
            if (N->progress >= 0)
                th += (th > 0 ? PROGRESS_GAP : 0) + PROGRESS_H;
            th += BTN_BLOCK;

            const double CH = HERO ? std::min(MAXH, FRAME + HEROH + PADY + th + PADY) : std::min(MAXH, std::max(ih, th) + 2 * PADY);

            // fill under the whole card, frame ring over its outer edge: no
            // corner seam, and 5 rects become 2 calls
            const auto& FC = CRITICAL ? COLURGENT : (N->id == hoveredId && hoveredBtn < 0 ? COLKICKER : COLFRAME);
            P.rect(CBox{X, y, W, CH}, COLBG, ROUNDP);
            P.border(CBox{X, y, W, CH}, FC, ROUNDP, FRAMEP);

            if (HERO)
                P.texFit(N->iconTex, CBox{X + FRAME, y + FRAME, W - 2 * FRAME, HEROH}, ROUNDP);
            else if (N->iconTex && iw > 0)
                P.texFit(N->iconTex, CBox{X + PADX, y + (CH - ih) / 2, iw, ih}, ROUNDP);

            const double                 TX = X + PADX + (iw > 0 ? iw + ICON_GAP : 0);
            double                       ty = HERO ? y + FRAME + HEROH + PADY : y + std::max(PADY, (CH - th) / 2); // sans hero the text block centers, like the old boxes
            std::vector<SCard::SLinkHit> cardLinks;
            if (N->kickerTex) {
                P.tex(N->kickerTex, TX, ty);
                ty += KICKERH + (TITLEH > 0 || BODYH > 0 ? KICKER_GAP : 0);
            }
            if (N->titleTex) {
                P.tex(N->titleTex, TX, ty);
                ty += TITLEH + (BODYH > 0 ? TITLE_GAP : 0);
            }
            if (N->bodyTex) {
                P.tex(N->bodyTex, TX, ty);
                for (const auto& L : N->links) // link hit rects, body-relative -> global logical
                    cardLinks.push_back({CBox{TX + L.rel.x, ty + L.rel.y, L.rel.w, L.rel.h}, L.href});
                ty += BODYH;
            }
            if (!imgBoxes.empty()) {
                ty += IMG_ROW_GAP;
                size_t bi = 0;
                for (const auto& IM : N->bodyImages)
                    if (IM.tex && bi < imgBoxes.size()) {
                        const auto& B = imgBoxes[bi++];
                        P.texFit(IM.tex, CBox{TX + B.x, ty + B.y, B.w, B.h}, ROUNDP);
                    }
                ty += IMAGESH;
            }
            if (N->progress >= 0) {
                ty += th > 0 ? PROGRESS_GAP : 0;
                const double FILLW = TEXTW * N->progress / 100.0;
                P.rect(CBox{TX, ty, TEXTW, PROGRESS_H}, COLFRAME, ROUNDP);
                if (N->progress > 0) // a sliver narrower than its corners renders square
                    P.rect(CBox{TX, ty, FILLW, PROGRESS_H}, CRITICAL ? COLURGENT : COLHL, FILLW * P.scale >= 2 * ROUNDP ? ROUNDP : 0);
                ty += PROGRESS_H;
            }

            // action buttons: bordered pills below the content, label (+ icon)
            // centered; the hovered one warms like the card frame does
            std::vector<SCard::SBtn> cardBtns;
            if (!btnBoxes.empty()) {
                ty += BTN_ROW_GAP;
                const auto& BFC = CRITICAL ? COLURGENT : COLFRAME;
                for (size_t i = 0; i < btnBoxes.size(); i++) {
                    const auto& A    = N->actions[i];
                    const CBox  BOX{TX + btnBoxes[i].x, ty + btnBoxes[i].y, btnBoxes[i].w, btnBoxes[i].h};
                    const bool  BHOV = N->id == hoveredId && (int)i == hoveredBtn;
                    if (BHOV)
                        P.rect(BOX, COLKICKER.modifyA(0.16F), ROUNDP);
                    P.border(BOX, BHOV ? COLKICKER : BFC, ROUNDP, FRAMEP);
                    const double ICONW = (N->actionIcons && A.iconTex) ? BTN_ICON + BTN_ICON_GAP : 0;
                    const double LW    = A.tex ? A.tex->m_size.x / P.scale : 0;
                    double       cx    = BOX.x + std::max(BTN_PADX, (BOX.w - ICONW - LW) / 2);
                    if (N->actionIcons && A.iconTex) {
                        P.texFit(A.iconTex, CBox{cx, BOX.y + (BOX.h - BTN_ICON) / 2, BTN_ICON, BTN_ICON}, 0);
                        cx += BTN_ICON + BTN_ICON_GAP;
                    }
                    if (A.tex)
                        P.tex(A.tex, cx, BOX.y + (BOX.h - A.tex->m_size.y / P.scale) / 2);
                    cardBtns.push_back({BOX, A.id});
                }
            }

            cards.push_back({CBox{X, y, W, CH}, N->id, std::move(cardBtns), std::move(cardLinks)});
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
        // renderBorder paints the frame ring OUTSIDE the fill box, so the damage
        // must grow by the ring width or it strands on appear/close (see c2e7c47)
        const auto   M      = cardsMon.lock();
        const double MARGIN = (M ? std::ceil(M->m_scale) : 1.0) + 1.0;
        const auto   CUR    = columnBox();
        const CBox   NEW    = CUR.w > 0 ? CBox{CUR}.expand(MARGIN) : CBox{};
        if (lastBox.w > 0)
            g_pHyprRenderer->damageBox(lastBox); // stored already expanded
        if (NEW.w > 0)
            g_pHyprRenderer->damageBox(NEW);
        lastBox = NEW;
    }

    // the hover affordance repaints exactly the cards whose frame changed;
    // no textures move, so no warm — plain damage from the motion listener
    void setHovered(uint32_t id, int btn) {
        if (id == hoveredId && btn == hoveredBtn)
            return;
        if (g_pHyprRenderer) {
            const auto   M      = cardsMon.lock();
            const double MARGIN = (M ? std::ceil(M->m_scale) : 1.0) + 1.0; // the frame ring lands outside C.box
            for (const auto& C : cards)
                if (C.id == hoveredId || C.id == id)
                    g_pHyprRenderer->damageBox(CBox{C.box}.expand(MARGIN));
        }
        hoveredId  = id;
        hoveredBtn = btn;
    }

    // ---- warm ----

    void warmNotifs() {
        if (warming || inRenderNotifs || !g_pCompositor)
            return;
        warming        = true;
        const auto MON = notifs.empty() ? nullptr : focusedMon();
        if (!MON) {
            // no cards — or no monitor (disconnect transition): stale boxes
            // must not linger to swallow clicks over nothing
            cards.clear();
            lastStackH = 0;
        } else
            renderCards(MON, true);
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
            // A card arriving over a solitary/scanned-out fullscreen window
            // (mpv under direct_scanout): the monitor presents the client's
            // buffer directly, so the per-card damageBox may not schedule a
            // compositor frame at all — and onRenderPreChecks, which drops the
            // scanout/solitary latch, only runs from renderMonitor. Force a
            // whole-monitor frame so renderMonitor runs and the card
            // composites. Full-monitor (not the card box) so it can't be
            // occlusion-culled behind the fullscreen surface; a no-op cost when
            // the monitor isn't latched.
            if (const auto MON = focusedMon(); MON && g_pHyprRenderer && (MON->m_directScanoutIsActive || !MON->m_solitaryClient.expired()))
                for (const auto& N : notifs)
                    if (!N->waiting) {
                        g_pHyprRenderer->damageMonitor(MON);
                        break;
                    }
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

    // A solitary fullscreen client (mpv) makes the compositor skip the whole
    // workspace render for its monitor — direct scanout, or a solitary-only
    // renderWindow — so RENDER_POST_WINDOWS never fires and the card is
    // invisible. naughty notifications are ontop, so while a VISIBLE card is up
    // we drop the monitor's solitary latch here, at preChecks (which fires per
    // monitor BEFORE the scanout decision): the normal render path then runs and
    // composites the card over the fullscreen window. Self-healing — once the
    // last card clears, the compositor re-latches solitary and scanout
    // re-engages, so this costs compositing only while a card shows.
    void onRenderPreChecks(PHLMONITOR mon) {
        if (!mon || notifs.empty() || mon != focusedMon())
            return;
        if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked())
            return; // never force a card to float over the lockscreen
        bool visible = false;
        for (const auto& N : notifs)
            if (!N->waiting) { // all-waiting = suspended (DND): nothing shown, don't inhibit
                visible = true;
                break;
            }
        if (!visible)
            return;
        mon->m_solitaryClient.reset(); // open the solitary gate -> renderWorkspace -> RENDER_POST_WINDOWS
        // resetting solitary alone would SEGV on the transition frame:
        // canAttemptDirectScanoutFast() stays true off m_lastScanout and
        // attemptDirectScanout() then derefs the now-null candidate. Leaving any
        // active scanout clears that latch so the scanout branch is skipped.
        if (!mon->m_lastScanout.expired() || mon->m_directScanoutIsActive)
            mon->handleDSleave();
    }

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
        hoveredId  = 0;
        hoveredBtn = -1;
    }

} // namespace NHyprnotify
