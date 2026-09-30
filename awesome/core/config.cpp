// awesome/core/config.cpp
#include "config.hpp"

#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/helpers/Color.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace NAwesome {

    void ConfigRegistry::init(HANDLE handle) {
        for (const auto& S : m_schema.specs()) {
            SP<Config::Values::IValue> V;
            if (S.kind == Kind::Int)
                V = makeShared<Config::Values::CIntValue>(S.key, S.desc, S.ival);
            else if (S.kind == Kind::Double) // the fork's float value is the double slot
                V = makeShared<Config::Values::CFloatValue>(S.key, S.desc, (float)S.dval);
            else if (S.kind == Kind::String)
                V = makeShared<Config::Values::CStringValue>(S.key, S.desc, std::string{S.sval});
            else
                V = makeShared<Config::Values::CColorValue>(S.key, S.desc, (uint64_t)S.cval);
            if (!HyprlandAPI::addConfigValueV2(handle, V))
                throw std::runtime_error(std::string{"[awesome] failed to register config key "} + S.key);
            m_values[S.key] = std::move(V);
        }
    }

    void ConfigRegistry::exit(HANDLE handle) {
        // the fork owns removal: CConfigManager::onPluginUnload erases
        // every value registered for this handle (there is no per-value
        // remove API); we just drop our handles
        m_values.clear();
    }

    int ConfigRegistry::getI(std::string_view key) const {
        const auto* S = m_schema.find(key);
        const auto  IT = m_values.find(std::string{key});
        if (!S || IT == m_values.end())
            return S ? S->ival : 0;
        const auto V = dynamic_cast<Config::Values::CIntValue*>(IT->second.get());
        const auto RAW = V ? V->value() : S->ival; // Config::INTEGER is a long
        return (int)std::clamp<long>(RAW, (long)S->imin, (long)S->imax);
    }

    double ConfigRegistry::getD(std::string_view key) const {
        const auto* S = m_schema.find(key);
        const auto  IT = m_values.find(std::string{key});
        if (!S || IT == m_values.end())
            return S ? S->dval : 0.0;
        const auto V = dynamic_cast<Config::Values::CFloatValue*>(IT->second.get());
        const auto RAW = V ? (double)V->value() : S->dval;
        if (!std::isfinite(RAW))
            return S->dval;
        return std::clamp(RAW, S->dmin, S->dmax);
    }

    uint32_t ConfigRegistry::getColor(std::string_view key) const {
        const auto* S = m_schema.find(key);
        const auto  IT = m_values.find(std::string{key});
        if (!S || IT == m_values.end())
            return S ? S->cval : 0;
        const auto V = dynamic_cast<Config::Values::CColorValue*>(IT->second.get());
        return V ? (uint32_t)V->value() : S->cval;
    }

    std::string ConfigRegistry::getS(std::string_view key) const {
        const auto* S = m_schema.find(key);
        const auto  IT = m_values.find(std::string{key});
        if (!S || IT == m_values.end())
            return S ? std::string{S->sval} : std::string{};
        const auto V = dynamic_cast<Config::Values::CStringValue*>(IT->second.get());
        return V ? V->value() : std::string{S->sval};
    }

} // namespace NAwesome
