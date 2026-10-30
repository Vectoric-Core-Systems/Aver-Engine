#pragma once
// Aver.Physics — COLLISION LAYERS, and the queries that can filter by them.
//
// WHAT WAS MISSING. This module has always had exactly two object layers, NON_MOVING and MOVING, and
// neither was reachable: a caller could not say "the player's own bullets pass through the player",
// "the camera probe ignores foliage", "enemies do not collide with each other", or "this trigger only
// notices the player". Every body collided with every other body it could reach, and the only way to
// express an exception was to not create the body.
//
// HOW A LAYER RELATES TO THE MOVING/STATIC SPLIT, because they are different questions and conflating
// them is the usual way this goes wrong. Whether a body MOVES decides which broad-phase tree it lives
// in, which is a performance structure Jolt owns and a caller has no business choosing. Which LAYER it
// is on decides who it collides with, which is entirely the game's business. Both are packed into
// Jolt's one object-layer number internally; from out here they are independent, and setting a body's
// layer never changes whether it is static.
//
// LAYER 0 IS THE DEFAULT AND COLLIDES WITH EVERYTHING. A project that never calls anything in this
// header behaves exactly as it did before the header existed -- not approximately, identically: a
// body on layer 0 lands on the same internal object layer it always did.
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

// How many collision layers exist, numbered 0 .. AVER_PHYS_LAYER_COUNT-1. A caller naming a layer
// outside that range is warned and falls back to 0 rather than being silently dropped into a layer
// that collides with nothing.
#define AVER_PHYS_LAYER_COUNT 16

// ---- The collision matrix ---------------------------------------------------------------------------

// Whether two layers collide. SYMMETRIC: setting (a, b) also sets (b, a), because Jolt may ask about a
// pair in either order and a matrix that disagreed with itself would make collision depend on which
// body the broad phase happened to reach first. A layer may be set against itself, which is how
// "enemies ignore each other" is spelled.
//
// Every pair starts enabled, so this is a subtractive API: you say what does NOT collide.
AVER_PHYS_API int32_t aver_phys_set_layer_collision(int32_t layerA, int32_t layerB, int32_t enabled);
AVER_PHYS_API int32_t aver_phys_layer_collision(int32_t layerA, int32_t layerB);

// Puts every pair back to colliding. What a level teardown wants, since the matrix belongs to the
// world and a rule set up for one level must not leak into the next.
AVER_PHYS_API void aver_phys_reset_layer_collisions(void);

// ---- A body's layer ------------------------------------------------------------------------------------

// Move a body onto a layer. Returns 0 for a dead handle or an out-of-range layer.
//
// KEEPS THE BODY'S MOVING/STATIC HALF, whatever it is: a static floor moved to layer 3 is still
// static, and a dynamic crate moved to layer 3 is still dynamic. Changing a body's layer is a
// statement about who it collides with and nothing else.
AVER_PHYS_API int32_t aver_phys_body_set_layer(int32_t body, int32_t layer);
// A body's layer, or -1 for a dead handle. -1 rather than 0, because 0 is a real layer.
AVER_PHYS_API int32_t aver_phys_body_layer(int32_t body);

// ---- Filtered queries ------------------------------------------------------------------------------
//
// SEPARATE FUNCTIONS RATHER THAN NEW ARGUMENTS ON THE EXISTING ONES. aver_phys_raycast,
// aver_phys_overlap_sphere and aver_phys_sphere_cast keep their exact signatures forever -- they are
// bound in C#, called from graph nodes and used across the editor, and widening them would break every
// one of those call sites to add a parameter most of them would pass a constant to.
//
// `layerMask` is a BITMASK of layers, not a layer number: bit N means layer N is included. Pass
// 0xFFFFFFFF for "all layers", which makes the _ex call behave exactly like the plain one.
// `ignoreBody` is a single body handle to skip, or 0 to skip nothing -- almost always the body doing
// the casting, because a ray fired from inside your own capsule otherwise hits yourself at distance 0
// and that is the single most common bug in a first-person weapon.

// Every layer. Passing this makes the filtered call identical to the unfiltered one.
#define AVER_PHYS_LAYER_MASK_ALL 0xFFFFFFFFu

// The bit for one layer, for building a mask: AVER_PHYS_LAYER_BIT(2) | AVER_PHYS_LAYER_BIT(5).
#define AVER_PHYS_LAYER_BIT(n) (1u << (n))

// As aver_phys_raycast, but only hitting bodies on a layer in `layerMask`, and never `ignoreBody`.
AVER_PHYS_API int32_t aver_phys_raycast_ex(float ox, float oy, float oz,
                                           float dx, float dy, float dz,
                                           float maxDistCm, uint32_t layerMask, int32_t ignoreBody,
                                           float* outPoint, float* outNormal, int32_t* outEntity);

// As aver_phys_overlap_sphere, filtered the same way.
AVER_PHYS_API int32_t aver_phys_overlap_sphere_ex(float x, float y, float z, float radius,
                                                  uint32_t layerMask, int32_t ignoreBody,
                                                  int32_t* outBodies, int32_t maxBodies);

