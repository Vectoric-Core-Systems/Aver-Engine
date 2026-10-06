// 1D and 2D blend spaces: sample points with a clip each, triangulated once, weighted per query.
//
// Pure arithmetic over an asset struct, so weights and sync phase are decidable with no skeleton
// and no GPU. Design notes and the asset format live in docs/ANIM_BLEND_STATE.md.
#pragma once

#include "aver/anim/Pose.hpp"

#include <array>
#include <string>
#include <vector>

namespace aver::anim {

// A foot-down (or any cyclic) landmark inside one clip, in that clip's own seconds.
struct SyncMarker {
    std::string name;
    f32 time = 0.0f;
};

struct BlendSample {
    std::string clip;             // asset reference, resolved by the host
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 rate = 1.0f;              // authored speed multiplier for this clip
    std::vector<SyncMarker> markers;
};

struct BlendAxis {
    std::string name;             // parameter name a state machine binds
    f32 min = 0.0f;
    f32 max = 1.0f;
    f32 smoothing = 0.0f;         // seconds; 0 follows the input exactly
};

struct BlendSpaceAsset {
    std::string name;
    u8 dims = 1;                  // 1 or 2
    BlendAxis axisX;
    BlendAxis axisY;
    bool syncMarkers = true;
    std::vector<BlendSample> samples;

    bool valid(std::string* why = nullptr) const;
};

struct SampleWeight {
    u32 index = 0;
    f32 weight = 0.0f;
};

// Built once per asset. `order` is the 1D sort (and the collinear fallback); `tris` the 2D mesh.
struct BlendTopology {
    std::vector<u32> order;
    std::vector<std::array<u32, 3>> tris;
    std::vector<Vec2> pts;        // axis-normalised sample positions
    Vec2 dir{1, 0};               // collinear fallback direction, normalised space
    bool collinear = false;
};

BlendTopology buildBlendTopology(const BlendSpaceAsset& space);

// Weights of the samples around (x, y). Always sums to 1 when the space has any sample, and an
// outside query is clamped to the nearest point of the space rather than extrapolated.
void computeBlendWeights(const BlendSpaceAsset& space, const BlendTopology& topo, f32 x, f32 y,
                         std::vector<SampleWeight>& out);

// One clip's cycle as sync sees it: marker times starting from the shared phase-0 marker.
struct SyncCycle {
    f32 duration = 0.0f;
    std::vector<f32> times;       // empty = no markers, phase maps linearly
};

// Rotates `markers` so the cycle starts at a marker named like names[0] and checks the whole name
// sequence matches. Returns false (and an empty cycle) when it does not, which means "unsynced".
bool makeSyncCycle(const std::vector<SyncMarker>& markers, f32 duration,
                   const std::vector<std::string>& names, SyncCycle& out);

f32 syncPhaseToTime(const SyncCycle& c, f32 phase);
f32 syncTimeToPhase(const SyncCycle& c, f32 time);

// Plays a blend space: smooths the inputs, picks weights, advances one shared phase and maps it into
// each sample's clip through its sync markers.
class BlendSpacePlayer {
public:
    // `clips` is parallel to space.samples; entries may be null (that sample poses as rest).
    void bind(const BlendSpaceAsset* space, std::vector<const fmt::OcAnimation*> clips);
    // Test seam: durations without clips.
    void bindDurations(const BlendSpaceAsset* space, std::vector<f32> durations);

    void setInput(f32 x, f32 y) { tx_ = x; ty_ = y; }
    void snapInput() { x_ = tx_; y_ = ty_; }
    void setPhase(f32 p);

    void advance(f32 dt);

    f32 x() const { return x_; }
    f32 y() const { return y_; }
    f32 phase() const { return phase_; }
    f32 cycles() const { return elapsed_; }            // unwrapped, for exit times
    f32 duration() const { return blendedDuration_; }  // seconds per cycle at the current weights
    const std::vector<SampleWeight>& weights() const { return weights_; }
    f32 sampleTime(u32 sampleIndex) const;

    void evaluate(const fmt::OcSkeleton& skel, Pose& out) const;

private:
    void refresh();

    const BlendSpaceAsset* space_ = nullptr;
    BlendTopology topo_;
    std::vector<const fmt::OcAnimation*> clips_;
    std::vector<f32> durations_;
    std::vector<SampleWeight> weights_;
    std::vector<SyncCycle> sync_;
    f32 tx_ = 0, ty_ = 0, x_ = 0, y_ = 0;
    f32 phase_ = 0.0f;
    f32 elapsed_ = 0.0f;
    f32 blendedDuration_ = 1.0f;
};

} // namespace aver::anim
