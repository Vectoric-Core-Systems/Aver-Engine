// PhysicsBodyGodot.cpp -- Godot rigid-body extras on Jolt (G2): the _integrate_forces direct-state callback, the custom integrator and constant
// velocity on static bodies (physics_body_ext_abi.h). Rationale and limits: docs/PHYSICS_SHAPES.md, section "Godot rigid-body extras".
//
// _integrate_forces is a fixed-loop hook, not a Jolt feature: before each solver step the registered awake bodies are read into a plain struct, the
// callback runs on the stepping thread, and velocities it changed are written back. The custom integrator holds the body's gravity factor and dampings
// at zero (Jolt applies both inside its own integration, so there is nothing to switch off but the numbers). A conveyor is Jolt's contact surface
// velocity, set from the contact listener for pairs that include the belt body.
#include "PhysicsBodyGodot.hpp"

#include "aver/physics/physics_areas_abi.h"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseQuery.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <cmath>
#include <cstring>
#include <vector>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

int32_t refuse(AbiError e) {
    setAbiError(e);
    return 0;
}

const JPH::BodyID* idOf(int32_t handle) {
    const auto it = g_world->bodies.find(handle);
    return it == g_world->bodies.end() ? nullptr : &it->second;
}

bool finite3(const float* v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

JPH::Vec3 vec(const JPH::Float3& f) { return JPH::Vec3(f.x, f.y, f.z); }
JPH::Float3 flt(JPH::Vec3Arg v) { return JPH::Float3(v.GetX(), v.GetY(), v.GetZ()); }

// Reads the body into the direct state. False for a body that is gone, asleep, or has no motion properties.
bool readState(int32_t handle, float dt, const CustomSaved* custom, aver_phys_direct_state& s) {
    const JPH::BodyID* id = idOf(handle);
    if (!id) return false;
    {
        JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), *id);
        if (!lock.Succeeded()) return false;
        const JPH::Body& b = lock.GetBody();
        const JPH::MotionProperties* mp = b.GetMotionPropertiesUnchecked();
        if (!mp || b.IsStatic() || !b.IsActive()) return false;
        std::memset(&s, 0, sizeof s);
        s.body = handle;
        s.dt = dt;
        s.invMass = b.IsDynamic() ? mp->GetInverseMassUnchecked() : 0.0f;
        s.gravityFactor = custom ? custom->gravityFactor : mp->GetGravityFactor();
        const Vec3 p = fromJolt(JPH::Vec3(b.GetCenterOfMassPosition()));
        s.position[0] = p.x; s.position[1] = p.y; s.position[2] = p.z;
        const Quat q = fromJolt(b.GetRotation());
        s.rotation[0] = q.x; s.rotation[1] = q.y; s.rotation[2] = q.z; s.rotation[3] = q.w;
        const Vec3 lv = fromJoltDir(mp->GetLinearVelocity());
        s.linearVelocity[0] = lv.x; s.linearVelocity[1] = lv.y; s.linearVelocity[2] = lv.z;
        const Vec3 av = fromJoltAngular(mp->GetAngularVelocity());
        s.angularVelocity[0] = av.x; s.angularVelocity[1] = av.y; s.angularVelocity[2] = av.z;
        if (b.IsDynamic()) {
            // M_engine = B^T M_jolt B for the axis map jolt = B * engine (engine x is Jolt -z, y is x, z is y); units 1/(kg m^2) to 1/(kg cm^2).
            const JPH::Mat44 m = b.GetInverseInertia();
            const int ja[3] = {2, 0, 1};
            const float sg[3] = {-1.0f, 1.0f, 1.0f};
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    s.invInertia[r * 3 + c] = sg[r] * sg[c] * m(ja[r], ja[c]) / (kCmPerMetre * kCmPerMetre);
        }
    }
    s.contactCount = aver_phys_body_contact_count(handle);
    aver_phys_body_area_gravity(handle, s.totalGravity);
    return true;
}

// Writes back the velocities the callback changed.
void applyVelocities(int32_t handle, const aver_phys_direct_state& after, const aver_phys_direct_state& before) {
    const bool lin = std::memcmp(after.linearVelocity, before.linearVelocity, sizeof after.linearVelocity) != 0;
    const bool ang = std::memcmp(after.angularVelocity, before.angularVelocity, sizeof after.angularVelocity) != 0;
    if (!lin && !ang) return;
    if ((lin && !finite3(after.linearVelocity)) || (ang && !finite3(after.angularVelocity))) {
        AVER_WARN("[Physics] body {}: _integrate_forces wrote a non-finite velocity; ignored", handle);
        return;
    }
    const JPH::BodyID* id = idOf(handle);
    if (!id) return;
    JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded()) return;
    JPH::MotionProperties* mp = lock.GetBody().GetMotionPropertiesUnchecked();
    if (!mp) return;
    if (lin) mp->SetLinearVelocityClamped(toJoltDir(Vec3(after.linearVelocity[0], after.linearVelocity[1], after.linearVelocity[2])));
    if (ang) mp->SetAngularVelocityClamped(toJoltAngular(Vec3(after.angularVelocity[0], after.angularVelocity[1], after.angularVelocity[2])));
}

