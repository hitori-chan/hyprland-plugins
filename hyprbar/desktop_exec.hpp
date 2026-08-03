#pragma once

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace NHyprbar::DesktopExec {

    // Localized desktop entries commonly exceed 16 KiB (Firefox and Thunar
    // do), while the asynchronous handoff still needs a firm memory bound.
    inline constexpr size_t MAX_DESKTOP_FILE_BYTES = 128 * 1024;

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

    enum class EFieldKind {
        LITERAL,
        SCALAR,
        SINGLE_INPUT,
        LIST_INPUT,
        ICON,
        DEPRECATED_EMPTY,
    };

    enum class EScalarSource {
        NONE,
        NAME,
        DESKTOP_FILE,
    };

    struct SFieldSemantics {
        EFieldKind    kind;
        EScalarSource scalarSource;
    };

    inline constexpr std::optional<SFieldSemantics> classifyField(char code) {
        switch (code) {
            case '%': return SFieldSemantics{EFieldKind::LITERAL, EScalarSource::NONE};
            case 'c': return SFieldSemantics{EFieldKind::SCALAR, EScalarSource::NAME};
            case 'k': return SFieldSemantics{EFieldKind::SCALAR, EScalarSource::DESKTOP_FILE};
            case 'f':
            case 'u': return SFieldSemantics{EFieldKind::SINGLE_INPUT, EScalarSource::NONE};
            case 'F':
            case 'U': return SFieldSemantics{EFieldKind::LIST_INPUT, EScalarSource::NONE};
            case 'i': return SFieldSemantics{EFieldKind::ICON, EScalarSource::NONE};
            case 'd':
            case 'D':
            case 'n':
            case 'N':
            case 'v':
            case 'm': return SFieldSemantics{EFieldKind::DEPRECATED_EMPTY, EScalarSource::NONE};
            default: return std::nullopt;
        }
    }

    inline std::optional<std::vector<std::string>> expand(const std::string& exec, const std::string& name, const std::string& file, const std::string& icon) {
        const auto WORDS = tokens(exec);
        if (!WORDS || WORDS->empty())
            return std::nullopt;

        std::vector<std::string> out;
        out.reserve(WORDS->size() + 1);
        size_t inputFields = 0;
        for (size_t wi = 0; wi < WORDS->size(); wi++) {
            const auto& T = (*WORDS)[wi];
            std::string value;
            value.reserve(T.value.size());
            bool omitArgument = false;

            for (size_t i = 0; i < T.value.size(); i++) {
                if (T.value[i] != '%' || T.escaped[i]) {
                    value += T.value[i];
                    continue;
                }

                const size_t FIELD_START = i;
                if (++i >= T.value.size())
                    return std::nullopt;

                const auto FIELD = classifyField(T.value[i]);
                if (!FIELD)
                    return std::nullopt;
                if (FIELD->kind != EFieldKind::LITERAL && (T.quoted[FIELD_START] || T.quoted[i]))
                    return std::nullopt;
                // Only a literal percent can participate in the executable
                // token. Every other field can expand to empty or user-facing
                // metadata and therefore cannot name the process safely.
                if (wi == 0 && FIELD->kind != EFieldKind::LITERAL)
                    return std::nullopt;

                const bool WHOLE_ARGUMENT = FIELD_START == 0 && T.value.size() == 2;
                switch (FIELD->kind) {
                    case EFieldKind::LITERAL: value += '%'; break;
                    case EFieldKind::SCALAR:
                        value += FIELD->scalarSource == EScalarSource::NAME ? name : file;
                        break;
                    case EFieldKind::SINGLE_INPUT:
                    case EFieldKind::LIST_INPUT:
                        if (++inputFields > 1)
                            return std::nullopt;
                        // The menubar launches applications without files or
                        // URLs. A lone empty field removes its argv element;
                        // an embedded one preserves the surrounding option.
                        omitArgument = WHOLE_ARGUMENT;
                        break;
                    case EFieldKind::ICON:
                        if (!WHOLE_ARGUMENT)
                            return std::nullopt;
                        if (!icon.empty()) {
                            out.emplace_back("--icon");
                            out.push_back(icon);
                        }
                        omitArgument = true;
                        break;
                    case EFieldKind::DEPRECATED_EMPTY:
                        omitArgument = WHOLE_ARGUMENT;
                        break;
                }
            }

            if (omitArgument)
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