// As aver_phys_sphere_cast, filtered the same way.
AVER_PHYS_API int32_t aver_phys_sphere_cast_ex(float ox, float oy, float oz,
                                               float dx, float dy, float dz,
                                               float maxDistCm, float radius,
                                               uint32_t layerMask, int32_t ignoreBody,
                                               float* outPoint, float* outNormal);

// ---- 32-bit layer and mask (additive) ------------------------------------------------------------------
//
// AN OPT-IN ALTERNATIVE TO THE 16-LAYER MATRIX ABOVE, for content that arrives with a layer/mask rule. A body
// that never calls anything in this group keeps today's filtering exactly.
//
// The rule: A and B collide when (A.mask & B.layer) != 0 OR (B.mask & A.layer) != 0 -- one side wanting
// the other is enough, which is NOT the matrix's symmetric yes/no. Bit N is layer N+1 in the usual naming.
//
// HOW THE TWO COEXIST. The object-layer matrix is still consulted first, so both must allow a pair. The
// mask rule applies only when BOTH bodies carry a layer filter; a pair with an unfiltered body is decided
// by the matrix alone. (Default matrix = everything collides, so for a filtered pair the mask decides.)
// Static-vs-static never collides, whatever the masks say. Filtering lives in the body's Jolt collision
// group, so it is applied in the broad phase and survives a body being rebuilt (static -> movable).
// A body that already has another collision group (a ragdoll part, a soft body) cannot take a filter and
// the setters return 0. Characters (CharacterVirtual) are not filtered by it: they keep the matrix.
// Changing a filter wakes the body, so something resting on a body that stops colliding falls.
//
// Not captured by physics snapshots (physics_state_abi.h), like the layer matrix: configuration, not state.
// Setters are refused (return 0) while aver_phys_step is running.

// Gives a body a 32-bit layer and mask. Both are bitmasks; 0 layer = nobody's mask hits it, 0 mask = it wants
// nobody (others' masks can still hit it). Returns 0 for a dead handle or a body that cannot take a filter.
AVER_PHYS_API int32_t aver_phys_body_set_collision_filter(int32_t body, uint32_t layerBits, uint32_t maskBits);
// 1 and the bits when the body carries a layer filter; 0 (outputs untouched) when it follows the matrix only.
AVER_PHYS_API int32_t aver_phys_body_collision_filter(int32_t body, uint32_t* outLayerBits, uint32_t* outMaskBits);
// Back to the matrix alone. Exceptions on the body stay.
AVER_PHYS_API int32_t aver_phys_body_clear_collision_filter(int32_t body);

// Two bodies that never collide (a collision exception). Symmetric, works with or without a
// layer/mask on either body, and stays until removed or one body is. Adding twice is one exception.
// Applies to rigid-body simulation, not to queries or CharacterVirtual movement.
AVER_PHYS_API int32_t aver_phys_body_add_collision_exception(int32_t bodyA, int32_t bodyB);
// 1 if an exception was removed, 0 if there was none (or a dead handle). A joint's own exclusion is separate.
AVER_PHYS_API int32_t aver_phys_body_remove_collision_exception(int32_t bodyA, int32_t bodyB);
AVER_PHYS_API int32_t aver_phys_body_has_collision_exception(int32_t bodyA, int32_t bodyB);
// Removes every exception this body has.
AVER_PHYS_API int32_t aver_phys_body_clear_collision_exceptions(int32_t body);

// Whether the simulation would let these two collide, from filtering alone: static-vs-static, the
// matrix, then the mask rule, exceptions and joint exclusion. 0 for a dead handle or the same body
// twice. Says nothing about geometry or motion type (a static and a kinematic body still never collide).
AVER_PHYS_API int32_t aver_phys_bodies_can_collide(int32_t bodyA, int32_t bodyB);

// Queries with a 32-bit mask: as aver_phys_raycast_ex / _overlap_sphere_ex / _sphere_cast_ex, but a
// body is hit when (collisionMask & its layer bits) != 0. A body without a layer filter has the single
// layer bit of its 16-layer user layer (layer 0 = bit 0), so a mask can still address it. Exceptions do not
// apply to queries. Sensors are not skipped, as in the _ex forms.
AVER_PHYS_API int32_t aver_phys_raycast_mask(float ox, float oy, float oz,
                                             float dx, float dy, float dz,
                                             float maxDistCm, uint32_t collisionMask, int32_t ignoreBody,
                                             float* outPoint, float* outNormal, int32_t* outEntity);
AVER_PHYS_API int32_t aver_phys_overlap_sphere_mask(float x, float y, float z, float radius,
                                                    uint32_t collisionMask, int32_t ignoreBody,
                                                    int32_t* outBodies, int32_t maxBodies);
AVER_PHYS_API int32_t aver_phys_sphere_cast_mask(float ox, float oy, float oz,
                                                 float dx, float dy, float dz,
                                                 float maxDistCm, float radius,
                                                 uint32_t collisionMask, int32_t ignoreBody,
                                                 float* outPoint, float* outNormal);

#ifdef __cplusplus
}
#endif
