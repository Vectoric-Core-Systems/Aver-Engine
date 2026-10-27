// PhysicsSpaceState.cpp -- Godot's PhysicsDirectSpaceState3D and body_test_motion on Jolt (G2): a ray with explicit back-face / from-inside /
// bodies / areas options, intersect_shape, collide_shape, get_rest_info, cast_motion, and the motion test move_and_collide is built on.
// The ABI and its semantics are in physics_queries_abi.h; the rationale in docs/physics/GODOT_QUERIES.md. Algorithms are written from the
// documented behaviour only. Every existing query is untouched: this file adds entry points and reads, it changes no simulation state.
#include "aver/physics/physics_queries_abi.h"
#include "Convert.hpp"
#include "PhysicsGodotFilter.hpp"
#include "PhysicsInternal.hpp"
#include "PhysicsQueryFilters.hpp"
#include "PhysicsShapeTable.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/DecoratedShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

constexpr float kOverlapM = 5e-4f;        // a start overlap deeper than this is "already colliding" and ignored by a cast
constexpr float kContactSlackM = 5e-4f;   // contacts this far beyond the margin still count at the point of contact
constexpr int   kRecoveryPasses = 4;
constexpr float kPushEpsM = 1e-5f;

bool finiteN(const float* v, int n) {
    for (int i = 0; i < n; ++i) if (!std::isfinite(v[i])) return false;
    return true;
}

// Mesh, heightfield and plane cannot be the moving/query shape in Jolt. Compounds and decorators are checked through to their leaves.
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

// The caller's exclude array as sorted Jolt ids; handles that do not resolve are dropped.
std::vector<JPH::BodyID> excludeIds(const int32_t* handles, int32_t count) {
    std::vector<JPH::BodyID> ids;
    if (!handles || count <= 0) return ids;
    ids.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        const JPH::BodyID id = ignoredIdOf(handles[i]);
        if (!id.IsInvalid()) ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end(), [](const JPH::BodyID& a, const JPH::BodyID& b) {
        return a.GetIndexAndSequenceNumber() < b.GetIndexAndSequenceNumber();
    });
    return ids;
}

bool isExcluded(const std::vector<JPH::BodyID>& ids, const JPH::BodyID& id) {
    return std::binary_search(ids.begin(), ids.end(), id, [](const JPH::BodyID& a, const JPH::BodyID& b) {
        return a.GetIndexAndSequenceNumber() < b.GetIndexAndSequenceNumber();
    });
}

// Godot's query filter: the 32-bit mask against the body's layer bits, bodies versus areas, and the exclude array.
class SpaceFilter final : public JPH::BodyFilter {
public:
    SpaceFilter(uint32_t mask, int32_t flags, std::vector<JPH::BodyID> exclude)
        : mask_(mask), bodies_((flags & AVER_PHYS_SQ_BODIES) != 0), areas_((flags & AVER_PHYS_SQ_AREAS) != 0),
          exclude_(std::move(exclude)) {}
    bool ShouldCollide(const JPH::BodyID& id) const override { return !isExcluded(exclude_, id); }
    bool ShouldCollideLocked(const JPH::Body& b) const override {
        if ((mask_ & godotLayerBitsOf(b)) == 0u) return false;
        return b.IsSensor() ? areas_ : bodies_;
    }
private:
    uint32_t mask_;
    bool bodies_, areas_;
    std::vector<JPH::BodyID> exclude_;
};

