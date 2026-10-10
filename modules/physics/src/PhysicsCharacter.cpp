// PhysicsCharacter.cpp -- the character-tuning half of the ABI: slope, stairs, richer ground state,
// shape swapping (crouch) and the mass/strength a CharacterVirtual uses to push the world around.
//
// physics_abi.h's own Character section (in PhysicsWorld.cpp) only reaches create/destroy/velocity/
// position/grounded -- a capsule built from a radius and a height with every other Jolt setting left
// at its default. This file is that gap, in its own translation unit and its own header
// (physics_character_abi.h) because physics_abi.h is not this change's to edit.
//
// SAME SHAPE AS PhysicsBody.cpp: resolve the handle, refuse a dead one, convert units, call Jolt. What
// earns a comment below is only where that is not the whole story -- the ground-body handle lookup that
// has to check two tables, stair stepping having nowhere on CharacterVirtual to live, and SetShape's
// failure being the feature rather than a bug to hide.
#include "aver/physics/physics_character_abi.h"
#include "Convert.hpp"
#include "PhysicsInternal.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <cmath>
#include <unordered_map>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

// ---- Stair stepping side table -----------------------------------------------------------------
//
// NOWHERE ELSE FOR THIS TO LIVE. WalkStairs and StickToFloor take a step-up and a step-down as
// arguments to ExtendedUpdate, not as persistent state on CharacterVirtual -- there is no Jolt-side
// getter or setter this could forward to, unlike every other function below. This table is that
// missing piece of state, keyed by the same handle as everything else in this module.
//
// DEFAULTS MIRROR JOLT'S OWN: ExtendedUpdateSettings ships mWalkStairsStepUp{0,0.4,0} and
// mStickToFloorStepDown{0,-0.5,0} -- 40cm and 50cm once converted -- so a character that never calls
// the setter reads back the same numbers the engine's own step loop already runs with today, not zero.
struct StairSettings {
    float stepUpCm = 40.0f;
    float stepDownCm = 50.0f;
};

// CLEARED ON SHUTDOWN through detail::clearCharacterStairSettings(), called from aver_phys_shutdown
// beside the water table's own clear -- character handles restart at 1 on the next aver_phys_init, so
// without that a new world's first character would silently inherit whatever the previous world's
// handle 1 last set here.
std::unordered_map<int32_t, StairSettings> g_stairSettings;

// The handle behind a Jolt body id, checking both bodies AND characters' inner bodies -- the same
// two-table lookup aver_phys_raycast already needs in PhysicsWorld.cpp, and for the same reason: the
// only broadphase-visible things this module ever creates are addBody()'s bodies and a character's own
// inner body, and the inner body was never registered in g_world->bodies because Jolt creates and owns
// that BodyID itself.
int32_t handleOfBody(const JPH::BodyID& id) {
    if (id.IsInvalid()) return 0;
    const auto it = g_world->byId.find(id);
    if (it != g_world->byId.end()) return it->second;
    for (const auto& [h, other] : g_world->characters)
        if (!other->GetInnerBodyID().IsInvalid() && other->GetInnerBodyID() == id) return h;
    return 0;
}

// Lookup with the three-way error shape of findVehicle (PhysicsVehicle.cpp). detail::findCharacter
// leaves the slot alone, so this wraps it rather than replacing it.
JPH::CharacterVirtual* character(int32_t h) {
    if (!g_world) { setAbiError(AbiError::NotInitialised); return nullptr; }
    JPH::CharacterVirtual* c = findCharacter(h);
    setAbiError(c ? AbiError::Ok : AbiError::BadHandle);
    return c;
}

// An argument refusal: the 0 return stays, the reason goes on aver_phys_last_error().
int32_t refuse(AbiError e) { setAbiError(e); return 0; }

} // namespace

