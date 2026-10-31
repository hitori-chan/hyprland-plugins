// The fork-facing config registry: the schema table registered with the
// compositor's config system, the live values, and the clamped typed
// getters modules read through.
//
// Modules never touch the config system directly: cfg().getI("...") et al.
// The key literal is checked when the call compiles (CfgKey, schema.hpp),
// and the read is an index. A config reload updates the live values in
// place; nothing to resync.
#pragma once

#include "schema.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>

#include <array>
#include <string>

namespace Config::Values {
    class CStringValue;
}

namespace NAwesome {

    class ConfigRegistry {
      public:
        // Register every schema row with the fork and keep the live values.
        // Throws when the fork refuses one.
        void init(HANDLE handle);
        void exit(HANDLE handle);

        // live, clamped reads (the schema default before init)
        int      getI(CfgKey key) const;
        double   getD(CfgKey key) const;
        uint32_t getColor(CfgKey key) const;
        // std::string BY VALUE: value() returns a temporary string, and a
        // string_view into it would dangle when the return completes.
        // Bounded (MAX_STRING_VALUE_BYTES).
        std::string getS(CfgKey key) const;

      private:
        // one slot per schema row, its typed value resolved once at init: a
        // read is a virtual value() call, not a scan, a std::string and a
        // dynamic_cast (the bar reads its height on every pointer motion)
        struct SSlot {
            SP<Config::Values::IValue>     value; // the registered handle
            Config::Values::CIntValue*     i = nullptr;
            Config::Values::CFloatValue*   d = nullptr;
            Config::Values::CColorValue*   c = nullptr;
            Config::Values::CStringValue*  s = nullptr;
        };
        std::array<SSlot, SPEC_COUNT> m_slots{};
    };

    inline ConfigRegistry& cfg() {
        static ConfigRegistry C;
        return C;
    }

} // namespace NAwesome