// The tested body's own filtering: the simulation's group rule (Godot layer/mask, exceptions, joint exclusion) plus the exclude array.
class MotionFilter final : public JPH::BodyFilter {
public:
    MotionFilter(const JPH::BodyID& self, const JPH::CollisionGroup& group, int32_t flags, std::vector<JPH::BodyID> exclude)
        : self_(self), group_(group), bodies_((flags & AVER_PHYS_SQ_BODIES) != 0), areas_((flags & AVER_PHYS_SQ_AREAS) != 0),
          exclude_(std::move(exclude)) {}
    bool ShouldCollide(const JPH::BodyID& id) const override { return id != self_ && !isExcluded(exclude_, id); }
    bool ShouldCollideLocked(const JPH::Body& b) const override {
        if (!(b.IsSensor() ? areas_ : bodies_)) return false;
        return group_.CanCollide(b.GetCollisionGroup());
    }
private:
    JPH::BodyID self_;
    JPH::CollisionGroup group_;
    bool bodies_, areas_;
    std::vector<JPH::BodyID> exclude_;
};

// The 16-layer matrix for a MOVING body of the given user layer (a static one is tested as if it moved).
class MovingPairLayerFilter final : public JPH::ObjectLayerFilter {
public:
    explicit MovingPairLayerFilter(u32 userLayer) : layer_(Layers::encode(userLayer, true)) {}
    bool ShouldCollide(JPH::ObjectLayer other) const override { return g_world->objPair.ShouldCollide(layer_, other); }
private:
    JPH::ObjectLayer layer_;
};

// A shape handle at a pose, as a Jolt query shape: the pose is the shape ORIGIN, Jolt wants the centre of mass.
struct QueryShape {
    JPH::RefConst<JPH::Shape> shape;
    JPH::RMat44 com;
};

bool resolveShape(int32_t handle, const float* pose7, QueryShape& out) {
    if (!g_world) { setAbiError(AbiError::NotInitialised); return false; }
    if (!pose7 || !finiteN(pose7, 7)) { setAbiError(AbiError::InvalidArgument); return false; }
    JPH::RefConst<JPH::Shape> s = shapeTableGet(handle);
    if (!s) { setAbiError(AbiError::BadHandle); return false; }
    if (!usableAsQueryShape(s)) {
        AVER_WARN("[Physics] shape {} is a mesh, heightfield or plane and cannot be a query shape", handle);
        setAbiError(AbiError::Unsupported);
        return false;
    }
    JPH::Quat q;
    if (!readJoltQuat(pose7 + 3, q)) { setAbiError(AbiError::InvalidArgument); return false; }
    const JPH::RMat44 origin = JPH::RMat44::sRotationTranslation(q, toJolt(Vec3(pose7[0], pose7[1], pose7[2])));
    out.com = origin.PreTranslated(s->GetCenterOfMass());
    out.shape = std::move(s);
    return true;
}

// One contact of the query shape (1) against a collider (2), in Jolt units, normal away from the collider.
struct Contact {
    JPH::BodyID body;
    JPH::SubShapeID sub;
    JPH::Vec3 pointOnShape, pointOnOther, normal;
    float depthM = 0.0f;       // signed: negative = separated by that much
    int32_t handle = 0;
};

// Every contact within `separationM`, deepest first; ties by body handle then sub-shape id so the order is deterministic.
void collectContacts(const JPH::Shape* shape, const JPH::RMat44& com, float separationM, const JPH::BodyFilter& body,
                     const JPH::ObjectLayerFilter& layers, std::vector<Contact>& out) {
    JPH::CollideShapeSettings settings;
    settings.mMaxSeparationDistance = separationM;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    g_world->system.GetNarrowPhaseQuery().CollideShape(shape, JPH::Vec3::sReplicate(1.0f), com, settings, JPH::RVec3::sZero(),
                                                       collector, {}, layers, body);
    out.clear();
    for (const JPH::CollideShapeResult& r : collector.mHits) {
        const int32_t h = handleForBodyId(r.mBodyID2);
        if (!h) continue;
        Contact c;
        c.body = r.mBodyID2;
        c.sub = r.mSubShapeID2;
        c.pointOnShape = r.mContactPointOn1;
        c.pointOnOther = r.mContactPointOn2;
        c.normal = (-r.mPenetrationAxis).NormalizedOr(JPH::Vec3::sAxisY());
        c.depthM = r.mPenetrationDepth;
        c.handle = h;
        bool dup = false;
        for (const Contact& o : out) if (o.body == c.body && o.sub == c.sub) { dup = true; break; }
        if (!dup) out.push_back(c);
    }
    std::sort(out.begin(), out.end(), [](const Contact& a, const Contact& b) {
        if (a.depthM != b.depthM) return a.depthM > b.depthM;
        if (a.handle != b.handle) return a.handle < b.handle;
        return a.sub.GetValue() < b.sub.GetValue();
    });
}

