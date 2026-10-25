// PhysicsConfig.hpp -- the settings table, the pending/active world config and the job-system choice
// (physics_world_abi.h). SRC-LOCAL like PhysicsInternal.hpp: it names JPH:: types.
#pragma once
#include "PhysicsInternal.hpp"
#include "aver/physics/physics_world_abi.h"

#include <Jolt/Core/JobSystem.h>
#include <Jolt/Physics/PhysicsSettings.h>

#include <memory>

namespace aver::physics::detail {

// One live solver setting: the name the ABI, the console and the project key are all built from, its unit
// string, range and default. Lengths and speeds are in the engine's centimetres.
struct SettingRow {
    const char* name;
    const char* unit;
    float min, max, def;
};

// JPH::PhysicsSettings built from the table. A setting still at its default keeps Jolt's own default bit for
// bit, so an untouched world steps exactly as it did before the table existed.
JPH::PhysicsSettings makePhysicsSettings();

// Pushes the table into the live world (no-op with none). Called by aver_phys_init and by every set.
void applySettingsToWorld();

// The collision-step count the fixed loop passes to Update.
int collisionStepsSetting();

// The init-time ceilings and thread count, in the units system.Init takes.
struct WorldConfig {
    JPH::uint maxBodies = 65536;
    JPH::uint maxBodyPairs = 65536;
    JPH::uint maxContacts = 16384;
    size_t    tempBytes = 10u * 1024u * 1024u;
    int       threads = -1;    // as the ABI: -1 auto, 0 single-threaded, N workers
};
WorldConfig pendingWorldConfig();

// Records what the live world was built with (World::activeConfig, in the ABI's float form) and the resolved
// worker count. Called by aver_phys_init once the world exists; it dies with the world.
void noteActiveConfig(const WorldConfig& cfg, int workers);

// The job system for `threads` (see WorldConfig); `workersOut` receives the resolved worker count (0 =
// single-threaded). THE ONE PLACE a future engine scheduler would replace.
std::unique_ptr<JPH::JobSystem> makeJobSystem(int threads, int& workersOut);

// Why the world is not empty, or nullptr when it is (no bodies, characters, joints, ragdolls, vehicles).
const char* worldNotEmptyReason();

// The layout version of a state blob. aver_phys_build_string reports it and the snapshot header carries it, so the two cannot disagree.
inline constexpr int kStateLayoutVersion = 2;   // 2: the vehicle section carries the last step's telemetry arrays

} // namespace aver::physics::detail
