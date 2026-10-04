// Which chunk owns which entity, and the round trip through ChunkPayload.
//
// THREE RULES, and each one exists because breaking it is a crash or an ambiguity rather than a
// glitch:
//
//   1. OWNERSHIP IS BY THE ROOT'S ORIGIN -- its own transform position, not its bounds. Bounds
//      overlap, origins do not, and an entity that belongs to two chunks is an entity that gets
//      saved twice and loaded twice.
//
//   2. A HIERARCHY BELONGS ENTIRELY TO ITS ROOT'S CHUNK, whatever its children's positions.
//      CHierarchy is intrusive -- parent/child/sibling links live in the components themselves -- so
//      a child loaded without its parent is a dangling handle, which is a crash and not a hole.
//
//   3. AN ENTITY BIGGER THAN A CHUNK IS LEGAL and stays in its origin chunk, but is also listed as
//      OVERSIZE so a streamer can keep it resident regardless of distance. A 200 m bridge in a 16 m
//      chunk world would otherwise pop out of existence while the camera is still standing on it.
//      This is the hidden cost of small chunks, and it is why the list is reported rather than
//      silently handled.
#pragma once

#include "aver/core/Types.hpp"
#include "aver/world/ChunkCoord.hpp"
#include "aver/world/ChunkPayload.hpp"

#if AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"

#  include <unordered_map>
#  include <vector>

namespace aver::world {

struct PartitionOptions {
    i32 chunkSizeCm = kDefaultChunkSizeCm;

    // An entity whose bounding radius exceeds this many chunks is listed oversize. 1 means "bigger
    // than the chunk that owns it".
    f32 oversizeChunks = 1.0f;

    // Radius assumed for an entity whose CMeshRenderer bounds are degenerate.
    //
    // NOT HYPOTHETICAL: neither level loader fills aabbMin/aabbMax -- both leave them zero and the
    // draw walk re-copies them from the mesh table every frame (GameRender.cpp:67-72). So at
    // partition time, today, essentially every entity has no bounds. Leaving it at 0 means "assume
    // nothing is oversize", which is the honest default; `unknownBounds` below reports how often it
    // was used, so a caller can tell "no oversize entities" from "no bounds to judge by".
    f32 assumeRadiusCm = 0.0f;
};

struct Partition {
    // Roots per chunk. Children are NOT listed -- they travel with their root, by rule 2.
    std::unordered_map<ChunkCoord, std::vector<scene::Entity>> chunks;
    // Roots that are bigger than a chunk. These appear in `chunks` too: oversize changes residency
    // policy, not ownership.
    std::vector<scene::Entity> oversize;

    u32 rootCount = 0;
    u32 entityCount = 0;      // roots plus every descendant, i.e. what a flat load produced
    u32 unknownBounds = 0;    // roots whose size could not be judged; see assumeRadiusCm
};

// The chunk that owns `e`, following it up to its root first.
ChunkCoord ownerChunkOf(const scene::World& w, scene::Entity e, i32 chunkSizeCm);

// Partitions every live entity in `w`.
Partition partitionWorld(const scene::World& w, const PartitionOptions& opt = {});

// Captures every chunk of a partition. The map is keyed the same way, so a caller can hand one
// chunk's payload to a writer without re-deriving anything.
std::unordered_map<ChunkCoord, ChunkPayload> captureAll(const scene::World& w, const Partition& part,
                                                        i32 chunkSizeCm);

} // namespace aver::world

#endif // AVER_MODULE_SCENE
