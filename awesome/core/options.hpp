// awesome/core/options.hpp — the choices the plugin remembers for the
// user: what they picked from a menu (Do Not Disturb, quiet
// automatically), never what they configured. The
// config files stay the user's and read-only to the plugin; no option is
// both a config key and a remembered choice.
//
// One schema row per option: its state.tsv name, default and range.
// StateStore holds the values and writes only those off their default,
// as `option <value> <name>` rows; a row the schema does not name, or out
// of its range, is dropped at load.
#pragma once

#include "store.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace NAwesome {

    enum class eOption : uint8_t {
        DND,              // 0 off, -1 until turned off, else its end (unix seconds)
        QUIET_FULLSCREEN, // hold banners and sounds over a fullscreen focused window
        QUIET_SHARING,    // ... and while a display, or a region of one, is shared
        COUNT_,
    };

    struct SOptionSpec {
        std::string_view name;
        int64_t          def, min, max;
    };

    inline constexpr std::array<SOptionSpec, (size_t)eOption::COUNT_> OPTION_SPECS = {{
        {"dnd", 0, -1, MAX_STORE_TIME - 1},
        {"quiet-fullscreen", 0, 0, 1},
        {"quiet-sharing", 0, 0, 1},
    }};

    // the schema row of an option, and the option a row names
    inline constexpr const SOptionSpec& optionSpec(eOption o) {
        return OPTION_SPECS[(size_t)o];
    }

    inline constexpr std::optional<eOption> optionNamed(std::string_view name) {
        for (size_t i = 0; i < OPTION_SPECS.size(); i++)
            if (OPTION_SPECS[i].name == name)
                return (eOption)i;
        return std::nullopt;
    }

    class COptions {
      public:
        int64_t get(eOption o) const {
            return m_values[(size_t)o];
        }
        bool on(eOption o) const {
            return get(o) != 0;
        }
        // False when out of the option's range, or unchanged: the caller
        // dirties the state only on true.
        bool set(eOption o, int64_t v) {
            const auto& S = optionSpec(o);
            if (v < S.min || v > S.max || v == get(o))
                return false;
            m_values[(size_t)o] = v;
            return true;
        }
        bool operator==(const COptions&) const = default;

      private:
        static constexpr std::array<int64_t, OPTION_SPECS.size()> defaults() {
            std::array<int64_t, OPTION_SPECS.size()> v{};
            for (size_t i = 0; i < v.size(); i++)
                v[i] = OPTION_SPECS[i].def;
            return v;
        }
        std::array<int64_t, OPTION_SPECS.size()> m_values = defaults();
    };

} // namespace NAwesome
