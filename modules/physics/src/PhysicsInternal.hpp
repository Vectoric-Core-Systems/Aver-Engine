// PhysicsInternal.hpp -- the live world, and the handful of lookups every ABI file needs to reach it.
//
// SRC-LOCAL, NEVER include/. Exactly like Convert.hpp and Buoyancy.hpp beside it, and for the same
// reason: this header names JPH:: types, and modules/physics/CMakeLists.txt links Aver.Physics.Jolt
// PRIVATE precisely so nothing above this module can include a Jolt header by accident. Moving this
// file into include/ would silently make the backend part of the module's public contract.
//
// WHY IT EXISTS NOW, WHEN World WAS DELIBERATELY FILE-LOCAL BEFORE. BuoyancyAbi.cpp's own header
// comment says World is "private to PhysicsWorld.cpp's translation unit" and that findBody is
// "injected rather than reached for directly" -- and that was the right call for what buoyancy needed,
// which is one lookup and one iteration. It does not survive contact with the joint, shape and body-
// dynamics surface: those are dozens of functions of which nearly every one needs bi(), findBody() and
// the handle counter, and injecting three function pointers per file to avoid a shared header would be
// ceremony that buys nothing. The invariant that actually mattered -- no Jolt type escapes the module
// -- is unchanged and is enforced by this file's location, not by its contents being unreachable.
//
// BuoyancyAbi.cpp is deliberately NOT rewritten to use this header. It works today by borrowing the
// public ABI (aver_phys_body_position) to validate a handle, its own comment explains why that beats
// exporting an internal, and that reasoning is still true for a file that needs exactly one check.
// Changing it would be churn in a file this work has no other reason to touch.
#pragma once
#include "aver/core/Types.hpp"
#include "Convert.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace aver::physics::detail {

// ---- Layers ---------------------------------------------------------------------------------------
//
// AN OBJECT LAYER ENCODES TWO THINGS AT ONCE, and the encoding is what makes user-defined collision
// layers possible without changing a single existing body's behaviour.
//
// Jolt asks two questions of a layer: which BROAD-PHASE tree a body belongs in (a coarse, structural
// split -- things that never move are kept apart from things that do, so the static half of a level
// need not be re-inserted every frame), and which other object layers it COLLIDES with. This module
// used to answer the first with the whole layer -- exactly two of them, NON_MOVING and MOVING -- which
// left the second question with no room to say anything at all: there was one static layer and one
// moving layer, so a caller could not put the player and the player's own bullets on layers that
// ignore each other.
//
// So an object layer is now a PAIR: `userLayer * 2 + moving`. The broad-phase question is answered by
// the low bit alone, exactly as before; the collision question is answered by the user layer through
// a matrix the caller owns. And because user layer 0 is the default, a body created without asking for
// a layer lands on object layer 0 (static) or 1 (moving) -- THE SAME TWO NUMBERS AS BEFORE THIS
// CHANGE, with the same broad-phase mapping and the same pair rule. Existing content is not merely
// compatible, it is identical.
namespace Layers {

// How many user layers exist. 16 is the number that fits a u16 ObjectLayer at two per user layer with
// room to spare, and a mask of them fits a u32 for the query filters -- more would cost a bigger
// matrix for layers nobody has asked for.
inline constexpr u32 kUserLayerCount = 16;

// The default user layer's two halves, unchanged in name and in VALUE from when they were the only
// two layers that existed.
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

// Maps each object layer onto its broad-phase layer -- which is now the low bit and nothing else.
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

// Decides which object layers are tested against which broad-phase layers.
class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    // Static geometry only needs testing against things that move -- the same rule as before, now
    // asked of the low bit rather than of the whole layer.
    bool ShouldCollide(JPH::ObjectLayer a, JPH::BroadPhaseLayer b) const override {
        return Layers::movingOf(a) || b == BroadPhaseLayers::MOVING;
    }
};

