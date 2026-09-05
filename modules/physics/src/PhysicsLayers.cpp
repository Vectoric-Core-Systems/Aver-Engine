// PhysicsLayers.cpp -- the collision matrix, a body's layer, and the filtered forms of the three
// spatial queries.
//
// THE FILTERED QUERIES ARE _ex FUNCTIONS RATHER THAN WIDER SIGNATURES, and that is a deliberate cost:
// this file repeats a good deal of PhysicsWorld.cpp's query logic rather than sharing it. The
// alternative was to widen aver_phys_raycast/overlap_sphere/sphere_cast with two more parameters, and
// those three are bound in C#, reached from graph nodes and called from several places in the editor
// -- every one of which would have had to change to pass a constant. A frozen signature is worth more
// than the duplication, and the duplication is bounded: three functions, each about twenty lines.
#include "aver/physics/physics_layers_abi.h"
#include "aver/physics/physics_abi.h"
#include "Convert.hpp"
#include "PhysicsInternal.hpp"

#include "aver/core/Log.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <cmath>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

// Accepts an object layer whose USER half is in the caller's mask. The broad-phase half of the layer
// is deliberately not consulted: a caller filtering by layer is asking about game categories, and
// whether a body happens to be static is not one.
class MaskLayerFilter final : public JPH::ObjectLayerFilter {
public:
    explicit MaskLayerFilter(uint32_t mask) : mask_(mask) {}
    bool ShouldCollide(JPH::ObjectLayer l) const override {
        return (mask_ & (1u << Layers::userOf(l))) != 0u;
    }
private:
    uint32_t mask_;
};

// Skips one body. THE SINGLE MOST USEFUL FILTER THERE IS: a ray fired from inside your own capsule
// hits yourself at distance zero, which is the first bug every first-person weapon has.
class IgnoreBodyFilter final : public JPH::BodyFilter {
public:
    explicit IgnoreBodyFilter(const JPH::BodyID& id) : id_(id) {}
    bool ShouldCollide(const JPH::BodyID& other) const override { return other != id_; }
    bool ShouldCollideLocked(const JPH::Body& body) const override { return body.GetID() != id_; }
private:
    JPH::BodyID id_;
};

// The body handle behind a Jolt id, searching characters too -- a character's inner body is created
// and owned by Jolt, so it is never in g_world->bodies. Lifted from aver_phys_raycast, which needs
// the same answer for the same reason.
int32_t handleForBodyId(const JPH::BodyID& id) {
    for (const auto& [h, bid] : g_world->bodies) if (bid == id) return h;
    for (const auto& [h, ch] : g_world->characters)
        if (!ch->GetInnerBodyID().IsInvalid() && ch->GetInnerBodyID() == id) return h;
    return 0;
}

// The Jolt id to skip for a caller's `ignoreBody`, or an invalid id for "skip nothing". A handle that
// does not resolve is treated as "nothing to ignore" rather than as an error: the common way to get
// one is to pass the handle of a body destroyed a moment ago, and refusing the whole query for that
// would turn a stale handle into a weapon that stops firing.
JPH::BodyID ignoredIdOf(int32_t ignoreBody) {
    if (ignoreBody == 0) return JPH::BodyID();
    if (const JPH::BodyID* id = findBody(ignoreBody)) return *id;
    if (JPH::CharacterVirtual* ch = findCharacter(ignoreBody)) return ch->GetInnerBodyID();
    return JPH::BodyID();
}

bool layerInRange(int32_t l) { return l >= 0 && l < AVER_PHYS_LAYER_COUNT; }

} // namespace

extern "C" {

// ---- the matrix --------------------------------------------------------------------------------------

int32_t aver_phys_set_layer_collision(int32_t layerA, int32_t layerB, int32_t enabled) {
    if (!g_world) return 0;
    if (!layerInRange(layerA) || !layerInRange(layerB)) {
        AVER_WARN("[Physics] collision layers must be 0..{}; got {} and {}",
                  AVER_PHYS_LAYER_COUNT - 1, layerA, layerB);
        return 0;
    }
    g_world->objPair.setCollides(static_cast<u32>(layerA), static_cast<u32>(layerB), enabled != 0);
    return 1;
}

int32_t aver_phys_layer_collision(int32_t layerA, int32_t layerB) {
    if (!g_world || !layerInRange(layerA) || !layerInRange(layerB)) return 0;
    return g_world->objPair.collides(static_cast<u32>(layerA), static_cast<u32>(layerB)) ? 1 : 0;
}

void aver_phys_reset_layer_collisions(void) {
    if (!g_world) return;
    for (u32 i = 0; i < Layers::kUserLayerCount; ++i)
        for (u32 j = 0; j < Layers::kUserLayerCount; ++j) g_world->objPair.setCollides(i, j, true);
}

// ---- a body's layer ------------------------------------------------------------------------------------

int32_t aver_phys_body_set_layer(int32_t body, int32_t layer) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    if (!layerInRange(layer)) {
        AVER_WARN("[Physics] collision layer must be 0..{}; got {}", AVER_PHYS_LAYER_COUNT - 1, layer);
        return 0;
    }
    // THE MOVING HALF IS READ BACK AND PUT BACK. It is the body's own structural fact, not the
    // caller's to change here -- moving a static floor onto layer 3 must leave it static, and
    // re-encoding from `dynamic` would silently make it moving instead.
    const bool moving = Layers::movingOf(bi().GetObjectLayer(*id));
    bi().SetObjectLayer(*id, Layers::encode(static_cast<u32>(layer), moving));
    return 1;
}

