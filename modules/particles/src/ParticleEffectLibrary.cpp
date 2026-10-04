#include "aver/particles/ParticleEffectLibrary.hpp"

#include "aver/core/Log.hpp"

namespace aver::particles {

void ParticleEffectLibrary::set(u64 id, const ParticleEffect& effect) {
    if (id == 0) {
        AVER_WARN("[Particles] refusing to register effect id 0 (0 means \"no effect\" on CParticleEmitter)");
        return;
    }
    effects_[id] = effect;
}

const ParticleEffect* ParticleEffectLibrary::find(u64 id) const {
    if (id == 0) return nullptr;
    auto it = effects_.find(id);
    return it == effects_.end() ? nullptr : &it->second;
}

void ParticleEffectLibrary::clear() { effects_.clear(); }

ParticleEffectLibrary& particleEffects() {
    static ParticleEffectLibrary lib;
    return lib;
}

} // namespace aver::particles
