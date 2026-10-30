// PhysicsLayerFilter.cpp -- 32-bit layer/mask collision filtering on top of Jolt: a layer and mask per body, per-pair exceptions, joint
// exclusion, and the queries that take a 32-bit mask. The rationale and the coexistence rule with the 16-layer matrix are in
// docs/physics/LAYER_FILTERING.md; the ABI is in physics_layers_abi.h and physics_joints_abi.h.
//
// HOW IT HOOKS IN: a filtered or excepted body carries a Jolt CollisionGroup whose filter is OURS, whose group id is the invalid
// one (so ragdoll and soft-body group filters, which compare group ids, never see a clash) and whose sub-group id is a SLOT in a
// world-side table holding that body's layer, mask and a "uses the mask rule" flag. Jolt asks the group filter in the broad phase
// pair finder (and the CCD path), so a pair that cannot collide never reaches the narrow phase, and the group travels in
// BodyCreationSettings so it survives a body being rebuilt under the same handle.
#include "PhysicsLayerFilter.hpp"

#include "aver/physics/physics_layers_abi.h"
#include "Convert.hpp"
#include "PhysicsQueryFilters.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Collision/GroupFilter.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace aver::physics::detail {

inline constexpr uint32_t kNone = 0xFFFFFFFFu;

struct LayerSlot {
    uint32_t layer = 0, mask = 0;
    bool useMask = false;     // false: tagged only because of an exception or a joint
    bool used = false;
    uint32_t pins = 0;        // exception entries naming this slot; a pinned slot is never freed
    int32_t handle = 0;       // body handle, to find the partner of an exception
};

inline uint64_t pairKey(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    return (static_cast<uint64_t>(a) << 32) | b;
}

struct LayerFilterState {
    JPH::Ref<JPH::GroupFilter> filter;
    std::vector<LayerSlot> slots;           // indexed by sub-group id
    std::vector<uint32_t> freeSlots;
    struct Exc { uint16_t user = 0, joints = 0; };
    std::unordered_map<uint64_t, Exc> exc;  // an entry exists only while it excludes the pair

    uint32_t slotOf(const JPH::CollisionGroup& g) const {
        if (g.GetGroupFilter() != filter.GetPtr()) return kNone;
        const uint32_t s = g.GetSubGroupID();
        return s < slots.size() && slots[s].used ? s : kNone;
    }

    // Runs on workers during a step; the tables change only between steps.
    bool collide(uint32_t s1, uint32_t s2) const {
        if (s1 >= slots.size() || s2 >= slots.size()) return true;
        if (!exc.empty() && exc.find(pairKey(s1, s2)) != exc.end()) return false;
        const LayerSlot &a = slots[s1], &b = slots[s2];
        if (!a.useMask || !b.useMask) return true;
        return ((a.mask & b.layer) | (b.mask & a.layer)) != 0u;
    }
};

class LayerGroupFilter final : public JPH::GroupFilter {
public:
    explicit LayerGroupFilter(const LayerFilterState* st) : st_(st) {}
    // A pair with a body outside this filter (no group, a ragdoll's, a soft body's) is the matrix's business alone.
    bool CanCollide(const JPH::CollisionGroup& g1, const JPH::CollisionGroup& g2) const override {
        if (g1.GetGroupFilter() != this || g2.GetGroupFilter() != this) return true;
        return st_->collide(g1.GetSubGroupID(), g2.GetSubGroupID());
    }
private:
    const LayerFilterState* st_;
};

