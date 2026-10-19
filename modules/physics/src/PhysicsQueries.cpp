// PhysicsQueries.cpp -- box/capsule/body shape casts, box/capsule/body overlaps, every hit along a ray,
// and the point query (physics_queries_abi.h).
//
// The layer-mask, ignore-body and handle-lookup plumbing is PhysicsQueryFilters.hpp, shared with the
// _ex queries in PhysicsLayers.cpp. What is new is one thing: every filter here also drops sensors
// unless the caller's `flags` ask for them.
#include "aver/physics/physics_queries_abi.h"
#include "Convert.hpp"
#include "PhysicsInternal.hpp"
#include "PhysicsQueryFilters.hpp"

#include "aver/core/Log.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/DecoratedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <cmath>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

constexpr float kTinyCm = 1e-4f;

float lengthOf(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// Engine quaternion to Jolt's, normalised; an all-zero one is identity rather than NaN.
JPH::Quat joltRotation(float qx, float qy, float qz, float qw) {
    const float n = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    if (n < 1e-6f) return JPH::Quat::sIdentity();
    return toJolt(Quat(qx / n, qy / n, qz / n, qw / n));
}

// Half extents are a SIZE: the axis permutation applies, the sign flip does not (as aver_phys_add_*_box).
JPH::Vec3 joltHalfExtents(float hx, float hy, float hz) {
    return JPH::Vec3(std::abs(cmToM(hy)), std::abs(cmToM(hz)), std::abs(cmToM(hx)));
}

// A capsule's cylinder half-height in metres from the ABI's TOTAL height, or <= 0 when it is too short
// for its radius (the rule physics_shapes_abi.h states).
float capsuleHalfCylinderM(float radius, float height) {
    return cmToM(height * 0.5f - radius);
}

// Jolt cannot collide a mesh or heightfield AS the moving/query shape, so refuse them up front rather
// than let a deep assert decide. Compounds and decorators are checked through to their leaves.
bool usableAsQueryShape(const JPH::Shape* s) {
    if (!s) return false;
    switch (s->GetType()) {
    case JPH::EShapeType::Convex: return true;
    case JPH::EShapeType::Decorated:
        return usableAsQueryShape(static_cast<const JPH::DecoratedShape*>(s)->GetInnerShape());
    case JPH::EShapeType::Compound: {
        const auto* c = static_cast<const JPH::CompoundShape*>(s);
        for (JPH::uint i = 0; i < c->GetNumSubShapes(); ++i)
            if (!usableAsQueryShape(c->GetSubShape(i).mShape)) return false;
        return true;
    }
    default: return false;
    }
}

// What a body-based query needs from the body, copied out so its lock is not held during the query.
struct BodyShapeCopy {
    JPH::RefConst<JPH::Shape> shape;
    JPH::RMat44 com;          // centre-of-mass transform, as aver_phys_body_position/rotation report
    JPH::BodyID id;
};

bool copyBodyShape(int32_t handle, BodyShapeCopy& out) {
    if (!g_world) return false;
    const JPH::BodyID* id = findBody(handle);
    if (!id) return false;
    JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded()) return false;
    const JPH::Body& b = lock.GetBody();
    if (b.IsSoftBody() || !usableAsQueryShape(b.GetShape())) {
        AVER_WARN("[Physics] body {} has a soft, mesh or heightfield shape and cannot be cast or overlapped", handle);
        return false;
    }
    out.shape = b.GetShape();
    out.com = b.GetCenterOfMassTransform();
    out.id = *id;
    return true;
}

// The body filter every query here uses: the caster, the caller's extra ignore, and sensors per flags.
QueryBodyFilter makeFilter(const JPH::BodyID& caster, int32_t ignoreBody, int32_t flags) {
    return QueryBodyFilter(caster, ignoredIdOf(ignoreBody),
                           (flags & AVER_PHYS_QUERY_INCLUDE_SENSORS) != 0);
}

// ---- cast -----------------------------------------------------------------------------------------

