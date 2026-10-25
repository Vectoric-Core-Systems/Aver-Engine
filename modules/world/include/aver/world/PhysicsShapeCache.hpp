#pragma once
// PhysicsShapeCache -- the disk half of the physics shape binary cache (physics_state_abi.h).
//
// A mesh collision shape is mostly its BVH, and building that is the costly part of a placement's body (the reason the
// shapes are shared per mesh id at all). aver_phys_shape_save / _restore turn a built shape into a blob and back; this file
// keeps the blobs: one AVR1 container (subtype PSHP, one chunk "DATA") per mesh, in
//
//     <project>/Saved/DerivedDataCache/Physics/<16 hex digits of the key>.pshp
//
// beside the Collision cache whose triangles the shapes are built from. DERIVED DATA, never authored: deleting the directory
// costs one rebuild per mesh. A MISS IS NEVER AN ERROR -- a missing, short, foreign, stale or damaged file all answer "not
// cached" and the caller builds the shape the slow way.
//
// OFF BY DEFAULT (aver_phys_shape_cache_enabled(): PHYSICS.SHAPECACHE, or AVER_PHYS_SHAPE_CACHE=1 for tools). The default
// waits for a measurement of restore against build on the real levels (the PhysicsBenchTool shapes in tests/physics).
//
// The file layer below needs only Core and Formats. createMeshShapeCached needs the physics module and exists only where it does.
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver::world {

// "<projectDir>/Saved/DerivedDataCache/Physics"; empty when projectDir is empty (= no cache).
std::string physicsShapeCacheDir(const std::string& projectDir);

// The cache key of a mesh's shape: FNV-1a 64 over `sourceKey`, the physics module's shape cache version
// (aver_phys_shape_cache_version) and the triangle counts the shape is built from. `sourceKey` is the CALLER'S hash of
// everything the triangles derive from -- GameContent passes the one it already keys its collision cache by (mesh path, source
// size, source mtime, collision cache version), so a stale source invalidates both caches. The counts are what notices a
// material slot that stopped colliding: the slot filter removes triangles, so the same file builds a different shape. Never 0.
u64 physicsShapeKey(u64 sourceKey, u32 shapeCacheVersion, u32 vertexCount, u32 indexCount);

// "<dir>/<16 hex digits>.pshp".
std::string physicsShapeCachePath(const std::string& dir, u64 key);

// Reads the blob stored under `key`. False is a miss, whatever the reason (no directory, no file, a bad container or subtype).
bool loadPhysicsShapeBlob(const std::string& dir, u64 key, std::vector<u8>& outBlob);

// Writes the blob under `key`, creating the directory. The write goes through a temporary beside the target and a swap, so a
// crash leaves the previous file or none, never half of one. False with `why` on failure; the caller carries on uncached.
bool storePhysicsShapeBlob(const std::string& dir, u64 key, const u8* blob, usize size, std::string* why = nullptr);

// What the cache has done since the process started (or the last reset), for the log and the bench tool.
struct PhysicsShapeCacheStats {
    u32 hits = 0;          // shapes restored from a blob
    u32 misses = 0;        // looked up and not found / not usable
    u32 stores = 0;        // blobs written
    u32 unsaveable = 0;    // shapes the physics module refused to save (a palette, per-triangle data)
    double restoreMs = 0;  // total time in aver_phys_shape_restore, hits only
    double buildMs = 0;    // total time building shapes on a miss
};
PhysicsShapeCacheStats physicsShapeCacheStats();
void resetPhysicsShapeCacheStats();

#if AVER_MODULE_PHYSICS
// A mesh collision shape (aver_phys_create_mesh_shape's: engine-space cm vertices, 3 indices per triangle, no surface palette),
// restored from the cache when `cacheDir` is non-empty, `key` is non-zero, the cache is enabled and the blob is there, and
// otherwise built and stored. Returns the shape handle, or 0 when the build failed. With the cache off it IS
// aver_phys_create_mesh_shape. Safe from a worker thread (the shape table is guarded).
i32 createMeshShapeCached(const f32* verticesXyz, i32 vertexCount, const i32* indices, i32 indexCount,
                          const std::string& cacheDir, u64 key);

// The call the level loaders make: createMeshShapeCached with the key built here from the caller's `sourceKey` (0 = this mesh is
// not cacheable, e.g. no file behind it: the cache is skipped) and the physics module's shape cache version and the triangle counts
// (physicsShapeKey). With `cacheDir` empty or `sourceKey` 0 it IS aver_phys_create_mesh_shape.
i32 createMeshShapeFromSource(const f32* verticesXyz, i32 vertexCount, const i32* indices, i32 indexCount,
                              const std::string& cacheDir, u64 sourceKey);
#endif

} // namespace aver::world
