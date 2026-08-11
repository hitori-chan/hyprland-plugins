// hyprnotify/markup_attr.hpp — quoted attribute parsing for the markup subset
#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace NHyprnotify::Parse {

    // Attribute names are tokens, not substrings: src must not match data-src
    // or srcset. Only quoted values are accepted by the markup subset.
    inline std::string attrValue(std::string_view tag, std::string_view attr) {
        if (tag.empty() || tag.front() != '<')
            return "";

        const auto isNameStart = [](unsigned char c) { return std::isalpha(c) || c == '_' || c == ':'; };
        const auto isNameChar  = [&](unsigned char c) { return isNameStart(c) || std::isdigit(c) || c == '-' || c == '.'; };
        const auto equalName   = [](std::string_view lhs, std::string_view rhs) {
            if (lhs.size() != rhs.size())
                return false;
            for (size_t i = 0; i < lhs.size(); i++)
                if (std::tolower((unsigned char)lhs[i]) != std::tolower((unsigned char)rhs[i]))
                    return false;
            return true;
        };
        const auto skipSpace = [&](size_t& pos) {
            while (pos < tag.size() && std::isspace((unsigned char)tag[pos]))
                pos++;
        };

        size_t pos = 1;
        skipSpace(pos);
        if (pos < tag.size() && tag[pos] == '/')
            pos++;
        skipSpace(pos);
        while (pos < tag.size() && isNameChar((unsigned char)tag[pos]))
            pos++; // element name

        while (pos < tag.size()) {
            skipSpace(pos);
            if (pos >= tag.size() || tag[pos] == '>' || tag[pos] == '/')
                return "";
            if (!isNameStart((unsigned char)tag[pos])) {
                pos++;
                continue;
            }

            const size_t NAME_START = pos++;
            while (pos < tag.size() && isNameChar((unsigned char)tag[pos]))
                pos++;
            const std::string_view NAME{tag.data() + NAME_START, pos - NAME_START};
            skipSpace(pos);
            if (pos >= tag.size() || tag[pos] != '=') {
                if (equalName(NAME, attr))
                    return ""; // a valueless attribute has no quoted value
                continue;
            }
            pos++;
            skipSpace(pos);
            if (pos >= tag.size())
                return "";

            if (tag[pos] != '\'' && tag[pos] != '"') {
                while (pos < tag.size() && !std::isspace((unsigned char)tag[pos]) && tag[pos] != '>')
                    pos++;
                if (equalName(NAME, attr))
                    return ""; // only quoted values are accepted
                continue;
            }

            const char QUOTE = tag[pos++];
            const size_t VALUE_START = pos;
            const size_t VALUE_END   = tag.find(QUOTE, VALUE_START);
            if (VALUE_END == std::string::npos || tag.find('>', VALUE_START) < VALUE_END)
                return "";
            if (equalName(NAME, attr))
                return std::string{tag.substr(VALUE_START, VALUE_END - VALUE_START)};
            pos = VALUE_END + 1;
        }
        return "";
    }

} // namespace NHyprnotify::Parse
