// Merging static meshes into one (dynamic HLOD proxies, docs/LEVEL_STREAMING.md). Pure CPU on OcMeshData: no GPU, no scene.
#pragma once

#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcMesh.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// One placed mesh: its world transform as a row-major 3x4 (rotation and scale in the left 3x3, translation in the last column), f64.
struct OcMergeItem {
    const OcMeshData* mesh = nullptr;
    f64 m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    // Overrides every slot of the mesh when non-empty (a placement's `material`); empty = the mesh's own slot names.
    std::string material;
};

// The row-major 3x4 of a pose: columns of the rotation scaled per axis, then the translation (f64).
void ocMergeMatrix(const f64 pos[3], const Quat& rot, const Vec3& scale, f64 m[12]);

struct OcMergeOptions {
    // Subtracted from the translation: the merged vertices are relative to this world point, so they stay precise far from the origin.
    f64 origin[3] = {0, 0, 0};
    // Vertices closer than this (cm), with matching normal and uv, become one.
    f32 weldCm = 0.01f;
    // Items whose world bounding-box diagonal is under this are dropped (cm); 0 keeps everything.
    f32 dropSmallerCm = 0.0f;
    // Drops items whose bounds lie inside another kept item's bounds and are smaller (a cheap "fully enclosed" test). Off by default.
    bool dropEnclosed = false;
};

struct OcMergeReport {
    u32 merged = 0, droppedSmall = 0, droppedEnclosed = 0, skipped = 0;   // skipped: no mesh, no triangles or an invalid index
    u32 trianglesIn = 0, trianglesOut = 0;   // out is after degenerate triangles are dropped
    u32 verticesBeforeWeld = 0, verticesOut = 0;
};

// Bakes each item's transform into its vertices and groups triangles by material name into submeshes (first appearance order),
// so source materials survive and nothing is atlased. Skin, morphs, meshlets and LODs are not carried: the result is plain LOD 0
// (positions, normals, uv0, indices) with bounds computed. Winding flips under a mirroring transform. Deterministic: the same items
// in the same order give the same bytes. Returns false only if nothing was left to merge.
bool mergeStaticMeshes(const std::vector<OcMergeItem>& items, const OcMergeOptions& opt, OcMeshData& out, OcMergeReport* report = nullptr,
                       std::string* why = nullptr);

} // namespace aver::fmt
