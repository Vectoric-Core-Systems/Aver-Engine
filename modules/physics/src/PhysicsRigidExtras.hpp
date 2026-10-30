// PhysicsRigidExtras.hpp -- rigid-body extras: integrate-forces callbacks, the custom integrator, constant velocity on static bodies and the
// bookkeeping for disabled shapes. SRC-LOCAL like PhysicsInternal.hpp. The rules are in docs/PHYSICS_SHAPES.md (section "Rigid-body
// extras") and physics_body_ext_abi.h. Every hook is a pointer test when nothing uses the feature.
#pragma once
#include "PhysicsInternal.hpp"

#include "aver/physics/physics_body_ext_abi.h"

#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <map>
#include <unordered_map>

namespace aver::physics::detail {

struct IntegrateEntry { aver_phys_integrate_forces_fn fn = nullptr; int64_t user = 0; };
struct CustomSaved { float gravityFactor = 1.0f, linDamping = 0.0f, angDamping = 0.0f; };
struct Conveyor { JPH::Float3 lin{0, 0, 0}, ang{0, 0, 0}; };   // Jolt axes: m/s and rad/s, world space

struct RigidExtrasState {
    std::map<int32_t, IntegrateEntry> integrate;                                   // by body handle: walked in handle order
    std::map<int32_t, CustomSaved> custom;                                         // what a custom-integrator body had before
    std::unordered_map<JPH::BodyID, Conveyor> conveyors;                           // read by workers, written between steps
    std::map<int32_t, std::map<uint32_t, JPH::RefConst<JPH::Shape>>> disabledChildren;   // body -> mutable-compound child index -> the shape that was there
    std::map<int32_t, JPH::RefConst<JPH::Shape>> disabledWhole;                    // body -> the whole shape it had before it was disabled
};

RigidExtrasState& rigidExtrasOrCreate();

// Contact listener hook (workers): a conveyor body gives its partner the surface velocity. Solid pairs only (callers skip sensors).
void conveyorOnContact(const JPH::Body& a, const JPH::Body& b, JPH::ContactSettings& io);

// Fixed-loop hook (PhysicsWorld.cpp), after the area overrides and before system.Update: keeps custom integrators off gravity and damping,
// then calls each registered _integrate_forces in handle order.
void rigidExtrasStepBegin(float dt);

// aver_phys_remove_body, before the body goes.
void rigidExtrasOnBodyRemoved(int32_t handle);

} // namespace aver::physics::detail
