#pragma once
// Aver.Physics — CHARACTER, the rest of it. physics_abi.h's own Character section only reaches
// create/destroy/velocity/position/grounded: a JPH::CharacterVirtual built from nothing but a radius
// and a height, with every other setting Jolt offers left at its default and unreachable. This header
// is that gap, in a file of its own because physics_abi.h is not this change's to edit.
//
// UNITS AND AXES ARE THE ENGINE'S, exactly as in physics_abi.h: centimetres, +X forward, +Y right,
// +Z up, left-handed, and no Jolt type crosses this boundary. Angles are RADIANS. 0 is always an
// invalid handle; a setter returns 1/0; a getter with a null out-pointer returns 0 rather than
// dereferencing it.
#include <stdint.h>

#if defined(_WIN32)
#  if defined(AVER_PHYS_BUILD)
#    define AVER_PHYS_API __declspec(dllexport)
#  else
#    define AVER_PHYS_API __declspec(dllimport)
#  endif
#else
#  define AVER_PHYS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- Max slope angle -------------------------------------------------------------------------------
// THE ONE THAT MATTERS MOST. aver_phys_character_create hands Jolt a radius and a height and nothing
// else, so every character in this engine has been climbing (or refusing to climb) whatever Jolt's own
// 50-degree default says, with no way for a game to make a ramp steeper or shallower than that. Without
// this, a level's ramps are climbable or not by an accident of a constant this ABI never exposed.

// Set the maximum angle of slope a character can still walk up, in radians. Steeper ground is reported
// through aver_phys_character_ground_state as OnSteepGround rather than OnGround.
AVER_PHYS_API int32_t aver_phys_character_set_max_slope_angle(int32_t ch, float radians);
// The current max slope angle, in radians. Returns 0 for a dead handle or a null out-pointer.
AVER_PHYS_API int32_t aver_phys_character_max_slope_angle(int32_t ch, float* outRadians);

// ---- Stair stepping ---------------------------------------------------------------------------------
// A step up (climbing a stair) and a step down (staying glued to the floor going down one, or off a
// small ledge) are both a per-update PARAMETER to Jolt's ExtendedUpdate, not a property stored on the
// character -- so unlike everything else in this file, there is nothing on CharacterVirtual itself for
// these to read or write. They are recorded here for whatever drives the character's update loop to
// consult; the step loop that already calls ExtendedUpdate is outside this change's two files.

// `stepUpCm` is the tallest stair a WalkStairs pass may climb; `stepDownCm` is how far a StickToFloor
// pass may pull the character back down onto ground it lost contact with. Both in centimetres.
AVER_PHYS_API int32_t aver_phys_character_set_stair_stepping(int32_t ch, float stepUpCm,
                                                              float stepDownCm);
// Reads the current pair back. Un-set, a character reports Jolt's own ExtendedUpdateSettings defaults
// (40cm up, 50cm down) mirrored into centimetres, not zero.
AVER_PHYS_API int32_t aver_phys_character_stair_stepping(int32_t ch, float* outStepUpCm,
                                                          float* outStepDownCm);

// ---- Ground state -------------------------------------------------------------------------------
// aver_phys_character_grounded (physics_abi.h) collapses Jolt's four-way answer to a single boolean.
// This is the four-way answer itself, IN JOLT'S OWN DECLARED ORDER (see CharacterBase::EGroundState) so
// this enum and Jolt's cannot drift apart silently the way AVER_PHYS_MOTION_* already guards against for
// motion types.
#define AVER_PHYS_GROUND_ON_GROUND       0   // walking freely
#define AVER_PHYS_GROUND_ON_STEEP_GROUND 1   // touching ground too steep to climb; slides if not held
#define AVER_PHYS_GROUND_NOT_SUPPORTED   2   // touching something, but not standing on it -- should fall
#define AVER_PHYS_GROUND_IN_AIR          3   // touching nothing

