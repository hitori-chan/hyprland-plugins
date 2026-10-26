// awesome/system/wpctl.hpp — the wpctl readback and the sinks, pure and
// testable.
//
// `wpctl get-volume @DEFAULT_AUDIO_SINK@` prints
//   Volume: 0.85 [MUTED]
// with the token locale-formatted (0,85 under a comma decimal). The parse
// is the single consumer of that output: one number, one flag, nothing
// else — no shell, no library, PipeWire stays out of the process.
#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace NAwesome::System::Wpctl {

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

    // `wpctl status` lists the outputs under its Audio tree:
    //   Audio
    //    ├─ Devices:
    //    │      44. Built-in Audio                      [alsa]
    //    ├─ Sinks:
    //    │  *   53. Built-in Audio Analog Stereo        [vol: 0.35]
    //    │      61. USB Headset Analog Stereo           [vol: 1.00 MUTED]
    //    ├─ Sources:
    // The sinks are the rows of that one branch: an id, a name, a star on the
    // default. Bounded: MAX_SINKS rows, a name clipped to MAX_SINK_NAME bytes
    // on a code point; anything else in the tree is not read.
    inline constexpr size_t MAX_SINKS = 16, MAX_SINK_NAME = 96;

    struct SSink {
        uint32_t    id = 0;
        std::string name;
        bool        current = false;

        bool operator==(const SSink&) const = default;
    };

    inline std::vector<SSink> parseSinks(std::string_view status) {
        std::vector<SSink> out;
        bool               audio = false, sinks = false;
        while (!status.empty() && out.size() < MAX_SINKS) {
            const auto EOL  = status.find('\n');
            auto       line = status.substr(0, EOL);
            status.remove_prefix(EOL == std::string_view::npos ? status.size() : EOL + 1);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            // a top-level line opens a tree: only Audio's is read
            if (!line.empty() && line.front() != ' ') {
                audio = line == "Audio";
                sinks = false;
                continue;
            }
            if (!audio)
                continue;
            // the tree's drawing: spaces and the box characters before a row
            size_t at = 0;
            while (at < line.size()) {
                if (line[at] == ' ')
                    at++;
                else if (line.substr(at).starts_with("│") || line.substr(at).starts_with("├") || line.substr(at).starts_with("└") ||
                         line.substr(at).starts_with("─"))
                    at += 3; // each is three bytes of UTF-8
                else
                    break;
            }
            auto row = line.substr(at);
            // a branch's header ("Sinks:", "Sources:", …) opens or closes ours
            if (row.ends_with(':') && row.find('.') == std::string_view::npos) {
                sinks = row == "Sinks:";
                continue;
            }
            if (!sinks || row.empty())
                continue;
            SSink s;
            if (row.starts_with('*')) {
                s.current = true;
                row.remove_prefix(1);
            }
            row.remove_prefix(std::min(row.size(), row.find_first_not_of(' ')));
            const auto DOT = row.find('.');
            if (DOT == std::string_view::npos || DOT == 0)
                continue;
            const auto [END, ERR] = std::from_chars(row.data(), row.data() + DOT, s.id);
            if (ERR != std::errc{} || END != row.data() + DOT)
                continue;
            row.remove_prefix(DOT + 1);
            // the name runs up to its "[vol: …]" column, trailing spaces trimmed
            if (const auto VOL = row.rfind("[vol:"); VOL != std::string_view::npos)
                row = row.substr(0, VOL);
            row.remove_prefix(std::min(row.size(), row.find_first_not_of(' ')));
            while (!row.empty() && row.back() == ' ')
                row.remove_suffix(1);
            if (row.empty())
                continue;
            if (row.size() > MAX_SINK_NAME) {
                size_t cut = MAX_SINK_NAME;
                while (cut > 0 && ((unsigned char)row[cut] & 0xC0) == 0x80)
                    cut--;
                row = row.substr(0, cut);
            }
            s.name = std::string{row};
            out.push_back(std::move(s));
        }
        return out;
    }

} // namespace NAwesome::System::Wpctl
