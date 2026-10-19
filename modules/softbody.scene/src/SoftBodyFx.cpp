// Break effects: the particles and sounds an .ocsoft binds to impact, tear, detach, dust and release, queued where the event
// is raised and dispatched at the end of the tick under budgets. Output only: nothing here reaches the simulation, and no
// RNG is used (a sound variant is a hash of a counter). Particles go through GPU_PARTICLES' dependency-free seam
// (aver/core/EffectSpawn.hpp); sounds go through the SoundFn the host installs. See docs/SOFTBODY.md 2.5.
#include "aver/softbody/SoftBodySystem.hpp"

#include "aver/core/EffectSpawn.hpp"
#include "aver/core/Hash.hpp"

#include <algorithm>
#include <cmath>

namespace aver::softbody {
namespace {

u32 mix(u32 x) {
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return x;
}

}  // namespace

void SoftBodySystem::emitFx(Entry& en, FxSlot slot, const Vec3& pos, const Vec3& dir, f32 value, u32 particles) {
    if (restoring_ || !effectsOn_) return;
    const u32 si = static_cast<u32>(slot);
    const FxSlotParams& s = en.asset.effects.slot[si];
    if (!s.bound()) return;
    if (slot == FxSlot::Impact && value < s.minSpeedCmS) return;
    if (en.fxCooldown[si] > 0.0f) return;
    en.fxCooldown[si] = s.cooldownS;

    f32 n;
    if (slot == FxSlot::Dust) n = s.countPerParticle * static_cast<f32>(particles);
    else                      n = s.count + s.countPerMs * (value * 0.01f);
    n = std::clamp(n, 0.0f, 64.0f);

    FxRequest r;
    r.slot = slot;
    r.entity = en.entity;
    r.pos = pos;
    r.dir = dir.sizeSquared() > 1e-8f ? dir.getSafeNormal() : Vec3{0, 0, 1};
    r.count = static_cast<u32>(std::lround(n));
    const u32 seq = fxSeq_++;
    r.particle = s.particle.empty() || r.count == 0 ? 0 : fnv1a64(s.particle);
    if (!s.sounds.empty()) r.sound = fnv1a64(s.sounds[mix(seq) % s.sounds.size()]);
    r.volume = s.volume;
    r.pitch = 1.0f + s.pitchJitter * ((static_cast<f32>(mix(seq * 2654435761u + 17u) & 0xFFFFu) / 32767.5f) - 1.0f);
    r.innerCm = s.innerCm;
    r.outerCm = s.outerCm;
    if (r.particle == 0 && r.sound == 0) return;
    fxQueue_.push_back(r);
}

void SoftBodySystem::flushFx() {
    u32 sent = 0, sounds = 0, dropped = 0;
    for (const FxRequest& r : fxQueue_) {
        if (dist(r.pos, viewPos_) > opt_.fxMaxDistanceCm) { ++dropped; continue; }
        if (sent >= opt_.fxMaxPerTick) { ++dropped; continue; }
        ++sent;
        if (r.particle) spawnEffectAt(r.particle, r.pos, r.dir, r.count);
        if (r.sound) {
            if (sounds >= opt_.fxSoundsPerTick) { ++dropped; continue; }
            ++sounds;
            if (soundFn_) soundFn_(r, soundUser_);
        }
    }
    fxQueue_.clear();
    stats_.fxSent += sent;
    stats_.fxDropped += dropped;
}

}  // namespace aver::softbody
