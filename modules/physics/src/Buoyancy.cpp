// Buoyancy.cpp -- the whole of the physics is one call to Jolt's own ApplyBuoyancyImpulse; the rest
// of this file is bookkeeping. See Buoyancy.hpp for the reasoning behind the shape of this class.
#include "Buoyancy.hpp"
#include "Convert.hpp"
#include "PhysicsOrigin.hpp"   // LARGE_WORLD: physicsOriginHook

#include <Jolt/Physics/Body/BodyType.h>
#include <Jolt/Physics/Body/MotionType.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <algorithm>
#include <cmath>

namespace aver::phys::water {

namespace {

// SOFT BODIES ARE NOT BUOYANT, and asking Jolt to make one so is not a soft failure -- it is
// `JPH_ASSERT(IsRigidBody())` at the top of Body::ApplyBuoyancyImpulse, which is a hard stop in a
// debug build. Jolt says why in the line right beside it: buoyancy is "Only implemented for rigid
// bodies currently". Nothing in this engine created a soft body inside a water plane until fluid
// volumes arrived, and a fluid volume is the worst possible case -- it sits BELOW its own surface
// by construction, so it is fully submerged from its first step and hits the assert immediately.
//
// Skipped rather than clamped, because there is no sensible impulse to apply: a soft body's shape
// is its particles, GetSubmergedVolume is a rigid-shape query, and a volume of water floating on
// itself is not a thing the plane is describing in the first place.
bool buoyant(const JPH::BodyInterface& bi, const JPH::BodyID& id) {
    return bi.GetBodyType(id) == JPH::EBodyType::RigidBody;
}

// ---- wave3 water sampler ----
// One rigid dynamic body against the host's sampler. 1 = `out` is the water at the body, 0 = no water
// here (no buoyancy), -1 = no opinion. The centre of mass is sampled; a body wider than 300 cm also
// samples the four corners of its bounds and averages what answers, so a long hull or a raft takes the
// slope of the surface under it rather than the height under its middle.
int sampleBody(JPH::BodyInterface& bi, const JPH::BodyID& id, aver_phys_water_sampler_fn fn, void* user,
               aver::Vec3& surfacePoint, aver::Vec3& normal, aver::Vec3& current, float& buoyancy,
               float& linDrag, float& angDrag) {
    using namespace aver;
    const Vec3 c = physics::fromJolt(JPH::Vec3(bi.GetCenterOfMassPosition(id)));
    AverPhysWaterSample s{};
    const int32_t r = fn(c.x, c.y, c.z, &s, user);
    if (r != 1) return r == 0 ? 0 : -1;
    f32 z = s.surfaceZCm;
    Vec3 n{s.normal[0], s.normal[1], s.normal[2]};
    Vec3 cur{s.currentCmS[0], s.currentCmS[1], s.currentCmS[2]};
    buoyancy = s.buoyancy;
    linDrag = s.linearDrag;
    angDrag = s.angularDrag;

    const JPH::AABox box = bi.GetTransformedShape(id).GetWorldSpaceBounds();
    const JPH::Vec3 ext = box.GetExtent();
    // Jolt (x right, z -forward) to engine (x forward, y right): engine X extent is Jolt Z, engine Y is Jolt X.
    const f32 hx = ext.GetZ() * physics::kCmPerMetre, hy = ext.GetX() * physics::kCmPerMetre;
    if (std::max(hx, hy) * 2.0f > 300.0f) {
        int count = 1;
        const f32 off[4][2] = {{hx, hy}, {hx, -hy}, {-hx, hy}, {-hx, -hy}};
        for (const auto& o : off) {
            AverPhysWaterSample cs{};
            if (fn(c.x + o[0], c.y + o[1], c.z, &cs, user) != 1) continue;
            z += cs.surfaceZCm;
            n.x += cs.normal[0]; n.y += cs.normal[1]; n.z += cs.normal[2];
            cur.x += cs.currentCmS[0]; cur.y += cs.currentCmS[1]; cur.z += cs.currentCmS[2];
            ++count;
        }
        const f32 inv = 1.0f / static_cast<f32>(count);
        z *= inv;
        cur.x *= inv; cur.y *= inv; cur.z *= inv;
        const f32 len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        if (len > 1e-6f) { n.x /= len; n.y /= len; n.z /= len; } else { n = Vec3{0.0f, 0.0f, 1.0f}; }
    }
    surfacePoint = Vec3{c.x, c.y, z};
    normal = n;
    current = cur;
    return 1;
}
// ---- end wave3 water sampler ----

} // namespace

WaterSamplerSlot& waterSampler() {
    static WaterSamplerSlot slot;
    return slot;
}

SurfaceSamplerSlot& surfaceSampler() {
    static SurfaceSamplerSlot slot;
    return slot;
}

void WaterVolumeTable::set(int32_t bodyHandle, const WaterVolume& volume) {
    volumes_[bodyHandle] = volume;
}

void WaterVolumeTable::clear(int32_t bodyHandle) {
    volumes_.erase(bodyHandle);
}

void WaterVolumeTable::clearAll() {
    volumes_.clear();
}

size_t WaterVolumeTable::count() const {
    return volumes_.size();
}

void WaterVolumeTable::setPlane(const WaterVolume& plane) {
    plane_ = plane;
    hasPlane_ = true;
}

void WaterVolumeTable::clearPlane() {
    hasPlane_ = false;
}

bool WaterVolumeTable::hasPlane() const {
    return hasPlane_;
}

const WaterVolume& WaterVolumeTable::plane() const {
    return plane_;
}

void WaterVolumeTable::shiftSurfaces(JPH::Vec3Arg dj) {
    for (auto& kv : volumes_) kv.second.surfacePosition -= dj;
    plane_.surfacePosition -= dj;
}
int32_t WaterVolumeTable::lastAppliedCount() const {
    return lastApplied_;
}

int32_t WaterVolumeTable::evaluate(
        JPH::PhysicsSystem& system,
        const std::function<const JPH::BodyID*(int32_t)>& findBody,
        const std::function<void(const std::function<void(int32_t, const JPH::BodyID&)>&)>& forEachBody,
        JPH::Vec3Arg gravity, float dt) const {
    JPH::BodyInterface& bi = system.GetBodyInterface();
    int32_t applied = 0;

    // Per-body overrides first, each against its own registered plane. The return value is
    // intentionally ignored beyond counting it: false just means "no impulse this substep" (the body
    // is above the surface, or static, or asleep with nothing to wake it) -- not an error, per the
    // Jolt source this module was scouted against.
    for (const auto& [handle, v] : volumes_) {
        const JPH::BodyID* id = findBody(handle);
        if (!id) continue;   // a stale or since-removed handle drops out silently, by design
        // Checked on the override path too, not just the plane below: registering a water volume
        // against a soft body's handle is a thing an ABI caller can do, and it would assert in
        // exactly the same place.
        if (!buoyant(bi, *id)) continue;
        if (bi.ApplyBuoyancyImpulse(*id, v.surfacePosition, v.surfaceNormal, v.buoyancy,
                                     v.linearDrag, v.angularDrag, v.fluidVelocity, gravity, dt))
            ++applied;
    }

    // Then the global plane, for every body forEachBody reports EXCEPT one already handled above --
    // "a per-body volume overrides the plane for that body" is enforced right here, by checking the
    // ENGINE HANDLE against volumes_ rather than the BodyID, which is exactly why forEachBody hands
    // back the handle: a plain map lookup, where checking by BodyID would need a second, reverse
    // table this file has no other reason to keep. Buoyancy <= 0 is the documented way to disable the
    // plane without forgetting its height/normal/drag (see aver_phys_set_water_plane), checked once
    // here instead of by every caller of setPlane().
    //
    // wave3: a host sampler (aver_phys_set_water_sampler) answers per body first -- the surface, its
    // slope and the current at the body -- and the plane is the fallback for a body it has no opinion on.
    aver_phys_water_sampler_fn sampler = waterSampler().fn.load();
    void* samplerUser = waterSampler().user.load();
    const bool planeOn = hasPlane_ && plane_.buoyancy > 0.0f;
    if ((planeOn || sampler) && forEachBody) {
        forEachBody([&](int32_t handle, const JPH::BodyID& id) {
            if (volumes_.count(handle) != 0) return;   // this body already has its own water
            if (!buoyant(bi, id)) return;
            if (sampler) {
                if (bi.GetMotionType(id) != JPH::EMotionType::Dynamic) return;
                aver::Vec3 point, normal, current;
                float buoy = 1.0f, lin = 0.5f, ang = 0.01f;
                const int r = sampleBody(bi, id, sampler, samplerUser, point, normal, current, buoy, lin, ang);
                if (r == 0) return;
                if (r == 1) {
                    if (buoy <= 0.0f) return;
                    if (bi.ApplyBuoyancyImpulse(id, aver::physics::toJolt(point), aver::physics::toJoltUnit(normal), buoy,
                                                 lin, ang, aver::physics::toJoltDir(current), gravity, dt))
                        ++applied;
                    return;
                }
                // -1: no opinion, fall through to the plane
            }
            if (!planeOn) return;
            if (bi.ApplyBuoyancyImpulse(id, plane_.surfacePosition, plane_.surfaceNormal,
                                         plane_.buoyancy, plane_.linearDrag, plane_.angularDrag,
                                         plane_.fluidVelocity, gravity, dt))
                ++applied;
        });
    }

    lastApplied_ = applied;
    return applied;
}

namespace {
// LARGE_WORLD: registered with the physics origin shift at load (PhysicsOrigin.hpp). The hook gets the origin's delta in engine cm; Jolt's is toJolt(delta).
void shiftWaterSurfaces(const aver::physics::detail::Vec3d& d) {
    waterVolumes().shiftSurfaces(JPH::Vec3(static_cast<float>(d.y / 100.0), 0.0f, static_cast<float>(-d.x / 100.0)));
}
const struct OriginHookRegistrar {
    OriginHookRegistrar() { aver::physics::detail::physicsOriginHook("water.surfaces", &shiftWaterSurfaces); }
} g_originHookRegistrar;
} // namespace
WaterVolumeTable& waterVolumes() {
    static WaterVolumeTable table;
    return table;
}

} // namespace aver::phys::water
