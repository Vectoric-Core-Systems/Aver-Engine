// PhysicsMeshBuild.hpp -- the one triangle-mesh shape builder.
//
// aver_phys_add_mesh, aver_phys_create_mesh_shape and aver_phys_shape_mesh all build through this: the same
// axis map, the same winding reversal (the axis map has determinant -1), the same skip of out-of-range
// triangles. SRC-LOCAL (it returns a JPH::Shape).
#pragma once
#include "PhysicsShapeTable.hpp"

namespace aver::physics::detail {

// Builds a mesh shape from engine-space vertices (3 floats each, cm) and indices (3 per triangle).
//
// `palette` is up to 32 surface ids and `triPaletteIndex` one byte per INPUT triangle indexing it (null = all
// 0); with no palette the mesh carries no materials, exactly as before. `triUserData` is one int32 per input
// triangle (null = none) and turns on Jolt's per-triangle user data. A triangle skipped for a bad index, and
// one Jolt's Sanitize drops as duplicate or degenerate, takes its palette index and user data with it.
// Returns null (reason logged and recorded) for no usable triangles, a palette over 32 or a bad palette index.
// `label` names the caller in the log ("mesh", "mesh shape").
JPH::Ref<JPH::Shape> buildMeshShape(const float* verticesXyz, int32_t vertexCount, const int32_t* indices, int32_t indexCount,
                                    const uint8_t* triPaletteIndex, const int32_t* palette, int32_t paletteCount,
                                    const int32_t* triUserData, const ShapeParams& sp, const char* label);

} // namespace aver::physics::detail
