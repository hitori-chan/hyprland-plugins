// hyprnotify/text.cpp — the pango rasterizer, the keyed text cache, and the
// markup/link/age helpers every drawing unit shares.
//
// Everything text becomes a texture here, keyed on content + style + width:
// staleness needs no bookkeeping — a replace or an age-bucket move simply
// misses to a new key, and a grace-generation sweep bounds the map (evicting
// only what no recent warm wanted — the hyprbar pattern).

#include "ui.hpp"

#include <format>

namespace NHyprnotify {

    // ---- small shared helpers ----

    std::string hexOf(const CHyprColor& c) {
        return std::format("#{:02x}{:02x}{:02x}", (int)std::lround(c.r * 255), (int)std::lround(c.g * 255), (int)std::lround(c.b * 255));
    }

    // escape a RAW string (app names) for insertion into pango markup;
    // summary/body are already sanitized markup and embed verbatim
    std::string esc(const std::string& raw) {
        std::string out;
        out.reserve(raw.size());
        for (const char C : raw) {
            if (C == '&')
                out += "&amp;";
            else if (C == '<')
                out += "&lt;";
            else if (C == '>')
                out += "&gt;";
            else
                out += C;
        }
        return out;
    }

    // the collapsed row's one-liner: the LAST non-empty line — a joined
    // conversation shows its newest message, like Android's collapsed card
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

    // renderText word-wraps nothing (maxWidth only ellipsizes), so everything
    // here rasters through its own pango layout — then the same
    // premultiplied-ARGB32 cairo -> createTexture path renderText uses.
    // maxHeightPx > 0 caps pixels (tail line ellipsized); < 0 caps LINES —
    // pango's cap is per PARAGRAPH, so line-capped text must be single-line
    // flattened first (the collapsed rows are).
    static SP<ITexture> buildText(const std::string& text, const CHyprColor& col, int pt, int maxWidthPx, int maxHeightPx, float lineSpacing, bool markup, int weight,
                                  const CHyprColor* linkCol = nullptr, std::vector<std::pair<std::string, CBox>>* outLinks = nullptr) {
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
        pango_layout_set_height(layout, maxHeightPx < 0 ? maxHeightPx : std::max(maxHeightPx, pt) * PANGO_SCALE);
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

    // ---- the keyed cache ----

    static std::unordered_map<std::string, SCachedText> texCache;
    static uint64_t                                     texGen         = 0;
    static constexpr uint64_t                           TEX_CACHE_LIFE = 24;

    const SCachedText* cachedText(const std::string& text, const CHyprColor& col, int pt, int maxWpx, int maxHpx, float lineSp, bool markup, int weight,
                                  const CHyprColor* linkCol) {
        if (text.empty())
            return nullptr;
        char      meta[80];
        const int METALEN = std::snprintf(meta, sizeof(meta), "|%llx|%d|%d|%d|%d|%d|%d|%llx", (unsigned long long)col.getAsHex(), pt, maxWpx, maxHpx, (int)(lineSp * 100), markup,
                                          weight, linkCol ? (unsigned long long)linkCol->getAsHex() : 0ULL);

        static std::string KEY; // reused; main thread only
        KEY.clear();
        KEY += text;
        KEY.append(meta, METALEN > 0 ? (size_t)METALEN : 0);

        if (const auto IT = texCache.find(KEY); IT != texCache.end()) {
            IT->second.gen = texGen;
            return &IT->second;
        }
        if (!warmGate.mayBuild())
            return nullptr;

        SCachedText entry;
        entry.gen = texGen;
        if (linkCol) {
            std::vector<std::pair<std::string, CBox>> lrects;
            entry.tex = buildText(text, col, pt, maxWpx, maxHpx, lineSp, markup, weight, linkCol, &lrects);
            for (auto& [HREF, R] : lrects)
                entry.links.push_back({HREF, R}); // physical px; the drawing unit descales
        } else
            entry.tex = buildText(text, col, pt, maxWpx, maxHpx, lineSp, markup, weight);
        return &texCache.emplace(KEY, std::move(entry)).first->second;
    }

    double texH(const SCachedText* e, double scale) {
        return e && e->tex ? e->tex->m_size.y / scale : 0;
    }
    double texW(const SCachedText* e, double scale) {
        return e && e->tex ? e->tex->m_size.x / scale : 0;
    }

    void textCacheTick() {
        texGen++;
    }
    void textCacheSweep() {
        if (texGen > TEX_CACHE_LIFE)
            std::erase_if(texCache, [](const auto& E) { return E.second.gen + TEX_CACHE_LIFE < texGen; });
    }
    void textCacheClear() {
        texCache.clear();
    }

} // namespace NHyprnotify
