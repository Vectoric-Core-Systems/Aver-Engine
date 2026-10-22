// SoftBodyAero.hpp -- wind and drag on soft bodies, and the hooks PhysicsWorld.cpp calls.
//
// SRC-LOCAL, like PhysicsInternal.hpp: it names JPH:: types. The wind field and the per-triangle drag/lift
// are here (SoftBodyAero.cpp); the registry, the builder ABI and the contact listener are in
// PhysicsSoftBody.cpp. Everything is a pure function of (position, fixed step index, dt): no clock, no rand.
#pragma once
#include "aver/core/Math.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>

#include <cstdint>

namespace aver::physics::softbody {

struct WindParams {
    Vec3 base{0, 0, 0};          // cm/s
    float gustSpeed = 0.0f;      // cm/s, along the base direction (or +X when there is no base)
    float gustHz = 0.0f;
    float turbulence = 0.0f;     // cm/s
    bool any() const { return base.sizeSquared() > 0.0f || gustSpeed != 0.0f || turbulence != 0.0f; }
};

// The wind at a world position (cm) on fixed step `step`, cm/s.
Vec3 windAt(const WindParams& w, const Vec3& worldCm, uint64_t step, float dt);

// Adds the drag/lift velocity change of one fixed step to the vertices of `mp`. `com` is the body's
// centre-of-mass transform (identity rotation). Kinematic vertices (inverse mass 0) are skipped.
void applyAero(JPH::SoftBodyMotionProperties& mp, const JPH::RMat44& com, float drag, float lift,
               const WindParams& wind, uint64_t step, float dt);

// ---- PhysicsWorld.cpp hooks ------------------------------------------------------------------------

void installListener();                 // after SetContactListener at init
void removeListener();                  // at shutdown
void beginCall();                       // top of aver_phys_step: clears last call's impact events
void onStepBegin(float dt);             // inside the fixed loop, before system.Update
void onStepEnd(float dt);               // inside the fixed loop, after system.Update (sensor enter / exit events)
void onBodyRemoved(int32_t handle);     // aver_phys_remove_body
void destroyAll();                      // shutdown, before the body loop

} // namespace aver::physics::softbody

// Strand hair (PhysicsHair.cpp) is torn down by the same shutdown path: aver_phys_shutdown calls this before the world goes, so no Jolt Hair outlives the PhysicsSystem it collides against.
namespace aver::physics::detail { void hairDestroyAll(); }