namespace {

LayerFilterState& stateOrCreate() {
    if (!g_world->layerFilter) {
        auto s = std::make_shared<LayerFilterState>();
        s->filter = new LayerGroupFilter(s.get());
        g_world->layerFilter = std::move(s);
    }
    return *g_world->layerFilter;
}

// Setters touch tables the workers read, so they wait for the step to end.
bool mutableNow() {
    if (!g_world) return false;
    if (g_world->inStep) { setAbiError(AbiError::Unsupported); return false; }
    return true;
}

const JPH::BodyID* idOf(int32_t handle) {
    const auto it = g_world->bodies.find(handle);
    return it == g_world->bodies.end() ? nullptr : &it->second;
}

// Tags the body with a slot (reusing the one it has). kNone if it cannot take one: dead, or already in another group (ragdoll, soft body).
uint32_t acquire(LayerFilterState& st, int32_t handle, const JPH::BodyID& id) {
    JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), id);
    if (!lock.Succeeded()) { setAbiError(AbiError::BadHandle); return kNone; }
    JPH::Body& b = lock.GetBody();
    const JPH::CollisionGroup& g = b.GetCollisionGroup();
    if (const uint32_t s = st.slotOf(g); s != kNone) return s;
    if (g.GetGroupFilter() != nullptr || g.GetGroupID() != JPH::CollisionGroup::cInvalidGroup) {
        AVER_WARN("[Physics] body {} is in a collision group of its own (ragdoll part or soft body) and cannot take a layer filter", handle);
        setAbiError(AbiError::Unsupported);
        return kNone;
    }
    uint32_t s;
    if (!st.freeSlots.empty()) { s = st.freeSlots.back(); st.freeSlots.pop_back(); }
    else { s = static_cast<uint32_t>(st.slots.size()); st.slots.emplace_back(); }
    st.slots[s] = LayerSlot{};
    st.slots[s].used = true;
    st.slots[s].handle = handle;
    b.SetCollisionGroup(JPH::CollisionGroup(st.filter.GetPtr(), JPH::CollisionGroup::cInvalidGroup, s));
    return s;
}

uint32_t slotOfHandle(LayerFilterState& st, int32_t handle) {
    const JPH::BodyID* id = idOf(handle);
    if (!id) return kNone;
    JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), *id);
    return lock.Succeeded() ? st.slotOf(lock.GetBody().GetCollisionGroup()) : kNone;
}

// Frees the body's slot and clears its group once nothing needs it (no mask rule, no exception).
void releaseIfIdle(LayerFilterState& st, int32_t handle) {
    const JPH::BodyID* id = idOf(handle);
    if (!id) return;
    JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded()) return;
    JPH::Body& b = lock.GetBody();
    const uint32_t s = st.slotOf(b.GetCollisionGroup());
    if (s == kNone || st.slots[s].useMask || st.slots[s].pins != 0) return;
    st.slots[s].used = false;
    st.freeSlots.push_back(s);
    b.SetCollisionGroup(JPH::CollisionGroup());
}

// A body resting on one it no longer collides with must fall, and a sleeping pair that now collides must be reconsidered.
void wake(int32_t handle) {
    const JPH::BodyID* id = idOf(handle);
    if (id && bi().GetMotionType(*id) != JPH::EMotionType::Static) bi().ActivateBody(*id);
}

void retain(LayerFilterState& st, uint32_t sa, uint32_t sb, bool byJoint) {
    LayerFilterState::Exc& e = st.exc[pairKey(sa, sb)];
    const bool fresh = e.user == 0 && e.joints == 0;
    if (byJoint) ++e.joints; else e.user = 1;
    if (fresh) { ++st.slots[sa].pins; ++st.slots[sb].pins; }
}

// Returns 0 if there was nothing of that kind to drop, 1 if a reference went, 2 if the entry is gone and the slots are unpinned.
int drop(LayerFilterState& st, uint32_t sa, uint32_t sb, bool byJoint) {
    const auto it = st.exc.find(pairKey(sa, sb));
    if (it == st.exc.end()) return 0;
    LayerFilterState::Exc& e = it->second;
    if (byJoint) { if (e.joints == 0) return 0; --e.joints; }
    else         { if (e.user == 0) return 0; e.user = 0; }
    if (e.user != 0 || e.joints != 0) return 1;
    st.exc.erase(it);
    --st.slots[sa].pins;
    --st.slots[sb].pins;
    return 2;
}

} // namespace

uint32_t layerBitsOfBody(const JPH::Body& body) {
    if (g_world && g_world->layerFilter) {
        const LayerFilterState& st = *g_world->layerFilter;
        const uint32_t s = st.slotOf(body.GetCollisionGroup());
        if (s != kNone && st.slots[s].useMask) return st.slots[s].layer;
    }
    return 1u << Layers::userOf(body.GetObjectLayer());
}

