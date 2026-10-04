// awesome/core/config.cpp
#include "config.hpp"

#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/helpers/Color.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace NAwesome {

    void ConfigRegistry::init(HANDLE handle) {
        m_slots.clear();
        m_index.clear();
        m_slots.reserve(m_schema.specs().size());
        for (const auto& S : m_schema.specs()) {
            SSlot slot{.spec = &S};
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
            m_index.emplace(std::string_view{S.key}, m_slots.size());
            m_slots.push_back(std::move(slot));
        }
    }

    void ConfigRegistry::exit(HANDLE handle) {
        // the fork owns removal: CConfigManager::onPluginUnload erases
        // every value registered for this handle (there is no per-value
        // remove API); we just drop our handles
        m_index.clear();
        m_slots.clear();
    }

    // nullptr before init (and in the headless tests): the schema default
    const ConfigRegistry::SSlot* ConfigRegistry::slot(std::string_view key) const {
        const auto IT = m_index.find(key);
        return IT == m_index.end() ? nullptr : &m_slots[IT->second];
    }

    int ConfigRegistry::getI(std::string_view key) const {
        if (const auto* SL = slot(key); SL && SL->i) // Config::INTEGER is a long
            return (int)std::clamp<long>(SL->i->value(), (long)SL->spec->imin, (long)SL->spec->imax);
        const auto* S = m_schema.find(key);
        return S ? S->ival : 0;
    }

    double ConfigRegistry::getD(std::string_view key) const {
        if (const auto* SL = slot(key); SL && SL->d) {
            const double RAW = SL->d->value();
            return std::isfinite(RAW) ? std::clamp(RAW, SL->spec->dmin, SL->spec->dmax) : SL->spec->dval;
        }
        const auto* S = m_schema.find(key);
        return S ? S->dval : 0.0;
    }

    uint32_t ConfigRegistry::getColor(std::string_view key) const {
        if (const auto* SL = slot(key); SL && SL->c)
            return (uint32_t)SL->c->value();
        const auto* S = m_schema.find(key);
        return S ? S->cval : 0;
    }

    std::string ConfigRegistry::getS(std::string_view key) const {
        if (const auto* SL = slot(key); SL && SL->s)
            return SL->s->value();
        const auto* S = m_schema.find(key);
        return S ? std::string{S->sval} : std::string{};
    }

} // namespace NAwesome
