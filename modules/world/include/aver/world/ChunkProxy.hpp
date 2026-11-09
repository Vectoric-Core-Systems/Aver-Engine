// An HLOD proxy for a generated chunk (docs/LEVEL_STREAMING.md, plan W2 step 7): the same merger as the editor bake, run on a chunk's
// payload right after it is generated -- on a generation worker, since it is pure CPU and touches no scene, GPU or physics.
// Vertices are relative to the chunk's minimum corner (X and Y), the same convention as a baked cell proxy.
#pragma once

#include "aver/core/Types.hpp"
#include "aver/formats/OcHlodBake.hpp"
#include "aver/world/ChunkPayload.hpp"

#include <functional>
#include <string>
#include <vector>

namespace aver::world {

struct ChunkProxyOptions {
    // Budget and simplifier as the bake uses them (minTriangles, maxTriangles, simplify, dropSmallerCm, dropEnclosed ...).
    fmt::OcHlodOptions hlod;
    // The distance the proxy is wanted out to (feeds the triangle budget).
    f32 hlodCm = 0.0f;
    // Gathering stops once this much time has passed (checked before each mesh is fetched); at least one item is always taken.
    f64 timeBudgetMs = 8.0;
    usize maxItems = 4096;
    // The level's layer definitions: an entity in a layer that is not always on is left out (W10-6). Null = no layers.
    const std::vector<fmt::OcDataLayer>* layers = nullptr;
};

struct ChunkProxyResult {
    fmt::OcMeshData mesh;
    u32 items = 0;       // entities merged
    u32 skipped = 0;     // no mesh, not visible, layered out, or the loader had none
    bool truncated = false;   // the time or item cap ended gathering early
    f64 elapsedMs = 0;
    u32 trianglesIn = 0;
};

// `meshOf` maps an entity's mesh ObjectId to its geometry (null = unknown); the pointer need only live for the call. Returns false when
// nothing was left to merge. Deterministic for a given payload unless the time cap truncates.
bool buildChunkProxy(const ChunkPayload& payload, const std::function<const fmt::OcMeshData*(u64 objectId)>& meshOf,
                     const ChunkProxyOptions& opt, ChunkProxyResult& out, std::string* why = nullptr);

} // namespace aver::world
