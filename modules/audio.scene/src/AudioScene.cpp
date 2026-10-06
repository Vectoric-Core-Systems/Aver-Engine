#include "aver/audio/AudioScene.hpp"

#include "aver/core/Assert.hpp"

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

} // namespace

AudioControl abiAudioControl() {
    AudioControl c;
#if AVER_MODULE_AUDIO_ABI
    c.setReverb = &abiSetReverb;
    c.setVoiceOcclusion = &abiSetOcclusion;
    c.setVoicePosition = &abiSetPosition;
    c.voicePlaying = &abiPlaying;
    c.getListener = &abiGetListener;
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
            .field("occlusion", scene::FieldKind::F32, static_cast<u16>(offsetof(CAudioOcclusion, occlusion)), 0, true);
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

                // The listener in the zone's frame, in world centimetres along each axis; the
                // entity's scale stretches the volume.
                const Mat4& m = world.worldMatrix(pool->entityAt(i));
                const f32 d[3] = {lis[0] - m.m[3][0], lis[1] - m.m[3][1], lis[2] - m.m[3][2]};
                f32 local[3], axisLen[3];
                for (int k = 0; k < 3; ++k) {
                    const f32 ax = m.m[k][0], ay = m.m[k][1], az = m.m[k][2];
                    axisLen[k] = std::sqrt(ax * ax + ay * ay + az * az);
                    const f32 inv = axisLen[k] > 1.0e-6f ? 1.0f / axisLen[k] : 0.0f;
                    local[k] = (d[0] * ax + d[1] * ay + d[2] * az) * inv;
                }
                f32 extent[3];
                const ZoneShape shape = z->shape == static_cast<i32>(ZoneShape::Sphere) ? ZoneShape::Sphere : ZoneShape::Box;
                if (shape == ZoneShape::Sphere) {
                    const f32 s = std::fmax(axisLen[0], std::fmax(axisLen[1], axisLen[2]));
                    extent[0] = extent[1] = extent[2] = z->radiusCm * s;
                } else {
                    for (int k = 0; k < 3; ++k) extent[k] = z->halfExtentsCm[k] * axisLen[k];
                }
                z->weight = zoneWeight(zoneSignedDistance(shape, extent, local), z->blendDistanceCm);

                ReverbZoneInput in;
                in.weight = z->weight;
                in.priority = z->priority;
                in.params.wet = z->wet;
                in.params.decaySec = z->decaySec;
                in.params.damping = z->damping;
                inputs_.push_back(in);
            }
        }

        const ReverbParams blended = blendReverbZones(inputs_.data(), inputs_.size(), ambient_);
        const bool changed = !pushed_ || !nearEqual(blended.wet, applied_.wet) ||
                             !nearEqual(blended.decaySec, applied_.decaySec) ||
                             !nearEqual(blended.damping, applied_.damping);
        if (changed) {
            applied_ = blended;
            pushed_ = true;
            if (control_.setReverb) control_.setReverb(control_.user, blended);
        }
    }

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
                    c->measured = rayFn_ ? measureOcclusion(rayFn_, rayUser_, lis, pos, probe) : 0.0f;
                }
                c->occlusion = smoothOcclusion(c->occlusion, c->measured, dt, c->riseSec, c->fallSec);
                if (control_.setVoiceOcclusion) control_.setVoiceOcclusion(control_.user, c->voice, c->occlusion);
            }
        }
    }
}

AudioSceneSystem& audioSceneSystem() {
    static AudioSceneSystem s;
    return s;
}

} // namespace aver::audio
