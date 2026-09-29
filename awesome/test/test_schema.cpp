// awesome/test/test_schema.cpp — table integrity, defaults, clamping,
// unknown-key admission.
#include "../core/schema.hpp"

#include <cmath>

#include "harness.hpp"

using namespace NAwesome;

bool test_schema() {
    Schema S;

    // the surface: 48 keys, unique, all namespaced
    AW_CHECK(S.specs().size() == 48);
    for (const auto& A : S.specs()) {
        const auto K = std::string_view{A.key};
        AW_CHECK(K.starts_with("plugin:awesome:"));
        // "plugin:awesome:<module>:<name>": a module segment must follow
        AW_CHECK(K.find(':', K.find(':') + 1) != K.npos);
        for (const auto& B : S.specs())
            if (&A != &B)
                AW_CHECK(K != B.key);
    }
    // every key resolves; modules are the five of the design (the system
    // module takes no config)
    const char* MODULES[] = {"shell", "windows", "notify"};
    size_t perModule[3] = {0, 0, 0};
    for (const auto& K : S.specs()) {
        AW_CHECK(S.find(K.key) == &K);
        for (int i = 0; i < 3; i++)
            if (std::string_view{K.key}.starts_with(std::string{"plugin:awesome:"} + MODULES[i] + ":"))
                perModule[i]++;
    }
    AW_CHECK(perModule[0] == 21 && perModule[1] == 3 && perModule[2] == 24);

    // defaults are the glass·ink tokens
    AW_CHECK(S.getI("plugin:awesome:shell:height") == 26);
    AW_CHECK(S.getI("plugin:awesome:notify:width") == 348);
    AW_CHECK(S.getI("plugin:awesome:windows:edge") == 16);
    AW_CHECK(S.getD("plugin:awesome:notify:rounding_power") == 3.0);
    AW_CHECK(S.getColor("plugin:awesome:shell:col_focus") == 0xff9acbffu);
    AW_CHECK(S.getColor("plugin:awesome:notify:col_bg") == 0x9e0f1218u);
    AW_CHECK(S.getS("plugin:awesome:shell:font") == std::string_view{"Roboto"});
    AW_CHECK(S.getS("plugin:awesome:notify:font") == std::string_view{"IBM Plex Sans"});

    // clamping: out-of-range delivers, the getter clamps
    Schema::Raw R;
    R.i = 1 << 30;
    S.set("plugin:awesome:shell:height", R);
    AW_CHECK(S.getI("plugin:awesome:shell:height") == 64);
    R.i = -5;
    S.set("plugin:awesome:shell:height", R);
    AW_CHECK(S.getI("plugin:awesome:shell:height") == 16);
    R.d = std::nan("");
    S.set("plugin:awesome:notify:rounding_power", R);
    AW_CHECK(S.getD("plugin:awesome:notify:rounding_power") == 3.0);
    R.d = 99.0;
    S.set("plugin:awesome:notify:rounding_power", R);
    AW_CHECK(S.getD("plugin:awesome:notify:rounding_power") == 8.0);

    // string keys admit at the bound, clipped at a codepoint boundary
    R.s = std::string(Schema::MAX_STRING_KEY_BYTES + 8, '\xF0');
    S.set("plugin:awesome:notify:fallback_icon_dir", R);
    AW_CHECK(S.getS("plugin:awesome:notify:fallback_icon_dir").size() == Schema::MAX_STRING_KEY_BYTES);

    // unknown keys: ignored, counted
    const auto BEFORE = S.unknownCount();
    S.set("plugin:awesome:shell:nope", R);
    AW_CHECK(S.unknownCount() == BEFORE + 1);
    AW_CHECK(S.getI("plugin:awesome:shell:nope") == 0);
    return true;
}
