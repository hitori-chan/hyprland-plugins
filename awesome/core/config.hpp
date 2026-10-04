// awesome/core/config.hpp — the fork-facing config registry: the schema
// table registered with the compositor's config system, the live values,
// and the clamped typed getters modules read through.
//
// Modules never touch the config system directly: cfg().getI("...") et al.
// A config reload updates the live values in place; nothing to resync (the
// old per-plugin config structs and their re-read-on-reload dance are gone).
#pragma once

#include "schema.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>

#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Config::Values {
    class CStringValue;
}

namespace NAwesome {

    class ConfigRegistry {
      public:
        // Register every schema key with the fork and keep the live values.
        // Throws on an unknown key (a table bug, not a user error).
        void init(HANDLE handle);
        void exit(HANDLE handle);

        // live, clamped reads
        int      getI(std::string_view key) const;
        double   getD(std::string_view key) const;
        uint32_t getColor(std::string_view key) const;
        // std::string BY VALUE: value() returns a temporary string, and a
        // string_view into it would dangle the instant the return completes
        // (callers then read freed memory — the 4 KiB launcher and the
        // sound-command spawns were the first casualties).
        std::string getS(std::string_view key) const;

      private:
        // One slot per schema key, its typed value resolved once at init:
        // a read is one hash of the key (no allocation — the index views
        // the schema's static key strings) and a virtual value() call, not
        // a linear scan, a std::string and a dynamic_cast (the bar reads
        // its height on every pointer motion).
        struct SSlot {
            const Spec*                    spec = nullptr;
            SP<Config::Values::IValue>     value; // the registered handle
            Config::Values::CIntValue*     i = nullptr;
            Config::Values::CFloatValue*   d = nullptr;
            Config::Values::CColorValue*   c = nullptr;
            Config::Values::CStringValue*  s = nullptr;
        };
        const SSlot*                                 slot(std::string_view key) const;
        std::vector<SSlot>                           m_slots;
        std::unordered_map<std::string_view, size_t> m_index;
        Schema                                       m_schema;
    };

    inline ConfigRegistry& cfg() {
        static ConfigRegistry C;
        return C;
    }

} // namespace NAwesome
