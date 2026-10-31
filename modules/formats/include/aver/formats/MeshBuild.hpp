#pragma once
// Meshes built at run time from arrays, and instance tables baked into one mesh (G4 R7). Engine space (centimetres, +Z up), no device: the Aver.Framework
// runtime-mesh ABI and the converter share this. Contract: R7 of https://github.com/Vectoric-Core-Systems/Fabric-Connector/blob/main/docs/plans/G4_RENDER.md.
#include "aver/formats/OcMesh.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// One surface of a script-built mesh (an array-mesh surface, a surface-tool commit, an immediate-mesh surface), already in engine space and winding.
struct RuntimeSurface {
    std::vector<f32> pos;            // 3 per vertex (required)
    std::vector<f32> nrm;            // 3 per vertex, or empty = smooth normals are computed
    std::vector<f32> uv;             // 2 per vertex, or empty = zero
    std::vector<f32> col;            // 4 per vertex, linear, or empty
    std::vector<f32> uv1;            // 2 per vertex, or empty
    std::vector<f32> tan;            // 4 per vertex (engine tangent contract), or empty
    std::vector<u32> idx;            // triangle list; empty = the vertices in order
    std::string material;            // the surface's material name ("" = the project's default slot name)
};

// The vertex array, one submesh and one material slot per surface. A colour / tangent / UV1 stream that only some surfaces have is padded (white / no tangent /
// zero). False with *why when a surface is malformed (an index past the vertices, a stream of the wrong length, no triangles at all).
bool assembleSurfaces(const std::vector<RuntimeSurface>& surfaces, OcMeshData& out, std::string* why);

// Area-weighted smooth normals from a triangle list.
void computeSmoothNormals(const std::vector<f32>& pos, const std::vector<u32>& idx, std::vector<f32>& nrmOut);

// An instance table: 12 floats per instance, the engine's row-vector world matrix with its 4th column dropped (aver/formats/OcInstances.hpp): the basis rows
// (scale and rotation) then the translation, centimetres.
struct MeshInstanceSet {
    std::vector<f32> transforms;
    std::vector<f32> colors;         // 4 per instance (linear, multiplies the mesh's own vertex colour), or empty
    std::vector<f32> custom;         // 4 per instance, or empty: custom.xy become the second UV set (UV1), custom.zw are not carried
    u32 count() const { return static_cast<u32>(transforms.size() / 12); }
};

// Draws the first `visible` instances of `src` as one mesh: the vertices transformed per instance, the triangles grouped by the source's submeshes, the
// colour stream = the mesh's colour times the instance colour. Skin, morph targets, meshlets and LODs are dropped. When `visible * vertexCount` would pass
// maxVertices the count is lowered to fit and *used says how many were drawn. False when nothing could be drawn.
bool bakeInstances(const OcMeshData& src, const MeshInstanceSet& set, u32 visible, u32 maxVertices, OcMeshData& out, u32* used, std::string* why);

}  // namespace aver::fmt