int32_t aver_phys_body_layer(int32_t body) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return -1;   // not 0: 0 is a real layer
    return static_cast<int32_t>(Layers::userOf(bi().GetObjectLayer(*id)));
}

// ---- the filtered queries --------------------------------------------------------------------------

int32_t aver_phys_raycast_ex(float ox, float oy, float oz, float dx, float dy, float dz,
                             float maxDistCm, uint32_t layerMask, int32_t ignoreBody,
                             float* outPoint, float* outNormal, int32_t* outEntity) {
    if (!g_world) return 0;
    const Vec3 dir(dx, dy, dz);
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len <= 1e-6f || maxDistCm <= 0.0f) return 0;

    const JPH::Vec3 from = toJolt(Vec3(ox, oy, oz));
    const JPH::Vec3 d = toJoltUnit(Vec3(dir.x / len, dir.y / len, dir.z / len)) * cmToM(maxDistCm);
    JPH::RRayCast ray{from, d};
    JPH::RayCastResult hit;

    const MaskLayerFilter layers(layerMask);
    const IgnoreBodyFilter skip(ignoredIdOf(ignoreBody));
    if (!g_world->system.GetNarrowPhaseQuery().CastRay(ray, hit, {}, layers, skip)) return 0;

    const int32_t handle = handleForBodyId(hit.mBodyID);
    if (outPoint) writeVec(outPoint, fromJolt(ray.GetPointOnRay(hit.mFraction)));
    if (outEntity || outNormal) {
        JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), hit.mBodyID);
        if (lock.Succeeded()) {
            if (outEntity) *outEntity = static_cast<int32_t>(lock.GetBody().GetUserData());
            if (outNormal) {
                const JPH::Vec3 n = lock.GetBody().GetWorldSpaceSurfaceNormal(
                    hit.mSubShapeID2, ray.GetPointOnRay(hit.mFraction));
                writeVec(outNormal, fromJoltUnit(n));
            }
        } else {
            if (outEntity) *outEntity = 0;
            if (outNormal) writeVec(outNormal, Vec3(0, 0, 1));
        }
    }
    return handle;
}

int32_t aver_phys_overlap_sphere_ex(float x, float y, float z, float radius,
                                    uint32_t layerMask, int32_t ignoreBody,
                                    int32_t* outBodies, int32_t maxBodies) {
    if (!g_world || !outBodies || maxBodies <= 0 || radius <= 0.0f) return 0;

    JPH::SphereShape sphere(cmToM(radius));
    // SetEmbedded, exactly as aver_phys_overlap_sphere does: the shape is stack-allocated and lives
    // only for this query, so it must be told not to expect reference counting to own it.
    sphere.SetEmbedded();

    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    const JPH::RMat44 xf = JPH::RMat44::sTranslation(toJolt(Vec3(x, y, z)));
    const MaskLayerFilter layers(layerMask);
    const IgnoreBodyFilter skip(ignoredIdOf(ignoreBody));
    g_world->system.GetNarrowPhaseQuery().CollideShape(
        &sphere, JPH::Vec3::sReplicate(1.0f), xf, JPH::CollideShapeSettings{},
        JPH::RVec3::sZero(), collector, {}, layers, skip);

    int32_t n = 0;
    for (const JPH::CollideShapeResult& r : collector.mHits) {
        if (n >= maxBodies) break;
        const int32_t h = handleForBodyId(r.mBodyID2);
        if (!h) continue;
        bool already = false;
        for (int32_t i = 0; i < n; ++i) if (outBodies[i] == h) { already = true; break; }
        if (already) continue;   // one body can report several sub-shape hits; the caller wants bodies
        outBodies[n++] = h;
    }
    return n;
}

int32_t aver_phys_sphere_cast_ex(float ox, float oy, float oz, float dx, float dy, float dz,
                                 float maxDistCm, float radius,
                                 uint32_t layerMask, int32_t ignoreBody,
                                 float* outPoint, float* outNormal) {
    if (!g_world || radius <= 0.0f || maxDistCm <= 0.0f) return 0;
    const Vec3 dir(dx, dy, dz);
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len <= 1e-6f) return 0;

    JPH::SphereShape sphere(cmToM(radius));
    sphere.SetEmbedded();
    const JPH::Vec3 from = toJolt(Vec3(ox, oy, oz));
    const JPH::Vec3 d = toJoltUnit(Vec3(dir.x / len, dir.y / len, dir.z / len)) * cmToM(maxDistCm);
    const JPH::RShapeCast cast(&sphere, JPH::Vec3::sReplicate(1.0f),
                               JPH::Mat44::sTranslation(from), d);

    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    const MaskLayerFilter layers(layerMask);
    const IgnoreBodyFilter skip(ignoredIdOf(ignoreBody));
    g_world->system.GetNarrowPhaseQuery().CastShape(cast, {}, JPH::RVec3::sZero(), collector,
                                                    {}, layers, skip);
    if (!collector.HadHit()) return 0;

    if (outPoint)  writeVec(outPoint, fromJolt(collector.mHit.mContactPointOn2));
    // NEGATED, because Jolt reports the penetration axis -- which points INTO the surface -- and every
    // other normal this ABI hands out points away from it. aver_phys_sphere_cast does the same.
    if (outNormal) writeVec(outNormal, fromJoltUnit(-collector.mHit.mPenetrationAxis.Normalized()));
    return handleForBodyId(collector.mHit.mBodyID2);
}

} // extern "C"
