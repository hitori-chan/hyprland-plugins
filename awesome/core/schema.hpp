// awesome/core/schema.hpp — the config surface: ONE declarative table for
// every key the plugin reads, with its kind, default, and sanity range.
//
// The plugin-side loader (the supervisor) maps the fork's Config values
// into this table once at init; modules read through the typed getters and
// never touch the config system directly. Defaults are the glass·ink
// tokens: an unset shell inherits the whole material.
//
// Ranges are soft: wide enough that every value the old plugins accepted
// stays valid. They exist so a hostile or typo'd value clamps instead of
// shaping geometry, not to enforce aesthetics.
#pragma once

#include "bounded.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace NAwesome {

    enum class Kind { Int, Double, String, Color };

    struct Spec {
        const char*      key; // "plugin:awesome:<module>:<name>"
        const char*      desc; // config validation and error messages
        Kind             kind;
        int              ival = 0; // Int default
        double           dval = 0.0;
        std::string_view sval{};
        uint32_t         cval = 0; // Color default, raw hex
        int              imin = 0, imax = 1 << 20; // Int clamp
        double           dmin = 0.0, dmax = 1e9;   // Double clamp
    };

    class Schema {
      public:
        static constexpr size_t MAX_STRING_KEY_BYTES = 4096;

        Schema();
        const std::vector<Spec>& specs() const {
            return m_specs;
        }
        const Spec* find(std::string_view key) const;

        // What the config system delivered for a key, before defaults and
        // clamping.
        struct Raw {
            int      i  = 0;
            double   d  = 0.0;
            uint32_t c  = 0;
            std::string s;
        };
        // Unknown keys are ignored and counted (the loader reports them).
        void   set(std::string_view key, Raw r);
        size_t unknownCount() const {
            return m_unknown;
        }

        // Typed access: default when unset, clamped when out of range.
        int      getI(std::string_view key) const;
        double   getD(std::string_view key) const;
        uint32_t getColor(std::string_view key) const;
        std::string_view getS(std::string_view key) const;

      private:
        const Spec* spec(std::string_view key) const {
            return find(key);
        }
        std::vector<Spec>      m_specs;
        std::map<std::string, Raw> m_raw;
        size_t                  m_unknown = 0;
    };

} // namespace NAwesome
