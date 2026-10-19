// How a scene::CParticleEmitter::effect (an opaque id) resolves to authored data. A composition root fills
// this table (loadProjectParticleEffects reads *.ocparticle under the content root); ParticleSystem only ever
// asks find(id) / findSystem(id) and never learns whether a definition came from a file or from a line of C++.
//
// An id names a SYSTEM (one or more emitters). set / find keep their 0.7 signatures and mean "emitter 0" so
// every existing caller and test is untouched.
#pragma once
#include "aver/core/Hash.hpp"
#include "aver/particles/ParticleTypes.hpp"

#include <string>
#include <string_view>
#include <unordered_map>

namespace aver::particles {

// The id an effect is registered under: fnv1a64 of its content-relative path with '\' folded to '/'. Equals
// Assets.ObjectIdOf(path) in C#.
inline u64 effectId(std::string_view contentRelativePath) {
    std::string p(contentRelativePath);
    for (char& c : p) if (c == '\\') c = '/';
    return fnv1a64(std::string_view(p));
}

class ParticleEffectLibrary {
public:
    // Registers (or replaces) emitter 0 of the system at `id`. Rejects id 0: CParticleEmitter::effect == 0 means
    // "no effect", the same "0 is invalid" idiom every opaque asset id in this engine uses.
    void set(u64 id, const ParticleEffect& effect);
    // Emitter 0 of the system at `id`, or nullptr when unregistered. Valid until the next set*/clear() touching it.
    const ParticleEffect* find(u64 id) const;

    // The whole system. A definition with no emitters is refused.
    void setSystem(u64 id, const ParticleSystemDef& def);
    const ParticleSystemDef* findSystem(u64 id) const;

    // Drops every registered effect. Call when a project closes.
    void clear();

    u32 count() const { return static_cast<u32>(systems_.size()); }

    // Bumped by every mutation; a cache of resolved pointers re-resolves when it changes.
    u64 revision() const { return revision_; }

private:
    std::unordered_map<u64, ParticleSystemDef> systems_;
    u64 revision_ = 1;
};

// The process-global library, matching scene::World::instance() / aver::anim::animSystem().
ParticleEffectLibrary& particleEffects();

} // namespace aver::particles
