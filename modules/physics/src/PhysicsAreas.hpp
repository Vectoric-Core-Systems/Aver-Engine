// PhysicsAreas.hpp -- per-body contact monitoring and Godot-style Areas (G2). SRC-LOCAL like PhysicsInternal.hpp. The design and the rules
// are in docs/physics/GODOT_AREAS.md; the ABI is physics_areas_abi.h. Every hook is a pointer test when nothing uses the feature.
#pragma once
#include "PhysicsInternal.hpp"

namespace aver::physics::detail {

struct ContactMonitorState;   // PhysicsAreas.cpp
struct AreaState;

// Contact listener hooks. Worker threads, possibly several at once; they only append to a locked list. Solid pairs only (callers skip sensors).
void monitorOnAdded(int32_t handleA, int32_t handleB, const JPH::ContactManifold& manifold);
void monitorOnRemoved(int32_t handleA, int32_t handleB);

// Fixed-loop hooks (PhysicsWorld.cpp). Begin: last call's events go and the ones queued between steps arrive.
void areasBeginCall();
// Before system.Update: area overrides (gravity, damping, wind) on the bodies inside.
void areasStepBegin(float dt);
// After system.Update: contact monitor BEGIN/END, then area enter/exit.
void areasStepEnd();

// aver_phys_remove_body, before the body goes: ends the body's monitored pairs and drops its area.
void areasOnBodyRemoved(int32_t handle);
// A snapshot restore succeeded: monitors are cleared and re-seeded silently.
void areasOnRestore();

} // namespace aver::physics::detail
