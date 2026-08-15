// The interim way scene::CParticleEmitter::effect resolves to a ParticleEffect, until DECIDED 3's
// .ocparticle reader exists. Same SHAPE as aver::anim::AnimSystem's asset cache -- an opaque id
// resolved by a tier above modules/scene -- but simpler, because there is no file yet: a composition
// root fills this table directly (register()) instead of AnimSystem's AssetPathFn + a loader.
//
// A LATER SLICE'S ASSET LOADER IS A DROP-IN REPLACEMENT FOR THE CALLER OF set(), never for
// ParticleSystem: it only ever asks find(id), and never learns whether the effect behind that id
// came from a file or from a line of C++ in a composition root's own test content.
#pragma once
#include "aver/particles/ParticleTypes.hpp"

#include <unordered_map>

namespace aver::particles {

class ParticleEffectLibrary {
public:
    // Registers (or replaces) the effect at `id`. Rejects id 0: CParticleEmitter::effect == 0 means
    // "no effect", the same "0 is invalid" idiom every opaque asset id in this engine uses.
    void set(u64 id, const ParticleEffect& effect);
    // The effect at `id`, or nullptr when unregistered. Valid until the next set()/clear() touching it.
    const ParticleEffect* find(u64 id) const;
    // Drops every registered effect. Call when a project closes.
    void clear();

    u32 count() const { return static_cast<u32>(effects_.size()); }

private:
    std::unordered_map<u64, ParticleEffect> effects_;
};

// The process-global library, matching scene::World::instance() / aver::anim::animSystem().
ParticleEffectLibrary& particleEffects();

} // namespace aver::particles
