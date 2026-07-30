#pragma once
// One node's geometry: a 65x65 surface grid plus a skirt around its rim.
//
// PURE CPU, like the rest of this module. It emits plain arrays; uploading them is the renderer half's
// job, and keeping that seam is what lets the counts and the index range below be checked with no
// device.
#include "aver/landscape/LandscapeTree.hpp"

namespace aver::landscape {

// The vertex this module emits.
//
// LAYOUT-IDENTICAL to rhi::MeshVertex by construction -- position, normal, uv, eight floats, 32 bytes
// -- so the renderer half uploads it without a per-vertex conversion. It is declared HERE rather than
// reused from the RHI because this target must not depend on it: a landscape that pulled in a render
// header could not be tested on a machine with no device, which is the property the whole staging plan
// rests on. The static_assert below catches drift on this side; the renderer half asserts the two
// match, which is the only place that comparison can legally be made.
struct LandVertex {
    f32 px, py, pz;
    f32 nx, ny, nz;
    f32 u, v;
};
static_assert(sizeof(LandVertex) == 32, "LandVertex must stay uploadable as rhi::MeshVertex");

struct ChunkMesh {
    std::vector<LandVertex> vertices;
    std::vector<u32> indices;

    // Where the skirt begins. The surface is [0, skirtVertexStart) and the rim duplicates follow, so a
    // consumer that wants the surface alone -- a collision build, a debug draw -- does not have to
    // recompute the split.
    u32 skirtVertexStart = 0;
    u32 surfaceIndexCount = 0;
};

// Build one node's mesh.
//
// `uvTilingCm` is the world size one UV unit spans. World-aligned rather than per-node, so adjacent
// chunks at different levels share a continuous texture parameterisation instead of showing a seam
// exactly where the LOD changes -- which would defeat the skirt that hides the geometric one.
bool buildChunkMesh(const fmt::OcLandData& data, const LandscapeTree& tree, u32 nodeIndex,
                    ChunkMesh& out, f32 uvTilingCm = 1000.0f);

// The counts a node produces, without building it. For budgeting, and for the test that pins them.
//   surface  (q+1)^2 vertices, q*q*2*3 indices
//   skirt    4*(q+1) vertices, 4*q*2*3 indices
struct ChunkCounts {
    u32 vertices = 0, indices = 0, surfaceVertices = 0, surfaceIndices = 0;
};
ChunkCounts chunkCounts(u32 nodeQuads);

} // namespace aver::landscape
