// PhysicsShapeTable.cpp -- the generic shape-handle table, the SP parameter block, and the small shape
// queries the other shape files share. See PhysicsShapeTable.hpp.
#include "PhysicsShapeTable.hpp"

#include "aver/physics/physics_shape_builder_abi.h"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <Jolt/Physics/Collision/Shape/DecoratedShape.h>

#include <cmath>
#include <mutex>
#include <unordered_map>

namespace aver::physics::detail {

namespace {

// Guarded: level streaming builds shapes on worker threads.
std::mutex g_tableMutex;
std::unordered_map<int32_t, JPH::RefConst<JPH::Shape>> g_shapes;
int32_t g_nextShape = 1;

// Mutable compound -> the body handle that owns it.
std::unordered_map<const JPH::Shape*, int32_t> g_owners;

bool refuse(const char* what) {
    AVER_WARN("[Physics] shape parameters: {}", what);
    aver::setAbiError(aver::AbiError::InvalidArgument);
    return false;
}

} // namespace

int32_t shapeTableAdd(JPH::RefConst<JPH::Shape> shape) {
    std::lock_guard<std::mutex> lock(g_tableMutex);
    const int32_t h = g_nextShape++;
    g_shapes.emplace(h, std::move(shape));
    return h;
}

JPH::RefConst<JPH::Shape> shapeTableGet(int32_t handle) {
    std::lock_guard<std::mutex> lock(g_tableMutex);
    const auto it = g_shapes.find(handle);
    return it == g_shapes.end() ? JPH::RefConst<JPH::Shape>() : it->second;
}

bool shapeTableRelease(int32_t handle) {
    std::lock_guard<std::mutex> lock(g_tableMutex);
    return g_shapes.erase(handle) != 0;
}

int32_t shapeTableCount() {
    std::lock_guard<std::mutex> lock(g_tableMutex);
    return static_cast<int32_t>(g_shapes.size());
}

bool shapeTableOwnerFree(const JPH::Shape* shape) {
    const JPH::Shape* mc = mutableCompoundOf(shape);
    if (!mc) return true;
    std::lock_guard<std::mutex> lock(g_tableMutex);
    const auto it = g_owners.find(mc);
    return it == g_owners.end() || !g_world || g_world->bodies.count(it->second) == 0;
}

void shapeTableBindOwner(const JPH::Shape* shape, int32_t bodyHandle) {
    const JPH::Shape* mc = mutableCompoundOf(shape);
    if (!mc) return;
    std::lock_guard<std::mutex> lock(g_tableMutex);
    g_owners[mc] = bodyHandle;
}

void shapeTableOnShutdown() {
    std::lock_guard<std::mutex> lock(g_tableMutex);
    g_owners.clear();
}

bool parseShapeParams(const float* p, int32_t n, ShapeParams& out) {
    out = ShapeParams{};
    if (n < 0 || n > AVER_PHYS_SP_COUNT) return refuse("more entries than the block defines");
    if (n > 0 && !p) return refuse("a count without a block");
    for (int32_t i = 0; i < n; ++i)
        if (!std::isfinite(p[i])) return refuse("a non-finite entry");

    if (n > AVER_PHYS_SP_CONVEX_RADIUS && p[AVER_PHYS_SP_CONVEX_RADIUS] >= 0.0f)
        out.convexRadiusCm = p[AVER_PHYS_SP_CONVEX_RADIUS];
    if (n > AVER_PHYS_SP_DENSITY && p[AVER_PHYS_SP_DENSITY] >= 0.0f) {
        if (p[AVER_PHYS_SP_DENSITY] == 0.0f) return refuse("density must be positive");
        out.densityKgM3 = p[AVER_PHYS_SP_DENSITY];
    }
    if (n > AVER_PHYS_SP_HULL_TOLERANCE && p[AVER_PHYS_SP_HULL_TOLERANCE] >= 0.0f) {
        if (p[AVER_PHYS_SP_HULL_TOLERANCE] == 0.0f) return refuse("hull tolerance must be positive");
        out.hullToleranceCm = p[AVER_PHYS_SP_HULL_TOLERANCE];
    }
    if (n > AVER_PHYS_SP_EDGE_ANGLE_DEG) {
        out.edgeSet = true;
        out.edgeAngleDeg = p[AVER_PHYS_SP_EDGE_ANGLE_DEG];
    }
    if (n > AVER_PHYS_SP_BUILD_QUALITY && p[AVER_PHYS_SP_BUILD_QUALITY] >= 0.0f) {
        const float q = p[AVER_PHYS_SP_BUILD_QUALITY];
        if (q != 0.0f && q != 1.0f) return refuse("build quality is 0 or 1");
        out.favorBuildSpeed = q == 1.0f;
    }
    if (n > AVER_PHYS_SP_MAX_TRIS_PER_LEAF && p[AVER_PHYS_SP_MAX_TRIS_PER_LEAF] >= 0.0f) {
        const int t = static_cast<int>(std::lround(p[AVER_PHYS_SP_MAX_TRIS_PER_LEAF]));
        if (t < 1 || t > 8) return refuse("triangles per leaf is 1 to 8");
        out.maxTrisPerLeaf = t;
    }
    return true;
}

bool allFiniteN(const float* v, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(v[i])) return false;
    return true;
}

bool readJoltQuat(const float* q, JPH::Quat& out) {
    if (!q) { out = JPH::Quat::sIdentity(); return true; }
    if (!allFiniteN(q, 4)) return false;
    const float len = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (len < 1.0e-6f) return false;
    out = toJolt(Quat(q[0] / len, q[1] / len, q[2] / len, q[3] / len));
    return true;
}

const JPH::Shape* mutableCompoundOf(const JPH::Shape* shape) {
    while (shape) {
        if (shape->GetSubType() == JPH::EShapeSubType::MutableCompound) return shape;
        if (shape->GetType() != JPH::EShapeType::Decorated) return nullptr;
        shape = static_cast<const JPH::DecoratedShape*>(shape)->GetInnerShape();
    }
    return nullptr;
}

bool isMeshLike(const JPH::Shape* shape) {
    while (shape) {
        if (shape->GetSubType() == JPH::EShapeSubType::Mesh) return true;
        if (shape->GetType() != JPH::EShapeType::Decorated) return false;
        shape = static_cast<const JPH::DecoratedShape*>(shape)->GetInnerShape();
    }
    return false;
}

float shapeMassKg(const JPH::Shape* shape) {
    return shape ? shape->GetMassProperties().mMass : 0.0f;
}

} // namespace aver::physics::detail