Vec3 vel(const JPH::Vec3& v) { return fromJoltDir(v); }

// The collider's velocity at a point, and its angular velocity. Zero for a sensor, a static body or a dead one.
void colliderMotion(const JPH::BodyID& id, const JPH::Vec3& pointM, Vec3& linear, Vec3& angular, int32_t* entity) {
    linear = Vec3(0, 0, 0);
    angular = Vec3(0, 0, 0);
    if (entity) *entity = 0;
    JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), id);
    if (!lock.Succeeded()) return;
    const JPH::Body& b = lock.GetBody();
    if (entity) *entity = static_cast<int32_t>(b.GetUserData());
    if (b.IsSensor() || b.IsStatic()) return;
    linear = vel(b.GetPointVelocity(JPH::RVec3(pointM)));
    angular = fromJoltAngular(b.GetAngularVelocity());
}

// The first thing a shape cast hits, ignoring (at fraction 0) the bodies the shape already overlaps.
class FirstHitCollector final : public JPH::CastShapeCollector {
public:
    explicit FirstHitCollector(const std::vector<JPH::BodyID>& ignoreAtStart) : ignore_(ignoreAtStart) {}
    void AddHit(const ResultType& r) override {
        if (r.mFraction <= 1e-6f && isExcluded(ignore_, r.mBodyID2)) return;
        const float e = r.GetEarlyOutFraction();
        if (!had_ || e < hit_.GetEarlyOutFraction()) {
            UpdateEarlyOutFraction(e);
            hit_ = r;
            had_ = true;
        }
    }
    bool had() const { return had_; }
    const ResultType& hit() const { return hit_; }
private:
    const std::vector<JPH::BodyID>& ignore_;
    ResultType hit_;
    bool had_ = false;
};

struct CastOutcome {
    bool hit = false;
    float unsafe = 1.0f, safe = 1.0f;
    JPH::BodyID body;
    JPH::SubShapeID sub;
    JPH::Vec3 point = JPH::Vec3::sZero(), normal = JPH::Vec3::sAxisY();
};

// Casts `shape` by dispM from `com`. Overlaps deeper than kOverlapM at the start are ignored for a hit at fraction 0. Safe = the fraction at
// which the shape is still marginM from the surface: unsafe minus margin / cos(contact angle), the cosine floored so a grazing contact
// cannot ask for an unbounded backoff.
CastOutcome castMotion(const JPH::Shape* shape, const JPH::RMat44& com, const JPH::Vec3& dispM, float marginM,
                       const JPH::BodyFilter& body, const JPH::ObjectLayerFilter& layers) {
    CastOutcome o;
    const float len = dispM.Length();
    if (len < 1e-7f) return o;

    std::vector<Contact> start;
    collectContacts(shape, com, 0.0f, body, layers, start);
    std::vector<JPH::BodyID> ignore;
    for (const Contact& c : start) if (c.depthM > kOverlapM) ignore.push_back(c.body);
    std::sort(ignore.begin(), ignore.end(), [](const JPH::BodyID& a, const JPH::BodyID& b) {
        return a.GetIndexAndSequenceNumber() < b.GetIndexAndSequenceNumber();
    });

    const JPH::Vec3 dir = dispM / len;
    JPH::ShapeCastSettings settings;
    settings.mActiveEdgeMovementDirection = dir;
    settings.mUseShrunkenShapeAndConvexRadius = true;   // without it a long, shallow approach to a flat face can miss it
    const JPH::RShapeCast cast(shape, JPH::Vec3::sReplicate(1.0f), com, dispM);
    FirstHitCollector collector(ignore);
    g_world->system.GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), collector, {}, layers, body);
    if (!collector.had()) return o;

    const JPH::ShapeCastResult& h = collector.hit();
    o.hit = true;
    o.unsafe = std::clamp(h.mFraction, 0.0f, 1.0f);
    o.body = h.mBodyID2;
    o.sub = h.mSubShapeID2;
    o.point = h.mContactPointOn2;
    o.normal = (-h.mPenetrationAxis).NormalizedOr(JPH::Vec3::sAxisY());
    const float cosTheta = std::max(-dir.Dot(o.normal), 0.05f);
    const float back = marginM > 0.0f ? marginM / cosTheta : 0.0f;
    o.safe = std::clamp((o.unsafe * len - back) / len, 0.0f, o.unsafe);
    return o;
}

