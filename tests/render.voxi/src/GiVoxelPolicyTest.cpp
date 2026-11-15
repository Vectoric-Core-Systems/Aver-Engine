// GiVoxelPolicy: the pure rules behind the live voxel-resolution rebuild and the skinned-draw refresh bucket. No GPU.
#include "aver/voxi/GiVoxelPolicy.hpp"

#include <cstdio>

using namespace aver;
using namespace aver::voxi;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

int main() {
    check(clampVoxelResolution(0) == 32 && clampVoxelResolution(31) == 32, "below the floor clamps to 32");
    check(clampVoxelResolution(128) == 128 && clampVoxelResolution(512) == 512, "in-range edges pass through");
    check(clampVoxelResolution(513) == 512 && clampVoxelResolution(0xFFFFFFFFu) == 512, "above the ceiling clamps to 512");

    check(voxelResolutionRebuildDue(256, 128, 0), "a different edge than built is due");
    check(!voxelResolutionRebuildDue(128, 128, 0), "the built edge is not due");
    check(!voxelResolutionRebuildDue(512, 128, 512), "the edge that just failed to allocate is not retried");
    check(voxelResolutionRebuildDue(256, 128, 512), "a different request after a failure is due");
    check(!voxelResolutionRebuildDue(256, 0, 0), "no volume built yet: init creates it, not the live path");

    check(giSkinBucket(12345, 0) == 0, "interval 0: skinned draws are out, bucket 0");
    check(giSkinBucket(0, 60) == 1 && giSkinBucket(59, 60) == 1, "one bucket across an interval");
    check(giSkinBucket(60, 60) == 2, "the bucket moves at the interval boundary");
    check(giSkinBucket(0, 1) == giSkinBucket(29, 1) && giSkinBucket(30, 1) == giSkinBucket(0, 1) + 1,
          "an interval below the floor is floored at 30 ticks");
    check(giSkinBucket(1, 1000000) == 1, "a huge interval stays in bucket 1 for a long time");

    std::printf(g_fail ? "=== %d FAILED ===\n" : "=== all passed ===\n", g_fail);
    return g_fail ? 1 : 0;
}
