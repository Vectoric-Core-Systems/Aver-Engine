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

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace aver::physics::detail {

// ---- Layers ---------------------------------------------------------------------------------------
// Two object layers: things that never move, and things that do.
namespace Layers {
inline constexpr JPH::ObjectLayer NON_MOVING = 0;
inline constexpr JPH::ObjectLayer MOVING     = 1;
inline constexpr JPH::uint        NUM        = 2;
}
namespace BroadPhaseLayers {
inline constexpr JPH::BroadPhaseLayer NON_MOVING(0);
inline constexpr JPH::BroadPhaseLayer MOVING(1);
inline constexpr JPH::uint            NUM = 2;
}

// Maps each object layer onto its broad-phase layer.
class BPLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
    // Builds the object-layer to broad-phase-layer table.
    BPLayerInterface() {
        m_[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
        m_[Layers::MOVING]     = BroadPhaseLayers::MOVING;
    }
    JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override { return m_[l]; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer l) const override {
        return static_cast<JPH::BroadPhaseLayer::Type>(l) == 0 ? "NON_MOVING" : "MOVING";
    }
#endif
private:
    JPH::BroadPhaseLayer m_[Layers::NUM];
};

// Decides which object layers are tested against which broad-phase layers.
class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    // Static geometry only needs testing against things that move.
    bool ShouldCollide(JPH::ObjectLayer a, JPH::BroadPhaseLayer b) const override {
        return a != Layers::NON_MOVING || b == BroadPhaseLayers::MOVING;
    }
};

// Decides which object layers collide with each other.
class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    // Static geometry is not tested against static geometry.
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return a != Layers::NON_MOVING || b == Layers::MOVING;
    }
};

// ---- Events ---------------------------------------------------------------------------------------

// Two solid bodies that began touching, and where.
struct ContactEvent { int32_t a = 0, b = 0; Vec3 point, normal; };
// A body entering or leaving a sensor volume.
struct OverlapEvent { int32_t sensor = 0, body = 0; int32_t entered = 0; };

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
int32_t addBody(const JPH::Shape* shape, const Vec3& centreCm, bool dynamic, float massKg,
                bool sensor = false);

// The Jolt body id behind a handle, or nullptr.
const JPH::BodyID* findBody(int32_t h);

// The character behind a handle, or nullptr.
JPH::CharacterVirtual* findCharacter(int32_t h);

// Writes a vector into a caller-owned float[3].
void writeVec(float* out, const Vec3& v);

} // namespace aver::physics::detail
