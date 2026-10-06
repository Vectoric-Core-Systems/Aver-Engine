#pragma once
// A send reverb (parallel damped combs into series all-passes) and the zone volumes that drive it.
// The processor allocates only in init(); process() is audio-thread safe.
#include "aver/core/Types.hpp"

#include <vector>

namespace aver::audio {

struct ReverbParams {
    f32 wet      = 0.0f;    // linear gain on the reverb return; 0 turns the reverb off
    f32 decaySec = 1.5f;    // RT60
    f32 damping  = 0.4f;    // 0 bright .. 1 dark
};

class Reverb {
public:
    // Sizes the delay lines for a sample rate. Allocates.
    void init(u32 sampleRate);
    bool ready() const { return rate_ != 0; }
    // Sets decay and damping. Wet is applied by the caller. Cheap; call per block.
    void setParams(f32 decaySec, f32 damping);
    // Mono send in, stereo return out (adds nothing to the input; overwrites outL/outR).
    void process(const f32* in, f32* outL, f32* outR, u32 frames);
    // Clears the delay lines.
    void reset();

private:
    static constexpr u32 kCombs = 8;
    static constexpr u32 kAllpass = 4;
    struct Line { std::vector<f32> buf; u32 pos = 0; f32 store = 0.0f; f32 feedback = 0.0f; };

    u32 rate_ = 0;
    f32 decay_ = -1.0f, damping_ = -1.0f;
    f32 damp_ = 0.0f;
    Line combL_[kCombs], combR_[kCombs];
    Line apL_[kAllpass], apR_[kAllpass];
};

// ---- zones ----

enum class ZoneShape : u32 { Box = 0, Sphere = 1 };

// Signed distance (cm) from a point to a zone's surface: negative inside. The point is in the
// zone's own frame, centred on the zone, with half extents (box) or radius (sphere, extent[0]).
f32 zoneSignedDistance(ZoneShape shape, const f32 halfExtents[3], const f32 localPoint[3]);
// 1 inside the volume, falling smoothly to 0 at `blendDistanceCm` outside its surface.
f32 zoneWeight(f32 signedDistanceCm, f32 blendDistanceCm);

struct ReverbZoneInput {
    f32 weight   = 0.0f;
    i32 priority = 0;
    ReverbParams params;
};

// Priority blend: the highest-priority zone takes its weight first and lower zones fill what is
// left, with the remainder going to `ambient`. Wet blends with the ambient; decay and damping
// blend among the zones only, so a zone fading out keeps its own tail length.
ReverbParams blendReverbZones(const ReverbZoneInput* zones, usize count, const ReverbParams& ambient);

} // namespace aver::audio
