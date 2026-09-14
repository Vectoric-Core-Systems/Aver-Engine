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

#ifdef __cplusplus
}
#endif
