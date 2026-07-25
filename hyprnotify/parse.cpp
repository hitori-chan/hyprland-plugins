// hyprnotify/parse.cpp — the incoming payload, turned into things a card can
// hold. Everything here is a pure transform of what an arbitrary D-Bus peer
// sent us: markup we must not trust, an image buffer whose own header may
// lie, a path that may be an icon name instead. It is deliberately separate
// from the model (which decides what a card DOES) and from the bus (which
// only carries the bytes) — this is the hostile-input surface, and it is the
// one worth reading on its own.

#include "common/icons.hpp"

#include "hyprnotify.hpp"

namespace NHyprnotify::Parse {

    // We advertise body-markup, so the whitelisted Pango tags pass through
    // live; everything else is neutralized into content. Two client dialects
    // must both come out right: a markup-aware client escapes its reserved
    // chars ("a &amp;amp; b"), a naive one sends them raw ("a & b") — a bare
    // '&'/'<' that forms no entity/tag is escaped so Pango renders it
    // verbatim either way. Disallowed tags are dropped (spec: "filter them
    // out"); allowLinks adds <a> (hyperlinks phase). <img> never reaches
    // here — it is extracted before sanitizing (body-images phase).
    static bool allowedTag(const std::string& name, bool allowLinks) {
        return name == "b" || name == "i" || name == "u" || name == "span" || name == "br" || (allowLinks && name == "a");
    }

    std::string sanitizeMarkup(const std::string& in, bool allowLinks) {
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
                    const auto END = in.find('>', j);
                    if (END != std::string::npos) {
                        std::string name = in.substr(NS, j - NS);
                        std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });
                        if (name == "br")
                            out += '\n'; // a line break, whatever the card does with it
                        else if (allowedTag(name, allowLinks))
                            out += in.substr(i, END - i + 1); // live tag, verbatim (Pango validates attrs)
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
                const auto END = in.find(';', i);
                if (END != std::string::npos && END - i <= 10) {
                    const auto E = in.substr(i + 1, END - i - 1);
                    if (E == "amp" || E == "lt" || E == "gt" || E == "quot" || E == "apos" || (E.size() > 1 && E[0] == '#')) {
                        out += in.substr(i, END - i + 1); // a real entity: Pango decodes it
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
    std::vector<std::string> extractImages(std::string& body, int sizePx) {
        std::vector<std::string> out;
        for (size_t i = 0; i < body.size();) {
            if (body[i] != '<') {
                i++;
                continue;
            }
            size_t j = i + 1;
            while (j < body.size() && std::isalpha((unsigned char)body[j]))
                j++;
            std::string name = body.substr(i + 1, j - i - 1);
            std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });
            if (name != "img") {
                i++;
                continue;
            }
            const auto END = body.find('>', j);
            if (END == std::string::npos)
                break;
            const auto  TAG = body.substr(i, END - i + 1);
            std::string tl  = TAG; // case-insensitive attr search, same offsets as TAG
            std::ranges::transform(tl, tl.begin(), [](unsigned char c) { return std::tolower(c); });
            std::string src;
            if (const auto SP = tl.find("src"); SP != std::string::npos)
                if (const auto Q = TAG.find_first_of("\"'", SP); Q != std::string::npos)
                    if (const auto Q2 = TAG.find(TAG[Q], Q + 1); Q2 != std::string::npos)
                        src = TAG.substr(Q + 1, Q2 - Q - 1);
            if (!src.empty() && !src.starts_with("http") && !src.starts_with("data:"))
                if (const auto P = resolveImage(src, sizePx); !P.empty())
                    out.push_back(P);
            body.erase(i, END - i + 1); // drop the tag from the text
        }
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
        n.pixels.resize((size_t)W * H * 4);
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
        n.pw        = W;
        n.ph        = H;
        n.hasPixels = true;
        // keep only what a card can ever paint: warm frees visible cards'
        // buffers after upload, but an off-screen card would hold its
        // full-size pixmap until it scrolls on. The caller's cap covers the
        // hero layout at any monitor scale; warm still scales exactly.
        shrinkPixels(n, capPx);
    }

    // Join an appended conversation body under the cap: newest lines
    // append at the back, oldest lines drop off the front whole.
    std::string joinAppend(const std::string& oldBody, const std::string& add) {
        std::string joined = oldBody.empty() ? add : oldBody + "\n" + add;
        constexpr size_t CAP = 8192;
        while (joined.size() > CAP) {
            const auto NL = joined.find('\n');
            if (NL == std::string::npos) {
                joined.erase(0, joined.size() - CAP);
                break;
            }
            joined.erase(0, NL + 1);
        }
        return joined;
    }

} // namespace NHyprnotify::Parse