// Pushes the shape out of everything it overlaps, to half the margin away from each surface: up to kRecoveryPasses passes, each solving the
// contacts one at a time against the push already chosen (so coplanar duplicates and corners do not overshoot). Returns the total push (Jolt
// metres). `first` receives the first pass's contacts that needed pushing.
JPH::Vec3 recover(const JPH::Shape* shape, const JPH::RMat44& com, float marginM, const JPH::BodyFilter& body,
                  const JPH::ObjectLayerFilter& layers, std::vector<Contact>* first) {
    JPH::Vec3 total = JPH::Vec3::sZero();
    std::vector<Contact> contacts;
    for (int pass = 0; pass < kRecoveryPasses; ++pass) {
        collectContacts(shape, com.PostTranslated(total), marginM, body, layers, contacts);
        JPH::Vec3 step = JPH::Vec3::sZero();
        for (const Contact& c : contacts) {
            const float need = c.depthM + 0.5f * marginM;
            if (need <= kPushEpsM) continue;
            if (pass == 0 && first) first->push_back(c);
            const float remaining = need - step.Dot(c.normal);
            if (remaining > kPushEpsM) step += c.normal * remaining;
        }
        if (step.LengthSq() < kPushEpsM * kPushEpsM) break;
        total += step;
    }
    return total;
}

void writeContact(const Contact& c, float marginM, float* f, int32_t* iv) {
    Vec3 lin, ang;
    int32_t entity = 0;
    colliderMotion(c.body, c.pointOnOther, lin, ang, &entity);
    if (f) {
        writeVec(f + AVER_PHYS_TC_F_POINT, fromJolt(c.pointOnOther));
        writeVec(f + AVER_PHYS_TC_F_NORMAL, fromJoltUnit(c.normal));
        writeVec(f + AVER_PHYS_TC_F_VELOCITY, lin);
        writeVec(f + AVER_PHYS_TC_F_ANGULAR_VELOCITY, ang);
        f[AVER_PHYS_TC_F_DEPTH] = mToCm(std::max(0.0f, c.depthM + marginM));
    }
    if (iv) {
        iv[AVER_PHYS_TC_I_BODY] = c.handle;
        iv[AVER_PHYS_TC_I_ENTITY] = entity;
        iv[AVER_PHYS_TC_I_SUBSHAPE] = static_cast<int32_t>(c.sub.GetValue());
    }
}

} // namespace