// Decides which object layers collide with each other: the structural rule first, then the caller's.
//
// THE MATRIX LIVES HERE, INSIDE THE FILTER, rather than in a global. Jolt holds a pointer to this
// object for the life of the PhysicsSystem and asks it from worker threads; keeping the table in the
// same object means it is created and destroyed with the world, so a matrix set up for one level
// cannot leak into the next. Reads are unsynchronised and that is deliberate -- it is a table of
// bools written between steps and read during them, and the alternative is a lock taken on every
// broad-phase pair.
class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    ObjectLayerPairFilter() {
        for (u32 i = 0; i < Layers::kUserLayerCount; ++i)
            for (u32 j = 0; j < Layers::kUserLayerCount; ++j) collides_[i][j] = true;
    }

    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        // STATIC AGAINST STATIC IS STILL NEVER TESTED, whatever the matrix says. Two bodies that
        // cannot move cannot begin to overlap, so the pair is work with no possible outcome -- and
        // this is the rule that was here before, kept ahead of the caller's so no matrix entry can
        // accidentally reintroduce that cost.
        if (!Layers::movingOf(a) && !Layers::movingOf(b)) return false;
        return collides_[Layers::userOf(a)][Layers::userOf(b)];
    }

    // Symmetric by construction: Jolt may ask about a pair in either order, and a matrix that
    // disagreed with itself would make collision depend on which body the broad-phase happened to
    // visit first.
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

// ---- Events ---------------------------------------------------------------------------------------

// Two solid bodies that began touching, and where.
struct ContactEvent { int32_t a = 0, b = 0; Vec3 point, normal; };
// A body entering or leaving a sensor volume.
struct OverlapEvent { int32_t sensor = 0, body = 0; int32_t entered = 0; };

// ---- Vehicles -------------------------------------------------------------------------------------

// One wheel as the caller described it, in the engine's units. Kept until aver_phys_vehicle_finish
// turns the list into Jolt's WheelSettingsWV: Jolt wants every wheel at once, the ABI hands them over
// one call at a time.
struct VehicleWheelDesc {
    Vec3  attachCm;                          // top of the suspension travel, vehicle-origin relative
    float radiusCm = 0.0f, widthCm = 0.0f;
    float suspMinCm = 0.0f, suspMaxCm = 0.0f;
    float suspHz = 0.0f, suspDamping = 0.0f;
    float maxSteerDeg = 0.0f;
    float maxBrakeNm = 0.0f, maxHandBrakeNm = 0.0f;
    bool  driven = false;
};

// A vehicle: its chassis body and, once finished, the Jolt constraint that drives it. The constraint is
// also a step listener registered with the system, so it must be removed from both BEFORE the chassis
// body goes -- PhysicsVehicle.cpp's releaseVehicleOfBody is the one place that does it.
struct VehicleEntry {
    int32_t body = 0;                                  // the chassis' ordinary body handle
    JPH::Ref<JPH::VehicleConstraint> constraint;       // null until aver_phys_vehicle_finish
    std::vector<VehicleWheelDesc> wheels;              // the builder's list; the constraint owns the real ones
    float engineMaxTorque = 500.0f;                    // N*m, Jolt's own defaults until set_engine
    float engineMinRpm = 1000.0f;
    float engineMaxRpm = 6000.0f;
};

// ---- The world ------------------------------------------------------------------------------------

// The whole simulation: Jolt's system, the handle tables, and the event queues.
struct World {
    JPH::PhysicsSystem                       system;
    BPLayerInterface                         bpLayers;
    ObjectVsBroadPhaseFilter                 objVsBp;
    ObjectLayerPairFilter                    objPair;
    std::unique_ptr<JPH::TempAllocatorImpl>  temp;
    std::unique_ptr<JPH::JobSystemThreadPool> jobs;

    // Handles are dense int32 starting at 1, because 0 must stay invalid.
    std::unordered_map<int32_t, JPH::BodyID> bodies;
    std::unordered_map<int32_t, JPH::Ref<JPH::CharacterVirtual>> characters;
    // Vehicles share the one handle counter below, and the chassis is an ordinary entry in `bodies`.
    // `vehicleOfBody` is the way back from that body, so aver_phys_remove_body can find the vehicle that
    // must go first without walking the table.
    std::unordered_map<int32_t, VehicleEntry> vehicles;
    std::unordered_map<int32_t, int32_t> vehicleOfBody;
    // What each character's GROUND added to its velocity on the last fixed step, in Jolt's units and
    // axes: the moving platform, lift or train it rides. aver_phys_step adds it for the update and takes
    // it back out afterwards, so the velocity a driver reads and writes stays its own. Kept between
    // steps because a character that leaves a moving ground keeps its horizontal part until it lands.
    // Gone with the world; erased with its character, and by a teleport.
    std::unordered_map<int32_t, JPH::Vec3> characterCarry;
    // aver_phys_character_set_gravity_factor; absent means 1. Erased with its character.
    std::unordered_map<int32_t, float> characterGravity;
    int32_t nextHandle = 1;