// Wakes the sleeping bodies around `id` (a conveyor's value changed under them).
class WakeCollector final : public JPH::CollideShapeBodyCollector {
public:
    void AddHit(const JPH::BodyID& id) override { hits.push_back(id); }
    std::vector<JPH::BodyID> hits;
};

void wakeAround(const JPH::BodyID& id) {
    JPH::AABox box = bi().GetTransformedShape(id).GetWorldSpaceBounds();
    box.ExpandBy(JPH::Vec3::sReplicate(0.05f));
    WakeCollector c;
    g_world->system.GetBroadPhaseQuery().CollideAABox(box, c);
    for (const JPH::BodyID& h : c.hits)
        if (h != id && bi().GetMotionType(h) != JPH::EMotionType::Static) bi().ActivateBody(h);
}

} // namespace

namespace aver::physics::detail {

BodyGodotState& bodyGodotOrCreate() {
    if (!g_world->bodyGodot) g_world->bodyGodot = std::make_shared<BodyGodotState>();
    return *g_world->bodyGodot;
}

void conveyorOnContact(const JPH::Body& a, const JPH::Body& b, JPH::ContactSettings& io) {
    const BodyGodotState* st = g_world->bodyGodot.get();
    if (!st || st->conveyors.empty()) return;
    const auto ca = st->conveyors.find(a.GetID()), cb = st->conveyors.find(b.GetID());
    if (ca == st->conveyors.end() && cb == st->conveyors.end()) return;

    // The surface velocity of the belt AT ITS CENTRE OF MASS: the linear part plus the spin about the body's origin carried over to the COM.
    JPH::Vec3 lin1 = JPH::Vec3::sZero(), ang1 = JPH::Vec3::sZero(), lin2 = JPH::Vec3::sZero(), ang2 = JPH::Vec3::sZero();
    const JPH::Vec3 c1 = JPH::Vec3(a.GetCenterOfMassPosition()), c2 = JPH::Vec3(b.GetCenterOfMassPosition());
    if (ca != st->conveyors.end()) {
        ang1 = vec(ca->second.ang);
        lin1 = vec(ca->second.lin) + ang1.Cross(c1 - JPH::Vec3(a.GetPosition()));
    }
    if (cb != st->conveyors.end()) {
        ang2 = vec(cb->second.ang);
        lin2 = vec(cb->second.lin) + ang2.Cross(c2 - JPH::Vec3(b.GetPosition()));
    }
    // Jolt: relative = body 2 minus body 1, the angular part about body 1's centre of mass, so body 2's spin is moved to it.
    io.mRelativeLinearSurfaceVelocity = (lin2 + ang2.Cross(c1 - c2)) - lin1;
    io.mRelativeAngularSurfaceVelocity = ang2 - ang1;
}

void bodyGodotStepBegin(float dt) {
    const std::shared_ptr<BodyGodotState> keep = g_world->bodyGodot;
    if (!keep || (keep->integrate.empty() && keep->custom.empty())) return;
    const uint64_t worldId = g_world->id;

    for (const auto& [h, saved] : keep->custom) {
        const JPH::BodyID* id = idOf(h);
        if (!id) continue;
        JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), *id);
        if (!lock.Succeeded()) continue;
        if (JPH::MotionProperties* mp = lock.GetBody().GetMotionPropertiesUnchecked()) {
            mp->SetGravityFactor(0.0f);
            mp->SetLinearDamping(0.0f);
            mp->SetAngularDamping(0.0f);
        }
    }

    // A copy of the keys: the callback may register, clear or remove bodies.
    std::vector<int32_t> handles;
    handles.reserve(keep->integrate.size());
    for (const auto& [h, e] : keep->integrate) handles.push_back(h);
    for (const int32_t h : handles) {
        if (!g_world || g_world->id != worldId || g_world->bodyGodot != keep) return;   // the callback shut physics down
        const auto it = keep->integrate.find(h);
        if (it == keep->integrate.end()) continue;
        const IntegrateEntry entry = it->second;
        const auto ci = keep->custom.find(h);
        aver_phys_direct_state s;
        if (!readState(h, dt, ci == keep->custom.end() ? nullptr : &ci->second, s)) continue;
        const aver_phys_direct_state before = s;
        entry.fn(&s, entry.user);
        if (!g_world || g_world->id != worldId) return;
        applyVelocities(h, s, before);
    }
}