bool layerJointExclusion(int32_t ha, int32_t hb, bool on) {
    if (!mutableNow()) return false;
    if (ha == 0 || hb == 0 || ha == hb) return true;   // the world side: nothing to exclude
    const JPH::BodyID *ia = idOf(ha), *ib = idOf(hb);
    if (on) {
        if (!ia || !ib) return false;
        LayerFilterState& st = stateOrCreate();
        const uint32_t sa = acquire(st, ha, *ia);
        if (sa == kNone) return false;
        const uint32_t sb = acquire(st, hb, *ib);
        if (sb == kNone) { releaseIfIdle(st, ha); return false; }
        retain(st, sa, sb, true);
    } else {
        if (!g_world->layerFilter) return true;
        LayerFilterState& st = *g_world->layerFilter;
        const uint32_t sa = slotOfHandle(st, ha), sb = slotOfHandle(st, hb);
        if (sa == kNone || sb == kNone) return true;   // a body went first and took its exceptions with it
        if (drop(st, sa, sb, true) == 2) { releaseIfIdle(st, ha); releaseIfIdle(st, hb); }
    }
    wake(ha);
    wake(hb);
    return true;
}

void layerFilterOnBodyRemoved(int32_t handle, const JPH::BodyID& id) {
    if (!g_world || !g_world->layerFilter) return;
    LayerFilterState& st = *g_world->layerFilter;
    uint32_t s;
    {
        JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), id);
        s = lock.Succeeded() ? st.slotOf(lock.GetBody().GetCollisionGroup()) : kNone;
    }
    if (s == kNone) return;
    std::vector<int32_t> partners;
    for (auto it = st.exc.begin(); it != st.exc.end();) {
        const uint32_t lo = static_cast<uint32_t>(it->first >> 32), hi = static_cast<uint32_t>(it->first & 0xFFFFFFFFu);
        if (lo != s && hi != s) { ++it; continue; }
        const uint32_t other = lo == s ? hi : lo;
        --st.slots[other].pins;
        partners.push_back(st.slots[other].handle);
        it = st.exc.erase(it);
    }
    st.slots[s] = LayerSlot{};
    st.freeSlots.push_back(s);
    (void)handle;
    for (const int32_t p : partners) releaseIfIdle(st, p);
}

} // namespace aver::physics::detail

