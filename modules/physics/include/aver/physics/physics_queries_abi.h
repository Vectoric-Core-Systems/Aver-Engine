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

// ---- space-state queries (additive) ---------------------------------------------------------------------------
//
// Space-state queries and body test motion, as explicit options. NOTHING ABOVE CHANGES: the
// queries earlier in this header, and aver_phys_raycast / _ex / _mask, keep their exact behaviour. No
// DEFAULTS (hit back faces on, hit from inside off, collide with bodies on, areas off, margin 0.001 m) are
// built in anywhere: the caller passes each option, so a flags value of 0 means "no bodies, no areas" and hits
// nothing. An importer or the caller chooses the values; docs/physics/SPACE_QUERIES.md lists the usual ones.
//
// COMMON ARGUMENTS. `collisionMask` is a 32-bit mask: a body is a candidate when (mask & its layer bits) != 0,
// where a body without a layer filter has the single bit of its 16-layer user layer (see physics_layers_abi.h).
// `flags` are AVER_PHYS_SQ_*. `excludeBodies`/`excludeCount` is an exclude array: body handles (or character
// handles) the query skips; null or a count <= 0 means none; a handle that does not resolve is skipped quietly.
// A sensor is an area. Units and axes are the engine's. `pose7` is x, y, z, qx, qy, qz, qw: the shape's
// ORIGIN (as aver_phys_body_from_shape places it, not its centre of mass); the quaternion is normalised here.
// Shapes are physics_shape_builder_abi.h handles; a mesh, heightfield or plane cannot be the QUERY shape (refused,
// error recorded) though any of them can be hit. Every out-pointer may be null.
//
// A body-type query in this group never moves anything and takes no lock beyond the reads it needs.

#define AVER_PHYS_SQ_NONE                  0   // collides with nothing
#define AVER_PHYS_SQ_BODIES                1   // non-sensor bodies and characters count
#define AVER_PHYS_SQ_AREAS                 2   // sensors count
#define AVER_PHYS_SQ_BACK_FACES            4   // ray: report the back side of triangles and the exit of a convex body
#define AVER_PHYS_SQ_FROM_INSIDE           8   // ray: a ray starting inside a convex body hits it at distance 0, normal 0
#define AVER_PHYS_SQ_RECOVERY_AS_COLLISION 16  // test motion: depenetration counts as a collision

// A ray from origin3 along direction3 (any length, normalised here) up to maxDistCm. Returns the nearest body hit
// or 0. outDistance is cm along the ray; outNormal the surface's outward normal at the hit (zero for a hit from
// inside); outEntity the body's entity stamp; outSubShape the Jolt sub-shape id (same number as aver_phys_contact_detail).
// Without AVER_PHYS_SQ_BACK_FACES a ray that starts inside a convex body and leaves it reports nothing; with it, it
// reports the exit face. Triangle meshes and heightfields have no inside: FROM_INSIDE does not affect them.
AVER_PHYS_API int32_t aver_phys_space_raycast(const float* origin3, const float* direction3, float maxDistCm,
                                              uint32_t collisionMask, int32_t flags,
                                              const int32_t* excludeBodies, int32_t excludeCount,
                                              float* outDistance, float* outPoint, float* outNormal,
                                              int32_t* outEntity, int32_t* outSubShape);

// Intersect shape: every (body, sub-shape) the shape overlaps, deepest first (ties by handle, then sub-shape),
// up to maxResults. `marginCm` inflates the shape: colliders closer than that count. outBodies[i] and outSubShapes[i]
// (both optional) receive each hit. Returns the count.
AVER_PHYS_API int32_t aver_phys_space_intersect_shape(int32_t shape, const float* pose7, float marginCm,
                                                      uint32_t collisionMask, int32_t flags,
                                                      const int32_t* excludeBodies, int32_t excludeCount,
                                                      int32_t maxResults,
                                                      int32_t* outBodies, int32_t* outSubShapes);

// Collide shape: the contact of every overlapping (body, sub-shape), deepest first, up to maxContacts. Per
// contact i: outBodies[i], outPointsOnShape[3i..] (on the query shape), outPointsOnOther[3i..] (on the collider),
// outNormals[3i..] (unit, away from the collider toward the shape), outDepths[i] (cm, signed: negative when the two
// are separated by less than marginCm). Returns the count.
AVER_PHYS_API int32_t aver_phys_space_collide_shape(int32_t shape, const float* pose7, float marginCm,
                                                    uint32_t collisionMask, int32_t flags,
                                                    const int32_t* excludeBodies, int32_t excludeCount,
                                                    int32_t maxContacts,
                                                    int32_t* outBodies, float* outPointsOnShape, float* outPointsOnOther,
                                                    float* outNormals, float* outDepths);

// Rest info: the single deepest contact. Returns the collider's handle, or 0 for none. outPoint is on the
// collider, outNormal away from it, outVelocity (cm/s) the collider's velocity at that point (zero for a sensor or a
// static body), outDepth cm as above.
AVER_PHYS_API int32_t aver_phys_space_rest_info(int32_t shape, const float* pose7, float marginCm,
                                                uint32_t collisionMask, int32_t flags,
                                                const int32_t* excludeBodies, int32_t excludeCount,
                                                float* outPoint, float* outNormal, float* outVelocity, float* outDepth,
                                                int32_t* outEntity, int32_t* outSubShape);

