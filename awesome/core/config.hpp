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

#include <map>
#include <memory>

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
        std::string_view getS(std::string_view key) const;

      private:
        using ValueBase = Config::Values::IValue;
        std::map<std::string, SP<ValueBase>> m_values;
        Schema m_schema;
    };

    inline ConfigRegistry& cfg() {
        static ConfigRegistry C;
        return C;
    }

} // namespace NAwesome