extern "C" {

// ---- Max slope angle ---------------------------------------------------------------------------

int32_t aver_phys_character_set_max_slope_angle(int32_t ch, float radians) {
    JPH::CharacterVirtual* c = character(ch);
    if (!c) return 0;
    c->SetMaxSlopeAngle(radians);
    return 1;
}

int32_t aver_phys_character_max_slope_angle(int32_t ch, float* outRadians) {
    JPH::CharacterVirtual* c = character(ch);
    if (!c) return 0;
    if (!outRadians) return refuse(AbiError::NullPointer);
    // CharacterBase stores only the COSINE (mCosMaxSlopeAngle) and GetCosMaxSlopeAngle is its one
    // accessor -- acos recovers the angle this same setter put in, not a second definition of it.
    *outRadians = std::acos(c->GetCosMaxSlopeAngle());
    return 1;
}

// ---- Stair stepping -----------------------------------------------------------------------------

int32_t aver_phys_character_set_stair_stepping(int32_t ch, float stepUpCm, float stepDownCm) {
    if (!character(ch)) return 0;
    g_stairSettings[ch] = StairSettings{stepUpCm, stepDownCm};
    return 1;
}

int32_t aver_phys_character_stair_stepping(int32_t ch, float* outStepUpCm, float* outStepDownCm) {
    if (!character(ch)) return 0;
    if (!outStepUpCm || !outStepDownCm) return refuse(AbiError::NullPointer);
    const auto it = g_stairSettings.find(ch);
    const StairSettings s = it != g_stairSettings.end() ? it->second : StairSettings{};
    *outStepUpCm = s.stepUpCm;
    *outStepDownCm = s.stepDownCm;
    return 1;
}

// ---- Gravity factor ----------------------------------------------------------------------------

int32_t aver_phys_character_set_gravity_factor(int32_t ch, float factor) {
    if (!character(ch)) return 0;
    if (!std::isfinite(factor) || factor < 0.0f) return refuse(AbiError::InvalidArgument);
    g_world->characterGravity[ch] = factor;
    return 1;
}

int32_t aver_phys_character_gravity_factor(int32_t ch, float* outFactor) {
    if (!character(ch)) return 0;
    if (!outFactor) return refuse(AbiError::NullPointer);
    const auto it = g_world->characterGravity.find(ch);
    *outFactor = it != g_world->characterGravity.end() ? it->second : 1.0f;
    return 1;
}

// ---- Ground state ------------------------------------------------------------------------------

int32_t aver_phys_character_ground_state(int32_t ch) {
    JPH::CharacterVirtual* c = character(ch);
    if (!c) return -1;   // not 0: 0 is OnGround, a real answer -- same convention as body motion type
    switch (c->GetGroundState()) {
        case JPH::CharacterBase::EGroundState::OnGround:      return AVER_PHYS_GROUND_ON_GROUND;
        case JPH::CharacterBase::EGroundState::OnSteepGround: return AVER_PHYS_GROUND_ON_STEEP_GROUND;
        case JPH::CharacterBase::EGroundState::NotSupported:  return AVER_PHYS_GROUND_NOT_SUPPORTED;
        case JPH::CharacterBase::EGroundState::InAir:         return AVER_PHYS_GROUND_IN_AIR;
    }
    return -1;
}

// ---- What the character is standing on ----------------------------------------------------------

int32_t aver_phys_character_ground_normal(int32_t ch, float* outXyz) {
    JPH::CharacterVirtual* c = character(ch);
    if (!c) return 0;
    if (!outXyz) return refuse(AbiError::NullPointer);
    // fromJoltUnit, not fromJoltDir: a ground normal is a unit vector, same as a contact normal in
    // EventListener::OnContactAdded (PhysicsWorld.cpp).
    writeVec(outXyz, fromJoltUnit(c->GetGroundNormal()));
    return 1;
}

int32_t aver_phys_character_ground_position(int32_t ch, float* outXyz) {
    JPH::CharacterVirtual* c = character(ch);
    if (!c) return 0;
    if (!outXyz) return refuse(AbiError::NullPointer);
    writeVec(outXyz, fromJolt(c->GetGroundPosition()));
    return 1;
}

int32_t aver_phys_character_ground_body(int32_t ch) {
    JPH::CharacterVirtual* c = character(ch);
    if (!c) return 0;
    return handleOfBody(c->GetGroundBodyID());
}

int32_t aver_phys_character_ground_velocity(int32_t ch, float* outXyz) {
    JPH::CharacterVirtual* c = character(ch);
    if (!c) return 0;
    if (!outXyz) return refuse(AbiError::NullPointer);
    writeVec(outXyz, fromJoltDir(c->GetGroundVelocity()));
    return 1;
}

// ---- Inherited motion ---------------------------------------------------------------------------------
// World::characterCarry, which aver_phys_step owns (PhysicsWorld.cpp): read and written here, in Jolt's
// units and axes there.

int32_t aver_phys_character_inherited_velocity(int32_t ch, float* outXyz) {
    if (!character(ch)) return 0;
    if (!outXyz) return refuse(AbiError::NullPointer);
    const auto it = g_world->characterCarry.find(ch);
    writeVec(outXyz, it != g_world->characterCarry.end() ? fromJoltDir(it->second) : Vec3(0.0f, 0.0f, 0.0f));
    return 1;
}

int32_t aver_phys_character_set_inherited_velocity(int32_t ch, float x, float y, float z) {
    if (!character(ch)) return 0;
    g_world->characterCarry.insert_or_assign(ch, toJoltDir(Vec3(x, y, z)));
    return 1;
}

// ---- Shape (crouching) --------------------------------------------------------------------------

// Returns 0 with Unsupported when SetShape refuses because the new shape would not fit (a ceiling
// above a standing request); InvalidArgument when the dimensions or the Jolt capsule are illegal.
int32_t aver_phys_character_set_shape(int32_t ch, float radius, float height, float maxPenetrationCm) {
    JPH::CharacterVirtual* c = character(ch);
    if (!c) return 0;

    // Same derivation as aver_phys_character_create: Jolt's capsule takes the HALF height of its
    // cylinder, so the two caps come out of the total height first.
    const float rM = cmToM(radius);
    const float halfCyl = cmToM(height) * 0.5f - rM;
    if (halfCyl <= 0.0f) {
        AVER_WARN("[Physics] character shape {}cm tall is too short for radius {}cm", height, radius);
        return refuse(AbiError::InvalidArgument);
    }
    JPH::CapsuleShapeSettings capsule(halfCyl, rM);
    capsule.SetEmbedded();
    auto res = capsule.Create();
    if (res.HasError()) { AVER_WARN("[Physics] character shape: {}", res.GetError().c_str()); return refuse(AbiError::InvalidArgument); }

    // SetShape checks the NEW shape against the world before committing and reports failure rather
    // than embedding the character in whatever it no longer fits under -- that refusal is the entire
    // reason crouch/stand exists as a question with a real answer instead of a character clipping
    // through a low ceiling the moment it tries to stand.
    const bool ok = c->SetShape(res.Get(), cmToM(maxPenetrationCm),
                                g_world->system.GetDefaultBroadPhaseLayerFilter(Layers::MOVING),
                                g_world->system.GetDefaultLayerFilter(Layers::MOVING),
                                {}, {}, *g_world->temp);
    if (!ok) return refuse(AbiError::Unsupported);

    // The inner body is a separate shape reference Jolt does not update on its own (its own doc
    // comment on SetInnerBodyShape says to call this after a successful SetShape) -- skipping it
    // would leave every broadphase query (a raycast, above all) seeing the character's OLD size while
    // its actual collision volume had already changed.
    c->SetInnerBodyShape(res.Get());

    // The supporting-volume plane is also sized to the OLD radius (aver_phys_character_create sets it
    // to -rM at creation, to keep the capsule from catching on its own bottom cap) -- carried forward
    // unchanged here it would reject or accept ground contacts by the wrong capsule's geometry, so it
    // is recomputed for the new one.
    c->SetSupportingVolume(JPH::Plane(toJoltUnit(Vec3(0, 0, 1)), -rM));
    return 1;
}

int32_t aver_phys_character_shape(int32_t ch, float* outRadius, float* outHeight) {
    JPH::CharacterVirtual* c = character(ch);
    if (!c) return 0;
    if (!outRadius || !outHeight) return refuse(AbiError::NullPointer);
    // A capsule is the only shape either creator (aver_phys_character_create, and set_shape above)
    // ever gives a character, which is what makes the downcast safe; the sub-type is still asked
    // rather than assumed, so a third creator with another shape reads as a refusal, not as garbage.
    const JPH::Shape* shape = c->GetShape();
    if (!shape || shape->GetSubType() != JPH::EShapeSubType::Capsule) return refuse(AbiError::Unsupported);
    const JPH::CapsuleShape* capsule = static_cast<const JPH::CapsuleShape*>(shape);
    // The creators' derivation run backwards: the caps go back onto the cylinder's half height.
    *outRadius = mToCm(capsule->GetRadius());
    *outHeight = mToCm((capsule->GetHalfHeightOfCylinder() + capsule->GetRadius()) * 2.0f);
    return 1;
}

// ---- Mass and push strength -----------------------------------------------------------------------

int32_t aver_phys_character_set_mass(int32_t ch, float massKg) {
    JPH::CharacterVirtual* c = character(ch);
    if (!c) return 0;
    if (massKg <= 0.0f) {
        AVER_WARN("[Physics] character mass must be positive; {} kg refused", massKg);
        return refuse(AbiError::InvalidArgument);
    }
    c->SetMass(massKg);
    return 1;
}

int32_t aver_phys_character_mass(int32_t ch, float* outMassKg) {
    JPH::CharacterVirtual* c = character(ch);
    if (!c) return 0;
    if (!outMassKg) return refuse(AbiError::NullPointer);
    *outMassKg = c->GetMass();
    return 1;
}

int32_t aver_phys_character_set_max_strength(int32_t ch, float maxStrengthKgCmS2) {
    JPH::CharacterVirtual* c = character(ch);
    if (!c) return 0;
    // Same conversion as any other force in this ABI (physics_abi.h's force/impulse block): one
    // length dimension, centimetres here and metres (Jolt's Newtons) there.
    c->SetMaxStrength(cmToM(maxStrengthKgCmS2));
    return 1;
}

int32_t aver_phys_character_max_strength(int32_t ch, float* outMaxStrengthKgCmS2) {
    JPH::CharacterVirtual* c = character(ch);
    if (!c) return 0;
    if (!outMaxStrengthKgCmS2) return refuse(AbiError::NullPointer);
    *outMaxStrengthKgCmS2 = mToCm(c->GetMaxStrength());
    return 1;
}

// ---- Entity to character ------------------------------------------------------------------------------

int32_t aver_phys_character_of_entity(int32_t entity) {
    // No match is an ordinary answer, so a live world records Ok.
    if (!g_world) { setAbiError(AbiError::NotInitialised); return 0; }
    setAbiError(AbiError::Ok);
    // 0 is every unstamped character's user data, so asking for it would name one at random.
    if (entity == 0) return 0;
    // A walk of the character table, like handleOfBody above: a world holds a handful of characters.
    // The same truncation aver_phys_raycast reads the stamp back with.
    for (const auto& [h, c] : g_world->characters)
        if (static_cast<int32_t>(c->GetUserData()) == entity) return h;
    return 0;
}

} // extern "C"

// ---- the step loop's half of stair stepping -------------------------------------------------------
//
// See PhysicsInternal.hpp for why this exists: Jolt takes both distances as arguments to
// ExtendedUpdate rather than as state, so the setter above can only record them and this is where they
// are read back. PhysicsWorld.cpp's fixed-step loop calls this once per character per step.
namespace aver::physics::detail {

void applyCharacterStairSettings(int32_t handle, JPH::CharacterVirtual::ExtendedUpdateSettings& out) {
    const auto it = g_stairSettings.find(handle);
    const StairSettings s = it != g_stairSettings.end() ? it->second : StairSettings{};
    // The engine's up, in Jolt's axes, scaled to each distance. toJoltUnit rather than toJolt because
    // the direction is a unit vector and the LENGTH is applied separately -- running (0,0,stepUpCm)
    // through toJolt would convert the centimetres twice.
    const JPH::Vec3 up = toJoltUnit(Vec3(0.0f, 0.0f, 1.0f));
    out.mWalkStairsStepUp     =  up * cmToM(s.stepUpCm);
    out.mStickToFloorStepDown = -up * cmToM(s.stepDownCm);
}

void clearCharacterStairSettings() { g_stairSettings.clear(); }

} // namespace aver::physics::detail