// Cast motion: how far the shape can move along motion3 (a displacement, cm). Returns 1 when the query ran, 0
// when it was refused (bad shape or arguments). outUnsafe is the fraction (0..1) at which the shape first touches
// something; outSafe the fraction at which it is still `marginCm` away (backed off by margin / cos of the contact
// angle, clamped to [0, unsafe]); both 1 and outBody 0 when nothing is hit. A collider the shape ALREADY
// overlaps at the start (deeper than 0.05 cm) is ignored for a hit at fraction 0; use collide_shape for those.
// outBody, outPoint, outNormal describe the first contact (point on the collider, normal away from it).
AVER_PHYS_API int32_t aver_phys_space_cast_motion(int32_t shape, const float* pose7, const float* motion3, float marginCm,
                                                  uint32_t collisionMask, int32_t flags,
                                                  const int32_t* excludeBodies, int32_t excludeCount,
                                                  float* outSafe, float* outUnsafe,
                                                  int32_t* outBody, float* outPoint, float* outNormal);

// ---- body_test_motion ----------------------------------------------------------------------------------------
//
// Body test motion, the base of move_and_collide, test_move and move_and_slide: where would this body get to
// if it moved by motion3? The body is NOT moved; the caller applies outF travel. Steps: (1) RECOVERY, push the
// body's shape out of whatever it overlaps (up to 4 passes, to marginCm/2 away from each surface, so a body that
// rests in contact is nudged off by half the margin); (2) a cast of the shape along motion3 from the recovered pose;
// (3) the contacts at the point of contact. The pair filter is the body's own: its object layer (the 16-layer matrix),
// its 32-bit layer/mask, collision exceptions and joint exclusion, exactly as the simulation applies them; plus
// `excludeBodies`. The body itself is never a candidate. A static body may be tested. Refused (-1, error recorded)
// for a dead handle, a character, a soft body, or a mesh/heightfield body.
//
// `originPose7` is where to test it (the body's ORIGIN, x y z qx qy qz qw), or null for its current pose.
// `flags`: SQ_BODIES / SQ_AREAS choose what blocks; SQ_RECOVERY_AS_COLLISION reports a depenetration as a collision.
// `maxContacts` is how many contacts to report (at least one is computed when there is a collision).
//
// RETURN: 1 collided, 0 clear, -1 refused. Collided means the shape stops short of the full motion (safe < 1) or
// recovery was asked to count and had something to push out of.
//
// outF (AVER_PHYS_TM_F_COUNT floats; engine cm, cm/s, rad/s): TRAVEL = recovery + motion * safe, which is what the
// body should be moved by; REMAINDER = motion * (1 - safe); the primary contact's POINT (on the collider), NORMAL
// (away from the collider), VELOCITY and ANGULAR_VELOCITY of the collider at that point, DEPTH (the margin-inflated
// shape's overlap with it, >= 0); SAFE and UNSAFE fractions as in cast_motion; RECOVERY alone.
// outI (AVER_PHYS_TM_I_COUNT ints): the primary COLLIDER's handle (0 for none), its ENTITY stamp, the SUBSHAPE hit
// and the CONTACTS count written to the contact arrays.
// outContactF (maxContacts * AVER_PHYS_TC_F_COUNT floats) and outContactI (maxContacts * AVER_PHYS_TC_I_COUNT ints):
// the contacts, primary first, then deepest first.
#define AVER_PHYS_TM_F_TRAVEL            0   // 3
#define AVER_PHYS_TM_F_REMAINDER         3   // 3
#define AVER_PHYS_TM_F_POINT             6   // 3
#define AVER_PHYS_TM_F_NORMAL            9   // 3
#define AVER_PHYS_TM_F_VELOCITY          12  // 3
#define AVER_PHYS_TM_F_ANGULAR_VELOCITY  15  // 3
#define AVER_PHYS_TM_F_DEPTH             18
#define AVER_PHYS_TM_F_SAFE              19
#define AVER_PHYS_TM_F_UNSAFE            20
#define AVER_PHYS_TM_F_RECOVERY          21  // 3
#define AVER_PHYS_TM_F_COUNT             24

#define AVER_PHYS_TM_I_COLLIDER          0
#define AVER_PHYS_TM_I_ENTITY            1
#define AVER_PHYS_TM_I_SUBSHAPE          2
#define AVER_PHYS_TM_I_CONTACTS          3
#define AVER_PHYS_TM_I_COUNT             4

#define AVER_PHYS_TC_F_POINT             0   // 3
#define AVER_PHYS_TC_F_NORMAL            3   // 3
#define AVER_PHYS_TC_F_VELOCITY          6   // 3
#define AVER_PHYS_TC_F_ANGULAR_VELOCITY  9   // 3
#define AVER_PHYS_TC_F_DEPTH             12
#define AVER_PHYS_TC_F_COUNT             13

#define AVER_PHYS_TC_I_BODY              0
#define AVER_PHYS_TC_I_ENTITY            1
#define AVER_PHYS_TC_I_SUBSHAPE          2
#define AVER_PHYS_TC_I_COUNT             3

AVER_PHYS_API int32_t aver_phys_body_test_motion(int32_t body, const float* originPose7, const float* motion3,
                                                 float marginCm, int32_t flags,
                                                 const int32_t* excludeBodies, int32_t excludeCount,
                                                 int32_t maxContacts,
                                                 float* outF, int32_t* outI,
                                                 float* outContactF, int32_t* outContactI);

#ifdef __cplusplus
}
#endif
