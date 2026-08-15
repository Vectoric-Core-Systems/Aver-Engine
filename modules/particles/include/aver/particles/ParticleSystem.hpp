// The CPU simulation: turns every entity's scene::CParticleEmitter into a per-emitter array of live
// particles. No RHI, no GPU, no device -- ParticleRenderer.hpp is the half that draws what this
// produces, the same split aver::anim::AnimSystem (no RHI) and render.skin's SkinnedScene (RHI) use.
//
// DECIDED 2: CPU now, GPU tier designed in. This is the "CPU now" half; see ParticleTypes.hpp's
// Particle struct for the buffer layout a future compute path would replace this loop with.
#pragma once
#include "aver/particles/ParticleEffectLibrary.hpp"
#include "aver/particles/ParticleTypes.hpp"
#include "aver/scene/World.hpp"

#include <random>
#include <unordered_map>
#include <vector>

namespace aver::particles {

// One emitter's state as ParticleRenderer needs to see it: its live particles and the effect they
// belong to, both resolved and valid as of the last tick().
struct EmitterView {
    scene::Entity entity;
    const ParticleEffect* effect;
    const std::vector<Particle>* particles;
};

class ParticleSystem {
public:
    // Advances every scene::CParticleEmitter's clock and its particles by `dt`. CALLED
    // UNCONDITIONALLY, not from a gameplay tick group -- matching AnimSystem::tick's identical
    // reasoning: an artist scrubbing a preview outside Play mode should still see an effect play.
    void tick(scene::World& world, f32 dt);

    // The library tick() resolves CParticleEmitter::effect through. Not owned: installed once by the
    // composition root (normally particleEffects(), the process global) so a test can point this at
    // a library holding nothing but its own fixture.
    void setEffectLibrary(ParticleEffectLibrary* lib) { effects_ = lib; }

    // Calls `fn(const EmitterView&)` once per emitter that has a resolved effect and at least one
    // live particle, IN THE ORDER EACH EMITTER WAS FIRST SEEN BY tick(). This is ParticleRenderer's
    // whole read surface -- everything to draw, snapshotted as of the last tick().
    //
    // EMISSION ORDER, NOT DEPTH ORDER, AND THAT IS A REAL LIMITATION: DECIDED — sort back-to-front
    // WITHIN an emitter (ParticleRenderer does that part, per particle); there is no sort ACROSS
    // emitters in this slice. Two overlapping emitters -- a fire and a smoke column rising through
    // it, say -- draw in whichever order they were first attached, not by which is actually nearer
    // the camera, so a smoke column created after a fire it now stands in FRONT of can still draw
    // behind it. Fine for one effect at a time; wrong the moment two occupy the same screen space
    // from a moving camera. A global cross-emitter sort is future work, not this slice's.
    template <class Fn>
    void forEachEmitter(Fn&& fn) const {
        for (scene::Entity e : order_) {
            auto it = emitters_.find(e);
            if (it == emitters_.end()) continue;
            const EmitterState& st = it->second;
            if (!st.effect || st.particles.empty()) continue;
            fn(EmitterView{e, st.effect, &st.particles});
        }
    }

    u32 liveEmitters() const { return static_cast<u32>(emitters_.size()); }
    u32 liveParticles(scene::Entity e) const;

    // This entity's live particles, or nullptr if it carries no emitter this system has ticked, or
    // has none alive. Valid until the next tick(). Exists beside forEachEmitter for a caller (a test,
    // an inspector) that wants one specific emitter's raw state rather than everything to draw.
    const std::vector<Particle>* particles(scene::Entity e) const;

private:
    struct EmitterState {
        std::vector<Particle> particles;
        std::mt19937 rng;
        const ParticleEffect* effect = nullptr;   // resolved this tick; see forEachEmitter
        bool wasPlaying = false;                  // for the stopped -> playing edge (fires the burst)
        bool burstFired = false;
    };

    // FIRST-SEEN order, kept separately from the map for forEachEmitter's iteration -- see its own
    // comment. std::unordered_map's own iteration order is an implementation detail (and can move on
    // a rehash), so it cannot stand in for "emission order" the way this explicit vector can.
    std::vector<scene::Entity> order_;
    std::unordered_map<scene::Entity, EmitterState> emitters_;
    ParticleEffectLibrary* effects_ = nullptr;
};

// The process-global system, matching scene::World::instance() / aver::anim::animSystem().
ParticleSystem& particleSystem();

} // namespace aver::particles
