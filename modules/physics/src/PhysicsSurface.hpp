// PhysicsSurface.hpp -- the physical-surface material a shape carries (docs/plans/wave11/SHAPES_MATERIALS.md, section 2.6).
//
// SRC-LOCAL like PhysicsInternal.hpp: it names JPH:: types. A surface is a 32-bit id (FNV-1a of its name,
// the id Aver.Framework.Surface.IdOf already hands scripts); one shared AverSurface per id holds the numbers,
// so editing a surface edits every shape that uses it. The builders intern through surfaceMaterial; the registry
// (names, tune), the combine hook, the body label and the detail queries live in PhysicsSurface.cpp.
#pragma once
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <Jolt/Physics/Collision/Shape/SubShapeID.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <atomic>
#include <cstdint>

namespace aver::physics::detail {

struct ContactEvent;   // PhysicsInternal.hpp

class AverSurface final : public JPH::PhysicsMaterial {
public:
    // Identity by pointer: GetDebugName returns this exact literal, so a material is ours without RTTI.
    static const char* const kName;

    explicit AverSurface(int32_t surfaceId) : id(surfaceId) {}
    const char* GetDebugName() const override { return kName; }

    const int32_t id;
    std::atomic<float> friction{-1.0f};      // < 0: defer to the body
    std::atomic<float> restitution{-1.0f};   // < 0: defer to the body
    std::atomic<int32_t> tag{0};
};

// The shared material for a surface id, interning it on first use. Null for id 0 ("none"). Safe from any thread.
JPH::RefConst<JPH::PhysicsMaterial> surfaceMaterial(int32_t surface);

// The surface id of a shape's material: 0 for null, Jolt's default or any material that is not an AverSurface.
int32_t surfaceIdOf(const JPH::PhysicsMaterial* material);

// True once any surface id has been interned (one relaxed load), so a project that never uses surfaces can skip work.
bool surfacesInterned();

// The interned surface for an id, or null. The object lives for the process (the registry never frees one).
const AverSurface* surfaceObject(int32_t surface);

// ---- the body label: bits 32 to 63 of Body::UserData; the low 32 bits are the entity id ---------------------------

inline constexpr uint64_t kLabelMask = 0xFFFFFFFF00000000ull;
inline int32_t bodyLabelOf(uint64_t userData) { return static_cast<int32_t>(static_cast<uint32_t>(userData >> 32)); }
// `userData` with the entity half replaced (a negative entity must not sign-extend into the label).
inline uint64_t withEntityHalf(uint64_t userData, int32_t entity) { return (userData & kLabelMask) | static_cast<uint32_t>(entity); }
inline uint64_t withLabelHalf(uint64_t userData, int32_t surface) { return (userData & ~kLabelMask) | (static_cast<uint64_t>(static_cast<uint32_t>(surface)) << 32); }

// Set as Shape::mUserData of a mesh built with per-triangle user data, so a hit can tell "triangle 0" from "none".
inline constexpr uint64_t kMeshTriangleUserData = 1;

// ---- leaf materials ------------------------------------------------------------------------------------------

// The friction / restitution of the surface the body's leaf shape carries at `sub`, or `fallback` when it carries none
// (or defers). Bodies that are soft bodies have no leaf materials. Safe on Jolt worker threads.
float surfaceFrictionOf(const JPH::Body& body, const JPH::SubShapeID& sub, float fallback);
float surfaceRestitutionOf(const JPH::Body& body, const JPH::SubShapeID& sub, float fallback);

// What a contact between `a` (value fa) and `b` (value fb) applies under the world's combine mode (Jolt's rules, or Godot's with the
// bodies' ROUGH / ABSORBENT flags). The values are already resolved per side by the caller. Safe on workers.
float combineFrictionValues(float fa, const JPH::Body& a, float fb, const JPH::Body& b);
float combineRestitutionValues(float ra, const JPH::Body& a, float rb, const JPH::Body& b);

// Installs the two combine functions (Jolt's own rules, leaf materials first). Called by aver_phys_init after Init.
void installSurfaceCombine(JPH::PhysicsSystem& system);

// Fills the detail fields of a contact the listener is recording (OnContactAdded, after any surface-sampler override).
void fillContactDetail(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold,
                       const JPH::ContactSettings& settings, ContactEvent& out);

} // namespace aver::physics::detail
