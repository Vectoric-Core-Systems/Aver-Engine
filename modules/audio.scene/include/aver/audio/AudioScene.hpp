#pragma once
// Scene-side audio: reverb zone volumes and per-emitter occlusion, ticked once a frame.
// Components are registered at runtime (like CSynapsePerception), all scalars.
#include "aver/audio/Music.hpp"
#include "aver/audio/Reverb.hpp"
#include "aver/core/Types.hpp"
#include "aver/scene/World.hpp"

#include <vector>

namespace aver::audio {

// A volume that gives the listener a reverb while inside it. The weight is 1 inside and falls to 0
// at blendDistanceCm outside the surface. The zone's entity transform places and scales it.
struct CReverbZone {
    i32 enabled = 1;
    i32 shape = 0;                                   // ZoneShape: 0 box, 1 sphere
    f32 halfExtentsCm[3] = {500.0f, 500.0f, 300.0f}; // box, in the entity's local axes
    f32 radiusCm = 500.0f;                           // sphere
    f32 blendDistanceCm = 200.0f;
    i32 priority = 0;                                // higher wins where zones overlap
    f32 wet = 0.35f;
    f32 decaySec = 1.8f;
    f32 damping = 0.4f;
    f32 weight = 0.0f;                               // runtime: last computed weight
};

// Drives one voice's occlusion from line-of-sight rays between the listener and this entity.
struct CAudioOcclusion {
    i32 enabled = 1;
    i32 voice = 0;                  // the voice to drive; 0 when none or ended
    i32 followEntity = 1;           // also move the voice to the entity each tick
    i32 rayCount = 5;               // 1 centre ray, plus a ring of rayCount-1 around the source
    f32 probeRadiusCm = 50.0f;
    f32 thinkIntervalSec = 0.1f;
    f32 riseSec = 0.08f;            // how quickly occlusion rises when something blocks
    f32 fallSec = 0.25f;            // and clears
    f32 thinkAccumulatorSec = 0.0f; // runtime
    f32 measured = 0.0f;
    f32 occlusion = 0.0f;
};

// How the system reaches the mixer. The default is the audio C ABI; tests install fakes.
struct AudioControl {
    void (*setReverb)(void* user, const ReverbParams& p) = nullptr;
    void (*setVoiceOcclusion)(void* user, i32 voice, f32 occlusion) = nullptr;
    void (*setVoicePosition)(void* user, i32 voice, f32 x, f32 y, f32 z) = nullptr;
    bool (*voicePlaying)(void* user, i32 voice) = nullptr;
    // Writes the listener position into out[3]. Null: the position set by setListenerPosition.
    void (*getListener)(void* user, f32 out[3]) = nullptr;
    void* user = nullptr;
};

// Bound to the audio C ABI. All members null when the build has no ABI.
AudioControl abiAudioControl();

class AudioSceneSystem {
public:
    AudioSceneSystem();

    // Registers both components, idempotently.
    u32 registerComponents(scene::World& world);
    u32 reverbZoneType() const { return zoneType_; }
    u32 occlusionType() const { return occType_; }

    // Adds a component with its real defaults applied (ComponentPool::add only zero-fills).
    CReverbZone* attachReverbZone(scene::World& world, scene::Entity e);
    CAudioOcclusion* attachOcclusion(scene::World& world, scene::Entity e, i32 voice);

    void setControl(const AudioControl& c) { control_ = c; }
    // Replaces the occlusion ray. Default: a Jolt raycast when physics is built in, else none.
    void setRayFn(RayBlockFn fn, void* user) { rayFn_ = fn; rayUser_ = user; }
    void setListenerPosition(f32 x, f32 y, f32 z) { listener_[0] = x; listener_[1] = y; listener_[2] = z; }
    // What the reverb is outside every zone.
    void setAmbientReverb(const ReverbParams& p) { ambient_ = p; }

    // Blends the zones around the listener into one reverb, and updates every emitter's occlusion.
    void tick(scene::World& world, f32 dt);

    const ReverbParams& appliedReverb() const { return applied_; }

private:
    u32 zoneType_ = 0, occType_ = 0;
    AudioControl control_;
    RayBlockFn rayFn_ = nullptr;
    void* rayUser_ = nullptr;
    f32 listener_[3] = {0.0f, 0.0f, 0.0f};
    ReverbParams ambient_{};
    ReverbParams applied_{};
    bool pushed_ = false;
    std::vector<ReverbZoneInput> inputs_;
};

// The process-global system, matching synapse::perceptionSystem().
AudioSceneSystem& audioSceneSystem();

} // namespace aver::audio