extern "C" {

// ---- layer and mask ----------------------------------------------------------------------------------

int32_t aver_phys_body_set_collision_filter(int32_t body, uint32_t layerBits, uint32_t maskBits) {
    if (!mutableNow()) return 0;
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    LayerFilterState& st = stateOrCreate();
    const uint32_t s = acquire(st, body, *id);
    if (s == kNone) return 0;
    st.slots[s].layer = layerBits;
    st.slots[s].mask = maskBits;
    st.slots[s].useMask = true;
    wake(body);
    return 1;
}

int32_t aver_phys_body_collision_filter(int32_t body, uint32_t* outLayerBits, uint32_t* outMaskBits) {
    if (!g_world || !g_world->layerFilter || !findBody(body)) return 0;
    LayerFilterState& st = *g_world->layerFilter;
    const uint32_t s = slotOfHandle(st, body);
    if (s == kNone || !st.slots[s].useMask) return 0;
    if (outLayerBits) *outLayerBits = st.slots[s].layer;
    if (outMaskBits) *outMaskBits = st.slots[s].mask;
    return 1;
}

int32_t aver_phys_body_clear_collision_filter(int32_t body) {
    if (!mutableNow()) return 0;
    if (!findBody(body)) return 0;
    if (!g_world->layerFilter) return 1;
    LayerFilterState& st = *g_world->layerFilter;
    const uint32_t s = slotOfHandle(st, body);
    if (s == kNone) return 1;
    st.slots[s].useMask = false;
    releaseIfIdle(st, body);
    wake(body);
    return 1;
}

// ---- exceptions --------------------------------------------------------------------------------------

int32_t aver_phys_body_add_collision_exception(int32_t bodyA, int32_t bodyB) {
    if (!mutableNow() || bodyA == bodyB) return 0;
    const JPH::BodyID* ia = findBody(bodyA);
    const JPH::BodyID* ib = findBody(bodyB);
    if (!ia || !ib) return 0;
    LayerFilterState& st = stateOrCreate();
    const uint32_t sa = acquire(st, bodyA, *ia);
    if (sa == kNone) return 0;
    const uint32_t sb = acquire(st, bodyB, *ib);
    if (sb == kNone) { releaseIfIdle(st, bodyA); return 0; }
    retain(st, sa, sb, false);
    wake(bodyA);
    wake(bodyB);
    return 1;
}

int32_t aver_phys_body_remove_collision_exception(int32_t bodyA, int32_t bodyB) {
    if (!mutableNow() || !g_world->layerFilter || !idOf(bodyA) || !idOf(bodyB)) return 0;
    LayerFilterState& st = *g_world->layerFilter;
    const uint32_t sa = slotOfHandle(st, bodyA), sb = slotOfHandle(st, bodyB);
    if (sa == kNone || sb == kNone) return 0;
    const int r = drop(st, sa, sb, false);
    if (r == 0) return 0;
    if (r == 2) { releaseIfIdle(st, bodyA); releaseIfIdle(st, bodyB); }
    wake(bodyA);
    wake(bodyB);
    return 1;
}

int32_t aver_phys_body_has_collision_exception(int32_t bodyA, int32_t bodyB) {
    if (!g_world || !g_world->layerFilter || !idOf(bodyA) || !idOf(bodyB)) return 0;
    LayerFilterState& st = *g_world->layerFilter;
    const uint32_t sa = slotOfHandle(st, bodyA), sb = slotOfHandle(st, bodyB);
    if (sa == kNone || sb == kNone) return 0;
    const auto it = st.exc.find(pairKey(sa, sb));
    return it != st.exc.end() && it->second.user != 0 ? 1 : 0;
}

int32_t aver_phys_body_clear_collision_exceptions(int32_t body) {
    if (!mutableNow() || !findBody(body)) return 0;
    if (!g_world->layerFilter) return 1;
    LayerFilterState& st = *g_world->layerFilter;
    const uint32_t s = slotOfHandle(st, body);
    if (s == kNone) return 1;
    std::vector<int32_t> partners;
    for (auto it = st.exc.begin(); it != st.exc.end();) {
        const uint32_t lo = static_cast<uint32_t>(it->first >> 32), hi = static_cast<uint32_t>(it->first & 0xFFFFFFFFu);
        if ((lo != s && hi != s) || it->second.user == 0) { ++it; continue; }
        const uint32_t other = lo == s ? hi : lo;
        partners.push_back(st.slots[other].handle);
        it->second.user = 0;
        if (it->second.joints != 0) { ++it; continue; }
        --st.slots[s].pins;
        --st.slots[other].pins;
        it = st.exc.erase(it);
    }
    for (const int32_t p : partners) { releaseIfIdle(st, p); wake(p); }
    releaseIfIdle(st, body);
    wake(body);
    return 1;
}

int32_t aver_phys_bodies_can_collide(int32_t bodyA, int32_t bodyB) {
    if (!g_world) return 0;
    const JPH::BodyID *ia = findBody(bodyA), *ib = findBody(bodyB);
    if (!ia || !ib || *ia == *ib) return 0;
    if (!g_world->objPair.ShouldCollide(bi().GetObjectLayer(*ia), bi().GetObjectLayer(*ib))) return 0;
    return bi().GetCollisionGroup(*ia).CanCollide(bi().GetCollisionGroup(*ib)) ? 1 : 0;
}

// ---- queries with a 32-bit mask ----------------------------------------------------------------------

int32_t aver_phys_raycast_mask(float ox, float oy, float oz, float dx, float dy, float dz,
                               float maxDistCm, uint32_t collisionMask, int32_t ignoreBody,
                               float* outPoint, float* outNormal, int32_t* outEntity) {
    if (!g_world) return 0;
    const Vec3 dir(dx, dy, dz);
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len <= 1e-6f || maxDistCm <= 0.0f) return 0;

    const JPH::Vec3 from = toJolt(Vec3(ox, oy, oz));
    const JPH::Vec3 d = toJoltUnit(Vec3(dir.x / len, dir.y / len, dir.z / len)) * cmToM(maxDistCm);
    JPH::RRayCast ray{from, d};
    JPH::RayCastResult hit;

    const LayerMaskBodyFilter filter(collisionMask, ignoredIdOf(ignoreBody));
    if (!g_world->system.GetNarrowPhaseQuery().CastRay(ray, hit, {}, {}, filter)) return 0;

    const int32_t handle = handleForBodyId(hit.mBodyID);
    if (outPoint) writeVec(outPoint, fromJolt(ray.GetPointOnRay(hit.mFraction)));
    if (outEntity || outNormal) {
        JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), hit.mBodyID);
        if (lock.Succeeded()) {
            if (outEntity) *outEntity = static_cast<int32_t>(lock.GetBody().GetUserData());
            if (outNormal) {
                const JPH::Vec3 n = lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, ray.GetPointOnRay(hit.mFraction));
                writeVec(outNormal, fromJoltUnit(n));
            }
        } else {
            if (outEntity) *outEntity = 0;
            if (outNormal) writeVec(outNormal, Vec3(0, 0, 1));
        }
    }
    return handle;
}