extern "C" {

// ---- ray ---------------------------------------------------------------------------------------------

int32_t aver_phys_space_raycast(const float* origin3, const float* direction3, float maxDistCm, uint32_t collisionMask, int32_t flags,
                                const int32_t* excludeBodies, int32_t excludeCount, float* outDistance, float* outPoint,
                                float* outNormal, int32_t* outEntity, int32_t* outSubShape) {
    if (!g_world) return 0;
    if (!origin3 || !direction3 || !finiteN(origin3, 3) || !finiteN(direction3, 3) || !std::isfinite(maxDistCm)) {
        setAbiError(AbiError::InvalidArgument);
        return 0;
    }
    const Vec3 dir(direction3[0], direction3[1], direction3[2]);
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len <= 1e-6f || maxDistCm <= 0.0f) return 0;

    const JPH::Vec3 from = toJolt(Vec3(origin3[0], origin3[1], origin3[2]));
    const JPH::Vec3 d = toJoltUnit(Vec3(dir.x / len, dir.y / len, dir.z / len)) * cmToM(maxDistCm);
    const JPH::RRayCast ray{from, d};

    JPH::RayCastSettings settings;
    const JPH::EBackFaceMode mode = (flags & AVER_PHYS_SQ_BACK_FACES) ? JPH::EBackFaceMode::CollideWithBackFaces
                                                                        : JPH::EBackFaceMode::IgnoreBackFaces;
    settings.mBackFaceModeTriangles = mode;
    settings.mBackFaceModeConvex = mode;
    settings.mTreatConvexAsSolid = (flags & AVER_PHYS_SQ_FROM_INSIDE) != 0;

    const SpaceFilter filter(collisionMask, flags, excludeIds(excludeBodies, excludeCount));
    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> collector;
    g_world->system.GetNarrowPhaseQuery().CastRay(ray, settings, collector, {}, {}, filter);
    if (!collector.HadHit()) return 0;

    const JPH::RayCastResult& r = collector.mHit;
    const int32_t handle = handleForBodyId(r.mBodyID);
    if (!handle) return 0;
    const JPH::Vec3 p = ray.GetPointOnRay(r.mFraction);
    const bool fromInside = r.mFraction <= 1e-7f && (flags & AVER_PHYS_SQ_FROM_INSIDE) != 0;
    if (outDistance) *outDistance = r.mFraction * maxDistCm;
    if (outPoint) writeVec(outPoint, fromJolt(p));
    if (outEntity) *outEntity = 0;
    if (outSubShape) *outSubShape = static_cast<int32_t>(r.mSubShapeID2.GetValue());
    JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), r.mBodyID);
    if (lock.Succeeded()) {
        if (outEntity) *outEntity = static_cast<int32_t>(lock.GetBody().GetUserData());
        if (outNormal) writeVec(outNormal, fromInside ? Vec3(0, 0, 0)
                                                      : fromJoltUnit(lock.GetBody().GetWorldSpaceSurfaceNormal(r.mSubShapeID2, p)));
    } else if (outNormal) {
        writeVec(outNormal, fromInside ? Vec3(0, 0, 0) : Vec3(0, 0, 1));
    }
    return handle;
}

// ---- shape queries -------------------------------------------------------------------------------------

int32_t aver_phys_space_intersect_shape(int32_t shape, const float* pose7, float marginCm, uint32_t collisionMask, int32_t flags,
                                        const int32_t* excludeBodies, int32_t excludeCount, int32_t maxResults,
                                        int32_t* outBodies, int32_t* outSubShapes) {
    QueryShape q;
    if (!resolveShape(shape, pose7, q)) return 0;
    if (maxResults <= 0 || !std::isfinite(marginCm)) return 0;
    const SpaceFilter filter(collisionMask, flags, excludeIds(excludeBodies, excludeCount));
    const MaskLayerFilter layers(0xFFFFFFFFu);
    std::vector<Contact> hits;
    collectContacts(q.shape, q.com, cmToM(std::max(0.0f, marginCm)), filter, layers, hits);
    const int32_t n = std::min(static_cast<int32_t>(hits.size()), maxResults);
    for (int32_t i = 0; i < n; ++i) {
        if (outBodies) outBodies[i] = hits[static_cast<size_t>(i)].handle;
        if (outSubShapes) outSubShapes[i] = static_cast<int32_t>(hits[static_cast<size_t>(i)].sub.GetValue());
    }
    return n;
}

