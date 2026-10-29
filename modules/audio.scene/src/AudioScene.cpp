#include "aver/audio/AudioScene.hpp"

#include "aver/core/Assert.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

#if AVER_MODULE_AUDIO_ABI
#include "aver/audio/audio_abi.h"
#endif
#if AVER_MODULE_PHYSICS
#include "aver/physics/physics_abi.h"
#endif

namespace aver::audio {
namespace {

#if AVER_MODULE_AUDIO_ABI
void abiSetReverb(void*, const ReverbParams& p) { aver_audio_set_reverb(p.wet, p.decaySec, p.damping); }
void abiSetOcclusion(void*, i32 v, f32 o) { aver_audio_set_voice_occlusion(v, o); }
void abiSetPosition(void*, i32 v, f32 x, f32 y, f32 z) { aver_audio_set_voice_position(v, x, y, z); }
bool abiPlaying(void*, i32 v) { return aver_audio_playing(v) != 0; }
void abiGetListener(void*, f32 out[3]) { aver_audio_get_listener(out, nullptr, nullptr); }
void abiSetZoneReverb(void*, u32 slot, const ZoneReverb& z) {
    aver_audio_set_zone_reverb(static_cast<i32>(slot), z.reverb.wet, z.reverb.decaySec, z.reverb.damping, static_cast<i32>(z.impulse), z.predelayMs);
}
void abiSetVoiceZoneSend(void*, i32 v, u32 slot, f32 send) { aver_audio_set_voice_zone_send(v, static_cast<i32>(slot), send); }
void abiSetVoiceReverbSend(void*, i32 v, f32 send) { aver_audio_set_voice_reverb_send(v, send); }
#endif

#if AVER_MODULE_PHYSICS
// A ray stops just short of the target so it does not report the source's own collider.
f32 physicsRayBlock(void*, const f32 from[3], const f32 to[3]) {
    f32 d[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    const f32 len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    constexpr f32 kMarginCm = 10.0f;
    if (len < 2.0f * kMarginCm) return 0.0f;
    for (f32& c : d) c /= len;
    float p[3], n[3];
    int32_t ent = 0;
    // The return value, not the entity: ownerless bodies (landscape) report entity 0.
    return aver_phys_raycast(from[0], from[1], from[2], d[0], d[1], d[2], len - kMarginCm, p, n, &ent) != 0
               ? 1.0f : 0.0f;
}
#endif

bool nearEqual(f32 a, f32 b) { return std::fabs(a - b) < 1.0e-3f; }

// A zone's weight at a world point: 1 inside, falling to 0 over its blend distance. The point is taken into the zone's frame, in world
// centimetres along each axis; the entity's scale stretches the volume.
f32 zoneWeightAt(const Mat4& m, const CReverbZone& z, const f32 p[3]) {
    const f32 d[3] = {p[0] - m.m[3][0], p[1] - m.m[3][1], p[2] - m.m[3][2]};
    f32 local[3], axisLen[3];
    for (int k = 0; k < 3; ++k) {
        const f32 ax = m.m[k][0], ay = m.m[k][1], az = m.m[k][2];
        axisLen[k] = std::sqrt(ax * ax + ay * ay + az * az);
        const f32 inv = axisLen[k] > 1.0e-6f ? 1.0f / axisLen[k] : 0.0f;
        local[k] = (d[0] * ax + d[1] * ay + d[2] * az) * inv;
    }
    f32 extent[3];
    const ZoneShape shape = z.shape == static_cast<i32>(ZoneShape::Sphere) ? ZoneShape::Sphere : ZoneShape::Box;
    if (shape == ZoneShape::Sphere) {
        const f32 s = std::fmax(axisLen[0], std::fmax(axisLen[1], axisLen[2]));
        extent[0] = extent[1] = extent[2] = z.radiusCm * s;
    } else {
        for (int k = 0; k < 3; ++k) extent[k] = z.halfExtentsCm[k] * axisLen[k];
    }
    return zoneWeight(zoneSignedDistance(shape, extent, local), z.blendDistanceCm);
}

} // namespace

AudioControl abiAudioControl() {
    AudioControl c;
#if AVER_MODULE_AUDIO_ABI
    c.setReverb = &abiSetReverb;
    c.setVoiceOcclusion = &abiSetOcclusion;
    c.setVoicePosition = &abiSetPosition;
    c.voicePlaying = &abiPlaying;
    c.getListener = &abiGetListener;
    c.setZoneReverb = &abiSetZoneReverb;
    c.setVoiceZoneSend = &abiSetVoiceZoneSend;
    c.setVoiceReverbSend = &abiSetVoiceReverbSend;
#endif
    return c;
}

AudioSceneSystem::AudioSceneSystem() {
    control_ = abiAudioControl();
#if AVER_MODULE_PHYSICS
    rayFn_ = &physicsRayBlock;
#endif
}

u32 AudioSceneSystem::registerComponents(scene::World& world) {
    if (zoneType_ != 0 && occType_ != 0) return zoneType_;

    {
        auto b = world.registerComponent<CReverbZone>("CReverbZone");
        b.field("enabled", scene::FieldKind::I32, static_cast<u16>(offsetof(CReverbZone, enabled)))
            .field("shape", scene::FieldKind::I32, static_cast<u16>(offsetof(CReverbZone, shape)))
            .field("halfExtentsCm", scene::FieldKind::Vec3, static_cast<u16>(offsetof(CReverbZone, halfExtentsCm)))
            .field("radiusCm", scene::FieldKind::F32, static_cast<u16>(offsetof(CReverbZone, radiusCm)))
            .field("blendDistanceCm", scene::FieldKind::F32, static_cast<u16>(offsetof(CReverbZone, blendDistanceCm)))
            .field("priority", scene::FieldKind::I32, static_cast<u16>(offsetof(CReverbZone, priority)))
            .field("wet", scene::FieldKind::F32, static_cast<u16>(offsetof(CReverbZone, wet)))
            .field("decaySec", scene::FieldKind::F32, static_cast<u16>(offsetof(CReverbZone, decaySec)))
            .field("damping", scene::FieldKind::F32, static_cast<u16>(offsetof(CReverbZone, damping)))
            .field("impulseSound", scene::FieldKind::I32, static_cast<u16>(offsetof(CReverbZone, impulseSound)))
            .field("predelayMs", scene::FieldKind::F32, static_cast<u16>(offsetof(CReverbZone, predelayMs)))
            .field("weight", scene::FieldKind::F32, static_cast<u16>(offsetof(CReverbZone, weight)), 0, /*readOnly*/ true);
        AVER_ASSERTM(b.verify(sizeof(CReverbZone)), "CReverbZone");
        zoneType_ = b.typeId();
    }
    {
        auto b = world.registerComponent<CAudioOcclusion>("CAudioOcclusion");
        b.field("enabled", scene::FieldKind::I32, static_cast<u16>(offsetof(CAudioOcclusion, enabled)))
            .field("voice", scene::FieldKind::I32, static_cast<u16>(offsetof(CAudioOcclusion, voice)))
            .field("followEntity", scene::FieldKind::I32, static_cast<u16>(offsetof(CAudioOcclusion, followEntity)))
            .field("rayCount", scene::FieldKind::I32, static_cast<u16>(offsetof(CAudioOcclusion, rayCount)))
            .field("probeRadiusCm", scene::FieldKind::F32, static_cast<u16>(offsetof(CAudioOcclusion, probeRadiusCm)))
            .field("thinkIntervalSec", scene::FieldKind::F32, static_cast<u16>(offsetof(CAudioOcclusion, thinkIntervalSec)))
            .field("riseSec", scene::FieldKind::F32, static_cast<u16>(offsetof(CAudioOcclusion, riseSec)))
            .field("fallSec", scene::FieldKind::F32, static_cast<u16>(offsetof(CAudioOcclusion, fallSec)))
            .field("thinkAccumulatorSec", scene::FieldKind::F32,
                   static_cast<u16>(offsetof(CAudioOcclusion, thinkAccumulatorSec)), 0, /*readOnly*/ true)
            .field("measured", scene::FieldKind::F32, static_cast<u16>(offsetof(CAudioOcclusion, measured)), 0, true)
            .field("occlusion", scene::FieldKind::F32, static_cast<u16>(offsetof(CAudioOcclusion, occlusion)), 0, true)
            .field("diffraction", scene::FieldKind::I32, static_cast<u16>(offsetof(CAudioOcclusion, diffraction)))
            .field("diffractionDirections", scene::FieldKind::I32, static_cast<u16>(offsetof(CAudioOcclusion, diffractionDirections)))
            .field("diffractionMaxRays", scene::FieldKind::I32, static_cast<u16>(offsetof(CAudioOcclusion, diffractionMaxRays)))
            .field("diffractionMaxOffsetCm", scene::FieldKind::F32, static_cast<u16>(offsetof(CAudioOcclusion, diffractionMaxOffsetCm)))
            .field("diffractionMaxOcclusion", scene::FieldKind::F32, static_cast<u16>(offsetof(CAudioOcclusion, diffractionMaxOcclusion)))
            .field("detourCm", scene::FieldKind::F32, static_cast<u16>(offsetof(CAudioOcclusion, detourCm)), 0, true)
            .field("zoneSends", scene::FieldKind::I32, static_cast<u16>(offsetof(CAudioOcclusion, zoneSends)))
            .field("reverbSendBase", scene::FieldKind::F32, static_cast<u16>(offsetof(CAudioOcclusion, reverbSendBase)));
        AVER_ASSERTM(b.verify(sizeof(CAudioOcclusion)), "CAudioOcclusion");
        occType_ = b.typeId();
    }
    return zoneType_;
}

CReverbZone* AudioSceneSystem::attachReverbZone(scene::World& world, scene::Entity e) {
    if (zoneType_ == 0) return nullptr;
    auto* z = static_cast<CReverbZone*>(world.addComponent(e, zoneType_));
    if (z) *z = CReverbZone{};
    return z;
}

CAudioOcclusion* AudioSceneSystem::attachOcclusion(scene::World& world, scene::Entity e, i32 voice) {
    if (occType_ == 0) return nullptr;
    auto* c = static_cast<CAudioOcclusion*>(world.addComponent(e, occType_));
    if (!c) return nullptr;
    *c = CAudioOcclusion{};
    c->voice = voice;
    return c;
}

void AudioSceneSystem::tick(scene::World& world, f32 dt) {
    f32 lis[3] = {listener_[0], listener_[1], listener_[2]};
    if (control_.getListener) control_.getListener(control_.user, lis);

    // ---- reverb zones ----
    if (zoneType_ != 0) {
        inputs_.clear();
        if (scene::ComponentPool* pool = world.pool(zoneType_)) {
            for (usize i = 0; i < pool->size(); ++i) {
                auto* z = static_cast<CReverbZone*>(pool->dataAt(i));
                if (!z) continue;
                if (!z->enabled) { z->weight = 0.0f; continue; }

                const Mat4& m = world.worldMatrix(pool->entityAt(i));
                z->weight = zoneWeightAt(m, *z, lis);

                ReverbZoneInput in;
                in.weight = z->weight;
                in.priority = z->priority;
                in.params.wet = z->wet;
                in.params.decaySec = z->decaySec;
                in.params.damping = z->damping;
                inputs_.push_back(in);
            }
        }

        ReverbParams blended = blendReverbZones(inputs_.data(), inputs_.size(), ambient_);
        if (reverbTau_ > 0.0f && pushed_) {
            // Ease toward the blend (the first tick snaps); a component within the push threshold of its target lands on it.
            const f32 k = smoothingCoeff(dt, reverbTau_);
            auto ease = [&](f32& cur, f32 target) {
                cur += (target - cur) * k;
                if (nearEqual(cur, target)) cur = target;
                return cur;
            };
            blended.wet = ease(smoothed_.wet, blended.wet);
            blended.decaySec = ease(smoothed_.decaySec, blended.decaySec);
            blended.damping = ease(smoothed_.damping, blended.damping);
        } else {
            smoothed_ = blended;
        }
        const bool changed = !pushed_ || !nearEqual(blended.wet, applied_.wet) ||
                             !nearEqual(blended.decaySec, applied_.decaySec) ||
                             !nearEqual(blended.damping, applied_.damping);
        if (changed) {
            applied_ = blended;
            pushed_ = true;
            if (control_.setReverb) control_.setReverb(control_.user, blended);
        }
    }

    tickZoneSends(world, dt);

    // ---- occlusion ----
    if (occType_ != 0) {
        if (scene::ComponentPool* pool = world.pool(occType_)) {
            for (usize i = 0; i < pool->size(); ++i) {
                auto* c = static_cast<CAudioOcclusion*>(pool->dataAt(i));
                if (!c || !c->enabled || c->voice == 0) continue;
                if (control_.voicePlaying && !control_.voicePlaying(control_.user, c->voice)) {
                    c->voice = 0;
                    c->occlusion = c->measured = 0.0f;
                    continue;
                }

                const Mat4& m = world.worldMatrix(pool->entityAt(i));
                const f32 pos[3] = {m.m[3][0], m.m[3][1], m.m[3][2]};
                if (c->followEntity && control_.setVoicePosition)
                    control_.setVoicePosition(control_.user, c->voice, pos[0], pos[1], pos[2]);

                c->thinkAccumulatorSec += dt;
                if (c->thinkAccumulatorSec >= c->thinkIntervalSec) {
                    c->thinkAccumulatorSec -= c->thinkIntervalSec;
                    if (c->thinkAccumulatorSec > c->thinkIntervalSec) c->thinkAccumulatorSec = 0.0f;
                    OcclusionProbe probe;
                    probe.rayCount = c->rayCount < 1 ? 1u : static_cast<u32>(c->rayCount);
                    probe.probeRadiusCm = c->probeRadiusCm;
                    if (c->diffraction && rayFn_) {
                        DiffractionProbe dp;
                        // A component added from script arrives zero-filled: 0 means "the default" for these four.
                        if (c->diffractionDirections > 0) dp.directions = static_cast<u32>(c->diffractionDirections);
                        if (c->diffractionMaxRays > 0) dp.maxRays = static_cast<u32>(c->diffractionMaxRays);
                        if (c->diffractionMaxOffsetCm > 0.0f) dp.maxOffsetCm = c->diffractionMaxOffsetCm;
                        if (c->diffractionMaxOcclusion > 0.0f) dp.maxOcclusion = c->diffractionMaxOcclusion;
                        DiffractionPath path;
                        c->measured = measureOcclusionDiffracted(rayFn_, rayUser_, lis, pos, probe, dp, &path);
                        c->detourCm = path.found ? path.detourCm : 0.0f;
                    } else {
                        c->measured = rayFn_ ? measureOcclusion(rayFn_, rayUser_, lis, pos, probe) : 0.0f;
                        c->detourCm = 0.0f;
                    }
                }
                c->occlusion = smoothOcclusion(c->occlusion, c->measured, dt, c->riseSec, c->fallSec);
                if (control_.setVoiceOcclusion) control_.setVoiceOcclusion(control_.user, c->voice, c->occlusion);
            }
        }
    }
}

// Zone sends. Emitters with CAudioOcclusion::zoneSends feed the pre-allocated send reverbs of the zones they are inside, so a sound in a
// cave rings in the cave wherever the listener stands, and the listener-side reverb (above, unchanged) carries the rest: a tracked voice's
// listener-side send is scaled by what no zone slot took. Both sides are continuous in the voice's position, so crossing a zone edge fades
// one reverb in as the other fades out. With no emitter asking, nothing here runs.
void AudioSceneSystem::tickZoneSends(scene::World& world, f32 dt) {
    tracked_.clear();
    if (occType_ != 0) {
        if (scene::ComponentPool* pool = world.pool(occType_)) {
            for (usize i = 0; i < pool->size(); ++i) {
                auto* c = static_cast<CAudioOcclusion*>(pool->dataAt(i));
                if (!c || !c->enabled || c->voice == 0 || !c->zoneSends) continue;
                if (control_.voicePlaying && !control_.voicePlaying(control_.user, c->voice)) continue;
                const Mat4& m = world.worldMatrix(pool->entityAt(i));
                Tracked t;
                t.voice = c->voice;
                t.pos[0] = m.m[3][0]; t.pos[1] = m.m[3][1]; t.pos[2] = m.m[3][2];
                t.base = c->reverbSendBase < 0.0f ? 0.0f : (c->reverbSendBase > 1.0f ? 1.0f : c->reverbSendBase);
                tracked_.push_back(t);
            }
        }
    }
    bool anySlot = false;
    for (u32 s = 0; s < kMaxZoneReverbs; ++s) anySlot = anySlot || slotKey_[s] != 0;
    if (tracked_.empty() && !anySlot && sent_.empty()) return;

    zoneRefs_.clear();
    if (zoneType_ != 0) {
        if (scene::ComponentPool* pool = world.pool(zoneType_)) {
            for (usize i = 0; i < pool->size(); ++i) {
                auto* z = static_cast<CReverbZone*>(pool->dataAt(i));
                if (!z || !z->enabled) continue;
                ZoneRef r;
                r.key = static_cast<u64>(pool->entityAt(i));
                r.priority = z->priority;
                r.z = z;
                r.m = world.worldMatrix(pool->entityAt(i));
                zoneRefs_.push_back(r);
            }
        }
    }

    // Every tracked voice's weight in every zone, and each zone's relevance: the largest weight any voice has in it.
    const usize nz = zoneRefs_.size();
    std::vector<f32> w(tracked_.size() * nz, 0.0f);
    for (usize v = 0; v < tracked_.size(); ++v)
        for (usize k = 0; k < nz; ++k) {
            const f32 wk = zoneWeightAt(zoneRefs_[k].m, *zoneRefs_[k].z, tracked_[v].pos);
            w[v * nz + k] = wk;
            if (wk > zoneRefs_[k].relevance) zoneRefs_[k].relevance = wk;
        }

    auto refOf = [&](u64 key) -> i32 {
        for (usize k = 0; k < nz; ++k) if (zoneRefs_[k].key == key) return static_cast<i32>(k);
        return -1;
    };
    auto release = [&](u32 s) {
        if (slotLive_[s] && control_.setZoneReverb) {
            ZoneReverb off = slotPushed_[s];
            off.reverb.wet = 0.0f;
            control_.setZoneReverb(control_.user, s, off);
        }
        slotKey_[s] = 0; slotIdle_[s] = 0.0f; slotLive_[s] = false;
    };

    // Slots whose zone is gone are freed; slots whose zone nobody is in age and are freed once their tail has had time to ring out.
    i32 slotRef[kMaxZoneReverbs];
    for (u32 s = 0; s < kMaxZoneReverbs; ++s) {
        slotRef[s] = slotKey_[s] ? refOf(slotKey_[s]) : -1;
        if (slotKey_[s] && slotRef[s] < 0) { release(s); continue; }
        if (!slotKey_[s]) continue;
        const ZoneRef& r = zoneRefs_[static_cast<usize>(slotRef[s])];
        if (r.relevance > 0.0f) slotIdle_[s] = 0.0f;
        else {
            slotIdle_[s] += dt;
            const f32 hold = r.z->decaySec > 0.25f ? r.z->decaySec : 0.25f;
            if (slotIdle_[s] > hold) { release(s); slotRef[s] = -1; }
        }
    }

    // Zones with a voice inside and no slot take a free one, or the slot of a zone nobody is in, or (higher priority only) the lowest one.
    std::vector<u32> want;
    for (usize k = 0; k < nz; ++k) {
        if (zoneRefs_[k].relevance <= 0.0f) continue;
        bool has = false;
        for (u32 s = 0; s < kMaxZoneReverbs; ++s) has = has || slotKey_[s] == zoneRefs_[k].key;
        if (!has) want.push_back(static_cast<u32>(k));
    }
    std::stable_sort(want.begin(), want.end(), [&](u32 a, u32 b) {
        if (zoneRefs_[a].priority != zoneRefs_[b].priority) return zoneRefs_[a].priority > zoneRefs_[b].priority;
        return zoneRefs_[a].relevance > zoneRefs_[b].relevance;
    });
    for (const u32 k : want) {
        i32 pick = -1;
        for (u32 s = 0; s < kMaxZoneReverbs && pick < 0; ++s) if (!slotKey_[s]) pick = static_cast<i32>(s);
        for (u32 s = 0; s < kMaxZoneReverbs; ++s)
            if (pick < 0 && slotKey_[s] && slotRef[s] >= 0 && zoneRefs_[static_cast<usize>(slotRef[s])].relevance <= 0.0f) pick = static_cast<i32>(s);
        if (pick < 0) {
            i32 low = -1;
            for (u32 s = 0; s < kMaxZoneReverbs; ++s) {
                if (slotRef[s] < 0) continue;
                const ZoneRef& r = zoneRefs_[static_cast<usize>(slotRef[s])];
                if (low < 0 || r.priority < zoneRefs_[static_cast<usize>(slotRef[low])].priority) low = static_cast<i32>(s);
            }
            if (low >= 0 && zoneRefs_[k].priority > zoneRefs_[static_cast<usize>(slotRef[low])].priority) pick = low;
        }
        if (pick < 0) continue;
        const u32 s = static_cast<u32>(pick);
        if (slotKey_[s]) release(s);
        slotKey_[s] = zoneRefs_[k].key;
        slotIdle_[s] = 0.0f;
        slotRef[s] = static_cast<i32>(k);
    }

    // A slot's settings follow its zone's component.
    for (u32 s = 0; s < kMaxZoneReverbs; ++s) {
        if (!slotKey_[s] || slotRef[s] < 0) continue;
        const CReverbZone& z = *zoneRefs_[static_cast<usize>(slotRef[s])].z;
        ZoneReverb zr;
        zr.reverb.wet = z.wet; zr.reverb.decaySec = z.decaySec; zr.reverb.damping = z.damping;
        zr.impulse = z.impulseSound > 0 ? static_cast<u32>(z.impulseSound) : 0u;
        zr.predelayMs = z.predelayMs;
        const ZoneReverb& p = slotPushed_[s];
        const bool same = slotLive_[s] && nearEqual(zr.reverb.wet, p.reverb.wet) && nearEqual(zr.reverb.decaySec, p.reverb.decaySec) &&
                          nearEqual(zr.reverb.damping, p.reverb.damping) && zr.impulse == p.impulse && nearEqual(zr.predelayMs, p.predelayMs);
        if (same) continue;
        if (control_.setZoneReverb) control_.setZoneReverb(control_.user, s, zr);
        slotPushed_[s] = zr;
        slotLive_[s] = true;
    }

    // Each tracked voice's shares of the slotted zones, by priority; what no slot took stays on the listener-side reverb.
    for (auto& e : sent_) e.second.seen = false;
    for (usize v = 0; v < tracked_.size(); ++v) {
        const Tracked& t = tracked_[v];
        f32 ws[kMaxZoneReverbs], shares[kMaxZoneReverbs];
        i32 prio[kMaxZoneReverbs];
        for (u32 s = 0; s < kMaxZoneReverbs; ++s) {
            ws[s] = slotRef[s] >= 0 ? w[v * nz + static_cast<usize>(slotRef[s])] : 0.0f;
            prio[s] = slotRef[s] >= 0 ? zoneRefs_[static_cast<usize>(slotRef[s])].priority : 0;
        }
        f32 remainder = 1.0f;
        zoneShares(ws, prio, kMaxZoneReverbs, shares, remainder);

        SentState* st = nullptr;
        for (auto& e : sent_) if (e.first == t.voice) { st = &e.second; break; }
        if (!st) {
            sent_.push_back({t.voice, SentState{}});
            st = &sent_.back().second;
            st->legacy = t.base;
        }
        st->seen = true;
        const f32 legacy = t.base * remainder;
        constexpr f32 kEps = 0.002f;
        for (u32 s = 0; s < kMaxZoneReverbs; ++s)
            if (std::fabs(shares[s] - st->send[s]) > kEps || (shares[s] == 0.0f && st->send[s] != 0.0f)) {
                st->send[s] = shares[s];
                if (control_.setVoiceZoneSend) control_.setVoiceZoneSend(control_.user, t.voice, s, shares[s]);
            }
        if (std::fabs(legacy - st->legacy) > kEps || (remainder == 1.0f && st->legacy != t.base)) {
            st->legacy = remainder == 1.0f ? t.base : legacy;
            if (control_.setVoiceReverbSend) control_.setVoiceReverbSend(control_.user, t.voice, st->legacy);
        }
        st->base = t.base;
    }

    // A voice that stopped asking (or ended) is let go; one still playing gets its own reverb send back.
    for (usize i = 0; i < sent_.size();) {
        SentState& st = sent_[i].second;
        if (st.seen) { ++i; continue; }
        const i32 voice = sent_[i].first;
        if (!control_.voicePlaying || control_.voicePlaying(control_.user, voice)) {
            for (u32 s = 0; s < kMaxZoneReverbs; ++s)
                if (st.send[s] != 0.0f && control_.setVoiceZoneSend) control_.setVoiceZoneSend(control_.user, voice, s, 0.0f);
            if (st.legacy != st.base && control_.setVoiceReverbSend) control_.setVoiceReverbSend(control_.user, voice, st.base);
        }
        sent_.erase(sent_.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

AudioSceneSystem& audioSceneSystem() {
    static AudioSceneSystem s;
    return s;
}

} // namespace aver::audio
