// awesome/test/test_schema.cpp — table integrity, defaults, and the clamps
// the live loader applies (core/config.cpp reads every value through them).
#include "../core/schema.hpp"

#include <cmath>
#include <string>

#include "harness.hpp"

using namespace NAwesome;

// a key compiles only if it is in the table (CfgKey is consteval): a typo
// in a getter fails the BUILD
static_assert(CfgKey{"plugin:awesome:shell:height"}.index < SPEC_COUNT);
static_assert(specIndex("plugin:awesome:windows:col_frame") == SPEC_COUNT - 1);

bool test_schema() {
    // the surface: 47 keys, unique, all namespaced
    AW_CHECK(SPEC_COUNT == 47);
    for (const auto& A : SPECS) {
        const auto K = std::string_view{A.key};
        AW_CHECK(K.starts_with("plugin:awesome:"));
        // "plugin:awesome:<module>:<name>": a module segment must follow
        AW_CHECK(K.find(':', K.find(':') + 1) != K.npos);
        for (const auto& B : SPECS)
            if (&A != &B)
                AW_CHECK(K != B.key);
        // an alpha-0 color default paints nothing: a six-digit hex in a 32-bit
        // AARRGGBB field is the trap (the 0xff8a5c incident — the urgent ring
        // and the battery fills were silently invisible)
        if (A.kind == Kind::Color)
            AW_CHECK((A.cval >> 24) != 0);
        // every default sits inside its own range
        if (A.kind == Kind::Int)
            AW_CHECK(A.ival >= A.imin && A.ival <= A.imax);
        if (A.kind == Kind::Double)
            AW_CHECK(A.dval >= A.dmin && A.dval <= A.dmax);
    }
    // every key resolves; the config-bearing modules are shell, windows
    // and notify (system takes no config)
    const char* MODULES[] = {"shell", "windows", "notify"};
    size_t      perModule[3] = {0, 0, 0};
    for (const auto& K : SPECS) {
        AW_CHECK(findSpec(K.key) == &K);
        for (int i = 0; i < 3; i++)
            if (std::string_view{K.key}.starts_with(std::string{"plugin:awesome:"} + MODULES[i] + ":"))
                perModule[i]++;
    }
    AW_CHECK(perModule[0] == 20 && perModule[1] == 3 && perModule[2] == 24);
    AW_CHECK(findSpec("plugin:awesome:shell:nope") == nullptr);

    // defaults are the glass·ink tokens
    AW_CHECK(SPECS[CfgKey{"plugin:awesome:shell:height"}.index].ival == 26);
    AW_CHECK(SPECS[CfgKey{"plugin:awesome:notify:width"}.index].ival == 356);
    AW_CHECK(SPECS[CfgKey{"plugin:awesome:windows:edge"}.index].ival == 16);
    AW_CHECK(SPECS[CfgKey{"plugin:awesome:notify:rounding_power"}.index].dval == 3.0);
    AW_CHECK(SPECS[CfgKey{"plugin:awesome:shell:col_focus"}.index].cval == 0xff9acbffu);
    AW_CHECK(SPECS[CfgKey{"plugin:awesome:notify:col_bg"}.index].cval == 0x8a26262cu);
    AW_CHECK(SPECS[CfgKey{"plugin:awesome:shell:font"}.index].sval == "Roboto");
    AW_CHECK(SPECS[CfgKey{"plugin:awesome:notify:font"}.index].sval == "Inter");

    // the loader's clamps: out-of-range delivers, the getter clamps
    const auto& HEIGHT = SPECS[CfgKey{"plugin:awesome:shell:height"}.index];
    AW_CHECK(clampInt(HEIGHT, 1L << 40) == 64);
    AW_CHECK(clampInt(HEIGHT, -5) == 16);
    AW_CHECK(clampInt(HEIGHT, 30) == 30);
    const auto& POWER = SPECS[CfgKey{"plugin:awesome:notify:rounding_power"}.index];
    AW_CHECK(clampDouble(POWER, std::nan("")) == 3.0); // not finite: the default
    AW_CHECK(clampDouble(POWER, INFINITY) == 3.0);
    AW_CHECK(clampDouble(POWER, 99.0) == 8.0);

    // string values admit at the bound, cut at a codepoint boundary
    AW_CHECK(boundString(std::string(MAX_STRING_VALUE_BYTES + 8, 'a')).size() == MAX_STRING_VALUE_BYTES);
    std::string utf8;
    while (utf8.size() < MAX_STRING_VALUE_BYTES + 8)
        utf8 += "é"; // two bytes each
    utf8.insert(0, "x"); // the cap lands mid-codepoint
    const auto CUT = boundString(utf8);
    AW_CHECK(CUT.size() == MAX_STRING_VALUE_BYTES - 1 && ((unsigned char)CUT.back() & 0xC0) == 0x80);
    AW_CHECK(boundString("short") == "short");
    return true;
}
