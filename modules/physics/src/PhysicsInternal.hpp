// PhysicsInternal.hpp: the live world and the lookups every ABI file needs. SRC-LOCAL, never include/: it names JPH:: types,
// and Aver.Physics.Jolt links PRIVATE so no Jolt header reaches anything above this module. Same rule as Convert.hpp and Buoyancy.hpp.
// BuoyancyAbi.cpp keeps its own public-ABI lookup on purpose.
#pragma once
#include "aver/core/Types.hpp"
#include "Convert.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystem.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/Profiler.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/EPhysicsUpdateError.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/VehicleEngine.h>
#include <Jolt/Physics/Vehicle/VehicleTransmission.h>

#include "aver/physics/physics_vehicle_ext_abi.h"
#include "aver/physics/physics_world_abi.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Profiler-table scope (physics_world_abi.h), nested under the open scope. Empty without the hook; with the hook built but profiling off,
// a relaxed load and a branch. `name` must be a string literal.
#ifdef JPH_EXTERNAL_PROFILE
#  define AVER_PHYS_SCOPE_CAT2(a, b) a##b
#  define AVER_PHYS_SCOPE_CAT(a, b) AVER_PHYS_SCOPE_CAT2(a, b)
#  define AVER_PHYS_SCOPE(name) ::JPH::ExternalProfileMeasurement AVER_PHYS_SCOPE_CAT(aver_phys_scope_, __LINE__)(name)
#else
#  define AVER_PHYS_SCOPE(name) static_cast<void>(0)
#endif

namespace aver::physics::detail {

// ---- Layers ----
// An object layer is userLayer * 2 + moving. The low bit alone picks the broad-phase tree; the user layer picks collisions via the caller's matrix.
// User layer 0 gives object layers 0 (static) and 1 (moving), as before layers were user-definable.
namespace Layers {

// 16 user layers fit a u16 ObjectLayer, and a mask of them fits a u32.
inline constexpr u32 kUserLayerCount = 16;

inline constexpr JPH::ObjectLayer NON_MOVING = 0;
inline constexpr JPH::ObjectLayer MOVING     = 1;
inline constexpr JPH::uint        NUM        = kUserLayerCount * 2;

inline JPH::ObjectLayer encode(u32 userLayer, bool moving) {
    return static_cast<JPH::ObjectLayer>(userLayer * 2u + (moving ? 1u : 0u));
}
inline u32  userOf(JPH::ObjectLayer l)   { return static_cast<u32>(l) >> 1; }
inline bool movingOf(JPH::ObjectLayer l) { return (static_cast<u32>(l) & 1u) != 0u; }

} // namespace Layers

namespace BroadPhaseLayers {
inline constexpr JPH::BroadPhaseLayer NON_MOVING(0);
inline constexpr JPH::BroadPhaseLayer MOVING(1);
inline constexpr JPH::uint            NUM = 2;
}

class BPLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override {
        return Layers::movingOf(l) ? BroadPhaseLayers::MOVING : BroadPhaseLayers::NON_MOVING;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer l) const override {
        return static_cast<JPH::BroadPhaseLayer::Type>(l) == 0 ? "NON_MOVING" : "MOVING";
    }
#endif
};

class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    // Static geometry is tested only against things that move.
    bool ShouldCollide(JPH::ObjectLayer a, JPH::BroadPhaseLayer b) const override {
        return Layers::movingOf(a) || b == BroadPhaseLayers::MOVING;
    }
};

// The collision matrix lives in the filter: Jolt keeps this pointer for the life of the PhysicsSystem and reads it from worker threads.
// Reads are unsynchronised by design: bools written between steps, read during them.
class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    ObjectLayerPairFilter() {
        for (u32 i = 0; i < Layers::kUserLayerCount; ++i)
            for (u32 j = 0; j < Layers::kUserLayerCount; ++j) collides_[i][j] = true;
    }

    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        // Static against static is never tested, whatever the matrix says: such a pair cannot begin to overlap.
        if (!Layers::movingOf(a) && !Layers::movingOf(b)) return false;
        return collides_[Layers::userOf(a)][Layers::userOf(b)];
    }

    // Kept symmetric: Jolt may ask about a pair in either order.
    void setCollides(u32 a, u32 b, bool on) {
        if (a >= Layers::kUserLayerCount || b >= Layers::kUserLayerCount) return;
        collides_[a][b] = on;
        collides_[b][a] = on;
    }
    bool collides(u32 a, u32 b) const {
        if (a >= Layers::kUserLayerCount || b >= Layers::kUserLayerCount) return false;
        return collides_[a][b];
    }

