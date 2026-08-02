#pragma once

#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace NHyprosd::Wpctl {

    struct SReadback {
        double value = 0;
        bool   muted = false;
    };

    inline std::optional<SReadback> parseReadback(std::string_view output) {
        const auto trimLeft = [](std::string_view& value) {
            const auto FIRST = value.find_first_not_of(" \t\r\n");
            value.remove_prefix(FIRST == std::string_view::npos ? value.size() : FIRST);
        };

        trimLeft(output);
        if (!output.starts_with("Volume:"))
            return std::nullopt;
        output.remove_prefix(7);
        trimLeft(output);

        const auto TOKEN_END = output.find_first_of(" \t\r\n[");
        std::string token{output.substr(0, TOKEN_END)};
        if (token.empty())
            return std::nullopt;
        if (token.find('.') == std::string::npos && token.find(',') != std::string::npos && token.find(',') == token.rfind(','))
            token[token.find(',')] = '.';

        double     number = 0;
        const auto [END, ERROR] = std::from_chars(token.data(), token.data() + token.size(), number, std::chars_format::general);
        if (ERROR != std::errc{} || END != token.data() + token.size() || !std::isfinite(number) || number < 0)
            return std::nullopt;

        output.remove_prefix(TOKEN_END == std::string_view::npos ? output.size() : TOKEN_END);
        trimLeft(output);
        if (output.empty())
            return SReadback{number, false};
        if (!output.starts_with("[MUTED]"))
            return std::nullopt;
        output.remove_prefix(7);
        trimLeft(output);
        if (!output.empty())
            return std::nullopt;
        return SReadback{number, true};
    }

} // namespace NHyprosd::Wpctl
