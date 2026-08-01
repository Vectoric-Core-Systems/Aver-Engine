#pragma once
// --refl-test: proves ray-traced reflections are GLOBAL -- that they reach geometry the voxel cone
// tracer cannot reach at all.
//
// "Reflections are ray traced now" is easy to claim and hard to demonstrate: a ray tracer and a
// cone tracer both produce a plausible reflection of nearby geometry, and comparing them tells you
// only that two approximations differ. The property actually worth proving is the one that was
// broken -- traceCone stops dead at the voxel volume (`if (!insideVolume(uvw)) break;`), so anything
// outside it was never reflected and the result fell back to sky.
//
// So the experiment puts a beacon OUTSIDE THE VOLUME, on the mirror's reflection vector, and runs
// the same scene change through both tracers:
//
//   ray  + beacon present  vs  ray  + beacon hidden   -- MUST differ
//   cone + beacon present  vs  cone + beacon hidden   -- MUST NOT differ
//
// The second half is what makes it a proof about globality rather than about brightness. It shows
// the same change in the world being visible to one tracer and invisible to the other, which is
// exactly the defect, stated as a measurement. Neither half needs a baseline: both are relations
// between two frames of one run.
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"

namespace aver { class Engine; }

namespace aver::editor {

// Drives a mirror, a distant beacon, and the two tracers.
class ReflTest {
public:
    // Where the beacon must sit: along the reflection of the camera's view in the mirror plane,
    // far enough out to be outside any sane voxel volume. Computed rather than hardcoded so the
    // geometry stays correct if the editor's default camera ever moves.
    static Vec3 beaconPosition(const Vec3& camPos, const Vec3& mirrorPoint);

    // Half-extent of the voxel volume this must sit outside of, for the report to state the margin.
    void setVolumeExtent(f32 e) { volumeExtent_ = e; }
    void setBeaconPosition(const Vec3& p) { beaconPos_ = p; }

    // Whether the beacon should be drawn, and whether the renderer should ray trace, RIGHT NOW.
    // The host applies both; this class owns the schedule because the schedule IS the experiment.
    bool beaconVisible() const { return phase_ == kRayWith || phase_ == kConeWith; }
    bool wantRayTracing() const { return phase_ == kRayWith || phase_ == kRayWithout; }

    void tick(Engine& e, f32 vpX, f32 vpY, f32 vpW, f32 vpH);
    bool finished() const { return done_; }

private:
    void report();

    // Order matters only in that each tracer sees both worlds; the two ray phases are adjacent so
    // the renderer toggles its settings twice rather than four times.
    enum Phase : u32 { kRayWith = 0, kRayWithout = 1, kConeWith = 2, kConeWithout = 3, kPhases = 4 };

    // Probes across the mirror. Several, because exactly where the beacon lands in the reflection
    // depends on the projection, and the assertion is over the set.
    static constexpr u32 kProbes = 9;
    struct Probe {
        f32  u = 0.5f, v = 0.5f;
        f32  c[kPhases][4] = {};
        bool have[kPhases] = {};
    };

    Probe probes_[kProbes];
    Vec3  beaconPos_{0, 0, 0};
    f32   volumeExtent_ = 0.0f;
    u32   step_ = 0;
    u32   phase_ = kRayWith;
    bool  done_ = false;
};

} // namespace aver::editor
