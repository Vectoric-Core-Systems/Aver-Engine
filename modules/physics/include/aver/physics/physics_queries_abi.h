#pragma once
// Aver.Physics — SPATIAL QUERIES beyond ray, sphere cast and sphere overlap.
//
// What physics_abi.h / physics_layers_abi.h could ask of the world was a ray, a swept sphere and a
// sphere overlap. A box that must fit through a doorway, a capsule that must not clip a wall before the
// character moves, "what is in front of this crate if it slides", every ray hit along a line, and "what
// is at this point" all had to be approximated with spheres or rays, or could not be asked at all.
//
// UNITS AND AXES ARE THE ENGINE'S, as everywhere in this ABI: centimetres, +X forward, +Y right, +Z up,
// left-handed, quaternions (x, y, z, w). Nothing here names a Jolt type.
//
// EVERY QUERY TAKES THE SAME THREE FILTER ARGUMENTS as the _ex forms in physics_layers_abi.h, in the
// same order: `layerMask` (bit N = layer N; AVER_PHYS_LAYER_MASK_ALL for every layer), `ignoreBody` (one
// handle to skip, 0 for none), and a new `flags`. This header does not repeat what those mean beyond
// the one thing that differs:
//
// SENSORS ARE IGNORED UNLESS `flags` SAYS OTHERWISE. A sensor is a trigger volume, not a surface, so a
// bullet, a camera probe or a "what is standing here" test should pass straight through it. The _ex
// queries in physics_layers_abi.h predate this and still see sensors; these do not, unless asked.
//
// SHAPES. A box is its half extents along the engine's X, Y, Z in its own frame, then rotated. A
// capsule is described exactly as in physics_shapes_abi.h: `radius` and TOTAL `height` (both
// hemispherical caps included), upright along the engine's +Z in its own frame, then rotated. A
// capsule too short for its radius (height <= 2*radius) is refused with a warning, as there. The
// rotation is NORMALISED here; an all-zero quaternion means identity.
//
// WHAT "A HIT" MEANS FOR A CAST. The shape is swept by the displacement (direction * distance, in cm)
// and the first body it touches is reported. A shape that STARTS overlapping something reports it at
// distance 0 unless the sweep is moving out of it. Distances are how far the shape's centre travelled.
// `outPoint` is the contact point on the HIT body's surface and `outNormal` the unit surface normal
// pointing back toward the cast shape (away from the hit surface), as aver_phys_raycast reports. Every
// out-pointer may be null.
//
// A cast or overlap whose own shape is a triangle mesh or heightfield is not possible (Jolt cannot
// collide mesh against mesh); aver_phys_body_cast / aver_phys_overlap_body refuse such a body with a
// warning. Meshes and heightfields are fine as the thing being HIT.
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

// `flags` bits. 0 is the default and what almost every caller wants.
#define AVER_PHYS_QUERY_DEFAULT          0
// Sensor bodies take part in the query like any other body.
#define AVER_PHYS_QUERY_INCLUDE_SENSORS  1

// ---- Shape casts: the first hit ------------------------------------------------------------------
// Return the hit body's handle, or 0 for a miss (no world, nothing in the way, a zero-length
// displacement, or an invalid shape). `outDistance` is how far the shape travelled before touching, cm.

// Sweeps a box by (dx, dy, dz) -- a displacement, so direction and distance in one vector.
AVER_PHYS_API int32_t aver_phys_box_cast(float cx, float cy, float cz,
                                         float hx, float hy, float hz,
                                         float qx, float qy, float qz, float qw,
                                         float dx, float dy, float dz,
                                         uint32_t layerMask, int32_t ignoreBody, int32_t flags,
                                         float* outDistance, float* outPoint, float* outNormal);

// Sweeps a capsule (radius, TOTAL height, centre) by (dx, dy, dz).
AVER_PHYS_API int32_t aver_phys_capsule_cast(float cx, float cy, float cz,
                                             float radius, float height,
                                             float qx, float qy, float qz, float qw,
                                             float dx, float dy, float dz,
                                             uint32_t layerMask, int32_t ignoreBody, int32_t flags,
                                             float* outDistance, float* outPoint, float* outNormal);

