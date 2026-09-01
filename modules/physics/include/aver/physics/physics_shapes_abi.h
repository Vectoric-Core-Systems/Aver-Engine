#pragma once
// Aver.Physics — RIGID-BODY SHAPES beyond box, sphere, convex hull, mesh and heightfield.
//
// Jolt compiles capsules, cylinders, tapered capsules and compounds into this engine's binary
// already -- Collision/Shape/*.obj is in every build -- and none of them had an entry point, so a
// barrel, a pill bottle, a limb bone and a chair welded from boxes were all equally unbuildable
// without going around this module and touching Jolt directly. This file is that entry point.
//
// A SEPARATE HEADER FROM physics_abi.h, for the same reason physics_joints_abi.h is one: the ABI
// parity test pairs one header with one C# file by path, so a shapes header and a Shapes.cs can be
// checked against each other as a unit. Including this one alone is fine -- it needs nothing from
// physics_abi.h but the handle convention, which it restates below.
//
// UNITS AND AXES ARE THE ENGINE'S, exactly as in physics_abi.h: centimetres, +X forward, +Y right,
// +Z up, left-handed, and no Jolt type crosses this boundary.
//
// CAPSULE AND TAPERED CAPSULE HEIGHT IS TOTAL, not the half-height of the cylindrical middle that
// Jolt's own constructors take. This matches aver_phys_character_create, which already made that
// choice for the one capsule this ABI could build before now -- a second convention for the same
// shape would be the kind of inconsistency a caller discovers by getting a body twice as tall as
// asked. A capsule's cylinder half-height is therefore `height/2 - radius`; a tapered capsule's is
// `(height - topRadius - bottomRadius)/2`, because each cap's own radius extends beyond the
// cylindrical middle at its end. Both are refused, the way aver_phys_character_create refuses a
// character too short for its radius, when the derived half-height is not positive.
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

// ---- Capsule -----------------------------------------------------------------------------------
// A cylinder with a hemispherical cap on each end, upright along the engine's +Z: a barrel, a
// pill, a limb segment that does not need to taper.

// A capsule that never moves. `height` is TOTAL, both caps included -- see the file comment.
AVER_PHYS_API int32_t aver_phys_add_static_capsule(float cx, float cy, float cz,
                                                   float radius, float height);

// A capsule that falls and collides. `massKg` <= 0 asks Jolt to derive mass from the shape's volume.
AVER_PHYS_API int32_t aver_phys_add_dynamic_capsule(float cx, float cy, float cz,
                                                    float radius, float height, float massKg);

// ---- Cylinder ------------------------------------------------------------------------------------
// Flat-ended, upright along the engine's +Z: a drum, a wheel lying on its side, a pipe segment.
// `height` is the FULL height end to end -- a cylinder has no caps of its own radius to add, so
// unlike the capsule above there is only one sensible reading of the word.

AVER_PHYS_API int32_t aver_phys_add_static_cylinder(float cx, float cy, float cz,
                                                    float radius, float height);
AVER_PHYS_API int32_t aver_phys_add_dynamic_cylinder(float cx, float cy, float cz,
                                                     float radius, float height, float massKg);

// ---- Tapered capsule -----------------------------------------------------------------------------
// A capsule whose two caps have DIFFERENT radii: a cone-ish limb -- a forearm thicker at the elbow
// than the wrist, a tapered leg. `topRadius` is the cap at +Z, `bottomRadius` the cap at -Z.
// `height` is TOTAL, both caps included -- see the file comment.

AVER_PHYS_API int32_t aver_phys_add_static_tapered_capsule(float cx, float cy, float cz,
                                                           float topRadius, float bottomRadius,
                                                           float height);
AVER_PHYS_API int32_t aver_phys_add_dynamic_tapered_capsule(float cx, float cy, float cz,
                                                            float topRadius, float bottomRadius,
                                                            float height, float massKg);

// ---- Compound of boxes ---------------------------------------------------------------------------
// N boxes welded into ONE rigid body: a chair is a seat, a back and four legs, a table is a top and
// four legs, and until now each would have had to be its own body with its own joints holding it
// together -- rigid where the real object has no give at all, and N times the broadphase and solver
// cost of the single body it actually is.
//
// `offsetsCm` and `halfExtentsCm` are `count` boxes' worth, xyz,xyz,... each, in the BODY'S OWN
// LOCAL FRAME: `offsetsCm[i]` is that box's centre relative to (cx,cy,cz), not a world position.
// Every box shares the compound's one rotation and motion type; there is no per-box orientation --
// add it if a caller ever needs a box that is not axis-aligned with its neighbours.

AVER_PHYS_API int32_t aver_phys_add_static_compound_boxes(float cx, float cy, float cz,
                                                           const float* offsetsCm,
                                                           const float* halfExtentsCm,
                                                           int32_t count);
AVER_PHYS_API int32_t aver_phys_add_dynamic_compound_boxes(float cx, float cy, float cz,
                                                            const float* offsetsCm,
                                                            const float* halfExtentsCm,
                                                            int32_t count, float massKg);

#ifdef __cplusplus
}
#endif
