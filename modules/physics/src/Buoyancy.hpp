// Buoyancy: a table of per-body water overrides plus one global water plane, both expressed as
// Jolt's own ApplyBuoyancyImpulse would want them, and both evaluated by calling straight into it.
// Implementation is in Buoyancy.cpp; this file is the declarations plus the reasoning behind them.
//
// INTERNAL to Aver.Physics on purpose: this file names JPH:: types directly, and the module's own
// CMakeLists.txt says outright that "nothing above should be able to include a JPH:: header by
// accident" -- so, like Convert.hpp and PhysicsWorld.cpp beside it, it lives in src/, not in
// include/aver/physics/, and is compiled only as part of the Aver.Physics target itself.
#pragma once

#include "aver/core/Types.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <cstdint>
#include <functional>
#include <unordered_map>

namespace aver::phys::water {

// One body of water: an infinite plane (a point on it plus its normal) and the numbers Jolt's own
// ApplyBuoyancyImpulse takes to turn "this body's shape crosses that plane" into an impulse. Used
// both for a per-body override and for the one global plane -- the two are the same shape of thing,
// a plane with buoyancy/drag/current attached to it, so one struct covers both.
//
// Fields are already JOLT-SPACE (metres, Jolt's -Z-forward/+X-right/+Y-up axes). The ABI boundary
// file (BuoyancyAbi.cpp) is the one place that converts from the engine's centimetres and +Z-up axes
// before ever constructing one of these -- exactly the division of labour Convert.hpp already draws
// for every other Jolt-facing type in this module.
struct WaterVolume {
    JPH::RVec3 surfacePosition;                       // a point on the water plane
    JPH::Vec3  surfaceNormal;                          // the plane's normal, unit length
    f32 buoyancy      = 1.f;                           // 1 = neutral, <1 sinks, >1 floats
    f32 linearDrag    = 0.5f;                          // Jolt's own suggested default
    f32 angularDrag   = 0.01f;                         // Jolt's own suggested default
    JPH::Vec3 fluidVelocity = JPH::Vec3::sZero();      // the current, m/s
};

// A registered table of per-body water overrides, plus one optional global plane, evaluated once per
// FIXED SUBSTEP against Jolt's own Body::ApplyBuoyancyImpulse.
//
// WHY PER-SUBSTEP, REGISTERED STATE, RATHER THAN A ONE-SHOT ABI CALL MIRRORING JOLT'S OWN
// ApplyBuoyancyImpulse SIGNATURE VERBATIM: aver_phys_step's accumulator can run 0 to 8 Jolt substeps
// per call (PhysicsWorld.cpp:317-362, the `while (accumulator >= fixedStep)` loop capped at
// fixedStep*8), and ApplyBuoyancyImpulse computes an INSTANTANEOUS impulse sized by whatever dt it is
// given -- so a once-per-gameplay-frame call would be wrong whenever the substep count for that call
// is not exactly 1: too few impulses when several substeps ran, one impulse sized for the wrong dt
// when zero substeps ran (a stall) and the call still fired anyway. The CharacterVirtual integration
// block already inside that same accumulator loop (PhysicsWorld.cpp:333-357, which reads gravity and
// re-integrates a character's vertical velocity every substep rather than once per aver_phys_step
// call) is the existing precedent this mirrors: anything that needs Jolt's per-substep dt has to live
// inside the loop that owns that dt, not beside it.
//
// WHY findBody AND forEachBody ARE INJECTED AS std::function PARAMETERS RATHER THAN THIS FILE
// REACHING INTO g_world DIRECTLY: g_world's World struct is deliberately file-local to
// PhysicsWorld.cpp (an anonymous-namespace type, not declared in any header), so that translation
// unit's internals stay free to change without this file caring. Injecting the two lookups this file
// actually needs -- "the BodyID behind this handle" and "every (handle, BodyID) pair currently in the
// world" -- keeps Buoyancy.cpp compilable and independently testable with a fake world of a few
// entries, and keeps PhysicsWorld.cpp's own struct un-exposed to a second translation unit for a
// feature that does not need to see the rest of it (fixedStep, the event queues, the character table,
// none of which buoyancy touches).
//
// WHY evaluate() IS DELIBERATELY UNGATED AGAINST STATIC, KINEMATIC, OR SOFT-BODY HANDLES: it does not
// check IsDynamic() and does not check "is this a rigid body" before calling ApplyBuoyancyImpulse, and
// that is not an oversight. Jolt's own BodyInterface::ApplyBuoyancyImpulse already requires
// body.IsDynamic() and returns false with no side effect otherwise (BodyInterface.cpp:812-827) -- a
// static or kinematic handle silently gets no impulse, which is exactly the behaviour
// tests/physics/src/BuoyancyTest.cpp's static-body case exists to prove rather than assume. And for a
// soft body: Body::ApplyBuoyancyImpulse's volume overload opens with
// `JPH_ASSERT(IsRigidBody());  // Only implemented for rigid bodies currently` (Body.cpp:198), which
// reads like a landmine for a soft-body handle -- except SoftBodyShape::GetSubmergedVolume is an
// unconditional stub that always reports zero submerged volume regardless of the surface plane it is
// given (SoftBodyShape.cpp:162-167: outSubmergedVolume = 0.0f, ignoring inSurfacePosition entirely),
// and the volume overload's very first line is `if (inSubmergedVolume > 0.0f)` (Body.cpp:200) -- so a
// soft body always takes the early return and the assert on the next line is never reached. Adding a
// redundant IsRigidBody() or IsDynamic() check here would just be duplicating a guard Jolt already
// enforces, on the mistaken belief this file is the one holding the safety line.
class WaterVolumeTable {
public:
    void set(int32_t bodyHandle, const WaterVolume& volume);
    void clear(int32_t bodyHandle);
    void clearAll();
    // Registered per-body overrides. NOT "bodies buoyancy was applied to" -- see lastAppliedCount()
    // below for that; this is the simple "how many volumes exist" a caller of set()/clear() expects.
    size_t count() const;