int32_t aver_phys_overlap_sphere_mask(float x, float y, float z, float radius,
                                      uint32_t collisionMask, int32_t ignoreBody,
                                      int32_t* outBodies, int32_t maxBodies) {
    if (!g_world || !outBodies || maxBodies <= 0 || radius <= 0.0f) return 0;

    JPH::SphereShape sphere(cmToM(radius));
    sphere.SetEmbedded();   // stack-allocated, lives for this query only

    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    const JPH::RMat44 xf = JPH::RMat44::sTranslation(toJolt(Vec3(x, y, z)));
    const LayerMaskBodyFilter filter(collisionMask, ignoredIdOf(ignoreBody));
    g_world->system.GetNarrowPhaseQuery().CollideShape(
        &sphere, JPH::Vec3::sReplicate(1.0f), xf, JPH::CollideShapeSettings{},
        JPH::RVec3::sZero(), collector, {}, {}, filter);

    int32_t n = 0;
    for (const JPH::CollideShapeResult& r : collector.mHits) {
        if (n >= maxBodies) break;
        const int32_t h = handleForBodyId(r.mBodyID2);
        if (!h) continue;
        bool already = false;
        for (int32_t i = 0; i < n; ++i) if (outBodies[i] == h) { already = true; break; }
        if (!already) outBodies[n++] = h;
    }
    return n;
}

int32_t aver_phys_sphere_cast_mask(float ox, float oy, float oz, float dx, float dy, float dz,
                                   float maxDistCm, float radius,
                                   uint32_t collisionMask, int32_t ignoreBody,
                                   float* outPoint, float* outNormal) {
    if (!g_world || radius <= 0.0f || maxDistCm <= 0.0f) return 0;
    const Vec3 dir(dx, dy, dz);
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len <= 1e-6f) return 0;

    JPH::SphereShape sphere(cmToM(radius));
    sphere.SetEmbedded();
    const JPH::Vec3 from = toJolt(Vec3(ox, oy, oz));
    const JPH::Vec3 d = toJoltUnit(Vec3(dir.x / len, dir.y / len, dir.z / len)) * cmToM(maxDistCm);
    const JPH::RShapeCast cast(&sphere, JPH::Vec3::sReplicate(1.0f), JPH::Mat44::sTranslation(from), d);

    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    const LayerMaskBodyFilter filter(collisionMask, ignoredIdOf(ignoreBody));
    g_world->system.GetNarrowPhaseQuery().CastShape(cast, {}, JPH::RVec3::sZero(), collector, {}, {}, filter);
    if (!collector.HadHit()) return 0;

    if (outPoint) writeVec(outPoint, fromJolt(collector.mHit.mContactPointOn2));
    // Negated: Jolt's penetration axis points into the surface, every normal this ABI returns points away.
    if (outNormal) writeVec(outNormal, fromJoltUnit(-collector.mHit.mPenetrationAxis.Normalized()));
    return handleForBodyId(collector.mHit.mBodyID2);
}

} // extern "C"
