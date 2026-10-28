#pragma once

#include <cctype>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace NAwesome::DesktopExec {

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
    // and Icon. Exec takes it too, BEFORE its own quote-and-backslash grammar
    // below (the spec: a literal backslash in a quoted argument is written
    // four times) — through unescapeExec, not this.
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

    // Exec's string pass: the five string escapes decode, and any other
    // backslash stays for the quoting grammar (GLib's behavior). Wine's
    // entries depend on it: `Start\\ Menu` must reach tokens() as `Start\ Menu`
    // (one argument), and `C:\\\\windows` as `C:\\windows`.
    inline std::string unescapeExec(std::string_view value) {
        std::string out;
        out.reserve(value.size());
        for (size_t i = 0; i < value.size(); i++) {
            if (value[i] == '\\' && i + 1 < value.size()) {
                if (const auto C = unescapeChar(value[i + 1], false)) {
                    out += *C;
                    i++;
                    continue;
                }
            }
            out += value[i];
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

    // One [Desktop Entry] group, string-level unescaped (Exec= keeps its
    // field codes and quoting for the grammar below).
    struct SEntry {
        std::string              name, icon, wmClass, exec;
        std::vector<std::string> onlyShowIn, notShowIn, categories;
        bool                     application = false, hidden = false, terminal = false, dbusActivatable = false;
        bool                     malformed = false; // a value that would not unescape: never listed
    };

    // The file's [Desktop Entry] group; nullopt when it has none. Space
    // around '=' is ignored (the spec), a key's first occurrence wins, and
    // localized keys (Name[de]) are skipped.
    inline std::optional<SEntry> parseEntry(std::string_view contents) {
        SEntry     E;
        bool       inEntry = false, sawEntry = false, sawExec = false;
        const auto str     = [&](std::string_view value, std::string& out) {
            if (auto U = unescapeString(value))
                out = std::move(*U);
            else
                E.malformed = true;
        };
        const auto list = [&](std::string_view value, std::vector<std::string>& out) {
            if (auto U = unescapeList(value))
                out = std::move(*U);
            else
                E.malformed = true;
        };
        for (size_t p = 0; p < contents.size();) {
            const auto       NL   = contents.find('\n', p);
            std::string_view line = contents.substr(p, NL == std::string_view::npos ? contents.size() - p : NL - p);
            p                     = NL == std::string_view::npos ? contents.size() : NL + 1;
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            if (line.starts_with('[')) {
                if (inEntry)
                    break; // only [Desktop Entry] matters
                inEntry  = line == "[Desktop Entry]";
                sawEntry = sawEntry || inEntry;
                continue;
            }
            if (!inEntry || line.starts_with('#'))
                continue;
            const auto EQ = line.find('=');
            if (EQ == std::string_view::npos)
                continue;
            size_t keyEnd = EQ, valStart = EQ + 1;
            while (keyEnd > 0 && line[keyEnd - 1] == ' ')
                keyEnd--;
            while (valStart < line.size() && line[valStart] == ' ')
                valStart++;
            const auto KEY = line.substr(0, keyEnd), VALUE = line.substr(valStart);
            if (KEY == "Name" && E.name.empty())
                str(VALUE, E.name);
            else if (KEY == "Exec" && !std::exchange(sawExec, true))
                E.exec = unescapeExec(VALUE);
            else if (KEY == "Icon" && E.icon.empty())
                str(VALUE, E.icon);
            else if (KEY == "StartupWMClass" && E.wmClass.empty())
                str(VALUE, E.wmClass);
            else if (KEY == "Terminal")
                E.terminal = VALUE == "true";
            else if (KEY == "DBusActivatable")
                E.dbusActivatable = VALUE == "true";
            else if (KEY == "Type")
                E.application = VALUE == "Application";
            else if ((KEY == "NoDisplay" || KEY == "Hidden") && VALUE == "true")
                E.hidden = true;
            else if (KEY == "OnlyShowIn")
                list(VALUE, E.onlyShowIn);
            else if (KEY == "NotShowIn")
                list(VALUE, E.notShowIn);
            else if (KEY == "Categories")
                list(VALUE, E.categories);
        }
        if (!sawEntry)
            return std::nullopt;
        return E;
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

    // ---- the launcher's typed command line (run through /bin/sh -c) ----

    // One well-formed UTF-8 sequence at s[i]: its length, its code point in
    // cp; 0 when malformed (a stray or bad lead, a short or bad tail, an
    // overlong form, a surrogate, past U+10FFFF)
    inline size_t utf8At(std::string_view s, size_t i, uint32_t& cp) {
        const auto C = (unsigned char)s[i];
        if (C < 0x80) {
            cp = C;
            return 1;
        }
        const size_t LEN = C >= 0xf0 && C < 0xf5 ? 4 : C >= 0xe0 && C < 0xf0 ? 3 : C >= 0xc2 && C < 0xe0 ? 2 : 0;
        if (LEN == 0 || i + LEN > s.size())
            return 0;
        cp = C & (0xff >> (LEN + 1));
        for (size_t k = 1; k < LEN; k++) {
            const auto B = (unsigned char)s[i + k];
            if ((B & 0xc0) != 0x80)
                return 0;
            cp = (cp << 6) | (B & 0x3f);
        }
        constexpr uint32_t MIN[] = {0, 0, 0x80, 0x800, 0x10000};
        return cp < MIN[LEN] || (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff ? 0 : LEN;
    }

    // a character the one-line prompt may hold: no C0/C1 control (a newline
    // ends the command), no line or paragraph separator (it shows as one),
    // no bidi control (it can hide or reorder what the prompt shows)
    inline bool typeableChar(uint32_t cp) {
        return cp >= 0x20 && cp != 0x7f && !(cp >= 0x80 && cp <= 0x9f) && cp != 0x61c && cp != 0x200e && cp != 0x200f && cp != 0x2028 && cp != 0x2029 &&
            !(cp >= 0x202a && cp <= 0x202e) && !(cp >= 0x2066 && cp <= 0x2069);
    }

    // A name that may be typed into a command line: well-formed UTF-8 of
    // typeable characters only
    inline bool typeableName(std::string_view s) {
        for (size_t i = 0; i < s.size();) {
            uint32_t     cp  = 0;
            const size_t LEN = utf8At(s, i, cp);
            if (LEN == 0 || !typeableChar(cp))
                return false;
            i += LEN;
        }
        return true;
    }

    // Text that may be typed into a command line: `s` with every untypeable
    // character and every malformed byte removed (one at a time: the bytes
    // after a bad one are read again)
    inline std::string typeableText(std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size();) {
            uint32_t     cp  = 0;
            const size_t LEN = utf8At(s, i, cp);
            if (LEN == 0) {
                i++;
                continue;
            }
            if (typeableChar(cp))
                out.append(s, i, LEN);
            i += LEN;
        }
        return out;
    }

    // A completed name as one sh word: every byte sh gives a meaning to is
    // backslash-escaped (a file named `x$(cmd)` must stay a file name; `=`
    // too, which would make a first word an assignment).
    // nullopt for a name that cannot be typed at all.
    inline std::optional<std::string> shellWord(std::string_view name) {
        if (!typeableName(name))
            return std::nullopt;
        std::string out;
        out.reserve(name.size());
        for (const char C : name) {
            const auto U = (unsigned char)C;
            if (!(std::isalnum(U) || U >= 0x80 || std::string_view{"._+,:@%/-"}.find(C) != std::string_view::npos))
                out += '\\';
            out += C;
        }
        return out;
    }

    // sh's backslash in a plain word: the next byte is literal
    inline std::string unescapeWord(std::string_view word) {
        std::string out;
        out.reserve(word.size());
        for (size_t i = 0; i < word.size(); i++) {
            if (word[i] == '\\' && i + 1 < word.size())
                i++;
            out += word[i];
        }
        return out;
    }

    // whether the byte at `at` is escaped (an odd run of backslashes before it)
    inline bool escapedAt(std::string_view s, size_t at) {
        size_t n = 0;
        while (at > n && s[at - n - 1] == '\\')
            n++;
        return n % 2 == 1;
    }

} // namespace NAwesome::DesktopExec
