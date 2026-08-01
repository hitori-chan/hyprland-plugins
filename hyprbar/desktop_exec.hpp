#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace NHyprbar::DesktopExec {

    inline std::optional<char> unescapeChar(char value, bool list) {
        switch (value) {
            case 's': return ' ';
            case 'n': return '\n';
            case 't': return '\t';
            case 'r': return '\r';
            case '\\': return '\\';
            case ';': return list ? std::optional<char>{';'} : std::nullopt;
            default: return std::nullopt;
        }
    }

    // Desktop Entry string escaping applies to ordinary values such as Name
    // and Icon. Exec has its own quote-and-backslash grammar below, so callers
    // must pass its raw value to tokens()/expand().
    inline std::optional<std::string> unescapeString(std::string_view value) {
        std::string out;
        out.reserve(value.size());
        for (size_t i = 0; i < value.size(); i++) {
            if (value[i] != '\\') {
                out += value[i];
                continue;
            }
            if (++i >= value.size())
                return std::nullopt;
            const auto unescaped = unescapeChar(value[i], false);
            if (!unescaped)
                return std::nullopt;
            out += *unescaped;
        }
        return out;
    }

    // String-list values split on literal semicolons before decoding. An
    // escaped semicolon belongs to its item rather than starting another one.
    inline std::optional<std::vector<std::string>> unescapeList(std::string_view value) {
        std::vector<std::string> out;
        std::string              item;
        item.reserve(value.size());
        for (size_t i = 0; i < value.size(); i++) {
            if (value[i] == ';') {
                if (!item.empty())
                    out.push_back(std::move(item));
                item.clear();
                continue;
            }
            if (value[i] != '\\') {
                item += value[i];
                continue;
            }
            if (++i >= value.size())
                return std::nullopt;
            const auto unescaped = unescapeChar(value[i], true);
            if (!unescaped)
                return std::nullopt;
            item += *unescaped;
        }
        if (!item.empty())
            out.push_back(std::move(item));
        return out;
    }

    struct SToken {
        std::string       value;
        std::vector<bool> quoted;
        std::vector<bool> escaped;
        bool              present = false;
    };

    // Exec= uses desktop-entry quoting, not shell quoting. Retain quote and
    // escape provenance so field-code validation can reject codes inside a
    // quoted argument without changing the final argument values.
    inline std::optional<std::vector<SToken>> tokens(const std::string& exec) {
        std::vector<SToken> out;
        SToken              word;
        bool                quoted = false;

        const auto append = [&](char value, bool isQuoted, bool isEscaped) {
            word.value.push_back(value);
            word.quoted.push_back(isQuoted);
            word.escaped.push_back(isEscaped);
            word.present = true;
        };

        for (size_t i = 0; i < exec.size(); i++) {
            const unsigned char C = (unsigned char)exec[i];
            if (exec[i] == '\\') {
                if (++i >= exec.size())
                    return std::nullopt;
                if (quoted && exec[i] != '"' && exec[i] != '`' && exec[i] != '$' && exec[i] != '\\')
                    return std::nullopt;
                append(exec[i], quoted, true);
                continue;
            }
            if (exec[i] == '"') {
                quoted        = !quoted;
                word.present  = true;
                continue;
            }
            if (!quoted && std::isspace(C)) {
                if (word.present) {
                    out.push_back(std::move(word));
                    word = {};
                }
                continue;
            }
            append(exec[i], quoted, false);
        }
        if (quoted)
            return std::nullopt;
        if (word.present)
            out.push_back(std::move(word));
        return out;
    }

    inline std::optional<std::vector<std::string>> words(const std::string& exec) {
        const auto parsed = tokens(exec);
        if (!parsed)
            return std::nullopt;
        std::vector<std::string> out;
        out.reserve(parsed->size());
        for (const auto& T : *parsed)
            out.push_back(T.value);
        return out;
    }

    inline bool isNoInputField(const std::string& word) {
        return word == "%f" || word == "%F" || word == "%u" || word == "%U" || word == "%d" || word == "%D" || word == "%n" || word == "%N" || word == "%v" || word == "%m";
    }

    inline bool isKnownField(char code) {
        return code == '%' || code == 'f' || code == 'F' || code == 'u' || code == 'U' || code == 'i' || code == 'c' || code == 'k' || code == 'd' || code == 'D' || code == 'n' || code == 'N' || code == 'v' || code == 'm';
    }

    inline bool isInputField(char code) {
        return code == 'f' || code == 'F' || code == 'u' || code == 'U';
    }

    inline std::optional<std::vector<std::string>> expand(const std::string& exec, const std::string& name, const std::string& file, const std::string& icon) {
        const auto WORDS = tokens(exec);
        if (!WORDS || WORDS->empty())
            return std::nullopt;

        size_t inputFields = 0;
        for (size_t wi = 0; wi < WORDS->size(); wi++) {
            const auto& T = (*WORDS)[wi];
            for (size_t i = 0; i < T.value.size(); i++) {
                if (T.value[i] != '%' || T.escaped[i])
                    continue;
                if (++i >= T.value.size())
                    return std::nullopt;
                const char CODE = T.value[i];
                if (!isKnownField(CODE))
                    return std::nullopt;
                if (CODE != '%' && (T.quoted[i - 1] || T.quoted[i]))
                    return std::nullopt;
                if (isInputField(CODE) && ++inputFields > 1)
                    return std::nullopt;
                if ((CODE == 'F' || CODE == 'U' || CODE == 'i') && T.value.size() != 2)
                    return std::nullopt;
                if (CODE == 'i' && wi == 0)
                    return std::nullopt; // the executable itself cannot be %i
            }
            // The executable slot cannot be a file, URL, icon, or deprecated
            // field code: those expand to no executable in this launcher.
            if (wi == 0 && T.value.find('%') != std::string::npos && T.value != "%%") {
                bool hasField = false;
                for (size_t i = 0; i < T.value.size(); i++)
                    if (T.value[i] == '%' && !T.escaped[i] && i + 1 < T.value.size() && T.value[i + 1] != '%')
                        hasField = true;
                if (hasField)
                    return std::nullopt;
            }
        }

        std::vector<std::string> out;
        for (const auto& T : *WORDS) {
            if (T.value == "%i" && !T.escaped[0] && !T.escaped[1]) {
                if (!icon.empty()) {
                    out.emplace_back("--icon");
                    out.push_back(icon);
                }
                continue;
            }

            std::string value;
            for (size_t i = 0; i < T.value.size(); i++) {
                if (T.value[i] != '%' || T.escaped[i]) {
                    value += T.value[i];
                    continue;
                }
                if (++i >= T.value.size())
                    return std::nullopt;
                switch (T.value[i]) {
                    case '%': value += '%'; break;
                    case 'c': value += name; break;
                    case 'k': value += file; break;
                    // This launcher supplies no files or URLs. Deprecated
                    // field codes likewise have no value at launch time.
                    case 'f':
                    case 'F':
                    case 'u':
                    case 'U':
                    case 'd':
                    case 'D':
                    case 'n':
                    case 'N':
                    case 'v':
                    case 'm': break;
                    case 'i': return std::nullopt; // %i must be a whole word
                    default: return std::nullopt;
                }
            }
            const bool RAW_NO_INPUT = isNoInputField(T.value) && std::ranges::none_of(T.escaped, [](bool escaped) { return escaped; });
            if (value.empty() && RAW_NO_INPUT)
                continue;
            out.push_back(std::move(value));
        }
        if (out.empty() || out.front().empty())
            return std::nullopt;
        return out;
    }

    inline std::string shellQuote(const std::string& value) {
        std::string out = "'";
        for (const char C : value) {
            if (C == '\'')
                out += "'\\''";
            else
                out += C;
        }
        out += '\'';
        return out;
    }

    inline std::string shellCommand(const std::vector<std::string>& words) {
        std::string out;
        for (const auto& W : words) {
            if (!out.empty())
                out += ' ';
            out += shellQuote(W);
        }
        return out;
    }

} // namespace NHyprbar::DesktopExec
