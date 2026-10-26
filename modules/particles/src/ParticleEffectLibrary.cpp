#include "aver/particles/ParticleEffectLibrary.hpp"

#include "aver/core/Log.hpp"

namespace aver::particles {

void ParticleEffectLibrary::set(u64 id, const ParticleEffect& effect) {
    if (id == 0) {
        AVER_WARN("[Particles] refusing to register effect id 0 (0 means \"no effect\" on CParticleEmitter)");
        return;
    }
    ParticleSystemDef& def = systems_[id];
    if (def.emitters.empty()) def.emitters.push_back(effect);
    else def.emitters[0] = effect;
    ++revision_;
}

const ParticleEffect* ParticleEffectLibrary::find(u64 id) const {
    const ParticleSystemDef* def = findSystem(id);
    return def ? &def->emitters[0] : nullptr;
}

void ParticleEffectLibrary::setSystem(u64 id, const ParticleSystemDef& def) {
    if (id == 0) {
        AVER_WARN("[Particles] refusing to register effect id 0 (0 means \"no effect\" on CParticleEmitter)");
        return;
    }
    if (def.emitters.empty()) {
        AVER_WARN("[Particles] refusing to register an effect system with no emitters (id {})", id);
        return;
    }
    systems_[id] = def;
    ++revision_;
}

const ParticleSystemDef* ParticleEffectLibrary::findSystem(u64 id) const {
    if (id == 0) return nullptr;
    auto it = systems_.find(id);
    return it == systems_.end() || it->second.emitters.empty() ? nullptr : &it->second;
}

void ParticleEffectLibrary::clear() {
    systems_.clear();
    ++revision_;
}

ParticleEffectLibrary& particleEffects() {
    static ParticleEffectLibrary lib;
    return lib;
}

} // namespace aver::particles