private:
    bool collides_[Layers::kUserLayerCount][Layers::kUserLayerCount] = {};
};

// ---- Events ----

struct ContactEvent {
    int32_t a = 0, b = 0; Vec3 point, normal;
    // SHAPES_MATERIALS (physics_surface_abi.h): filled by fillContactDetail in OnContactAdded; read by aver_phys_contact_detail.
    int32_t subA = 0, subB = 0;
    int32_t surfA = 0, surfB = 0;        // leaf material, else body label, else 0
    float closing = 0.0f;                // cm/s along the normal when the contact began; positive = approaching
    float friction = 0.0f, restitution = 0.0f;   // as applied, after leaf materials and any surface sampler
};
struct OverlapEvent { int32_t sensor = 0, body = 0; int32_t entered = 0; };

// ---- Vehicles ----

// Kept until aver_phys_vehicle_finish builds Jolt's WheelSettingsWV: Jolt needs every wheel at once.
struct VehicleWheelDesc {
    Vec3  attachCm;                          // top of suspension travel, vehicle-origin relative
    float radiusCm = 0.0f, widthCm = 0.0f;
    float suspMinCm = 0.0f, suspMaxCm = 0.0f;
    float suspHz = 0.0f, suspDamping = 0.0f;
    float maxSteerDeg = 0.0f;
    float maxBrakeNm = 0.0f, maxHandBrakeNm = 0.0f;
    bool  driven = false;
};

// Indices into the wheel list, -1 for a missing side. "Left" is -Y.
struct VehicleAxlePair {
    int left = -1;
    int right = -1;
};

// v2 builder data. Defaults are Jolt's or v1's, so a vehicle that never calls the ext ABI builds the same objects as v1.
struct VehicleAxleDesc {
    float finalDrive = 3.42f;
    int32_t diffMode = 1;            // 1 at 1.4 is Jolt's default, not "open"
    float lsdRatio = 1.4f, split = 0.5f, share = 0.0f, antiRoll = 1000.0f;
};
struct VehicleWheelExt {
    float inertia = 0.9f, angDamping = 0.2f, tvLong = 4.0f, tvLat = 2.0f, brakeBias = 1.0f;
    std::vector<float> longSlip, longMu, latDeg, latMu;     // empty = Jolt's default curve
};
struct VehicleTrackDesc {
    std::vector<int32_t> wheels;
    int32_t driven = -1;
    float inertia = 10.0f, damping = 0.5f, brakeNm = 15000.0f, finalDrive = 6.0f, steerGain = 1.6f, pivot = 0.6f;
};

struct VehicleWheelLive {
    float absFactor = 1.0f;          // ABS: 1 = full brake, falls to 0 while the wheel locks
    float airborne = 0.0f;           // seconds without contact
    bool  hadContact = false;
    bool  skidding = false;
    float skidPeak = 0.0f;
    float f[AVER_PHYS_VWS_COUNT] = {};
    int32_t i[AVER_PHYS_VWSI_COUNT] = {};
};

// The constraint is also a step listener: remove it from both before the chassis body goes (releaseVehicleOfBody does this).
struct VehicleEntry {
    int32_t body = 0;
    JPH::Ref<JPH::VehicleConstraint> constraint;       // null until aver_phys_vehicle_finish
    std::vector<VehicleWheelDesc> wheels;              // the builder's list; the constraint owns the real ones
    float engineMaxTorque = 500.0f;                    // N*m; Jolt's default until set_engine
    float engineMinRpm = 1000.0f;
    float engineMaxRpm = 6000.0f;

    // Chassis as create() was given it; set_chassis_boxes rebuilds the shape from these.
    float halfCm[3] = {0, 0, 0};
    float clearanceCm = 0.0f;
    float comCm[3] = {0, 0, 0};
    float massKg = 0.0f;
    std::vector<float> boxes;                          // 6 floats a box; empty = create()'s box

