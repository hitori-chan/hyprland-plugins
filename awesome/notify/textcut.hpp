// awesome/notify/textcut.hpp — the cuts a card's text takes (before pango,
// at its byte caps): pure and testable.
#pragma once

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

} // namespace NAwesome::Notify