// One of the AVER_PHYS_GROUND_* values above, or -1 for a dead handle -- not 0, because 0 is OnGround, a
// real answer, the same reason aver_phys_body_motion_type returns -1 rather than 0.
AVER_PHYS_API int32_t aver_phys_character_ground_state(int32_t ch);

// ---- What the character is standing on ---------------------------------------------------------
// Every one of these is only meaningful while ground_state is OnGround or OnSteepGround; Jolt still
// answers something for NotSupported/InAir (typically whatever it last touched), so check ground_state
// first rather than trusting these alone to say whether the character is actually supported.

// The ground's contact normal, a unit vector. Writes into a caller-owned float[3]; 0 for a dead handle
// or a null pointer.
AVER_PHYS_API int32_t aver_phys_character_ground_normal(int32_t ch, float* outXyz);
// The ground's contact position, engine centimetres.
AVER_PHYS_API int32_t aver_phys_character_ground_position(int32_t ch, float* outXyz);
// The handle of the body being stood on -- a body OR another character's handle, resolved the same way
// aver_phys_raycast resolves a hit -- or 0 for standing on nothing this module gave a handle to (a
// landscape heightfield, say) as well as for genuinely standing on nothing at all. Not one AND the
// other: check ground_state to tell "no owner" from "no ground".
AVER_PHYS_API int32_t aver_phys_character_ground_body(int32_t ch);
// The ground's own velocity, cm/s, world space -- THIS IS WHAT A MOVING PLATFORM IS. Add it to the
// character's desired horizontal velocity before the next update and the character rides the platform;
// leave it out and standing on a lift means being left behind as it rises. Jolt derives this from the
// platform's angular velocity as well as its linear one (a point on a spinning disc is not just "linear
// velocity of the disc"), which is why this is a query rather than something a caller could as easily
// compute from the platform body's own transform.
AVER_PHYS_API int32_t aver_phys_character_ground_velocity(int32_t ch, float* outXyz);

// ---- Shape (crouching) ------------------------------------------------------------------------------

// Swaps the character's capsule for a new radius/height, in centimetres -- crouching is a smaller
// capsule, standing back up is the same call with the original numbers. `maxPenetrationCm` is how much
// the NEW shape may overlap the world and still be accepted.
//
// THIS CAN FAIL, AND THAT IS THE WHOLE POINT: Jolt checks the new shape against the world before
// committing, and growing the capsule under a low ceiling would embed it in solid geometry -- refusing
// that is what makes "stand up" a real question with a real answer rather than a character clipping
// through whatever it stood up into. Returns 0 on that failure (the character keeps its old shape), as
// well as for a dead handle or a `height` too short for `radius` to form a capsule at all -- exactly
// aver_phys_character_create's own refusal for the same reason.
AVER_PHYS_API int32_t aver_phys_character_set_shape(int32_t ch, float radius, float height,
                                                     float maxPenetrationCm);

// ---- Mass and push strength -------------------------------------------------------------------------
// A CharacterVirtual is not a rigid body -- nothing ever applies a force TO it -- but it still has an
// opinion about the world: how hard IT can push, and how much of the world's weight it can be asked to
// carry. Both matter only when the character contacts a dynamic body; neither affects how the character
// itself moves.

// The character's mass in kilograms. Used to weigh down whatever the character is standing on top of;
// Jolt's own default is 70.
AVER_PHYS_API int32_t aver_phys_character_set_mass(int32_t ch, float massKg);
AVER_PHYS_API int32_t aver_phys_character_mass(int32_t ch, float* outMassKg);

// The maximum force the character can push other bodies with, kg*cm/s^2 -- the same force unit
// physics_abi.h's body forces use. A motor character shoving a crate stalls once the crate needs more
// than this to move; left at Jolt's default (equivalent to 100 Newtons) a character can shove anything.
AVER_PHYS_API int32_t aver_phys_character_set_max_strength(int32_t ch, float maxStrengthKgCmS2);
AVER_PHYS_API int32_t aver_phys_character_max_strength(int32_t ch, float* outMaxStrengthKgCmS2);

#ifdef __cplusplus
}
#endif