    // v2 builder data.
    int32_t kind = AVER_PHYS_VEH_KIND_WHEELED;
    float engineInertia = 0.0f;                        // 0 = Jolt's 0.5 scaled by torque / 500, as v1
    float engineDamping = 0.2f;
    std::vector<float> curveX, curveY;                 // empty = Jolt's torque curve
    bool manual = false;
    std::vector<float> gearFwd, gearRev;               // empty = Jolt's ratios; reverse stored as magnitudes
    float gearbox[AVER_PHYS_VG_COUNT] = {0.5f, 0.3f, 0.5f, 0.6f, 0.2f, 0.0f};
    std::vector<VehicleAxleDesc> axleDesc;             // by axle, front first
    bool centerSet = false;
    float centerLsd = 1.4f;                            // Jolt's default; FLT_MAX is written for "open"
    std::vector<VehicleWheelExt> wheelExt;
    std::vector<float> allLongSlip, allLongMu, allLatDeg, allLatMu;
    VehicleTrackDesc track[2];
    float assist[AVER_PHYS_VD_COUNT] = {0, 0, 0.25f, 8.0f, 0, 0.35f, 3.0f, 5.0f, 0, 6.0f, 8.0f, 8.0f, 1.0f, 50.0f, 0.3f, 30.0f, 0.45f, 12.0f};
    float drag = 0.0f, downforce = 0.0f;
    int32_t collisionPeriod = 1;
    float tune[AVER_PHYS_VU_COUNT] = {1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    float maxPitchRollDeg = 0.0f;

    // Live wheel settings from _finish; the step hook and _tune adjust them.
    std::vector<JPH::Ref<JPH::WheelSettings>> wheelSettings;
    std::vector<float> baseSpringHz;                   // builder's spring frequencies, for SUSPENSION_STIFFNESS_SCALE
    float baseTrackBrake[2] = {0, 0};

    // Fixed-step state. All derived; none of it reads a clock.
    // req*: pedals as set; app*: as applied after the aids.
    float reqT = 0, reqS = 0, reqB = 0, reqH = 0;
    float appT = 0, appS = 0, appB = 0, appH = 0;
    float tcsFactor = 1.0f;
    bool  absActive = false, tcsActive = false;
    int32_t gear = 0;                                  // manual: requested gear
    float shiftLeft = 0.0f, clutchRelease = 0.0f;     // manual: seconds of open clutch, then of release
    int32_t lastGear = 0;
    float lastRpm = 0.0f;
    float lastVel[3] = {0, 0, 0};                      // Jolt m/s at the end of the last step
    float longAccelG = 0.0f, latAccelG = 0.0f;
    float airborneAll = 0.0f;
    float sideSeconds = 0.0f;
    bool  flippedSent = false;
    std::vector<VehicleWheelLive> live;
    float vs[AVER_PHYS_VS_COUNT] = {};
    int32_t vsi[AVER_PHYS_VSI_COUNT] = {};
};

// Polled vehicle event (physics_vehicle_ext_abi.h).
struct VehicleEventRec {
    int32_t kind = 0, vehicle = 0, a = 0, b = 0;
    float f[8] = {};
};
// An impact noted by the contact listener, merged by vehicleStepEnd.
struct VehicleImpactRaw {
    int32_t vehicle = 0, other = 0;
    float closingCmS = 0.0f, otherMassKg = 0.0f;
    Vec3 point, normal;
};

// JPH::TempAllocatorImpl is final, so this wrapper tracks the high-water mark. Allocate and Free run in stack order by job
// dependency (TempAllocator.h), so a relaxed atomic is enough.
class TrackingTempAllocator final : public JPH::TempAllocator {
public:
    explicit TrackingTempAllocator(size_t bytes) : inner_(bytes), size_(bytes) {}
    void* Allocate(JPH::uint inSize) override {
        void* p = inner_.Allocate(inSize);
        const size_t use = inner_.GetUsage();
        if (use > peak_.load(std::memory_order_relaxed)) peak_.store(use, std::memory_order_relaxed);
        return p;
    }
    void Free(void* inAddress, JPH::uint inSize) override { inner_.Free(inAddress, inSize); }
    size_t size() const { return size_; }
    size_t peak() const { return peak_.load(std::memory_order_relaxed); }
    void resetPeak() { peak_.store(inner_.GetUsage(), std::memory_order_relaxed); }

private:
    JPH::TempAllocatorImpl inner_;
    size_t size_;
    std::atomic<size_t> peak_{0};
};

// Written by statsAfterUpdate and StepCallScope.
struct StepStats {
    int32_t stepsThisCall = 0;
    int32_t stepsLastCall = 0;
    int32_t contactsLastCall = 0;
    double  stepMsLast = 0.0, stepMsAvg = 0.0, stepMsMax = 0.0, callMsLast = 0.0;
    uint32_t updateErrors = 0;                 // OR of EPhysicsUpdateError since the window reset
    int64_t updateErrorSteps = 0;
    bool    updateErrorLogged = false;         // the one log line per world
    int32_t workers = 0;                       // resolved worker threads (0 = single-threaded)
};

// A fresh id per World. A snapshot keeps its World's id, so a handle that outlives its world answers BAD_HANDLE after a new world appears.
uint64_t nextWorldId();

struct GodotFilterState;   // PhysicsGodotFilter.cpp
struct ContactMonitorState;   // PhysicsAreas.cpp
struct AreaState;
struct BodyGodotState;   // PhysicsBodyGodot.hpp
struct RayVehicleState;  // PhysicsRaycastVehicle.cpp

struct World {
    uint64_t id = nextWorldId();
    JPH::PhysicsSystem                       system;
    BPLayerInterface                         bpLayers;
    ObjectVsBroadPhaseFilter                 objVsBp;
    ObjectLayerPairFilter                    objPair;
    std::unique_ptr<TrackingTempAllocator>   temp;
    // A thread pool, or JobSystemSingleThreaded for PHYSICS.THREADS 0 (PhysicsConfig.cpp).
    std::unique_ptr<JPH::JobSystem>          jobs;

