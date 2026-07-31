// hyprbar/desktop_exec.hpp — freedesktop Exec= parsing at the launcher boundary
#pragma once

#include <cctype>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace NHyprbar::DesktopExec {

    // Exec= uses desktop-entry quoting, not shell quoting. Double quotes and
    // backslash escapes are removed here; shell quoting happens only after
    // field-code expansion has produced the final argument vector.
    inline std::optional<std::vector<std::string>> words(const std::string& exec) {
        std::vector<std::string> out;
        std::string              word;
        bool                     quoted = false, have = false;

        for (size_t i = 0; i < exec.size(); i++) {
            const unsigned char C = (unsigned char)exec[i];
            if (exec[i] == '\\') {
                if (++i >= exec.size())
                    return std::nullopt;
                word += exec[i];
                have = true;
                continue;
            }
            if (exec[i] == '"') {
                quoted = !quoted;
                have   = true;
                continue;
            }
            if (!quoted && std::isspace(C)) {
                if (have) {
                    out.push_back(std::move(word));
                    word.clear();
                    have = false;
                }
                continue;
            }
            word += exec[i];
            have = true;
        }
        if (quoted)
            return std::nullopt;
        if (have)
            out.push_back(std::move(word));
        return out;
    }

    inline bool isNoInputField(const std::string& word) {
        return word == "%f" || word == "%F" || word == "%u" || word == "%U" || word == "%d" || word == "%D" || word == "%n" || word == "%N" || word == "%v" || word == "%m";
    }

    inline std::optional<std::vector<std::string>> expand(const std::string& exec, const std::string& name, const std::string& file, const std::string& icon) {
        const auto WORDS = words(exec);
        if (!WORDS || WORDS->empty() || WORDS->front().empty())
            return std::nullopt;

        std::vector<std::string> out;
        for (const auto& WORD : *WORDS) {
            // %i is the only field code that expands to two arguments and the
            // specification requires it to stand alone.
            if (WORD == "%i") {
                if (!icon.empty()) {
                    out.emplace_back("--icon");
                    out.push_back(icon);
                }
                continue;
            }

            std::string value;
            for (size_t i = 0; i < WORD.size(); i++) {
                if (WORD[i] != '%') {
                    value += WORD[i];
                    continue;
                }
                if (++i >= WORD.size())
                    return std::nullopt;
                switch (WORD[i]) {
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
            if (value.empty() && isNoInputField(WORD))
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
