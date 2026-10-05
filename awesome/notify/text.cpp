// awesome/notify/text.cpp — the pango rasterizer, the keyed texture cache, and
// the markup/link/age helpers every drawing unit shares.
//
// Everything text becomes a texture here, keyed on content + style + width
// into core/canvas.hpp's cache — see there for why the key needs no
// staleness bookkeeping and why the bound is a grace generation.

#include "ui.hpp"

#include "../core/config.hpp"

#include <cctype>
#include <format>

namespace NAwesome::Notify {

    // ---- small shared helpers ----

    std::string hexOf(const CHyprColor& c) {
        return std::format("#{:02x}{:02x}{:02x}", (int)std::lround(c.r * 255), (int)std::lround(c.g * 255), (int)std::lround(c.b * 255));
    }

    const std::string& hexOfCached(const CHyprColor& c) {
        static std::unordered_map<uint64_t, std::string> memo; // main thread only; a handful of theme colors
        auto&                                            S = memo[c.getAsHex()];
        if (S.empty())
            S = hexOf(c);
        return S;
    }

    std::string& scratch() {
        static std::string S; // main thread only; capacity retained across frames
        S.clear();
        return S;
    }

    void appendEsc(std::string& dst, const std::string& raw) {
        for (const char C : raw) {
            if (C == '&')
                dst += "&amp;";
            else if (C == '<')
                dst += "&lt;";
            else if (C == '>')
                dst += "&gt;";
            else
                dst += C;
        }
    }

    // the collapsed row's one-liner: the last non-empty line — every card
    // body is chronological, so the newest message ends it
    std::string lastLine(const std::string& body) {
        const size_t END = body.find_last_not_of('\n');
        if (END == std::string::npos)
            return "";
        const size_t NL    = body.rfind('\n', END);
        const size_t START = NL == std::string::npos ? 0 : NL + 1;
        return body.substr(START, END - START + 1);
    }

