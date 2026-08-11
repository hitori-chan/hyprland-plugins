// hyprnotify/parse.cpp — the incoming payload, turned into things a card can
// hold. Everything here is a pure transform of what an arbitrary D-Bus peer
// sent us: markup we must not trust, an image buffer whose own header may
// lie, a path that may be an icon name instead. It is deliberately separate
// from the model (which decides what a card DOES) and from the bus (which
// only carries the bytes) — this is the hostile-input surface, and it is the
// one worth reading on its own.

#include "common/icons.hpp"

#include "hyprnotify.hpp"

#include <cmath>

namespace NHyprnotify::Parse {

    static size_t utf8Prefix(const std::string_view in, size_t maxBytes) {
        const size_t END = std::min(in.size(), maxBytes);
        if (END == in.size())
            return END;

        size_t safe = END;
        while (safe > 0 && ((unsigned char)in[safe] & 0xC0) == 0x80)
            safe--;
        return safe;
    }

    std::string clipUtf8(const std::string_view in, const size_t maxBytes) {
        return std::string{in.substr(0, utf8Prefix(in, maxBytes))};
    }

    std::string boundedOpaque(const std::string_view in, const size_t maxBytes) {
        return in.size() <= maxBytes ? std::string{in} : std::string{};
    }

    static size_t entityEnd(const std::string_view in, const size_t start) {
        constexpr size_t MAX_ENTITY_BYTES = 10;
        if (start >= in.size() || in[start] != '&')
            return std::string_view::npos;
        const size_t LIMIT = std::min(in.size(), start + MAX_ENTITY_BYTES + 1);
        for (size_t pos = start + 1; pos < LIMIT; pos++) {
            if (in[pos] == ';')
                return pos;
            if (!std::isalnum((unsigned char)in[pos]) && in[pos] != '#')
                break;
        }
        return std::string_view::npos;
    }

    static bool supportedEntity(const std::string_view entity) {
        return entity == "amp" || entity == "lt" || entity == "gt" || entity == "quot" || entity == "apos" || (entity.size() > 1 && entity.front() == '#');
    }

    // A nested '<' cannot belong to a valid tag. Stopping there means every
    // byte is inspected at most twice even for malformed '<...<...>' input.
    static size_t tagEnd(const std::string_view in, const size_t start) {
        const size_t LIMIT = std::min(in.size(), start + MAX_MARKUP_TAG_BYTES);
        for (size_t pos = start + 1; pos < LIMIT; pos++) {
            if (in[pos] == '>')
                return pos;
            if (in[pos] == '<')
                return std::string_view::npos;
        }
        return std::string_view::npos;
    }

    static void appendHref(std::string& out, const std::string_view href) {
        for (size_t pos = 0; pos < href.size();) {
            if (href[pos] == '&') {
                const auto END = entityEnd(href, pos);
                if (END != std::string_view::npos && supportedEntity(href.substr(pos + 1, END - pos - 1))) {
                    out.append(href.substr(pos, END - pos + 1));
                    pos = END + 1;
                    continue;
                }
                out += "&amp;";
            } else if (href[pos] == '"')
                out += "&quot;";
            else if (href[pos] == '\'')
                out += "&apos;";
            else if (href[pos] == '<')
                out += "&lt;";
            else if (href[pos] == '>')
                out += "&gt;";
            else
                out += href[pos];
            pos++;
        }
    }

