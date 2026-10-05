// awesome/notify/parse.cpp — the incoming payload, turned into things a card
// can hold. Everything here is a pure transform of what an arbitrary D-Bus
// peer sent us: markup we must not trust, an image buffer whose own header
// may lie, a path that may be an icon name instead. It is deliberately
// separate from the model (which decides what a card DOES) and from the bus
// (which only carries the bytes) — this is the hostile-input surface, and
// it is the one worth reading on its own.

#include "model.hpp"

#include "../core/icons.hpp"

namespace NAwesome::Notify::Parse {

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

    // Linear in the input: a stranger's text full of '<x' or '&' that never
    // close must not rescan to the end for each one (a 130 KB summary of
    // '&' was ~0.5 s on the compositor thread per call). A tag can only end
    // at or before the LAST '>', and an entity is at most 10 bytes.
    std::string sanitizeMarkup(const std::string& in, bool allowLinks) {
        std::string out;
        out.reserve(in.size() + 16);
        const size_t LASTGT = in.rfind('>');
        for (size_t i = 0; i < in.size();) {
            const char CH = in[i];
            if (CH == '<') {
                size_t j = i + 1;
                if (j < in.size() && in[j] == '/')
                    j++;
                const size_t NS = j;
                while (j < in.size() && std::isalpha((unsigned char)in[j]))
                    j++;
                if (j > NS && LASTGT != std::string::npos && LASTGT >= j) {
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
                const auto SEMI = std::string_view{in}.substr(i, 11).find(';');
                const auto END  = SEMI == std::string_view::npos ? std::string::npos : i + SEMI;
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

    // One quoted attribute out of one tag — the fiddly part of reading a tag
    // a stranger wrote, so both readers (the <img> src here, the <a> href in
    // text.cpp) share it rather than each getting it nearly right. The scan
    // tracks the quote that opened the current value, and the NAME must stand
    // alone OUTSIDE any value (tag start or whitespace before it) followed by
    // '=' — a hit inside a quoted value (title="..src=..") is not the
    // attribute. Names match case-insensitively against a lowered COPY, whose
    // offsets still line up with the original the value is cut from. "" if
    // absent.
    std::string attrValue(const std::string& tag, const std::string& attr) {
        std::string lower = tag;
        std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return std::tolower(c); });
        char inQuote = 0;
        for (size_t i = 0; i < tag.size(); i++) {
            const auto CH = tag[i];
            if (inQuote) {
                if (CH == inQuote)
                    inQuote = 0;
                continue;
            }
            if (CH == '"' || CH == '\'') {
                inQuote = CH;
                continue;
            }
            if (lower.compare(i, attr.size(), attr) != 0)
                continue;
            if (i > 0 && lower[i - 1] != ' ' && lower[i - 1] != '\t')
                continue;
            size_t P = i + attr.size();
            while (P < lower.size() && (lower[P] == ' ' || lower[P] == '\t'))
                P++;
            if (P >= lower.size() || lower[P] != '=')
                continue;
            P++;
            while (P < lower.size() && (lower[P] == ' ' || lower[P] == '\t'))
                P++;
            if (P >= lower.size() || (lower[P] != '"' && lower[P] != '\''))
                continue;
            const char Q  = tag[P];
            const auto END = tag.find(Q, P + 1);
            return END == std::string::npos ? "" : tag.substr(P + 1, END - P - 1);
        }
        return "";
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
        return NAwesome::resolveIconName(s, sizePx);
    }

    // <img src="..."> is not a Pango tag; pull it from the body before the
    // markup sanitizer would drop it, resolve each src (path or themed
    // name), and return the thumbnails — removing the tags from the text.
    // http(s) and data: srcs aren't fetched, so they're skipped.
    // an <img> alt attribute is untrusted too: cut codepoint-safe before it
    // may become a fallback line
    static std::string clipAttr(std::string s) {
        constexpr size_t CAP = 512;
        if (s.size() <= CAP)
            return s;
        s.resize(CAP);
        while (!s.empty() && ((unsigned char)s.back() & 0xc0) == 0x80)
            s.pop_back();
        if (!s.empty() && (unsigned char)s.back() >= 0xc2)
            s.pop_back(); // a lead byte whose followers the cut took
        return s;
    }

    // a hostile sender's <img> src is a path: past this it is skipped whole
    // (the defined behavior at the bound, as MAX_BODY_IMAGES in model)
    constexpr size_t MAX_SRC_BYTES = 1024;

    // One linear pass: every <img> tag leaves the text, but only the first
    // `maxImages` with a usable src are resolved — a resolve can be a theme
    // scan, and a body of 2000 distinct tags once ran 2000 of them on the
    // compositor thread before the model's cap applied.
    std::vector<SImgRef> extractImages(std::string& body, int sizePx, size_t maxImages) {
        std::vector<SImgRef> out;
        std::string          kept;
        kept.reserve(body.size());
        const size_t LASTGT = body.rfind('>');
        size_t       i      = 0;
        while (i < body.size()) {
            const auto LT = body.find('<', i);
            if (LT == std::string::npos)
                break;
            size_t j = LT + 1;
            while (j < body.size() && j - LT <= 4 && std::isalpha((unsigned char)body[j]))
                j++;
            const bool IMG = j - LT - 1 == 3 && std::tolower((unsigned char)body[LT + 1]) == 'i' &&
                std::tolower((unsigned char)body[LT + 2]) == 'm' && std::tolower((unsigned char)body[LT + 3]) == 'g' &&
                (j >= body.size() || !std::isalpha((unsigned char)body[j]));
            if (!IMG || LASTGT == std::string::npos || LASTGT < j) {
                kept.append(body, i, LT + 1 - i);
                i = LT + 1;
                continue;
            }
            const auto END = body.find('>', j);
            kept.append(body, i, LT - i); // the text before the tag
            i = END + 1;                  // the tag itself is dropped
            if (out.size() >= maxImages)
                continue;
            const std::string_view TAG{body.data() + LT, END - LT + 1};
            const auto             SRC = attrValue(std::string{TAG}, "src");
            if (!SRC.empty() && SRC.size() <= MAX_SRC_BYTES && !SRC.starts_with("http") && !SRC.starts_with("data:"))
                if (const auto P = resolveImage(SRC, sizePx); !P.empty())
                    out.push_back(SImgRef{.src = P, .alt = oneLine(sanitizeMarkup(clipAttr(attrValue(std::string{TAG}, "alt"))))});
        }
        kept.append(body, i, std::string::npos);
        body = std::move(kept);
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
        // Box-decimate by an integer factor K while unpacking: a card paints
        // at most capPx, so a screenshot notification (up to 16 MP) never
        // allocates or premultiplies at full size inside the D-Bus handler.
        // K keeps >= 2x the cap; shrinkPixels does the exact final scale.
        const int32_t K  = std::max<int32_t>(1, std::max(W, H) / std::max(1, capPx * 2));
        const int32_t OW = std::max<int32_t>(1, W / K), OH = std::max<int32_t>(1, H / K);
        n.pixels.resize((size_t)OW * OH * 4);
        for (int32_t oy = 0; oy < OH; oy++) {
            uint8_t* out = n.pixels.data() + (size_t)oy * OW * 4;
            for (int32_t ox = 0; ox < OW; ox++) {
                uint64_t sb = 0, sg = 0, sr = 0, sa = 0; // premultiplied sums
                for (int32_t y = oy * K; y < std::min(H, (oy + 1) * K); y++) {
                    const uint8_t* row = DATA.data() + (size_t)y * STRIDE;
                    for (int32_t x = ox * K; x < std::min(W, (ox + 1) * K); x++) {
                        const uint32_t A = CH == 4 ? row[x * CH + 3] : 255;
                        sr += row[x * CH] * A;
                        sg += row[x * CH + 1] * A;
                        sb += row[x * CH + 2] * A;
                        sa += A;
                    }
                }
                const uint64_t N = (uint64_t)(std::min(H, (oy + 1) * K) - oy * K) * (std::min(W, (ox + 1) * K) - ox * K);
                out[ox * 4]     = (uint8_t)(sb / (255 * N));
                out[ox * 4 + 1] = (uint8_t)(sg / (255 * N));
                out[ox * 4 + 2] = (uint8_t)(sr / (255 * N));
                out[ox * 4 + 3] = (uint8_t)(sa / N);
            }
        }
        n.pw        = OW;
        n.ph        = OH;
        n.hasPixels = true;
        // keep only what a card can ever paint: warm frees visible cards'
        // buffers after upload, but an off-screen card would hold its
        // full-size pixmap until it scrolls on. The caller's cap covers the
        // hero layout at any monitor scale; warm still scales exactly.
        shrinkPixels(n, capPx);
    }

    // XDG conversation clients (Telegram Desktop's) embed the sender as a
    // LEADING bold line: "<b>Alice</b>\nmessage". Fold the block to the one
    // line "<b>Alice</b>: message" — the body stays one line per message,
    // so the newest message (and its sender) stays first-line reachable:
    // the collapsed row and the banner window read lines, and a bare-name
    // first line would hide the message under it
    std::string foldSenderPrefix(std::string body) {
        if (!body.starts_with("<b>"))
            return body;
        const auto CLOSE = body.find("</b>");
        if (CLOSE == std::string::npos)
            return body;
        const auto NL = body.find('\n', CLOSE);
        if (NL == std::string::npos)
            return body; // no message line after the sender: leave as-is
        std::string out;
        out.reserve(body.size());
        out.append(body, 0, CLOSE + 4); // through the '>' of "</b>"
        out += ": ";
        out += body.substr(NL + 1);
        return out;
    }

    // chronological, like the transcript: the visible lines are the LATEST
    // messages, the newest ends the body, and the cap drops the oldest (top)
    // lines
    std::string joinAppend(const std::string& oldBody, const std::string& add) {
        std::string joined = oldBody.empty() ? add : oldBody + "\n" + add;
        constexpr size_t CAP = 8192;
        while (joined.size() > CAP) {
            const auto NL = joined.find('\n');
            if (NL == std::string::npos) {
                joined.erase(0, joined.size() - CAP); // keep the newest tail
                break;
            }
            joined.erase(0, NL + 1); // the oldest line plus its separator
        }
        return joined;
    }

} // namespace NAwesome::Notify::Parse
