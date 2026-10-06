#pragma once
// Hearing in a live scene: noise events are queued by anything (gameplay, footsteps, the AN_
// emit-noise node), and each tick every CSynapseHearing listener decides which it heard. Heard
// noises become last-known-position memories that decay; the strongest is mirrored on the
// component so graphs and BT conditions can read it, and every change is reported through
// IHearingMemorySink -- the seam a blackboard binds to.
//
// Occlusion defaults to physics raycasts (up to four occluders counted along the line) when
// Aver.Physics is built in, and to "none" otherwise. A host may replace it with setOcclusion.
#include "aver/scene/World.hpp"
#include "aver/synapse/Hearing.hpp"
#include "aver/synapse/SynapsePerception.hpp"   // NotifyFn

#include <unordered_map>
#include <vector>

namespace aver::synapse {

// All 4-byte scalars (see CSynapsePerception for why).
struct CSynapseHearing {
    f32 sensitivity  = 1.0f;
    f32 maxRangeCm   = 5000.0f;
    i32 tagMask      = -1;          // bit (tag & 31); all tags by default
    f32 memorySec    = 8.0f;
    f32 earHeightCm  = 160.0f;
    // Outputs, written by the tick: the strongest live memory.
    i32 hasMemory    = 0;
    f32 heardXCm = 0.0f, heardYCm = 0.0f, heardZCm = 0.0f;
    f32 heardLevel   = 0.0f;
    i32 heardTag     = 0;
    i32 heardSource  = 0;
    f32 confidence   = 0.0f;
    f32 timeSinceHeardSec = -1.0f;  // -1 = nothing remembered
    i32 heardCount   = 0;           // noises heard so far (wraps)
};

class HearingSystem {
public:
    HearingSystem();

    u32 registerComponents(scene::World& world);
    u32 componentType() const { return type_; }
    CSynapseHearing* attach(scene::World& world, scene::Entity e);

    // Queues a noise for the next tick. Past maxQueue the noise is dropped and counted.
    bool emit(const NoiseEvent& ev);
    void setMaxQueue(u32 n) { maxQueue_ = n; }
    u32 droppedCount() const { return dropped_; }
    u32 pendingCount() const { return static_cast<u32>(queue_.size()); }

    // Replaces the occlusion query (null: none).
    void setOcclusion(OcclusionFn fn, void* user) { occlusion_ = fn; occlusionUser_ = user; }
    void setOcclusionFactor(f32 f) { occlusionFactor_ = f; }
    void setMemoryCapacity(u32 n) { capacity_ = n; }
    void setMemorySink(IHearingMemorySink* s) { sink_ = s; }
    // "OnHearNoise" fires through this, with the same signature the perception system uses.
    void setNotifySink(NotifyFn fn, void* user) { notify_ = fn; notifyUser_ = user; }

    // Decays memories, then lets every listener hear the queued noises, then publishes.
    void tick(scene::World& world, f32 dt);

    const HearingMemory* memoryOf(scene::Entity e) const;
    // Drops queued noises and every listener's memories, without notifying the sink.
    void reset() { queue_.clear(); memories_.clear(); dropped_ = 0; }
    // Drops one listener's memories (used when a graph wants "forget everything").
    void forget(scene::Entity e);

private:
    u32 type_ = 0;
    std::vector<NoiseEvent> queue_;
    u32 maxQueue_ = 256;
    u32 dropped_ = 0;
    u32 capacity_ = 4;
    f32 occlusionFactor_ = 0.5f;
    OcclusionFn occlusion_ = nullptr;
    void* occlusionUser_ = nullptr;
    IHearingMemorySink* sink_ = nullptr;
    NotifyFn notify_ = nullptr;
    void* notifyUser_ = nullptr;
    // Keyed by the full entity handle and pruned every tick, like the other Synapse side tables.
    std::unordered_map<scene::Entity, HearingMemory> memories_;
    std::vector<HeardMemory> forgotten_;
};

} // namespace aver::synapse