int32_t aver_phys_space_collide_shape(int32_t shape, const float* pose7, float marginCm, uint32_t collisionMask, int32_t flags,
                                      const int32_t* excludeBodies, int32_t excludeCount, int32_t maxContacts,
                                      int32_t* outBodies, float* outPointsOnShape, float* outPointsOnOther, float* outNormals,
                                      float* outDepths) {
    QueryShape q;
    if (!resolveShape(shape, pose7, q)) return 0;
    if (maxContacts <= 0 || !std::isfinite(marginCm)) return 0;
    const SpaceFilter filter(collisionMask, flags, excludeIds(excludeBodies, excludeCount));
    const MaskLayerFilter layers(0xFFFFFFFFu);
    std::vector<Contact> hits;
    collectContacts(q.shape, q.com, cmToM(std::max(0.0f, marginCm)), filter, layers, hits);
    const int32_t n = std::min(static_cast<int32_t>(hits.size()), maxContacts);
    for (int32_t i = 0; i < n; ++i) {
        const Contact& c = hits[static_cast<size_t>(i)];
        if (outBodies) outBodies[i] = c.handle;
        if (outPointsOnShape) writeVec(outPointsOnShape + 3 * i, fromJolt(c.pointOnShape));
        if (outPointsOnOther) writeVec(outPointsOnOther + 3 * i, fromJolt(c.pointOnOther));
        if (outNormals) writeVec(outNormals + 3 * i, fromJoltUnit(c.normal));
        if (outDepths) outDepths[i] = mToCm(c.depthM);
    }
    return n;
}

int32_t aver_phys_space_rest_info(int32_t shape, const float* pose7, float marginCm, uint32_t collisionMask, int32_t flags,
                                  const int32_t* excludeBodies, int32_t excludeCount, float* outPoint, float* outNormal,
                                  float* outVelocity, float* outDepth, int32_t* outEntity, int32_t* outSubShape) {
    QueryShape q;
    if (!resolveShape(shape, pose7, q)) return 0;
    if (!std::isfinite(marginCm)) { setAbiError(AbiError::InvalidArgument); return 0; }
    const SpaceFilter filter(collisionMask, flags, excludeIds(excludeBodies, excludeCount));
    const MaskLayerFilter layers(0xFFFFFFFFu);
    std::vector<Contact> hits;
    collectContacts(q.shape, q.com, cmToM(std::max(0.0f, marginCm)), filter, layers, hits);
    if (hits.empty()) return 0;
    const Contact& c = hits.front();
    Vec3 lin, ang;
    int32_t entity = 0;
    colliderMotion(c.body, c.pointOnOther, lin, ang, &entity);
    if (outPoint) writeVec(outPoint, fromJolt(c.pointOnOther));
    if (outNormal) writeVec(outNormal, fromJoltUnit(c.normal));
    if (outVelocity) writeVec(outVelocity, lin);
    if (outDepth) *outDepth = mToCm(c.depthM);
    if (outEntity) *outEntity = entity;
    if (outSubShape) *outSubShape = static_cast<int32_t>(c.sub.GetValue());
    return c.handle;
}

int32_t aver_phys_space_cast_motion(int32_t shape, const float* pose7, const float* motion3, float marginCm, uint32_t collisionMask,
                                    int32_t flags, const int32_t* excludeBodies, int32_t excludeCount, float* outSafe,
                                    float* outUnsafe, int32_t* outBody, float* outPoint, float* outNormal) {
    if (outSafe) *outSafe = 1.0f;
    if (outUnsafe) *outUnsafe = 1.0f;
    if (outBody) *outBody = 0;
    QueryShape q;
    if (!resolveShape(shape, pose7, q)) return 0;
    if (!motion3 || !finiteN(motion3, 3) || !std::isfinite(marginCm)) { setAbiError(AbiError::InvalidArgument); return 0; }
    const SpaceFilter filter(collisionMask, flags, excludeIds(excludeBodies, excludeCount));
    const MaskLayerFilter layers(0xFFFFFFFFu);
    const CastOutcome o = castMotion(q.shape, q.com, toJoltDir(Vec3(motion3[0], motion3[1], motion3[2])),
                                     cmToM(std::max(0.0f, marginCm)), filter, layers);
    if (!o.hit) return 1;
    if (outSafe) *outSafe = o.safe;
    if (outUnsafe) *outUnsafe = o.unsafe;
    if (outBody) *outBody = handleForBodyId(o.body);
    if (outPoint) writeVec(outPoint, fromJolt(o.point));
    if (outNormal) writeVec(outNormal, fromJoltUnit(o.normal));
    return 1;
}

