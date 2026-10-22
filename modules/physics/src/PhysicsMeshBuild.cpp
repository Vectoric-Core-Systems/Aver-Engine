// PhysicsMeshBuild.cpp -- see PhysicsMeshBuild.hpp.
#include "PhysicsMeshBuild.hpp"
#include "PhysicsSurface.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>

#include <cmath>

namespace aver::physics::detail {

namespace {
constexpr int32_t kMaxPalette = 32;   // Jolt packs the material index in 5 bits
}

JPH::Ref<JPH::Shape> buildMeshShape(const float* verts, int32_t vertexCount, const int32_t* indices, int32_t indexCount,
                                    const uint8_t* triPaletteIndex, const int32_t* palette, int32_t paletteCount,
                                    const int32_t* triUserData, const ShapeParams& sp, const char* label) {
    if (!g_world) { aver::setAbiError(aver::AbiError::NotInitialised); return nullptr; }
    if (!verts || !indices || vertexCount < 3 || indexCount < 3) { aver::setAbiError(aver::AbiError::InvalidArgument); return nullptr; }
    if (paletteCount < 0 || paletteCount > kMaxPalette || (paletteCount > 0 && !palette)) {
        AVER_WARN("[Physics] {}: a mesh holds at most {} surfaces (got {})", label, kMaxPalette, paletteCount);
        aver::setAbiError(aver::AbiError::InvalidArgument);
        return nullptr;
    }

    JPH::VertexList vlist;
    vlist.reserve(static_cast<size_t>(vertexCount));
    for (int32_t i = 0; i < vertexCount; ++i) {
        const JPH::Vec3 v = toJolt(Vec3(verts[i*3+0], verts[i*3+1], verts[i*3+2]));
        vlist.push_back(JPH::Float3(v.GetX(), v.GetY(), v.GetZ()));
    }

    JPH::PhysicsMaterialList materials;
    for (int32_t i = 0; i < paletteCount; ++i) {
        JPH::RefConst<JPH::PhysicsMaterial> m = palette[i] != 0 ? surfaceMaterial(palette[i]) : JPH::PhysicsMaterial::sDefault;
        materials.push_back(m);
    }

    JPH::IndexedTriangleList tris;
    tris.reserve(static_cast<size_t>(indexCount / 3));
    int32_t badIndex = 0, badPalette = 0;
    for (int32_t i = 0; i + 2 < indexCount; i += 3) {
        const JPH::uint32 a = static_cast<JPH::uint32>(indices[i]);
        const JPH::uint32 b = static_cast<JPH::uint32>(indices[i+1]);
        const JPH::uint32 c = static_cast<JPH::uint32>(indices[i+2]);
        if (a >= static_cast<JPH::uint32>(vertexCount) ||
            b >= static_cast<JPH::uint32>(vertexCount) ||
            c >= static_cast<JPH::uint32>(vertexCount)) {
            ++badIndex;
            continue;
        }
        const int32_t tri = i / 3;
        JPH::uint32 mat = 0;
        if (paletteCount > 0 && triPaletteIndex) {
            mat = triPaletteIndex[tri];
            if (mat >= static_cast<JPH::uint32>(paletteCount)) { mat = 0; ++badPalette; }
        }
        const JPH::uint32 user = triUserData ? static_cast<JPH::uint32>(triUserData[tri]) : 0u;
        // Winding is reversed: the axis map has determinant -1, which mirrors the mesh.
        tris.push_back(JPH::IndexedTriangle(a, c, b, mat, user));
    }
    if (badIndex) AVER_WARN("[Physics] {}: {} triangle(s) with an index out of range skipped", label, badIndex);
    if (badPalette) AVER_WARN("[Physics] {}: {} triangle(s) named a surface outside the palette and use entry 0", label, badPalette);
    if (tris.empty()) {
        AVER_WARN("[Physics] {}: no usable triangles", label);
        aver::setAbiError(aver::AbiError::InvalidArgument);
        return nullptr;
    }

    JPH::MeshShapeSettings s(std::move(vlist), std::move(tris), std::move(materials));
    s.SetEmbedded();
    if (sp.edgeSet) {
        // Negative = every edge active (the build is faster, bodies on it then want enhanced edge removal).
        s.mActiveEdgeCosThresholdAngle = sp.edgeAngleDeg < 0.0f
            ? -1.0f : std::cos(JPH::DegreesToRadians(std::fmin(sp.edgeAngleDeg, 90.0f)));
    }
    if (sp.favorBuildSpeed) s.mBuildQuality = JPH::MeshShapeSettings::EBuildQuality::FavorBuildSpeed;
    s.mMaxTrianglesPerLeaf = static_cast<JPH::uint>(sp.maxTrisPerLeaf);
    s.mPerTriangleUserData = triUserData != nullptr;
    // Marks the shape so a hit can tell "triangle 0" from "no per-triangle data" (Jolt returns 0 for both).
    if (triUserData) s.mUserData = kMeshTriangleUserData;

    auto res = s.Create();
    if (res.HasError()) {
        AVER_WARN("[Physics] {}: {}", label, res.GetError().c_str());
        aver::setAbiError(aver::AbiError::InvalidArgument);
        return nullptr;
    }
    return res.Get();
}

} // namespace aver::physics::detail
