#pragma once
// One node's geometry: a surface grid plus a skirt around its rim. Pure CPU -- it emits plain arrays.
#include "aver/landscape/LandscapeTree.hpp"

namespace aver::landscape {

// The vertex this module emits. Layout-identical to rhi::MeshVertex so it uploads without conversion.
struct LandVertex {
    f32 px, py, pz;
    f32 nx, ny, nz;
    f32 u, v;
};
// Catches layout drift on this side; the renderer half asserts the two types still agree.
static_assert(sizeof(LandVertex) == 32, "LandVertex must stay uploadable as rhi::MeshVertex");

// One node's built geometry: the surface vertices first, then the skirt ring.
struct ChunkMesh {
    std::vector<LandVertex> vertices;
    std::vector<u32> indices;

    u32 skirtVertexStart = 0;   // the surface is [0, skirtVertexStart)
    u32 surfaceIndexCount = 0;
};

// Builds one node's mesh. `uvTilingCm` is the world size one UV unit spans; UVs are world-aligned.
bool buildChunkMesh(const fmt::OcLandData& data, const LandscapeTree& tree, u32 nodeIndex,
                    ChunkMesh& out, f32 uvTilingCm = 1000.0f);

// The counts a node produces, without building it.
struct ChunkCounts {
    u32 vertices = 0, indices = 0, surfaceVertices = 0, surfaceIndices = 0;
};
// Computes the vertex and index counts for a node of `nodeQuads` quads a side.
ChunkCounts chunkCounts(u32 nodeQuads);

} // namespace aver::landscape