    // We advertise body-markup, so only the notification specification's
    // small tag set crosses the bus boundary. In particular, do not forward
    // arbitrary Pango <span> attributes: they are a large, implementation-
    // specific layout surface and are not part of the fd.o contract.
    static std::optional<std::string> safeTag(const std::string_view raw, bool allowLinks) {
        if (raw.size() < 3 || raw.front() != '<' || raw.back() != '>')
            return std::nullopt;

        const auto isNameStart = [](unsigned char C) { return std::isalpha(C); };
        const auto isNameChar  = [](unsigned char C) { return std::isalpha(C) || std::isdigit(C) || C == '-' || C == '_'; };
        size_t     POS         = 1;
        const bool CLOSE       = POS < raw.size() && raw[POS] == '/';
        if (CLOSE)
            POS++;
        const size_t NAME_START = POS;
        if (POS >= raw.size() || !isNameStart((unsigned char)raw[POS]))
            return std::nullopt;
        while (POS < raw.size() && isNameChar((unsigned char)raw[POS]))
            POS++;
        std::string LOWER{raw.substr(NAME_START, POS - NAME_START)};
        std::ranges::transform(LOWER, LOWER.begin(), [](unsigned char C) { return (char)std::tolower(C); });

        const auto skipSpace = [&]() {
            while (POS < raw.size() && std::isspace((unsigned char)raw[POS]))
                POS++;
        };
        const auto canonical = [&](const char* TAG) -> std::optional<std::string> {
            skipSpace();
            if (POS + 1 != raw.size() || raw[POS] != '>')
                return std::nullopt;
            return std::string{"<"} + (CLOSE ? "/" : "") + TAG + ">";
        };

        if (LOWER == "b" || LOWER == "i" || LOWER == "u")
            return canonical(LOWER.c_str());
        if (CLOSE) {
            if (LOWER == "a" && allowLinks)
                return canonical("a");
            return std::nullopt;
        }
        if (LOWER != "a" || !allowLinks)
            return std::nullopt;

        // The only legal attribute is the quoted href on an opening <a>.
        // Rebuild the tag so duplicate or unknown attributes cannot reach
        // Pango even if its parser accepts them.
        bool        haveHref = false;
        std::string href;
        while (true) {
            skipSpace();
            if (POS + 1 == raw.size() && raw[POS] == '>')
                break;
            if (POS >= raw.size() || !isNameStart((unsigned char)raw[POS]))
                return std::nullopt;
            const size_t ATTR_START = POS++;
            while (POS < raw.size() && isNameChar((unsigned char)raw[POS]))
                POS++;
            std::string ATTR{raw.substr(ATTR_START, POS - ATTR_START)};
            std::ranges::transform(ATTR, ATTR.begin(), [](unsigned char C) { return (char)std::tolower(C); });
            skipSpace();
            if (ATTR != "href" || haveHref || POS >= raw.size() || raw[POS] != '=')
                return std::nullopt;
            POS++;
            skipSpace();
            if (POS >= raw.size() || (raw[POS] != '"' && raw[POS] != '\''))
                return std::nullopt;
            const char   QUOTE       = raw[POS++];
            const size_t VALUE_START = POS;
            while (POS < raw.size() && raw[POS] != QUOTE && raw[POS] != '<')
                POS++;
            if (POS >= raw.size() || raw[POS] != QUOTE)
                return std::nullopt;
            href     = std::string{raw.substr(VALUE_START, POS - VALUE_START)};
            haveHref = true;
            POS++;
        }
        if (!haveHref)
            return std::nullopt;
        std::string out{"<a href=\""};
        appendHref(out, href);
        return out + "\">";
    }

