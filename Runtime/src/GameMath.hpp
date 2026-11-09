// Small geometry and transform helpers, copied from SandboxApp.cpp. The mesh builders (appendBox,
// appendSphere) live in aver/rhi/PrimitiveMeshes.hpp.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/PrimitiveMeshes.hpp"

#include <cmath>
#include <vector>

namespace aver::game {

using rhi::primitives::appendBox;
using rhi::primitives::appendSphere;


// Transforms a point by the row-vector matrix m. This engine is row-vector (v * M), so the
// translation lives in ROW 3 and not in column 3 -- transposing this is the classic way to get a
// world that renders mirrored about the origin.
inline Vec3 xformPoint(const Mat4& m, const Vec3& p) {
    return { p.x*m.m[0][0]+p.y*m.m[1][0]+p.z*m.m[2][0]+m.m[3][0],
             p.x*m.m[0][1]+p.y*m.m[1][1]+p.z*m.m[2][1]+m.m[3][1],
             p.x*m.m[0][2]+p.y*m.m[1][2]+p.z*m.m[2][2]+m.m[3][2] };
}

} // namespace aver::game