// ---- body_test_motion ------------------------------------------------------------------------------------

int32_t aver_phys_body_test_motion(int32_t body, const float* originPose7, const float* motion3, float marginCm, int32_t flags,
                                   const int32_t* excludeBodies, int32_t excludeCount, int32_t maxContacts, float* outF,
                                   int32_t* outI, float* outContactF, int32_t* outContactI) {
    if (outF) std::fill(outF, outF + AVER_PHYS_TM_F_COUNT, 0.0f);
    if (outI) std::fill(outI, outI + AVER_PHYS_TM_I_COUNT, 0);
    if (outF) outF[AVER_PHYS_TM_F_SAFE] = outF[AVER_PHYS_TM_F_UNSAFE] = 1.0f;
    if (!g_world) { setAbiError(AbiError::NotInitialised); return -1; }
    const JPH::BodyID* idp = findBody(body);
    if (!idp) { setAbiError(AbiError::BadHandle); return -1; }
    if (!motion3 || !finiteN(motion3, 3) || !std::isfinite(marginCm) || (originPose7 && !finiteN(originPose7, 7))) {
        setAbiError(AbiError::InvalidArgument);
        return -1;
    }

    // What the test needs from the body, copied out so its lock is not held during the queries.
    JPH::RefConst<JPH::Shape> shape;
    JPH::RMat44 com;
    JPH::CollisionGroup group;
    u32 userLayer = 0;
    const JPH::BodyID id = *idp;
    {
        JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), id);
        if (!lock.Succeeded()) { setAbiError(AbiError::BadHandle); return -1; }
        const JPH::Body& b = lock.GetBody();
        if (b.IsSoftBody() || !usableAsQueryShape(b.GetShape())) {
            AVER_WARN("[Physics] body {} has a soft, mesh or heightfield shape and cannot be motion-tested", body);
            setAbiError(AbiError::Unsupported);
            return -1;
        }
        shape = b.GetShape();
        group = b.GetCollisionGroup();
        userLayer = Layers::userOf(b.GetObjectLayer());
        com = b.GetCenterOfMassTransform();
    }
    if (originPose7) {
        JPH::Quat qr;
        if (!readJoltQuat(originPose7 + 3, qr)) { setAbiError(AbiError::InvalidArgument); return -1; }
        com = JPH::RMat44::sRotationTranslation(qr, toJolt(Vec3(originPose7[0], originPose7[1], originPose7[2])))
                  .PreTranslated(shape->GetCenterOfMass());
    }

    const float marginM = cmToM(std::max(0.0f, marginCm));
    const Vec3 motionCm(motion3[0], motion3[1], motion3[2]);
    const JPH::Vec3 motionM = toJoltDir(motionCm);
    const MotionFilter filter(id, group, flags, excludeIds(excludeBodies, excludeCount));
    const MovingPairLayerFilter layers(userLayer);

    // 1. recovery
    std::vector<Contact> recoveryContacts;
    const JPH::Vec3 recM = recover(shape, com, marginM, filter, layers, &recoveryContacts);
    const JPH::RMat44 comR = com.PostTranslated(recM);
    const bool recoveryCollides = (flags & AVER_PHYS_SQ_RECOVERY_AS_COLLISION) != 0 && !recoveryContacts.empty();

    // 2. cast from the recovered pose
    const CastOutcome cast = castMotion(shape, comR, motionM, marginM, filter, layers);
    const float safe = cast.hit ? cast.safe : 1.0f;
    const float unsafe = cast.hit ? cast.unsafe : 1.0f;
    const bool blocked = cast.hit && safe < 1.0f;

    // 3. contacts where the shape touches
    std::vector<Contact> contacts;
    if (blocked) {
        const JPH::Vec3 dir = motionM.NormalizedOr(JPH::Vec3::sZero());
        const JPH::RMat44 comU = comR.PostTranslated(dir * (motionM.Length() * unsafe));
        collectContacts(shape, comU, marginM + kContactSlackM, filter, layers, contacts);
        auto it = std::find_if(contacts.begin(), contacts.end(), [&](const Contact& c) { return c.body == cast.body && c.sub == cast.sub; });
        if (it != contacts.end()) std::rotate(contacts.begin(), it, it + 1);   // the contact that stopped us leads
        if (contacts.empty()) {
            Contact c;
            c.body = cast.body;
            c.sub = cast.sub;
            c.pointOnOther = c.pointOnShape = cast.point;
            c.normal = cast.normal;
            c.depthM = -marginM;
            c.handle = handleForBodyId(cast.body);
            if (c.handle) contacts.push_back(c);
        }
    } else if (recoveryCollides) {
        contacts = recoveryContacts;
    }

    const bool collided = blocked || recoveryCollides;
    if (outF) {
        const Vec3 rec = fromJolt(recM);
        writeVec(outF + AVER_PHYS_TM_F_TRAVEL, Vec3(rec.x + motionCm.x * safe, rec.y + motionCm.y * safe, rec.z + motionCm.z * safe));
        writeVec(outF + AVER_PHYS_TM_F_REMAINDER, Vec3(motionCm.x * (1 - safe), motionCm.y * (1 - safe), motionCm.z * (1 - safe)));
        writeVec(outF + AVER_PHYS_TM_F_RECOVERY, rec);
        outF[AVER_PHYS_TM_F_SAFE] = safe;
        outF[AVER_PHYS_TM_F_UNSAFE] = unsafe;
    }
    const int32_t written = std::min(static_cast<int32_t>(contacts.size()), std::max(maxContacts, 0));
    for (int32_t i = 0; i < written; ++i)
        writeContact(contacts[static_cast<size_t>(i)], marginM,
                     outContactF ? outContactF + static_cast<size_t>(i) * AVER_PHYS_TC_F_COUNT : nullptr,
                     outContactI ? outContactI + static_cast<size_t>(i) * AVER_PHYS_TC_I_COUNT : nullptr);
    if (collided && !contacts.empty()) {
        float f[AVER_PHYS_TC_F_COUNT];
        int32_t iv[AVER_PHYS_TC_I_COUNT];
        writeContact(contacts.front(), marginM, f, iv);
        if (outF) {
            writeVec(outF + AVER_PHYS_TM_F_POINT, Vec3(f[0], f[1], f[2]));
            writeVec(outF + AVER_PHYS_TM_F_NORMAL, Vec3(f[3], f[4], f[5]));
            writeVec(outF + AVER_PHYS_TM_F_VELOCITY, Vec3(f[6], f[7], f[8]));
            writeVec(outF + AVER_PHYS_TM_F_ANGULAR_VELOCITY, Vec3(f[9], f[10], f[11]));
            outF[AVER_PHYS_TM_F_DEPTH] = f[AVER_PHYS_TC_F_DEPTH];
        }
        if (outI) {
            outI[AVER_PHYS_TM_I_COLLIDER] = iv[AVER_PHYS_TC_I_BODY];
            outI[AVER_PHYS_TM_I_ENTITY] = iv[AVER_PHYS_TC_I_ENTITY];
            outI[AVER_PHYS_TM_I_SUBSHAPE] = iv[AVER_PHYS_TC_I_SUBSHAPE];
        }
    }
    if (outI) outI[AVER_PHYS_TM_I_CONTACTS] = written;
    return collided ? 1 : 0;
}

} // extern "C"
