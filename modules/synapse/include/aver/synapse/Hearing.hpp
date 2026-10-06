#pragma once
// Hearing, as pure arithmetic: how loud a noise is at a listener, and what a listener remembers.
//
// A noise has a LOUDNESS that is its audible radius in centimetres for a listener of sensitivity 1
// in the open. Level falls off linearly to zero at that radius. Each occluder on the line from the
// noise to the ear (walls found by whoever supplies the OcclusionFn, usually a physics raycast)
// multiplies the radius by `occlusionFactor`.
//
// Memory is a short list of last-known positions per listener. Confidence decays linearly to zero
// over `memorySec`; the entry is then forgotten. Aver.Synapse.Scene's HearingSystem owns one
// HearingMemory per listener and reports changes through IHearingMemorySink, the seam a blackboard
// binds to (see docs/AI_CROWDS_HEARING_COVER.md).
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"

#include <vector>

namespace aver::synapse {

struct NoiseEvent {
    Vec3 pos;
    f32  loudnessCm = 1000.0f;   // audible radius for sensitivity 1, unoccluded
    u32  tag = 0;                // game-defined kind: footstep, gunshot, ...
    u32  source = 0;             // emitting entity, 0 if none
};

struct HearingProfile {
    f32 sensitivity     = 1.0f;     // scales the audible radius
    f32 maxRangeCm      = 5000.0f;  // the ear's own hard limit
    f32 occlusionFactor = 0.5f;     // radius multiplier per occluder
    u32 tagMask         = 0xFFFFFFFFu;   // bit (tag & 31) must be set to hear a tag
};

// How many occluders lie between two points. 0 means a clear line. Never null-checked for
// `user`; the function owns its meaning.
using OcclusionFn = u32 (*)(void* user, const Vec3& from, const Vec3& to);

// Level in (0,1] at which `ev` is heard at `ear`, or 0 when it is not heard. `outDistanceCm`
// receives the straight-line distance when non-null. Cheap rejects (tag, range) run before the
// occlusion query.
f32 hearingLevel(const NoiseEvent& ev, const Vec3& ear, const HearingProfile& p, OcclusionFn occlusion,
                 void* occlusionUser, f32* outDistanceCm = nullptr);

struct HeardMemory {
    Vec3 pos;
    f32  level = 0.0f;        // level when last heard
    u32  tag = 0;
    u32  source = 0;
    f32  ageSec = 0.0f;       // since last heard
    f32  confidence = 1.0f;   // 1 fresh, 0 forgotten
};

// What a listener is told about its own memory. `listener` is the entity id the system was given.
class IHearingMemorySink {
public:
    virtual ~IHearingMemorySink() = default;
    // A noise was heard: a new entry, or an existing one refreshed.
    virtual void onHeard(u32 listener, const HeardMemory& m) = 0;
    // An entry's confidence reached zero and it was dropped.
    virtual void onForgotten(u32 listener, const HeardMemory& m) = 0;
};

class HearingMemory {
public:
    explicit HearingMemory(u32 capacity = 4) : capacity_(capacity ? capacity : 1) {}

    // Records a noise. An entry with the same source and tag, or (for sourceless noises) the same
    // tag within `mergeRadiusCm`, is refreshed in place; otherwise a new entry is added, replacing
    // the lowest-confidence one when full. Returns the stored entry.
    const HeardMemory& remember(const Vec3& pos, f32 level, u32 tag, u32 source, f32 mergeRadiusCm = 150.0f);

    // Ages every entry by dt. Entries whose confidence reaches zero are removed and appended to
    // `forgotten` when it is non-null.
    void tick(f32 dt, f32 memorySec, std::vector<HeardMemory>* forgotten = nullptr);

    // Most relevant entry: highest confidence, then loudest, then newest. Null when empty.
    const HeardMemory* best() const;

    const std::vector<HeardMemory>& entries() const { return entries_; }
    void clear() { entries_.clear(); }

    // How far from the remembered position the source may have moved, growing with age.
    static f32 searchRadiusCm(const HeardMemory& m, f32 baseCm = 100.0f, f32 spreadCmPerSec = 150.0f) {
        return baseCm + m.ageSec * spreadCmPerSec;
    }

private:
    u32 capacity_;
    std::vector<HeardMemory> entries_;
};

} // namespace aver::synapse