    std::string sanitizeMarkup(const std::string_view raw, bool allowLinks) {
        const std::string in = clipUtf8(raw, MAX_BODY_BYTES);
        std::string out;
        out.reserve(in.size() + 16);
        for (size_t i = 0; i < in.size();) {
            const char CH = in[i];
            if (CH == '<') {
                size_t j = i + 1;
                if (j < in.size() && in[j] == '/')
                    j++;
                const size_t NS = j;
                while (j < in.size() && std::isalpha((unsigned char)in[j]))
                    j++;
                if (j > NS) {
                    const auto END = tagEnd(in, i);
                    if (END != std::string::npos) {
                        std::string name = in.substr(NS, j - NS);
                        std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });
                        if (name == "br" && j == END)
                            out += '\n'; // a line break, whatever the card does with it
                        else if (const auto TAG = safeTag(std::string_view{in}.substr(i, END - i + 1), allowLinks))
                            out += *TAG;
                        // else: disallowed tag, dropped
                        i = END + 1;
                        continue;
                    }
                }
                out += "&lt;"; // a bare '<' that forms no tag: literal
                i++;
                continue;
            }
            if (CH == '&') {
                const auto END = entityEnd(in, i);
                if (END != std::string::npos) {
                    const auto E = std::string_view{in}.substr(i + 1, END - i - 1);
                    if (supportedEntity(E)) {
                        out.append(in, i, END - i + 1); // a real entity: Pango decodes it
                        i = END + 1;
                        continue;
                    }
                }
                out += "&amp;"; // a bare '&': literal
                i++;
                continue;
            }
            if (CH != '\r')
                out += CH;
            i++;
        }
        return out;
    }

    std::string oneLine(std::string s) {
        for (auto& c : s)
            if (c == '\n')
                c = ' ';
        return s;
    }

    // A path (file:// or absolute) is taken verbatim; anything else is a
    // freedesktop icon NAME resolved against the theme (dunst/mako do this,
    // and so should a compositor daemon). "" = nothing usable.
    std::string resolveImage(std::string s, int sizePx) {
        if (s.empty())
            return "";
        if (s.starts_with("file://"))
            s.erase(0, 7);
        if (s.starts_with('/'))
            return s;
        return NHyprCommon::resolveIconName(s, sizePx);
    }

    // <img src="..."> is not a Pango tag; pull it from the body before the
    // markup sanitizer would drop it, resolve each src (path or themed name),
    // and return the thumbnails — removing the tags from the text. http(s)
    // and data: srcs aren't fetched, so they're skipped.
    std::vector<SBodyImage> extractImages(std::string& body, int sizePx) {
        std::vector<SBodyImage> out;
        std::string              visible;
        body = clipUtf8(body, MAX_BODY_BYTES);
        visible.reserve(body.size());
        for (size_t i = 0; i < body.size();) {
            if (body[i] != '<') {
                visible += body[i];
                i++;
                continue;
            }
            const auto END = tagEnd(body, i);
            if (END == std::string::npos) {
                visible += body[i++];
                continue;
            }
            size_t j = i + 1;
            while (j < END && std::isalpha((unsigned char)body[j]))
                j++;
            std::string name = body.substr(i + 1, j - i - 1);
            std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });
            if (name != "img") {
                visible.append(body, i, END - i + 1);
                i = END + 1;
                continue;
            }
            const std::string_view TAG{body.data() + i, END - i + 1};
            const auto SRC = attrValue(TAG, "src");
            const auto ALT = clipUtf8(attrValue(TAG, "alt"), MAX_HINT_TEXT_BYTES);
            if (out.size() < MAX_BODY_IMAGES && SRC.size() <= MAX_SOURCE_BYTES && !SRC.empty() && !SRC.starts_with("http") && !SRC.starts_with("data:")) {
                if (const auto P = resolveImage(SRC, sizePx); !P.empty()) {
                    out.push_back(SBodyImage{.src = P, .alt = ALT});
                    i = END + 1; // rendered below the text
                    continue;
                }
            }
            // The spec requires alt text for an image. Keep it in the body
            // when the source is remote, malformed, or cannot be resolved.
            visible += ALT;
            i = END + 1;
        }
        body = std::move(visible);
        return out;
    }

    void unpackImageData(SNotif& n, const ImageData& d, int capPx) {
        const int32_t W = std::get<0>(d), H = std::get<1>(d), STRIDE = std::get<2>(d), BPS = std::get<4>(d), CH = std::get<5>(d);
        const auto&   DATA = std::get<6>(d);
        // STRIDE must cover a row: a lying stride (0) would let a tiny
        // message claim gigapixel W*H and the resize below map it all.
        // The 16 MP cap bounds a genuine ~128 MB image-data (the D-Bus
        // message max) that would otherwise map + premultiply in full.
        if (W <= 0 || H <= 0 || (int64_t)W * H > (16 << 20) || BPS != 8 || (CH != 3 && CH != 4) || (int64_t)STRIDE < (int64_t)W * CH || DATA.size() < (size_t)STRIDE * (H - 1) + (size_t)W * CH)
            return;
        const int CAP = std::max(capPx, 1);
        const double SCALE = std::min({1.0, (double)CAP / W, (double)CAP / H});
        const int OUTW = std::max(1, (int)std::lround(W * SCALE));
        const int OUTH = std::max(1, (int)std::lround(H * SCALE));

        n.pixels.resize((size_t)OUTW * OUTH * 4);

        // Keep the common, already-small path integer-only. Large hostile
        // pixmaps go through the bounded bilinear path below without ever
        // allocating a full-size intermediate image.
        if (OUTW == W && OUTH == H) {
            for (int32_t y = 0; y < H; y++) {
                const uint8_t* row = DATA.data() + (size_t)y * STRIDE;
                uint8_t*       out = n.pixels.data() + (size_t)y * W * 4;
                for (int32_t x = 0; x < W; x++) {
                    const uint8_t R = row[x * CH], G = row[x * CH + 1], B = row[x * CH + 2], A = CH == 4 ? row[x * CH + 3] : 255;
                    out[x * 4]     = (uint8_t)(B * A / 255);
                    out[x * 4 + 1] = (uint8_t)(G * A / 255);
                    out[x * 4 + 2] = (uint8_t)(R * A / 255);
                    out[x * 4 + 3] = A;
                }
            }
        } else {
            const auto sample = [&](int x, int y, int channel) {
                const auto* ROW = DATA.data() + (size_t)y * STRIDE;
                const uint8_t R = ROW[x * CH], G = ROW[x * CH + 1], B = ROW[x * CH + 2], A = CH == 4 ? ROW[x * CH + 3] : 255;
                if (channel == 3)
                    return (double)A;
                const uint8_t VALUE = channel == 0 ? B : channel == 1 ? G : R;
                return (double)VALUE * A / 255.0; // premultiplied BGRA
            };

            for (int y = 0; y < OUTH; y++) {
                const double SY = ((double)y + 0.5) * H / OUTH - 0.5;
                const int    Y0 = std::clamp((int)std::floor(SY), 0, H - 1);
                const int    Y1 = std::clamp(Y0 + 1, 0, H - 1);
                const double FY = std::clamp(SY - Y0, 0.0, 1.0);
                for (int x = 0; x < OUTW; x++) {
                    const double SX = ((double)x + 0.5) * W / OUTW - 0.5;
                    const int    X0 = std::clamp((int)std::floor(SX), 0, W - 1);
                    const int    X1 = std::clamp(X0 + 1, 0, W - 1);
                    const double FX = std::clamp(SX - X0, 0.0, 1.0);
                    uint8_t*     OUT = n.pixels.data() + ((size_t)y * OUTW + x) * 4;
                    for (int c = 0; c < 4; c++) {
                        const double TOP = sample(X0, Y0, c) * (1.0 - FX) + sample(X1, Y0, c) * FX;
                        const double BOT = sample(X0, Y1, c) * (1.0 - FX) + sample(X1, Y1, c) * FX;
                        OUT[c]           = (uint8_t)std::clamp(std::lround(TOP * (1.0 - FY) + BOT * FY), 0L, 255L);
                    }
                }
            }
        }
        n.pw        = OUTW;
        n.ph        = OUTH;
        n.hasPixels = true;
    }

} // namespace NHyprnotify::Parse