    // Handles are dense int32 from 1: 0 must stay invalid.
    std::unordered_map<int32_t, JPH::BodyID> bodies;
    std::unordered_map<int32_t, JPH::Ref<JPH::CharacterVirtual>> characters;
    // Vehicles share the handle counter; the chassis is an ordinary entry in bodies. vehicleOfBody maps back, so aver_phys_remove_body finds the vehicle first.
    // An ORDERED map: the vehicle step hooks walk it in handle order (determinism). Node-based, so pointers into entries survive inserts.
    std::map<int32_t, VehicleEntry> vehicles;
    std::unordered_map<int32_t, int32_t> vehicleOfBody;
    // Velocity each character's ground added on the last fixed step (moving platform, lift, train). aver_phys_step adds it for the update and removes it after.
    // Kept between steps, so a character leaving a moving ground keeps its horizontal part until it lands. Erased with the character and by a teleport.
    std::unordered_map<int32_t, JPH::Vec3> characterCarry;
    // Absent = 1. Erased with its character.
    std::unordered_map<int32_t, float> characterGravity;
    int32_t nextHandle = 1;

    std::unordered_map<JPH::BodyID, int32_t> byId;
    std::unordered_map<int32_t, bool> sensors;
    // Bodies (the terrain) whose contacts take the host's surface friction. Set by aver_phys_body_set_surface_sampled between steps; read by the contact listener on workers.
    std::unordered_set<JPH::BodyID> surfaceSampled;
    // Godot-style layer/mask, collision exceptions and joint exclusion (PhysicsGodotFilter.cpp). Null until first used; shared_ptr because the type is incomplete here.
    std::shared_ptr<GodotFilterState> godot;
    // Godot material combine (PhysicsSurface.cpp, physics_surface_abi.h): the mode (0 = Jolt's rules) and the per-body ROUGH/ABSORBENT bits by handle (absent = 0). Written between steps, read by the combine hook on workers.
    int32_t combineMode = 0;
    std::unordered_map<int32_t, uint8_t> combineFlags;
    // Per-body contact monitors and Godot-style areas (PhysicsAreas.cpp, physics_areas_abi.h). Null until first used; not simulation state, so not in snapshots.
    std::shared_ptr<ContactMonitorState> monitors;
    std::shared_ptr<AreaState> areas;
    // Godot rigid-body extras (PhysicsBodyGodot.hpp, physics_body_ext_abi.h): _integrate_forces, custom integrator, conveyor velocity, disabled shapes. Null until first used; configuration, not simulation state.
    std::shared_ptr<BodyGodotState> bodyGodot;
    // Raycast vehicles (PhysicsRaycastVehicle.cpp, physics_raycast_vehicle_abi.h). Null until first used; configuration and per-step input, not simulation state.
    std::shared_ptr<RayVehicleState> rayVehicles;

    // Written from Jolt's worker threads under the mutex, drained by the caller between steps.
    std::mutex                 eventMutex;
    std::vector<ContactEvent>  contacts;
    std::vector<OverlapEvent>  overlaps;
    // Polled like contacts, cleared at the top of aver_phys_step; vehicleStepEnd merges the raw impacts.
    std::vector<VehicleEventRec>  vehicleEvents;
    std::vector<VehicleImpactRaw> vehicleImpacts;

