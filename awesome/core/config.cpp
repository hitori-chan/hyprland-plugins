#include "config.hpp"

#include <hyprland/src/config/values/types/StringValue.hpp>

#include <stdexcept>

namespace NAwesome {

    void ConfigRegistry::init(HANDLE handle) {
        m_slots = {};
        for (size_t idx = 0; idx < SPEC_COUNT; idx++) {
            const auto& S    = SPECS[idx];
            auto&       slot = m_slots[idx];
            if (S.kind == Kind::Int) {
                auto V = makeShared<Config::Values::CIntValue>(S.key, S.desc, S.ival);
                slot.i = V.get(), slot.value = V;
            } else if (S.kind == Kind::Double) { // the fork's float value is the double slot
                auto V = makeShared<Config::Values::CFloatValue>(S.key, S.desc, (float)S.dval);
                slot.d = V.get(), slot.value = V;
            } else if (S.kind == Kind::String) {
                auto V = makeShared<Config::Values::CStringValue>(S.key, S.desc, std::string{S.sval});
                slot.s = V.get(), slot.value = V;
            } else {
                auto V = makeShared<Config::Values::CColorValue>(S.key, S.desc, (uint64_t)S.cval);
                slot.c = V.get(), slot.value = V;
            }
            if (!HyprlandAPI::addConfigValueV2(handle, slot.value))
                throw std::runtime_error(std::string{"[awesome] failed to register config key "} + S.key);
        }
    }

    void ConfigRegistry::exit(HANDLE handle) {
        // the fork owns removal: CConfigManager::onPluginUnload erases every
        // value registered for this handle (no per-value remove API)
        m_slots = {};
    }

    int ConfigRegistry::getI(CfgKey key) const {
        const auto& S = SPECS[key.index];
        if (const auto* V = m_slots[key.index].i) // Config::INTEGER is a long
            return clampInt(S, V->value());
        return S.ival;
    }

    double ConfigRegistry::getD(CfgKey key) const {
        const auto& S = SPECS[key.index];
        if (const auto* V = m_slots[key.index].d)
            return clampDouble(S, V->value());
        return S.dval;
    }

    uint32_t ConfigRegistry::getColor(CfgKey key) const {
        if (const auto* V = m_slots[key.index].c)
            return (uint32_t)V->value();
        return SPECS[key.index].cval;
    }

    std::string ConfigRegistry::getS(CfgKey key) const {
        if (const auto* V = m_slots[key.index].s)
            return boundString(V->value());
        return std::string{SPECS[key.index].sval};
    }

} // namespace NAwesome
