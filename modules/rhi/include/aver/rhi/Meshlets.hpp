#pragma once
// The mesh-shader path's geometry: a mesh's triangles in meshlets of at most kMeshletMaxTriangles triangles over at most
// kMeshletMaxVertices distinct vertices, laid out as the one buffer ASMain and MSMain read (shared_prelude.hlsl, AVER_MS).
// CPU-only, so the layout is unit-testable with no device (tests/rhi MeshletTest).
//
// Buffer layout, in 32-bit words:
//   meshlet i, kMeshletDescDwords words at i * kMeshletDescDwords:
//     [0] first word of its vertex list   [1] first word of its triangle list   [2] vertexCount | triangleCount << 8
//     [3] 0   [4..6] bounding-sphere centre (mesh space, f32 bits)   [7] radius (f32 bits; negative: no bounds, never culled)
//   then every meshlet's vertex list (global vertex indices), then every triangle list (local a | b << 8 | c << 16).
#include "aver/core/Types.hpp"

#include <vector>

namespace aver::rhi {

// 64 vertices and 124 triangles fit a 128-thread mesh-shader group with one output per thread, the size every
// mesh-shading vendor guide lands near.
constexpr u32 kMeshletMaxVertices = 64;
constexpr u32 kMeshletMaxTriangles = 124;
constexpr u32 kMeshletDescDwords = 8;
// Meshlets one amplification-shader group culls; ASMain's [numthreads] is emitted from this.
constexpr u32 kMeshletsPerAmplificationGroup = 32;
// MeshCB's fourth word: the vertices move after the meshlets were built (skinned, posed), so bounds are not trusted.
constexpr u32 kMeshletFlagNoCull = 1u;

struct MeshletBuffer {
    std::vector<u32> words;
    u32 meshletCount = 0;
};

// Builds the buffer for one index list, in index order: triangles reach the rasteriser in the order the input
// assembler draws them, so translucent meshes blend the same on both paths. Reuse is only as good as the index order's
// locality (a long row-order strip: about one vertex per triangle; compact patches: well under). Triangles naming a
// vertex at or past `vertexCount` are dropped (the input assembler's paths draw them as nothing either). `positions`
// (three f32 at `strideBytes` apart, one per vertex) gives each meshlet its bounding sphere; null leaves every meshlet
// unbounded.
MeshletBuffer buildMeshlets(const u32* indices, u32 indexCount, u32 vertexCount, const f32* positions, u32 strideBytes);

} // namespace aver::rhi