void bodyGodotOnBodyRemoved(int32_t handle) {
    BodyGodotState* st = g_world->bodyGodot.get();
    if (!st) return;
    st->integrate.erase(handle);
    st->custom.erase(handle);
    st->disabledChildren.erase(handle);
    st->disabledWhole.erase(handle);
    if (const JPH::BodyID* id = idOf(handle)) st->conveyors.erase(*id);
}

} // namespace aver::physics::detail

// ---- the ABI ---------------------------------------------------------------------------------------

extern "C" {

int32_t aver_phys_body_set_integrate_forces(int32_t body, aver_phys_integrate_forces_fn fn, int64_t user) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    if (!fn) {
        if (g_world->bodyGodot) g_world->bodyGodot->integrate.erase(body);
        return 1;
    }
    if (bi().GetMotionType(*id) == JPH::EMotionType::Static) return refuse(AbiError::Unsupported);
    bodyGodotOrCreate().integrate[body] = IntegrateEntry{fn, user};
    return 1;
}

int32_t aver_phys_body_set_custom_integrator(int32_t body, int32_t on) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded()) return refuse(AbiError::BadHandle);
    JPH::MotionProperties* mp = lock.GetBody().GetMotionPropertiesUnchecked();
    if (!mp) return refuse(AbiError::Unsupported);
    BodyGodotState& st = bodyGodotOrCreate();
    const auto it = st.custom.find(body);
    if (on && it == st.custom.end()) {
        st.custom[body] = CustomSaved{mp->GetGravityFactor(), mp->GetLinearDamping(), mp->GetAngularDamping()};
        mp->SetGravityFactor(0.0f);
        mp->SetLinearDamping(0.0f);
        mp->SetAngularDamping(0.0f);
    } else if (!on && it != st.custom.end()) {
        mp->SetGravityFactor(it->second.gravityFactor);
        mp->SetLinearDamping(it->second.linDamping);
        mp->SetAngularDamping(it->second.angDamping);
        st.custom.erase(it);
    }
    return 1;
}

int32_t aver_phys_body_custom_integrator(int32_t body, int32_t* outOn) {
    if (!outOn) return refuse(AbiError::NullPointer);
    if (!findBody(body)) return 0;
    *outOn = g_world->bodyGodot && g_world->bodyGodot->custom.count(body) ? 1 : 0;
    return 1;
}

int32_t aver_phys_body_set_constant_velocity(int32_t body, const float* linearCmS, const float* angularRadS) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    if ((linearCmS && !finite3(linearCmS)) || (angularRadS && !finite3(angularRadS))) return refuse(AbiError::InvalidArgument);
    if (bi().GetMotionType(*id) == JPH::EMotionType::Dynamic) {
        AVER_WARN("[Physics] body {}: constant velocity is for static and kinematic bodies", body);
        return refuse(AbiError::Unsupported);
    }
    Conveyor c;
    if (linearCmS) c.lin = flt(toJoltDir(Vec3(linearCmS[0], linearCmS[1], linearCmS[2])));
    if (angularRadS) c.ang = flt(toJoltAngular(Vec3(angularRadS[0], angularRadS[1], angularRadS[2])));
    const bool zero = c.lin.x == 0 && c.lin.y == 0 && c.lin.z == 0 && c.ang.x == 0 && c.ang.y == 0 && c.ang.z == 0;
    BodyGodotState& st = bodyGodotOrCreate();
    const auto it = st.conveyors.find(*id);
    const bool had = it != st.conveyors.end();
    const bool changed = had ? std::memcmp(&it->second, &c, sizeof c) != 0 : !zero;
    if (zero) st.conveyors.erase(*id);
    else st.conveyors[*id] = c;
    if (changed) wakeAround(*id);
    return 1;
}

int32_t aver_phys_body_constant_velocity(int32_t body, float* outLinearCmS, float* outAngularRadS) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    Vec3 lin(0, 0, 0), ang(0, 0, 0);
    if (g_world->bodyGodot) {
        const auto it = g_world->bodyGodot->conveyors.find(*id);
        if (it != g_world->bodyGodot->conveyors.end()) {
            lin = fromJoltDir(vec(it->second.lin));
            ang = fromJoltAngular(vec(it->second.ang));
        }
    }
    if (outLinearCmS) { outLinearCmS[0] = lin.x; outLinearCmS[1] = lin.y; outLinearCmS[2] = lin.z; }
    if (outAngularRadS) { outAngularRadS[0] = ang.x; outAngularRadS[1] = ang.y; outAngularRadS[2] = ang.z; }
    return 1;
}

} // extern "C"