    float fixedStep = 1.0f / 60.0f;
    float accumulator = 0.0f;   // real time; steps run at the fixed cadence whatever timeScale is
    float timeScale = 1.0f;     // game time per real time: each step simulates fixedStep * timeScale
    float simStep() const { return fixedStep * timeScale; }

    // Config this world was built with, in the ABI's float form (AVER_PHYS_CF_*); config_active reads it.
    float activeConfig[AVER_PHYS_CF_COUNT] = {};
    int64_t stepsTotal = 0;
    // topologyEpoch: every body removal and constraint add/remove. constraintEpoch: constraint ones only (noteConstraintTopology).
    // Snapshots record both: Jolt restores constraints BY ARRAY INDEX, so a restore into a changed constraint set is refused.
    uint32_t topologyEpoch = 0, constraintEpoch = 0;
    StepStats stats;

    // LARGE_WORLD (PhysicsOrigin.cpp, physics_origin_abi.h)
    bool     inStep = false;                 // true inside aver_phys_step; origin shifts are refused meanwhile
    uint32_t originGeneration = 0;           // +1 per shift; zero in every new World
    double   originTotalCm[3] = {0, 0, 0};   // sum of every shift: this world's origin in the engine's absolute frame
    bool     originOptimizePending = false;  // a shift moved more than 4,096 statics: OptimizeBroadPhase at the top of the next step
};

// Live world, or null between aver_phys_shutdown and the next init. Defined in PhysicsWorld.cpp, which owns its lifetime.
extern std::unique_ptr<World> g_world;

// Called wherever a Jolt constraint is added or removed (joints, vehicles, ragdolls): the one state-recording line (physics_state_abi.h).
inline void noteConstraintTopology() {
    if (!g_world) return;
    ++g_world->constraintEpoch;
    ++g_world->topologyEpoch;
}

// Undefined with no world: callers check g_world, or resolve a handle, first.
JPH::BodyInterface& bi();

// Warn-once "body limit reached" (re-armed by aver_phys_init). Defined in PhysicsWorld.cpp.
void warnBodyLimit();

// Creates a body, registers it in both handle tables, returns its handle. userLayer defaults to 0, so existing call sites and bodies keep their object layer.
int32_t addBody(const JPH::Shape* shape, const Vec3& centreCm, bool dynamic, float massKg,
                bool sensor = false, u32 userLayer = 0);

const JPH::BodyID* findBody(int32_t h);

JPH::CharacterVirtual* findCharacter(int32_t h);

void writeVec(float* out, const Vec3& v);

// Called by aver_phys_shutdown BEFORE the world goes. A hook, not a lazy clear: PhysicsJoints.cpp holds Refs into the PhysicsSystem,
// and waiting for the next lookup would leave them alive, with aver_phys_joint_count answering with a previous world's joints. Defined in PhysicsJoints.cpp.
void destroyAllJoints();

// Breakable joints: checked after every fixed step (PhysicsJoints.cpp).
void afterStepJoints();

// Ragdolls (PhysicsRagdoll.cpp): torn down before the bodies. A ragdoll part removed on its own takes its ragdoll with it.
void destroyAllRagdolls();
void releaseRagdollOfBody(int32_t body);

// Called by aver_phys_shutdown beside destroyAllJoints, for the same reason: constraints point at chassis bodies and are step listeners. Defined in PhysicsVehicle.cpp.
void destroyAllVehicles();

// If body is a chassis, removes its vehicle (constraint and step listener) and forgets it. Called by aver_phys_remove_body before the body goes. Defined in PhysicsVehicle.cpp.
void releaseVehicleOfBody(int32_t body);

// Jolt takes the stair step-up and step-down as ExtendedUpdate arguments, not as character state. The setter's numbers must be read back at update time;
// without this call the setter stores a value nothing reads. Defined in PhysicsCharacter.cpp; cleared on shutdown.
void applyCharacterStairSettings(int32_t handle, JPH::CharacterVirtual::ExtendedUpdateSettings& out);
void clearCharacterStairSettings();

// The vehicle behind a handle (sets aver_phys_last_error on failure); findFinished: the same, once finished.
VehicleEntry* findVehicle(int32_t h);
VehicleEntry* findFinished(int32_t h);

// Axles by the v1 geometry rule (same x within 5 cm, opposite sides of y = 0), front (largest x) first. The index is the axle number the ABI speaks of.
std::vector<VehicleAxlePair> vehicleAxles(const std::vector<VehicleWheelDesc>& wheels);

void vehicleFillPowertrain(const VehicleEntry& e, JPH::VehicleEngineSettings& engine,
                           JPH::VehicleTransmissionSettings& trans);

// Builds WheelSettingsTV wheels and the TrackedVehicleController into settings. False (logged) refuses the finish.
bool vehicleBuildTracked(VehicleEntry& e, JPH::VehicleConstraintSettings& settings, int32_t handle);
void vehicleTrackedInput(VehicleEntry& e, float throttle, float steer, float brake, float handbrake);
// Track speeds at the rim of the driven wheel, cm/s.
void vehicleTrackSpeeds(const VehicleEntry& e, float& leftCmS, float& rightCmS);
void vehicleTrackedBrakeScale(VehicleEntry& e, float scale);
// Average track angular velocity times final drive and gearbox ratio, as engine rpm.
float vehicleTrackedWheelRpm(const VehicleEntry& e);

JPH::VehicleEngine& vehicleEngine(VehicleEntry& e);
JPH::VehicleTransmission& vehicleTransmission(VehicleEntry& e);
// Pedals to either controller kind; throttle wakes a sleeping chassis.
void vehicleApplyInput(VehicleEntry& e, float throttle, float steer, float brake, float handbrake);
// Called by _finish once the constraint exists: sizes live state and applies the tune table.
void vehicleOnFinished(VehicleEntry& e);
// Stops spinning parts and puts the gearbox in neutral (teleport reset for _set_pose and _flip).
void vehicleResetDrivetrain(VehicleEntry& e);

// Fixed-loop hooks (PhysicsWorld.cpp): top of aver_phys_step, before and after system.Update.
void vehicleBeginCall();
void vehicleStepBegin(float dt);
void vehicleStepEnd(float dt);
// Called under World::eventMutex on a worker thread; notes a chassis hit for vehicleStepEnd. Normal is body a to body b, engine axes.
void vehicleOnContact(int32_t ha, int32_t hb, const JPH::Body& a, const JPH::Body& b,
                      const Vec3& pointCm, const Vec3& normal);

// One system.Update with the table's collision steps. The returned EPhysicsUpdateError is OR-ed into the stats and logged once per world with the cure named.
// Counts World::stepsTotal.
void stepWorldOnce();

// Brackets one aver_phys_step for the stats. No-op with no world.
class StepCallScope {
public:
    StepCallScope();
    ~StepCallScope();
    StepCallScope(const StepCallScope&) = delete;
    StepCallScope& operator=(const StepCallScope&) = delete;
private:
    std::chrono::steady_clock::time_point t0_;
};

// Lines Jolt draws during a step (contacts, faces, linear casts; armed by aver_phys_debug_arm) go to a buffer cleared by debugStepBegin,
// so it holds the last step only. One flag test when unarmed; empty without JPH_DEBUG_RENDERER.
void debugStepBegin();
void debugStepEnd();

// Last statement of every fixed step: appends to the hash log when one is open.
void stateAfterFixedStep();

// Diagnostics preset: world init (before workers start), step end, and teardown (Jolt's profiler and the determinism log).
void diagOnWorldInit();
void diagOnStepEnd();
void diagOnWorldShutdown();

// Drops every snapshot and closes the hash log; called after destroyAllJoints. Snapshots keep World::id, so a stale handle is dead even if this were missed.
void stateOnShutdown();

// Engine state outside this file set rides in snapshots: save appends bytes; restore gets them back (false if it cannot apply them).
// name is a unique string literal, hashed into the blob; a missing participant is skipped at restore. Registration is process-wide and may happen at static init. NOTHING REGISTERS TODAY.
struct StateParticipant {
    const char* name = nullptr;
    void (*save)(std::vector<uint8_t>& out) = nullptr;
    bool (*restore)(const uint8_t* data, size_t size) = nullptr;
};
void registerStateParticipant(const StateParticipant& p);

// File-static tables in their own .cpp, read by the manifest and cleared by a restore.
void jointHandles(std::vector<int32_t>& out);        // PhysicsJoints.cpp: every live joint handle, ascending
void clearJointBreakEvents();                        // PhysicsJoints.cpp: undrained break events; a restore re-simulates them
void ragdollHandles(std::vector<int32_t>& out);      // PhysicsRagdoll.cpp: every live ragdoll id, ascending

} // namespace aver::physics::detail
