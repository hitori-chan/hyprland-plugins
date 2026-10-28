// awesome/notify/textcut.hpp — the cuts a card's text takes (before pango,
// at its byte caps): pure and testable.
#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace NAwesome::Notify {

    // The text a layout of `lines` lines can show, with room to spare: pango
    // lays out (and shapes) everything it is given, and an end-ellipsized
    // last line takes the whole rest of its paragraph, so an 8 KiB body cost
    // its full length in every raster that showed four lines of it. The cut
    // falls after maxChars visible characters or maxBreaks + 1 line breaks;
    // the caller passes twice what its lines hold, so the overflow pango
    // ellipsizes is still there. Markup is cut between tags and entities,
    // and the tags still open are closed. Empty: nothing to cut.
    inline std::string cutToLines(const std::string& s, bool markup, size_t maxChars, size_t maxBreaks) {
        if (s.size() <= maxChars)
            return {};
        std::vector<std::string_view> open;
        size_t                        chars = 0, breaks = 0, i = 0;
        while (i < s.size()) {
            if (markup && s[i] == '<') {
                const auto END = s.find('>', i);
                if (END == std::string::npos)
                    return {}; // malformed: the parse fallback handles it as before
                const std::string_view TAG(s.data() + i + 1, END - i - 1);
                if (!TAG.empty() && TAG.front() == '/') {
                    if (!open.empty())
                        open.pop_back();
                } else if (!TAG.empty() && TAG.back() != '/')
                    open.push_back(TAG.substr(0, TAG.find_first_of(" \t\n")));
                i = END + 1;
                continue;
            }
            if (chars >= maxChars || breaks > maxBreaks)
                break;
            chars++;
            if (s[i] == '\n')
                breaks++;
            if (markup && s[i] == '&') {
                if (const auto SEMI = s.find(';', i); SEMI != std::string::npos && SEMI - i <= 12) {
                    i = SEMI + 1;
                    continue;
                }
            }
            for (i++; i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80; i++) {}
        }
        if (i >= s.size())
            return {};
        std::string out = s.substr(0, i);
        for (auto it = open.rbegin(); it != open.rend(); ++it) {
            out += "</";
            out += *it;
            out += '>';
        }
        return out;
    }

    namespace TextCut {
        struct STag {
            std::string_view name, text; // the element name, the whole opening tag
        };
        struct SOpen {
            std::vector<STag> tags;
            size_t            closers = 0; // the bytes their closing tags take
        };

        // One token of sanitized markup from i: a whole tag (the open-tag
        // stack follows it), a whole entity or one codepoint. False: a tag
        // that never closes.
        inline bool step(const std::string& s, size_t& i, SOpen& open) {
            if (s[i] == '<') {
                const auto END = s.find('>', i);
                if (END == std::string::npos)
                    return false;
                const std::string_view TAG(s.data() + i + 1, END - i - 1);
                if (!TAG.empty() && TAG.front() == '/') {
                    if (!open.tags.empty()) {
                        open.closers -= open.tags.back().name.size() + 3;
                        open.tags.pop_back();
                    }
                } else if (!TAG.empty() && TAG.back() != '/') {
                    const auto NAME = TAG.substr(0, TAG.find_first_of(" \t\n"));
                    open.tags.push_back({NAME, std::string_view(s.data() + i, END + 1 - i)});
                    open.closers += NAME.size() + 3;
                }
                i = END + 1;
                return true;
            }
            if (s[i] == '&')
                if (const auto SEMI = std::string_view(s).substr(i, 13).find(';'); SEMI != std::string_view::npos) {
                    i += SEMI + 1;
                    return true;
                }
            for (i++; i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80; i++) {}
            return true;
        }
    }

    // Sanitized markup within maxBytes, its head kept. A byte cut could land
    // inside a tag or an entity: the parse failed and the plain fallback
    // leaked the tag's tail (`oreground="#fff">`). The cut falls between
    // tokens and the tags still open are closed, within the cap.
    inline std::string capMarkupHead(const std::string& s, size_t maxBytes) {
        if (s.size() <= maxBytes)
            return s;
        TextCut::SOpen open;
        size_t         i = 0, best = 0;
        while (i < s.size() && i <= maxBytes) {
            if (i + open.closers <= maxBytes)
                best = i;
            if (!TextCut::step(s, i, open))
                break;
        }
        // the tags open at the cut: a second pass up to it (a copy of the
        // stack at every candidate was quadratic in a deep nest)
        TextCut::SOpen atBest;
        for (size_t j = 0; j < best;)
            TextCut::step(s, j, atBest);
        std::string out = s.substr(0, best);
        for (auto it = atBest.tags.rbegin(); it != atBest.tags.rend(); ++it) {
            out += "</";
            out += it->name;
            out += '>';
        }
        return out;
    }

    // Sanitized markup from the first token boundary at or past `from`, its
    // tail kept: the tags open at the cut are reopened in front, so their
    // closers still match.
    inline std::string capMarkupTail(const std::string& s, size_t from) {
        if (from == 0)
            return s;
        TextCut::SOpen open;
        size_t         i = 0;
        while (i < s.size() && i < from)
            if (!TextCut::step(s, i, open)) {
                // malformed: the codepoint-aligned byte cut, as before
                for (i = from; i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80;)
                    i++;
                return s.substr(std::min(i, s.size()));
            }
        std::string out;
        for (const auto& T : open.tags)
            out += T.text;
        out.append(s, std::min(i, s.size()));
        return out;
    }

    // A name too wide for its room, cut in the middle between whole words,
    // its end kept first: the end tells outputs apart ("…Speaker +
    // Headphones" from "…HDMI / DisplayPort 1 Output"). The first word, " …
    // ", the last; then words grow from the end and the start while they
    // fit, and a bare "/" or "+" never borders the cut. A name whose first
    // and last words alone overflow is cut by characters (55 % head, 45 %
    // tail). fits(text): whether the text fits the room.
    template <typename F>
    std::string midCut(const std::string& name, F&& fits) {
        if (fits(name))
            return name;
        std::vector<std::string_view> w;
        for (size_t i = 0; i < name.size();) {
            const size_t END = std::min(name.find(' ', i), name.size());
            if (END > i)
                w.emplace_back(name.data() + i, END - i);
            i = END + 1;
        }
        const size_t N    = w.size();
        const auto   join = [&](size_t h, size_t t) {
            std::string s;
            for (size_t i = 0; i < h; i++)
                s.append(i ? " " : "").append(w[i]);
            s.append(" \u2026 ");
            for (size_t i = N - t; i < N; i++)
                s.append(i > N - t ? " " : "").append(w[i]);
            return s;
        };
        // no letter or digit: a UTF-8 letter counts as one
        const auto bare = [](std::string_view x) { return std::ranges::none_of(x, [](unsigned char c) { return c >= 0x80 || std::isalnum(c); }); };
        if (N > 2 && fits(join(1, 1))) {
            size_t h = 1, t = 1;
            for (bool grew = true; grew;) {
                grew = false;
                if (h + t < N && fits(join(h, t + 1)))
                    t++, grew = true;
                if (h + t < N && fits(join(h + 1, t)))
                    h++, grew = true;
            }
            while (t > 1 && bare(w[N - t]))
                t--;
            while (h > 1 && bare(w[h - 1]))
                h--;
            while (h + t < N && !bare(w[h]) && fits(join(h + 1, t)))
                h++;
            return join(h, t);
        }
        std::vector<size_t> cp; // each code point's byte offset
        for (size_t i = 0; i < name.size(); i++)
            if (((unsigned char)name[i] & 0xC0) != 0x80)
                cp.push_back(i);
        const auto at = [&](size_t k) { return k < cp.size() ? cp[k] : name.size(); };
        for (size_t k = cp.size(); k-- > 2;) {
            std::string_view head(name.data(), at((k * 55 + 99) / 100)), tail(name.data() + at(cp.size() - k * 45 / 100), name.size() - at(cp.size() - k * 45 / 100));
            while (!head.empty() && head.back() == ' ')
                head.remove_suffix(1);
            while (!tail.empty() && tail.front() == ' ')
                tail.remove_prefix(1);
            if (const auto CUT = std::string{head}.append("\u2026").append(tail); fits(CUT))
                return CUT;
        }
        return name.substr(0, at(1)) + "\u2026";
    }

} // namespace NAwesome::Notify
