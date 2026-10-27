#pragma once
// PhysicsShapeCache: disk cache for physics mesh shape blobs (the BVH build is the costly part of a placement's body).
// Derived data: deleting the directory costs one rebuild per mesh. A miss is never an error; any missing or damaged file
// answers "not cached" and the caller builds the shape.
// Off by default (PHYSICS.SHAPECACHE, or AVER_PHYS_SHAPE_CACHE=1) until restore is measured against build.
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver::world {

// "<projectDir>/Saved/DerivedDataCache/Physics"; empty when projectDir is empty (= no cache).
std::string physicsShapeCacheDir(const std::string& projectDir);

// FNV-1a 64 of sourceKey, the shape cache version and the triangle counts. sourceKey must cover everything the triangles
// derive from (GameContent passes its collision cache key).
// The counts catch a material slot that stopped colliding (the slot filter changes the built shape). Never 0.
u64 physicsShapeKey(u64 sourceKey, u32 shapeCacheVersion, u32 vertexCount, u32 indexCount);

// <dir>/<16 hex digits>.pshp
std::string physicsShapeCachePath(const std::string& dir, u64 key);

// False is a miss, whatever the reason.
bool loadPhysicsShapeBlob(const std::string& dir, u64 key, std::vector<u8>& outBlob);

// Writes via a temporary beside the target and a swap; a crash leaves the previous file or none, never half of one.
bool storePhysicsShapeBlob(const std::string& dir, u64 key, const u8* blob, usize size, std::string* why = nullptr);

// Counters since process start or the last reset (log and bench tool). restoreMs is hits only; buildMs is misses.
struct PhysicsShapeCacheStats {
    u32 hits = 0;
    u32 misses = 0;
    u32 stores = 0;
    u32 unsaveable = 0;    // shapes the physics module refused to save (a palette, per-triangle data)
    double restoreMs = 0;
    double buildMs = 0;
};
PhysicsShapeCacheStats physicsShapeCacheStats();
void resetPhysicsShapeCacheStats();

#if AVER_MODULE_PHYSICS
// Restores from the cache when cacheDir and key are set, the cache is enabled and the blob exists; otherwise builds and stores.
// Returns 0 on build failure. Safe from a worker thread (the shape table is guarded).
// With the cache off this is aver_phys_create_mesh_shape.
i32 createMeshShapeCached(const f32* verticesXyz, i32 vertexCount, const i32* indices, i32 indexCount,
                          const std::string& cacheDir, u64 key);

// Builds the key from sourceKey (0 = not cacheable, e.g. no file behind it: the cache is skipped).
// With cacheDir empty or sourceKey 0 this is aver_phys_create_mesh_shape.
i32 createMeshShapeFromSource(const f32* verticesXyz, i32 vertexCount, const i32* indices, i32 indexCount,
                              const std::string& cacheDir, u64 sourceKey);
#endif

} // namespace aver::world
