#pragma once
// The optional SECOND vertex stream of a mesh (input slot 1): vertex colour, authored tangent and UV2.
// MeshVertex (slot 0, 32 B) is the HLSL MeshVtx / input-layout / DXR BLAS ABI and does not change; a mesh
// that has none of these binds no second buffer and draws through today's pipelines.
// Layout contract and shader semantics: section R1 of https://github.com/Vectoric-Core-Systems/Fabric-Connector/blob/main/docs/plans/G4_RENDER.md.
#include "aver/core/Types.hpp"

#include <cmath>
#include <cstddef>

namespace aver::rhi {

struct MeshVertexExtra {
    u8  color[4];     // RGBA8 unorm, LINEAR (255,255,255,255 = white, no tint)
    u8  tangent[4];   // xyz: unit tangent * 0.5 + 0.5, UNORM; w: 255 = +1, 0 = -1, ~128 = no tangent (derive the frame)
    f32 u2, v2;       // the second UV set
};

static_assert(sizeof(MeshVertexExtra) == 16, "MeshVertexExtra is the slot-1 input layout ABI (HLSL VSIn colour/tangent/uv2)");
static_assert(offsetof(MeshVertexExtra, color) == 0 && offsetof(MeshVertexExtra, tangent) == 4 &&
              offsetof(MeshVertexExtra, u2) == 8, "slot-1 attribute offsets are fixed by the pipeline's VertexLayout");

namespace detail {
inline u8 unorm8(f32 v) {
    if (!(v > 0.0f)) return 0;   // also catches NaN
    if (v >= 1.0f) return 255;
    return static_cast<u8>(v * 255.0f + 0.5f);
}
}

// Packs one vertex's extras. `color` is linear RGBA 0..1; `tangent` is {x, y, z, w} with w = +-1 (0 = none, as the .ocmesh
// reader hands it back); `uv1` is the second UV set. Any pointer may be null: white, no tangent, (0,0).
inline MeshVertexExtra packMeshVertexExtra(const f32* color, const f32* tangent, const f32* uv1) {
    MeshVertexExtra e{};
    for (int k = 0; k < 4; ++k) e.color[k] = color ? detail::unorm8(color[k]) : u8(255);
    if (tangent && (tangent[3] > 0.5f || tangent[3] < -0.5f)) {
        for (int k = 0; k < 3; ++k) e.tangent[k] = detail::unorm8(tangent[k] * 0.5f + 0.5f);
        e.tangent[3] = tangent[3] > 0.0f ? u8(255) : u8(0);
    } else {
        e.tangent[0] = e.tangent[1] = e.tangent[2] = e.tangent[3] = u8(128);
    }
    e.u2 = uv1 ? uv1[0] : 0.0f;
    e.v2 = uv1 ? uv1[1] : 0.0f;
    return e;
}

// The inverse of the tangent packing, as the shader decodes it (for tests and tools). Returns false for "no tangent".
inline bool unpackExtraTangent(const MeshVertexExtra& e, f32 out[4]) {
    for (int k = 0; k < 3; ++k) out[k] = static_cast<f32>(e.tangent[k]) / 255.0f * 2.0f - 1.0f;
    out[3] = static_cast<f32>(e.tangent[3]) / 255.0f * 2.0f - 1.0f;
    if (std::fabs(out[3]) <= 0.5f) { out[0] = out[1] = out[2] = out[3] = 0.0f; return false; }
    out[3] = out[3] > 0.0f ? 1.0f : -1.0f;
    return true;
}

} // namespace aver::rhi