int32_t castClosest(const JPH::Shape* shape, const JPH::RMat44& com, const Vec3& dispCm,
                    uint32_t layerMask, const QueryBodyFilter& filter,
                    float* outDistance, float* outPoint, float* outNormal) {
    const float lenCm = lengthOf(dispCm);
    if (lenCm <= kTinyCm) return 0;

    const JPH::RShapeCast cast(shape, JPH::Vec3::sReplicate(1.0f), com, toJoltDir(dispCm));
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    const MaskLayerFilter layers(layerMask);
    g_world->system.GetNarrowPhaseQuery().CastShape(cast, {}, JPH::RVec3::sZero(), collector,
                                                    {}, layers, filter);
    if (!collector.HadHit()) return 0;

    const JPH::ShapeCastResult& h = collector.mHit;
    const int32_t handle = handleForBodyId(h.mBodyID2);
    if (!handle) return 0;
    if (outDistance) *outDistance = h.mFraction * lenCm;
    if (outPoint)    writeVec(outPoint, fromJolt(h.mContactPointOn2));
    // NEGATED: Jolt's penetration axis points INTO the hit surface; this ABI's normals point away.
    if (outNormal)
        writeVec(outNormal, fromJoltUnit(-h.mPenetrationAxis.NormalizedOr(JPH::Vec3::sAxisY())));
    return handle;
}

// ---- overlap --------------------------------------------------------------------------------------

int32_t overlapShape(const JPH::Shape* shape, const JPH::RMat44& com, uint32_t layerMask,
                     const QueryBodyFilter& filter, int32_t* outBodies, int32_t maxCount) {
    if (!outBodies || maxCount <= 0) return 0;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    const MaskLayerFilter layers(layerMask);
    g_world->system.GetNarrowPhaseQuery().CollideShape(
        shape, JPH::Vec3::sReplicate(1.0f), com, JPH::CollideShapeSettings{},
        JPH::RVec3::sZero(), collector, {}, layers, filter);

    int32_t n = 0;
    for (const JPH::CollideShapeResult& r : collector.mHits) {
        if (n >= maxCount) break;
        const int32_t h = handleForBodyId(r.mBodyID2);
        if (!h) continue;
        bool seen = false;
        for (int32_t i = 0; i < n; ++i) if (outBodies[i] == h) { seen = true; break; }
        if (!seen) outBodies[n++] = h;   // one body can report several sub-shape hits
    }
    return n;
}

} // namespace

