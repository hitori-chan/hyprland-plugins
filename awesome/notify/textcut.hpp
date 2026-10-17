// awesome/notify/textcut.hpp — the cut a card's text takes before pango
// lays it out: pure and testable.
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

} // namespace NAwesome::Notify
