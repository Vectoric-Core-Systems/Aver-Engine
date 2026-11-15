// Pure rules of the GI volume that VoxiRenderer applies: which resolution a request means, whether a live rebuild is due,
// and the pose bucket skinned draws hash into the rebuild key. No GPU; tests/render.voxi GiVoxelPolicyTest.
#pragma once

#include "aver/core/Types.hpp"

namespace aver::voxi {

constexpr u32 kVoxelResMin = 32;
constexpr u32 kVoxelResMax = 512;

// The edge createVoxelVolume can honour for a requested one.
constexpr u32 clampVoxelResolution(u32 r) { return r < kVoxelResMin ? kVoxelResMin : (r > kVoxelResMax ? kVoxelResMax : r); }

// A live rebuild is due when a volume exists at another edge than wanted, and wanted is not the edge that just failed to allocate.
constexpr bool voxelResolutionRebuildDue(u32 wanted, u32 built, u32 failed) {
    return built != 0 && wanted != built && wanted != failed;
}

// Skinned draws are re-baked at most this often (GI ticks) however small an interval is asked for.
constexpr u32 kMinGiSkinnedInterval = 30;

// Which refresh window `tick` falls in; 0 = skinned draws are not voxelised. The window number changes once per interval, so
// the rebuild key moves once per interval while a skinned draw is in the volume and not otherwise.
constexpr u64 giSkinBucket(u64 tick, u32 interval) {
    if (interval == 0) return 0;
    const u64 n = interval < kMinGiSkinnedInterval ? kMinGiSkinnedInterval : interval;
    return tick / n + 1;
}

} // namespace aver::voxi