    std::unordered_map<JPH::BodyID, int32_t> byId;   // reverse of `bodies`
    std::unordered_map<int32_t, bool> sensors;       // which handles are sensors

    // Written from Jolt's worker threads under the mutex, drained by the caller between steps.
    std::mutex                 eventMutex;
    std::vector<ContactEvent>  contacts;
    std::vector<OverlapEvent>  overlaps;

    float fixedStep = 1.0f / 60.0f;
    float accumulator = 0.0f;
};

// The live world, or null between aver_phys_shutdown and the next aver_phys_init. DEFINED in
// PhysicsWorld.cpp, which owns its lifetime; every other file only reads it.
extern std::unique_ptr<World> g_world;

// Jolt's body interface for the live world. UNDEFINED BEHAVIOUR WITH NO WORLD -- every caller in this
// module checks g_world (or resolves a handle, which does) before reaching for it, exactly as the
// original file-local version required.
JPH::BodyInterface& bi();

// Creates a body from a shape, registers it in both handle tables, and returns its handle.
//
// `userLayer` DEFAULTS TO 0, which is why every existing call site is unchanged and every body this
// module made before user layers existed lands on exactly the object layer it always did.
int32_t addBody(const JPH::Shape* shape, const Vec3& centreCm, bool dynamic, float massKg,
                bool sensor = false, u32 userLayer = 0);

// The Jolt body id behind a handle, or nullptr.
const JPH::BodyID* findBody(int32_t h);

// The character behind a handle, or nullptr.
JPH::CharacterVirtual* findCharacter(int32_t h);

// Writes a vector into a caller-owned float[3].
void writeVec(float* out, const Vec3& v);

// Destroys every joint, and is called by aver_phys_shutdown BEFORE the world goes.
//
// A HOOK RATHER THAN A LAZY CLEAR, and the difference is real. PhysicsJoints.cpp keeps its own table
// of JPH::Ref<Constraint>, and those constraints point into the PhysicsSystem and at the bodies inside
// it. Noticing on the NEXT lookup that g_world had gone would leave that table holding references to
// constraints belonging to a destroyed system for however long it took someone to ask -- and would
// leave aver_phys_joint_count answering with a previous world's joints until then. Defined in
// PhysicsJoints.cpp, called from PhysicsWorld.cpp, so the ordering is stated in one place instead of
// being a property nobody wrote down.
void destroyAllJoints();

// Destroys every vehicle's constraint and step listener and empties both vehicle tables, called by
// aver_phys_shutdown beside destroyAllJoints and for the same reason: a VehicleConstraint points at a
// chassis body and into the PhysicsSystem, and is registered with it as a step listener. The chassis
// bodies themselves are ordinary bodies and go with the rest of the body table. Defined in
// PhysicsVehicle.cpp.
void destroyAllVehicles();

// If `body` is a vehicle's chassis, removes the vehicle -- constraint and step listener -- and forgets
// it, leaving the body for the caller to remove. Called by aver_phys_remove_body before it touches the
// body, so a chassis can never be destroyed while a constraint still points at it. A no-op for any
// other body. Defined in PhysicsVehicle.cpp.
void releaseVehicleOfBody(int32_t body);

// Applies the stair-stepping distances a character was given to the settings the step loop passes to
// ExtendedUpdate, and forgets them all on shutdown. Both defined in PhysicsCharacter.cpp.
//
// THIS SEAM IS THE DIFFERENCE BETWEEN A SETTER AND A FEATURE. Jolt takes the walk-stairs step-up and
// the stick-to-floor step-down as ARGUMENTS to ExtendedUpdate, not as state on the character, so there
// is no Jolt-side setter for aver_phys_character_set_stair_stepping to forward to -- it can only
// record the numbers and something must read them back at the moment of the update. Without this call
// the setter would be a function that stores a value nothing ever looks at, which is the shape this
// repository keeps shipping and then finding unused.
void applyCharacterStairSettings(int32_t handle, JPH::CharacterVirtual::ExtendedUpdateSettings& out);
void clearCharacterStairSettings();

} // namespace aver::physics::detail