extern "C" {

// ---- shape casts ----------------------------------------------------------------------------------

int32_t aver_phys_box_cast(float cx, float cy, float cz, float hx, float hy, float hz,
                           float qx, float qy, float qz, float qw, float dx, float dy, float dz,
                           uint32_t layerMask, int32_t ignoreBody, int32_t flags,
                           float* outDistance, float* outPoint, float* outNormal) {
    if (!g_world) return 0;
    if (std::abs(hx) <= kTinyCm || std::abs(hy) <= kTinyCm || std::abs(hz) <= kTinyCm) return 0;
    JPH::BoxShape box(joltHalfExtents(hx, hy, hz));
    box.SetEmbedded();
    const JPH::RMat44 com = JPH::RMat44::sRotationTranslation(joltRotation(qx, qy, qz, qw),
                                                              toJolt(Vec3(cx, cy, cz)));
    return castClosest(&box, com, Vec3(dx, dy, dz), layerMask,
                       makeFilter(JPH::BodyID(), ignoreBody, flags), outDistance, outPoint, outNormal);
}

int32_t aver_phys_capsule_cast(float cx, float cy, float cz, float radius, float height,
                               float qx, float qy, float qz, float qw, float dx, float dy, float dz,
                               uint32_t layerMask, int32_t ignoreBody, int32_t flags,
                               float* outDistance, float* outPoint, float* outNormal) {
    if (!g_world) return 0;
    const float halfCyl = capsuleHalfCylinderM(radius, height);
    if (radius <= kTinyCm || halfCyl <= 0.0f) {
        AVER_WARN("[Physics] capsule {}cm tall is too short for radius {}cm", height, radius);
        return 0;
    }
    JPH::CapsuleShape capsule(halfCyl, cmToM(radius));
    capsule.SetEmbedded();
    const JPH::RMat44 com = JPH::RMat44::sRotationTranslation(joltRotation(qx, qy, qz, qw),
                                                              toJolt(Vec3(cx, cy, cz)));
    return castClosest(&capsule, com, Vec3(dx, dy, dz), layerMask,
                       makeFilter(JPH::BodyID(), ignoreBody, flags), outDistance, outPoint, outNormal);
}

int32_t aver_phys_sphere_cast_q(float cx, float cy, float cz, float dx, float dy, float dz, float radius,
                                uint32_t layerMask, int32_t ignoreBodyA, int32_t ignoreBodyB, int32_t flags,
                                float* outDistance, float* outPoint, float* outNormal) {
    if (!g_world || radius <= kTinyCm) return 0;
    JPH::SphereShape sphere(cmToM(radius));
    sphere.SetEmbedded();
    const JPH::RMat44 com = JPH::RMat44::sRotationTranslation(JPH::Quat::sIdentity(), toJolt(Vec3(cx, cy, cz)));
    // Two skips: the camera passes the target's character and a vehicle chassis.
    const QueryBodyFilter filter(ignoredIdOf(ignoreBodyA), ignoredIdOf(ignoreBodyB),
                                 (flags & AVER_PHYS_QUERY_INCLUDE_SENSORS) != 0);
    return castClosest(&sphere, com, Vec3(dx, dy, dz), layerMask, filter, outDistance, outPoint, outNormal);
}

int32_t aver_phys_body_cast(int32_t body, float dx, float dy, float dz,
                            uint32_t layerMask, int32_t ignoreBody, int32_t flags,
                            float* outDistance, float* outPoint, float* outNormal) {
    BodyShapeCopy b;
    if (!copyBodyShape(body, b)) return 0;
    return castClosest(b.shape, b.com, Vec3(dx, dy, dz), layerMask,
                       makeFilter(b.id, ignoreBody, flags), outDistance, outPoint, outNormal);
}

// ---- overlaps -------------------------------------------------------------------------------------

int32_t aver_phys_overlap_box(float cx, float cy, float cz, float hx, float hy, float hz,
                              float qx, float qy, float qz, float qw,
                              uint32_t layerMask, int32_t ignoreBody, int32_t flags,
                              int32_t* outBodies, int32_t maxCount) {
    if (!g_world) return 0;
    if (std::abs(hx) <= kTinyCm || std::abs(hy) <= kTinyCm || std::abs(hz) <= kTinyCm) return 0;
    JPH::BoxShape box(joltHalfExtents(hx, hy, hz));
    box.SetEmbedded();
    const JPH::RMat44 com = JPH::RMat44::sRotationTranslation(joltRotation(qx, qy, qz, qw),
                                                              toJolt(Vec3(cx, cy, cz)));
    return overlapShape(&box, com, layerMask, makeFilter(JPH::BodyID(), ignoreBody, flags),
                        outBodies, maxCount);
}

int32_t aver_phys_overlap_capsule(float cx, float cy, float cz, float radius, float height,
                                  float qx, float qy, float qz, float qw,
                                  uint32_t layerMask, int32_t ignoreBody, int32_t flags,
                                  int32_t* outBodies, int32_t maxCount) {
    if (!g_world) return 0;
    const float halfCyl = capsuleHalfCylinderM(radius, height);
    if (radius <= kTinyCm || halfCyl <= 0.0f) {
        AVER_WARN("[Physics] capsule {}cm tall is too short for radius {}cm", height, radius);
        return 0;
    }
    JPH::CapsuleShape capsule(halfCyl, cmToM(radius));
    capsule.SetEmbedded();
    const JPH::RMat44 com = JPH::RMat44::sRotationTranslation(joltRotation(qx, qy, qz, qw),
                                                              toJolt(Vec3(cx, cy, cz)));
    return overlapShape(&capsule, com, layerMask, makeFilter(JPH::BodyID(), ignoreBody, flags),
                        outBodies, maxCount);
}

int32_t aver_phys_overlap_body(int32_t body, float x, float y, float z,
                               float qx, float qy, float qz, float qw,
                               uint32_t layerMask, int32_t ignoreBody, int32_t flags,
                               int32_t* outBodies, int32_t maxCount) {
    BodyShapeCopy b;
    if (!copyBodyShape(body, b)) return 0;
    const JPH::RMat44 com = JPH::RMat44::sRotationTranslation(joltRotation(qx, qy, qz, qw),
                                                              toJolt(Vec3(x, y, z)));
    return overlapShape(b.shape, com, layerMask, makeFilter(b.id, ignoreBody, flags),
                        outBodies, maxCount);
}

// ---- every hit along a ray ------------------------------------------------------------------------

int32_t aver_phys_raycast_all(float ox, float oy, float oz, float dx, float dy, float dz,
                              float maxDistCm, uint32_t layerMask, int32_t ignoreBody, int32_t flags,
                              int32_t maxCount, int32_t* outBodies, float* outDistances,
                              float* outPoints, float* outNormals) {
    if (!g_world || maxCount <= 0) return 0;
    const Vec3 dir(dx, dy, dz);
    const float len = lengthOf(dir);
    if (len <= 1e-6f || maxDistCm <= 0.0f) return 0;

    const JPH::Vec3 from = toJolt(Vec3(ox, oy, oz));
    const JPH::Vec3 d = toJoltUnit(Vec3(dx / len, dy / len, dz / len)) * cmToM(maxDistCm);
    const JPH::RRayCast ray{from, d};

    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    const MaskLayerFilter layers(layerMask);
    const QueryBodyFilter filter = makeFilter(JPH::BodyID(), ignoreBody, flags);
    g_world->system.GetNarrowPhaseQuery().CastRay(ray, JPH::RayCastSettings{}, collector,
                                                  {}, layers, filter);
    collector.Sort();   // nearest first; truncation below then keeps the nearest maxCount

    int32_t n = 0;
    for (const JPH::RayCastResult& r : collector.mHits) {
        if (n >= maxCount) break;
        const int32_t h = handleForBodyId(r.mBodyID);
        if (!h) continue;
        const JPH::Vec3 p = ray.GetPointOnRay(r.mFraction);
        if (outBodies)    outBodies[n] = h;
        if (outDistances) outDistances[n] = r.mFraction * maxDistCm;
        if (outPoints)    writeVec(outPoints + 3 * n, fromJolt(p));
        if (outNormals) {
            JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), r.mBodyID);
            writeVec(outNormals + 3 * n,
                     lock.Succeeded()
                         ? fromJoltUnit(lock.GetBody().GetWorldSpaceSurfaceNormal(r.mSubShapeID2, p))
                         : Vec3(0, 0, 1));
        }
        ++n;
    }
    return n;
}

// ---- point query ----------------------------------------------------------------------------------

int32_t aver_phys_point_query(float x, float y, float z, uint32_t layerMask, int32_t ignoreBody,
                              int32_t flags, int32_t* outBodies, int32_t maxCount) {
    if (!g_world || !outBodies || maxCount <= 0) return 0;
    JPH::AllHitCollisionCollector<JPH::CollidePointCollector> collector;
    const MaskLayerFilter layers(layerMask);
    const QueryBodyFilter filter = makeFilter(JPH::BodyID(), ignoreBody, flags);
    g_world->system.GetNarrowPhaseQuery().CollidePoint(toJolt(Vec3(x, y, z)), collector,
                                                       {}, layers, filter);

    int32_t n = 0;
    for (const JPH::CollidePointResult& r : collector.mHits) {
        if (n >= maxCount) break;
        const int32_t h = handleForBodyId(r.mBodyID);
        if (!h) continue;
        bool seen = false;
        for (int32_t i = 0; i < n; ++i) if (outBodies[i] == h) { seen = true; break; }
        if (!seen) outBodies[n++] = h;
    }
    return n;
}

} // extern "C"
