// awesome/notify/text.cpp — the pango rasterizer, the keyed texture cache, and
// the markup/link/age helpers every drawing unit shares.
//
// Everything text becomes a texture here, keyed on content + style + width
// into core/canvas.hpp's cache — see there for why the key needs no
// staleness bookkeeping and why the bound is a grace generation.

#include "textcut.hpp"
#include "transcript.hpp"
#include "ui.hpp"

#include "../core/config.hpp"
#include "../core/text.hpp"

#include <cctype>
#include <functional>
#include <string_view>
#include <unordered_map>
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
    std::string clockString(std::time_t t) {
        std::tm    at{}, today{};
        const auto NOW = std::time(nullptr);
        localtime_r(&t, &at);
        localtime_r(&NOW, &today);
        char buf[32];
        const bool SAMEDAY = at.tm_year == today.tm_year && at.tm_yday == today.tm_yday;
        return std::string{buf, std::strftime(buf, sizeof(buf), SAMEDAY ? "%H:%M" : "%d %b", &at)};
    }

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
        // a mailto's query is the mail client's to interpret, and some
        // honor ?attach=<path>: a local file pre-attached to the draft.
        // The address is the link; the rest is dropped.
        if (scheme == "mailto" && href.size() > COLON + 1) {
            href.resize(std::min(href.size(), href.find_first_of("?#")));
            return href.size() > COLON + 1 ? href : std::string{};
        }
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
    // A plain memo, not the warm-aged raster cache: a metric is a few bytes
    // and asked for on the draw path too, where nothing ticks a generation
    // (aged that way, its byte cap filled after a few styles and refused
    // every new one — two Pango layouts per call from then on). Lookups key
    // on the stack buffer; past the bound the memo starts over.
    struct SMetricHash {
        using is_transparent = void;
        size_t operator()(std::string_view s) const {
            return std::hash<std::string_view>{}(s);
        }
    };
    static bool lineMetric(const std::string& family, int pt, int weight, float lineSpacing, int& lineH, int& adv) {
        char                   mkey[128];
        const int              KLEN = snprintf(mkey, sizeof mkey, "|lm|%s|%d|%d|%d", family.c_str(), pt, weight, (int)(lineSpacing * 100));
        const std::string_view KEY(mkey, std::min<size_t>(KLEN > 0 ? (size_t)KLEN : 0, sizeof mkey - 1));
        static std::unordered_map<std::string, SLineMetric, SMetricHash, std::equal_to<>> cache;
        if (const auto IT = cache.find(KEY); IT != cache.end()) {
            lineH = IT->second.lineH;
            adv   = IT->second.adv;
            return true;
        }
        PangoContext* context = NAwesome::newTextContext();
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
        if (cache.size() >= 64)
            cache.clear();
        cache.emplace(std::string{KEY}, SLineMetric{lineH, adv});
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
    // line ellipsized); < 0 is pango's one line per paragraph, ellipsized —
    // text that must be ONE line is flattened before it gets here (the
    // collapsed rows).
    // The whole lines a pixel budget holds at this font's real advance, and
    // the height pango is asked to keep within for them: the pixel slack
    // covers the metrics' rounding (exactly N lines' height dropped the Nth)
    // and is far less than another line's advance. maxHeightPx < 0 is that
    // many lines of one paragraph; the height is then 0 (none).
    static int heightBudget(const std::string& family, int pt, int weight, float lineSpacing, int maxHeightPx, int& lines) {
        int LINEH = 0, ADV = 0;
        if (maxHeightPx < 0) {
            lines = -maxHeightPx;
            return 0;
        }
        if (!lineMetric(family, pt, weight, lineSpacing, LINEH, ADV)) {
            lines = std::max(1, maxHeightPx / std::max(pt, 1) + 1);
            return std::max(maxHeightPx, pt);
        }
        lines = std::max(1, (maxHeightPx - LINEH) / ADV + 1);
        return LINEH + (lines - 1) * ADV + 1;
    }

    // A layout as every raster and every measure of one builds it: the
    // font, the markup (links recoloured when asked, an unparseable one
    // laid out plain, its tags stripped), the width, the wrap, the spacing
    // and the hanging indent. The fit (fittedTranscript) measures with the
    // very layout the raster draws, so the raster never cuts what it kept.
    static PangoLayout* newLayout(const std::string& text, int pt, int maxWidthPx, float lineSpacing, bool markup, int weight, int indentPx, const CHyprColor* linkCol,
                                  std::vector<SLinkSpan>* linkSpans) {
        const std::string     FAMILY{NAwesome::cfg().getS("plugin:awesome:notify:font")};
        PangoContext*         context = NAwesome::newTextContext();
        PangoLayout*          layout  = pango_layout_new(context);
        PangoFontDescription* fd      = pango_font_description_new();
        g_object_unref(context);

        pango_font_description_set_family(fd, FAMILY.c_str());
        pango_font_description_set_absolute_size(fd, pt * PANGO_SCALE);
        if (weight != 400)
            pango_font_description_set_weight(fd, (PangoWeight)weight);
        pango_layout_set_font_description(layout, fd);
        pango_font_description_free(fd);

        if (markup) {
            std::vector<SLinkSpan> spans;
            std::string            md = text;
            if (linkCol)
                md = convertLinks(text, hexOf(*linkCol), spans);
            PangoAttrList* attrs    = nullptr;
            char*          stripped = nullptr;
            GError*        err      = nullptr;
            if (pango_parse_markup(md.c_str(), -1, 0, &attrs, &stripped, nullptr, &err)) {
                pango_layout_set_text(layout, stripped, -1);
                g_free(stripped);
                pango_layout_set_attributes(layout, attrs);
                pango_attr_list_unref(attrs);
                if (linkSpans)
                    *linkSpans = std::move(spans);
            } else {
                if (err)
                    g_error_free(err);
                const std::string PLAIN = stripMarkupTags(text); // never show raw <span>/<a> syntax
                pango_layout_set_text(layout, PLAIN.c_str(), -1);
            }
        } else
            pango_layout_set_text(layout, text.c_str(), -1);

        pango_layout_set_width(layout, std::max(maxWidthPx, 1) * PANGO_SCALE);
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        if (lineSpacing > 0)
            pango_layout_set_line_spacing(layout, lineSpacing);
        if (indentPx > 0)
            pango_layout_set_indent(layout, -indentPx * PANGO_SCALE); // hanging: every line but a paragraph's first
        return layout;
    }

    static SP<ITexture> buildText(const std::string& fullText, const CHyprColor& col, int pt, int maxWidthPx, int maxHeightPx, float lineSpacing, bool markup, int weight,
                                  const CHyprColor* linkCol = nullptr, std::vector<std::pair<std::string, CBox>>* outLinks = nullptr, bool headCut = false,
                                  eShadow shadow = eShadow::SOFT, int* padOut = nullptr, int indentPx = 0) {
        const std::string FAMILY{NAwesome::cfg().getS("plugin:awesome:notify:font")};
        // the lines this raster shows: a pixel budget's whole lines, or the
        // negative height's lines of one paragraph
        int       LINES  = 1;
        const int HEIGHT = heightBudget(FAMILY, pt, weight, lineSpacing, maxHeightPx, LINES);
        // a start-ellipsized line shows its END: only an end cut is safe
        const auto CUT = headCut ? std::string{} :
                                   cutToLines(fullText, markup, 2 * (size_t)LINES * ((size_t)std::max(maxWidthPx, 1) * 5 / std::max(pt, 1) + 1) + 64,
                                              maxHeightPx >= 0 ? (size_t)LINES + 1 : SIZE_MAX);
        const std::string& text = CUT.empty() ? fullText : CUT;

        std::vector<SLinkSpan> linkSpans;
        PangoLayout*           layout = newLayout(text, pt, maxWidthPx, lineSpacing, markup, weight, indentPx, outLinks ? linkCol : nullptr, &linkSpans);
        // the pixel budget is a count of WHOLE lines at this font's real
        // advance: the old 1.35x-body constant undershot the default font
        // by ~10%, so a seven-line transcript rendered six (the clip took
        // the oldest line first, and went unnoticed until the order went
        // chronological and started clipping the newest)
        if (maxHeightPx < 0) {
            pango_layout_set_height(layout, maxHeightPx);
            // ONE line means one paragraph: the negative height bounds the
            // lines of each paragraph, and a sender's &#10; or U+2029 would
            // otherwise make a thousand of them (a 27000 px texture)
            pango_layout_set_single_paragraph_mode(layout, TRUE);
        } else
            pango_layout_set_height(layout, HEIGHT * PANGO_SCALE);
        pango_layout_set_ellipsize(layout, headCut ? PANGO_ELLIPSIZE_START : PANGO_ELLIPSIZE_END);

        PangoRectangle ink = {}, log = {};
        pango_layout_get_pixel_extents(layout, &ink, &log);
        int W = std::max(log.width, ink.x + ink.width), H = std::max(log.height, ink.y + ink.height);
        // A stranger's <span size=/line_height=/rise=> can blow one glyph up
        // to thousands of px (a 180 MB raster per surface). Real markup
        // keeps a line within ~3x the type size and wraps inside the width:
        // past that the markup goes, and the plain text lays out instead.
        if (markup && (W > std::max(maxWidthPx, 1) + 4 * pt || H > pango_layout_get_line_count(layout) * pt * 3 + pt)) {
            g_object_unref(layout); // owns the attr list now
            return buildText(stripMarkupTags(text), col, pt, maxWidthPx, maxHeightPx, lineSpacing, false, weight, nullptr, nullptr, false, shadow, padOut, indentPx);
        }
        if (W <= 0 || H <= 0) {
            g_object_unref(layout);
            return nullptr;
        }
        // The raster never outgrows its slot, whatever the ink does: stacked
        // combining marks (plain text skips the markup check above) reach
        // far past the line box. A little overhang is kept for italics and
        // ordinary accents.
        W = std::min(W, std::max(maxWidthPx, 1) + 2 * pt);
        H = std::min(H, maxHeightPx >= 0 ? std::max(maxHeightPx, pt) + 2 * pt : 4 * pt);

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

        const auto SH  = textShadow(shadow, pt);
        const int  PAD = SH.pad();
        if (padOut)
            *padOut = PAD;

        auto* SURF = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W + 2 * PAD, H + 2 * PAD);
        auto* CR   = cairo_create(SURF);
        showShadowedLayout(CR, layout, W, H, SH, col.r, col.g, col.b, col.a);

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
    // measured in texture bytes (the shell's cache does the same): the byte
    // cap is what bounds a burst of large rasters, sizeof() never did
    static NAwesome::CGenCache<SCachedText, 24> texCache{64ull << 20, [](const SCachedText& t) {
        return sizeof(SCachedText) + (t.tex ? (size_t)std::max(0.0, t.tex->m_size.x) * (size_t)std::max(0.0, t.tex->m_size.y) * 4 : 0);
    }};

    const SCachedText* cachedText(const std::string& text, const CHyprColor& col, int pt, int maxWpx, int maxHpx, float lineSp, bool markup, int weight,
                                  const CHyprColor* linkCol, bool headCut, eShadow shadow, int indentPx) {
        if (text.empty())
            return nullptr;
        // wide enough for every conversion at ITS widest (two 16-digit hex,
        // eight 11-char signed decimals, the separators): snprintf reports what
        // it WOULD have written, so a buffer the key outgrew would be read
        // past its end — and a truncated key would alias two styles onto one
        // texture. The append clamps anyway, for the next field added here.
        char      meta[176];
        const int METALEN = std::snprintf(meta, sizeof(meta), "|%llx|%d|%d|%d|%d|%d|%d|%llx|%d|%d|%d", (unsigned long long)col.getAsHex(), pt, maxWpx, maxHpx, (int)(lineSp * 100),
                                          markup, weight, linkCol ? (unsigned long long)linkCol->getAsHex() : 0ULL, headCut, (int)shadow, indentPx);

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
            entry.tex = buildText(text, col, pt, maxWpx, maxHpx, lineSp, markup, weight, linkCol, &lrects, false, shadow, &entry.pad, indentPx);
            for (auto& [HREF, R] : lrects)
                entry.links.push_back({HREF, R}); // physical px; the drawing unit descales
        } else
            entry.tex = buildText(text, col, pt, maxWpx, maxHpx, lineSp, markup, weight, nullptr, nullptr, headCut, shadow, &entry.pad, indentPx);
        return texCache.insert(KEY, std::move(entry));
    }

    double texH(const SCachedText* e, double scale) {
        return e && e->tex ? (e->tex->m_size.y - 2 * e->pad) / scale : 0;
    }
    double texW(const SCachedText* e, double scale) {
        return e && e->tex ? (e->tex->m_size.x - 2 * e->pad) / scale : 0;
    }
    void texAt(const SPaint& P, const SCachedText* e, double x, double y) {
        if (e && e->tex)
            P.tex(e->tex, x - e->pad / P.scale, y - e->pad / P.scale);
    }

    // ---- a chat card's window ----

    // a font or theme change re-fits every window (the texture cache's own
    // clear is the same moment)
    static uint64_t fitGen = 1;

    const std::string& fittedTranscript(const SP<SNotif>& n, int pt, int widthPx, int capPx, int indentPx, const CHyprColor* linkCol) {
        auto& M = n->fitMemo;
        if (M.rev == n->rev && M.gen == fitGen && M.pt == pt && M.w == widthPx && M.cap == capPx && M.indent == indentPx && M.links == (linkCol != nullptr))
            return M.body;
        M = {.rev = n->rev, .gen = fitGen, .pt = pt, .w = widthPx, .cap = capPx, .indent = indentPx, .links = linkCol != nullptr};
        std::vector<std::string> paras;
        for (size_t at = 0; at <= n->body.size();) {
            const auto NL = std::min(n->body.find('\n', at), n->body.size());
            if (NL > at)
                paras.emplace_back(n->body, at, NL - at);
            at = NL + 1;
        }
        int        lines  = 0;
        const int  HEIGHT = heightBudget(std::string{NAwesome::cfg().getS("plugin:awesome:notify:font")}, pt, 400, 1.1f, capPx, lines);
        const auto LINES  = [&](const std::string& s) {
            PangoLayout* L = newLayout(s, pt, widthPx, 1.1f, true, 400, indentPx, linkCol, nullptr);
            const auto   N = (size_t)pango_layout_get_line_count(L);
            g_object_unref(L);
            return N;
        };
        const auto FITS = [&](const std::string& s) {
            PangoLayout* L = newLayout(s, pt, widthPx, 1.1f, true, 400, indentPx, linkCol, nullptr);
            PangoRectangle log{};
            pango_layout_get_pixel_extents(L, nullptr, &log);
            g_object_unref(L);
            return log.height <= HEIGHT;
        };
        M.body = Transcript::fitTail(paras, (size_t)lines, LINES, FITS);
        return M.body;
    }

    void textCacheTick() {
        texCache.tick();
    }
    void textCacheSweep() {
        texCache.sweep();
    }
    void textCacheClear() {
        texCache.clear();
        fitGen++;
    }

} // namespace NAwesome::Notify