// Sweeps a sphere of `radius` cm from (cx, cy, cz) by the displacement (dx, dy, dz) cm. Returns the first
// body hit, or 0. Sensors are skipped unless AVER_PHYS_QUERY_INCLUDE_SENSORS. TWO ignore bodies, the one
// place this header departs from a single ignoreBody: a camera skips the target's character AND a vehicle
// chassis. Each may be a body or a character handle. A zero-length displacement or radius <= 0 returns 0.
AVER_PHYS_API int32_t aver_phys_sphere_cast_q(float cx, float cy, float cz,
                                              float dx, float dy, float dz, float radius,
                                              uint32_t layerMask, int32_t ignoreBodyA, int32_t ignoreBodyB, int32_t flags,
                                              float* outDistance, float* outPoint, float* outNormal);

// Sweeps an existing body's own shape from its CURRENT pose by (dx, dy, dz), ignoring that body.
// `ignoreBody` is a second body to skip (a held object, say), or 0. Returns 0 for a dead handle, a
// character handle, a soft body, or a mesh/heightfield body.
AVER_PHYS_API int32_t aver_phys_body_cast(int32_t body,
                                          float dx, float dy, float dz,
                                          uint32_t layerMask, int32_t ignoreBody, int32_t flags,
                                          float* outDistance, float* outPoint, float* outNormal);

// ---- Overlaps ---------------------------------------------------------------------------------------
// Write up to `maxCount` distinct body handles into `outBodies` and return how many were written (a
// body with several overlapping sub-shapes appears once). A return equal to `maxCount` may mean
// truncated. Which bodies survive truncation is unspecified.

AVER_PHYS_API int32_t aver_phys_overlap_box(float cx, float cy, float cz,
                                            float hx, float hy, float hz,
                                            float qx, float qy, float qz, float qw,
                                            uint32_t layerMask, int32_t ignoreBody, int32_t flags,
                                            int32_t* outBodies, int32_t maxCount);

AVER_PHYS_API int32_t aver_phys_overlap_capsule(float cx, float cy, float cz,
                                                float radius, float height,
                                                float qx, float qy, float qz, float qw,
                                                uint32_t layerMask, int32_t ignoreBody, int32_t flags,
                                                int32_t* outBodies, int32_t maxCount);

// An existing body's shape placed at an ARBITRARY pose: "would it fit there?" without moving it. The
// pose is the body's centre of mass, the same quantity aver_phys_body_position / body_rotation report,
// so passing the body's current pose reproduces what it overlaps now. The body itself is never
// reported; `ignoreBody` is a second body to skip. Refusals are as for aver_phys_body_cast.
AVER_PHYS_API int32_t aver_phys_overlap_body(int32_t body,
                                             float x, float y, float z,
                                             float qx, float qy, float qz, float qw,
                                             uint32_t layerMask, int32_t ignoreBody, int32_t flags,
                                             int32_t* outBodies, int32_t maxCount);

// ---- Every hit along a ray -----------------------------------------------------------------------------
// Casts a ray from (ox, oy, oz) along (dx, dy, dz) (any length; normalised here) up to maxDistCm and
// writes up to `maxCount` hits, NEAREST FIRST, returning how many. Hit i goes in outBodies[i],
// outDistances[i] (cm along the ray), outPoints[3*i .. 3*i+2] and outNormals[3*i .. 3*i+2]. Any of the
// four arrays may be null; each non-null one needs room for maxCount entries (3*maxCount floats for
// the two vector arrays). When more than maxCount are hit, the NEAREST maxCount are kept.
//
// ONE ENTRY PER HIT, NOT PER BODY: a ray passing through a body twice, or through several faces of a
// compound or mesh, reports each. A ray starting inside a solid convex body hits it at distance 0.
AVER_PHYS_API int32_t aver_phys_raycast_all(float ox, float oy, float oz,
                                            float dx, float dy, float dz,
                                            float maxDistCm,
                                            uint32_t layerMask, int32_t ignoreBody, int32_t flags,
                                            int32_t maxCount,
                                            int32_t* outBodies, float* outDistances,
                                            float* outPoints, float* outNormals);

// ---- Point query ---------------------------------------------------------------------------------------
// Bodies whose shape CONTAINS the point (up to maxCount handles, each once, returned in outBodies;
// returns the count). Triangle meshes and heightfields have no inside and never match.
AVER_PHYS_API int32_t aver_phys_point_query(float x, float y, float z,
                                            uint32_t layerMask, int32_t ignoreBody, int32_t flags,
                                            int32_t* outBodies, int32_t maxCount);

#ifdef __cplusplus
}
#endif