    // bucketed so a texture key only moves when the display would
    std::string ageString(const Time::steady_tp& t) {
        const auto S = std::chrono::duration_cast<std::chrono::seconds>(Time::steadyNow() - t).count();
        if (S < 60)
            return "now";
        if (S < 3600)
            return std::format("{}m", S / 60);
        if (S < 86400)
            return std::format("{}h", S / 3600);
        return std::format("{}d", S / 86400);
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

    // A body link is opened with xdg-open on a click: only web and mail
    // links qualify. Any sender on the session bus writes these bodies, and
    // xdg-open will happily run a file:// desktop entry, a custom scheme
    // handler, or a local script — those stay plain text.
    static std::string openableLink(std::string href) {
        const auto COLON = href.find(':');
        if (COLON == std::string::npos || COLON == 0 || COLON > 16)
            return {};
        std::string scheme = href.substr(0, COLON);
        for (auto& c : scheme)
            c = (char)std::tolower((unsigned char)c);
        if ((scheme == "http" || scheme == "https") && href.compare(COLON, 3, "://") == 0)
            return href;
        if (scheme == "mailto" && href.size() > COLON + 1)
            return href;
        return {};
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
                            if (!cur.href.empty())
                                out.push_back(cur);
                            inLink = false;
                        }
                    } else {
                        const auto href = openableLink(decodeEntities(Parse::attrValue(in.substr(i, END - i + 1), "href")));
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

    // the font's real line advance, measured once per style: the pixel
    // budgets of the rows are line COUNTS, and a constant multiplier of the
    // point size drifts with the user's font
    struct SLineMetric {
        int lineH; // one line, no spacing beyond the font's own
        int adv;   // the per-line advance with the requested spacing
    };
    static bool lineMetric(const std::string& family, int pt, int weight, float lineSpacing, int& lineH, int& adv) {
        char mkey[128];
        const int KLEN = snprintf(mkey, sizeof mkey, "|lm|%s|%d|%d|%d", family.c_str(), pt, weight, (int)(lineSpacing * 100));
        const std::string KEY(mkey, std::min<size_t>(KLEN > 0 ? (size_t)KLEN : 0, sizeof mkey - 1));
        static NAwesome::CGenCache<SLineMetric, 16, 1024> cache;
        if (const auto* HIT = cache.find(KEY)) {
            lineH = HIT->lineH;
            adv   = HIT->adv;
            return true;
        }
        PangoFontMap* fontMap = pango_cairo_font_map_get_default();
        PangoContext* context = pango_font_map_create_context(fontMap);
        const auto    measure = [](const char* text, PangoContext* ctx, const char* fam, int size, int wt, float sp) -> int {
            PangoLayout*          l  = pango_layout_new(ctx);
            PangoFontDescription* f  = pango_font_description_new();
            pango_font_description_set_family_static(f, fam);
            pango_font_description_set_absolute_size(f, size * PANGO_SCALE);
            if (wt != 400)
                pango_font_description_set_weight(f, (PangoWeight)wt);
            pango_layout_set_font_description(l, f);
            if (sp > 0)
                pango_layout_set_line_spacing(l, sp);
            pango_layout_set_text(l, text, -1);
            PangoRectangle a, b;
            pango_layout_get_pixel_extents(l, &a, &b);
            const int h   = b.height;
            pango_font_description_free(f);
            g_object_unref(l);
            return h;
        };
        const int H1 = measure("x", context, family.c_str(), pt, weight, lineSpacing);
        const int H2 = measure("x\ny", context, family.c_str(), pt, weight, lineSpacing);
        g_object_unref(context);
        if (H1 <= 0 || H2 <= H1)
            return false;
        lineH = H1;
        adv   = H2 - H1;
        cache.insert(KEY, SLineMetric{lineH, adv});
        return true;
    }

    double bodyLineH(double scale) {
        const auto T      = typeScale(scale);
        const auto FAMILY = NAwesome::cfg().getS("plugin:awesome:notify:font");
        int LINEH = 0, ADV = 0;
        if (!lineMetric(FAMILY, T.body, 400, 1.1f, LINEH, ADV))
            return (double)T.body / scale * 1.35;
        return (double)LINEH / scale;
    }

    int bodyBudgetPx(double scale, int lines) {
        if (lines <= 0)
            return 0;
        const auto T      = typeScale(scale);
        const auto FAMILY = NAwesome::cfg().getS("plugin:awesome:notify:font");
        int LINEH = 0, ADV = 0;
        if (!lineMetric(FAMILY, T.body, 400, 1.1f, LINEH, ADV))
            return (int)std::lround((double)T.body * 1.35 * lines);
        return LINEH + (lines - 1) * ADV;
    }

    // renderText word-wraps nothing (maxWidth only ellipsizes), so everything
    // here rasters through its own pango layout — then the same
    // premultiplied-ARGB32 cairo -> createTexture path renderText uses.
    // maxHeightPx >= 0 is a pixel budget (rounded down to whole lines, tail
    // line ellipsized); < 0 caps nothing — text that must be single-line is
    // flattened to one line before it gets here (the collapsed rows).
    static SP<ITexture> buildText(const std::string& text, const CHyprColor& col, int pt, int maxWidthPx, int maxHeightPx, float lineSpacing, bool markup, int weight,
                                  const CHyprColor* linkCol = nullptr, std::vector<std::pair<std::string, CBox>>* outLinks = nullptr) {
        PangoFontMap*         fontMap = pango_cairo_font_map_get_default();
        PangoContext*         context = pango_font_map_create_context(fontMap);
        PangoLayout*          layout  = pango_layout_new(context);
        PangoFontDescription* fd      = pango_font_description_new();
        g_object_unref(context);

        const std::string FAMILY{NAwesome::cfg().getS("plugin:awesome:notify:font")};
        pango_font_description_set_family_static(fd, FAMILY.c_str());
        pango_font_description_set_absolute_size(fd, pt * PANGO_SCALE);
        if (weight != 400)
            pango_font_description_set_weight(fd, (PangoWeight)weight);
        pango_layout_set_font_description(layout, fd);

        PangoAttrList*         attrs = nullptr;
        std::vector<SLinkSpan> linkSpans;
        if (markup) {
            std::string md = text;
            if (outLinks && linkCol)
                md = convertLinks(text, hexOf(*linkCol), linkSpans);
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
        // the pixel budget is a count of WHOLE lines at this font's real
        // advance: the old 1.35x-body constant undershot the default font
        // by ~10%, so a seven-line transcript rendered six (the clip took
        // the oldest line first, and went unnoticed until the order went
        // chronological and started clipping the newest)
        if (maxHeightPx < 0) {
            pango_layout_set_height(layout, maxHeightPx);
        } else {
            int LINEH = 0, ADV = 0;
            int HEIGHT = std::max(maxHeightPx, pt);
            if (lineMetric(FAMILY, pt, weight, lineSpacing, LINEH, ADV)) {
                const int LINES = std::max(1, (maxHeightPx - LINEH) / ADV + 1); // whole lines that fit
                HEIGHT          = LINEH + (LINES - 1) * ADV;
            }
            pango_layout_set_height(layout, HEIGHT * PANGO_SCALE);
        }
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
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

        // probe the laid-out layout for each link's hit rects (physical px):
        // one per line the link's text actually occupies, from pango's own
        // visual ranges (layout-relative, alignment and bidi included). One
        // box from the link's start to its end once spanned the wrong area
        // when the link wrapped: the text after it on the first line opened
        // it, its own second-line part did not.
        if (outLinks && !linkSpans.empty()) {
            PangoLayoutIter* IT = pango_layout_get_iter(layout);
            do {
                const PangoLayoutLine* LINE = pango_layout_iter_get_line_readonly(IT);
                const int              LS = LINE->start_index, LE = LS + LINE->length;
                PangoRectangle         lineRect;
                pango_layout_iter_get_line_extents(IT, nullptr, &lineRect);
                for (const auto& L : linkSpans) {
                    const int S = std::max(L.start, LS), E = std::min(L.start + L.len, LE);
                    if (L.len <= 0 || S >= E)
                        continue;
                    int* ranges = nullptr;
                    int  n      = 0;
                    pango_layout_line_get_x_ranges(const_cast<PangoLayoutLine*>(LINE), S, E, &ranges, &n);
                    for (int i = 0; i < n; i++)
                        outLinks->push_back({L.href, CBox{ranges[2 * i] / (double)PANGO_SCALE, lineRect.y / (double)PANGO_SCALE,
                                                          (ranges[2 * i + 1] - ranges[2 * i]) / (double)PANGO_SCALE, lineRect.height / (double)PANGO_SCALE}});
                    g_free(ranges);
                }
            } while (pango_layout_iter_next_line(IT));
            pango_layout_iter_free(IT);
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

    // ---- the keyed cache ----

    // 24 warms of grace — the shade warms only when its model changes, so
    // this is a longer wall-clock reprieve than the same number gives the bar
    static NAwesome::CGenCache<SCachedText, 24> texCache;

    const SCachedText* cachedText(const std::string& text, const CHyprColor& col, int pt, int maxWpx, int maxHpx, float lineSp, bool markup, int weight,
                                  const CHyprColor* linkCol) {
        if (text.empty())
            return nullptr;
        // wide enough for every conversion at ITS widest (two 16-digit hex,
        // six 11-char signed decimals, the separators): snprintf reports what
        // it WOULD have written, so a buffer the key outgrew would be read
        // past its end — and a truncated key would alias two styles onto one
        // texture. The append clamps anyway, for the next field added here.
        char      meta[112];
        const int METALEN = std::snprintf(meta, sizeof(meta), "|%llx|%d|%d|%d|%d|%d|%d|%llx", (unsigned long long)col.getAsHex(), pt, maxWpx, maxHpx, (int)(lineSp * 100), markup,
                                          weight, linkCol ? (unsigned long long)linkCol->getAsHex() : 0ULL);

        static std::string KEY; // reused; main thread only
        KEY.clear();
        KEY += text;
        KEY.append(meta, std::min<size_t>(METALEN > 0 ? (size_t)METALEN : 0, sizeof(meta) - 1));

        if (const auto* HIT = texCache.find(KEY))
            return HIT;
        if (!NAwesome::Canvas::inst().gate().mayBuild())
            return nullptr;

        SCachedText entry;
        if (linkCol) {
            std::vector<std::pair<std::string, CBox>> lrects;
            entry.tex = buildText(text, col, pt, maxWpx, maxHpx, lineSp, markup, weight, linkCol, &lrects);
            for (auto& [HREF, R] : lrects)
                entry.links.push_back({HREF, R}); // physical px; the drawing unit descales
        } else
            entry.tex = buildText(text, col, pt, maxWpx, maxHpx, lineSp, markup, weight);
        return texCache.insert(KEY, std::move(entry));
    }

    // ---- the Material chevron ----

    // A stroked chevron, not a font glyph: Pixel's expand_more/less is two
    // 45° strokes with round caps, and a glyph's weight is the font's, never
    // ours. Same warm-gate contract as cachedText — built only on a warm
    // pass, drawn on a later frame.
    static SP<ITexture> buildChevron(int dir, const CHyprColor& col, int px) {
        if (px <= 4)
            return nullptr;
        const double S = px / 24.0; // Material's 24dp canvas
        auto*        SURF = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
        auto*        CR   = cairo_create(SURF);
        cairo_set_source_rgba(CR, col.r, col.g, col.b, col.a);
        cairo_set_line_width(CR, 2.0 * S);
        cairo_set_line_cap(CR, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(CR, CAIRO_LINE_JOIN_ROUND);
        // 12dp wide, 6dp tall, centered on the canvas
        if (dir > 0) { // up
            cairo_move_to(CR, 6.0 * S, 14.0 * S);
            cairo_line_to(CR, 12.0 * S, 8.0 * S);
            cairo_line_to(CR, 18.0 * S, 14.0 * S);
        } else { // down
            cairo_move_to(CR, 6.0 * S, 10.0 * S);
            cairo_line_to(CR, 12.0 * S, 16.0 * S);
            cairo_line_to(CR, 18.0 * S, 10.0 * S);
        }
        cairo_stroke(CR);
        cairo_surface_flush(SURF);
        auto tex = g_pHyprRenderer->createTexture(SURF);
        cairo_destroy(CR);
        cairo_surface_destroy(SURF);
        return tex;
    }

    const SCachedText* chevronTex(int dir, const CHyprColor& col, int px) {
        if (px <= 4)
            return nullptr;
        char      meta[64];
        const int METALEN = std::snprintf(meta, sizeof(meta), "chevron|%d|%llx|%d", dir, (unsigned long long)col.getAsHex(), px);
        static std::string KEY; // reused; main thread only
        KEY.clear();
        KEY.assign(meta, METALEN > 0 ? (size_t)METALEN : 0);

        if (const auto* HIT = texCache.find(KEY))
            return HIT;
        if (!NAwesome::Canvas::inst().gate().mayBuild())
            return nullptr;
        SCachedText entry;
        entry.tex = buildChevron(dir, col, px);
        return texCache.insert(KEY, std::move(entry));
    }

    double texH(const SCachedText* e, double scale) {
        return e && e->tex ? e->tex->m_size.y / scale : 0;
    }
    double texW(const SCachedText* e, double scale) {
        return e && e->tex ? e->tex->m_size.x / scale : 0;
    }

    void textCacheTick() {
        texCache.tick();
    }
    void textCacheSweep() {
        texCache.sweep();
    }
    void textCacheClear() {
        texCache.clear();
    }

} // namespace NAwesome::Notify