    // The global water plane. Buoyancy <= 0 is how aver_phys_set_water_plane documents disabling the
    // plane without forgetting its height/normal/drag -- evaluate() honours that convention itself
    // (see Buoyancy.cpp) so every caller does not have to remember it independently.
    void setPlane(const WaterVolume& plane);
    void clearPlane();
    bool hasPlane() const;
    const WaterVolume& plane() const;   // only meaningful while hasPlane()

    // How many bodies actually got an impulse (ApplyBuoyancyImpulse returned true) the last time
    // evaluate() ran -- aver_phys_buoyant_body_count's whole reason to exist, per its own doc comment:
    // "nothing floats" and "nothing is in the water" look identical from outside without this.
    int32_t lastAppliedCount() const;

    // Applies buoyancy for one fixed substep and returns how many bodies got an impulse. See
    // Buoyancy.cpp for what it actually does and why.
    int32_t evaluate(JPH::PhysicsSystem& system,
                      const std::function<const JPH::BodyID*(int32_t)>& findBody,
                      const std::function<void(const std::function<void(int32_t, const JPH::BodyID&)>&)>& forEachBody,
                      JPH::Vec3Arg gravity, float dt) const;

private:
    std::unordered_map<int32_t, WaterVolume> volumes_;
    WaterVolume plane_{};
    bool hasPlane_ = false;
    mutable int32_t lastApplied_ = 0;
};

// Meyer's singleton: the one shared table both PhysicsWorld.cpp's step loop (evaluate(), once per
// fixed substep) and BuoyancyAbi.cpp's ABI functions (set/clear/query) reach through. Constructed on
// first use, destroyed at process exit, same lifetime rule as every other file-local static in this
// module.
WaterVolumeTable& waterVolumes();

} // namespace aver::phys::water
