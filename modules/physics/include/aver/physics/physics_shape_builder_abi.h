#pragma once
// Aver.Physics -- GENERIC SHAPE HANDLES: every collision shape Jolt has, built, combined and put on bodies.
//
// One handle table for everything. A "shape handle" used to mean a static triangle mesh (physics_abi.h:
// aver_phys_create_mesh_shape); it now names any shape made here, and aver_phys_release_mesh_shape and
// aver_phys_add_mesh_shape_body accept any of them. A shape handle is NOT a body handle (the two tables
// are separate by design, as the mesh table always was): hand a shape to a shape function and a body
// to a body function. Builders and release are safe from any thread (level streaming builds on workers);
// body functions are main thread only. A shape stays alive while a body uses it, so releasing right
// after aver_phys_body_from_shape is correct.
//
// UNITS AND AXES ARE THE ENGINE'S: centimetres, +X forward, +Y right, +Z up, left-handed, quaternions
// (x, y, z, w). A NULL quaternion is identity, a NULL position is zero. Half extents and radii are sizes.
// A capsule's height is TOTAL (both caps), a cylinder's and tapered cylinder's is FULL end to end; all
// stand along +Z with topRadius at +Z.
//
// Failures return 0 (or -1 where 0 is a real answer) and set aver_phys_last_error.
//
// PARAMETER BLOCKS (`const float* p, int32_t n`): any prefix of the block, a longer one is refused, unset
// entries keep the default, and a negative value means "the shape's own default" (EDGE_ANGLE_DEG alone
// reads negative as "every edge active").
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

#define AVER_PHYS_SHAPE_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_shape_abi_version(void);

/* ---- shape parameter block (SP) ---- */
#define AVER_PHYS_SP_CONVEX_RADIUS    0   /* cm, default 5 (triangle 0); clamped by Jolt to the smallest extent */
#define AVER_PHYS_SP_DENSITY          1   /* kg/m^3, default 1000 */
#define AVER_PHYS_SP_HULL_TOLERANCE   2   /* cm, default 0.1 */
#define AVER_PHYS_SP_EDGE_ANGLE_DEG   3   /* mesh active-edge angle, default 5; negative = every edge active */
#define AVER_PHYS_SP_BUILD_QUALITY    4   /* mesh: 0 favour runtime performance, 1 favour build speed */
#define AVER_PHYS_SP_MAX_TRIS_PER_LEAF 5  /* mesh: 1..8, default 8 */
#define AVER_PHYS_SP_COUNT            6

/* ---- leaves ----
   `surface` is a physical-surface id (0 = none), see physics_surface_abi.h. */
AVER_PHYS_API int32_t aver_phys_shape_box(float hx, float hy, float hz, int32_t surface, const float* p, int32_t n);
AVER_PHYS_API int32_t aver_phys_shape_sphere(float radius, int32_t surface, const float* p, int32_t n);
AVER_PHYS_API int32_t aver_phys_shape_capsule(float radius, float height, int32_t surface, const float* p, int32_t n);
AVER_PHYS_API int32_t aver_phys_shape_cylinder(float radius, float height, int32_t surface, const float* p, int32_t n);
AVER_PHYS_API int32_t aver_phys_shape_tapered_capsule(float topRadius, float bottomRadius, float height, int32_t surface, const float* p, int32_t n);
AVER_PHYS_API int32_t aver_phys_shape_tapered_cylinder(float topRadius, float bottomRadius, float height, int32_t surface, const float* p, int32_t n);
/* A thin convex triangle (not a mesh): 9 floats, convex radius 0 unless SP says otherwise. One-sided, as in Jolt:
   bodies touch the face (B - A) x (C - A) points to (the winding aver_phys_shape_mesh takes) and pass through the back. */
AVER_PHYS_API int32_t aver_phys_shape_triangle(const float* abcXyz, int32_t surface, const float* p, int32_t n);
/* The points with n . x = offsetCm form the plane; the side opposite the normal is solid. halfExtentCm
   (1 to 1e6) bounds its broadphase box. Static or kinematic bodies only. */
AVER_PHYS_API int32_t aver_phys_shape_plane(const float* normalXyz, float offsetCm, float halfExtentCm, int32_t surface);
/* No volume, collides with nothing: a placeholder, a kinematic anchor for a joint. */
AVER_PHYS_API int32_t aver_phys_shape_empty(const float* comXyz);
/* At least 4 non-coplanar points; Jolt reduces more than 256 vertices itself. */
AVER_PHYS_API int32_t aver_phys_shape_convex_hull(const float* pointsXyz, int32_t count, int32_t surface, const float* p, int32_t n);
/* Triangle mesh. `palette` is up to 32 surface ids and triPaletteIndex one byte per input triangle indexing
   it (NULL = all 0); triangles skipped for bad indices are dropped from all arrays together. triUserData
   (one int32 per triangle, NULL = none) turns on Jolt's per-triangle user data (about 25% more memory). */
AVER_PHYS_API int32_t aver_phys_shape_mesh(const float* verticesXyz, int32_t vertexCount, const int32_t* indices, int32_t indexCount,
                                           const uint8_t* triPaletteIndex, const int32_t* palette, int32_t paletteCount,
                                           const int32_t* triUserData, const float* p, int32_t n);

/* ---- decorators and compounds (children are shape handles) ---- */
#define AVER_PHYS_COMPOUND_STATIC  0
#define AVER_PHYS_COMPOUND_MUTABLE 1
AVER_PHYS_API int32_t aver_phys_shape_rotated_translated(int32_t inner, const float* posCm, const float* quatXyzw);
/* The centre of mass moves by offsetCm; the geometry does not. */
AVER_PHYS_API int32_t aver_phys_shape_offset_com(int32_t inner, const float* offsetCm);
/* Jolt's ScaleShape: a scale the shape cannot wrap is baked when Jolt can, refused when it cannot. Zero components are refused. */
AVER_PHYS_API int32_t aver_phys_shape_scaled(int32_t inner, const float* scaleXyz);
/* 1 to 65536 children; per-child position/rotation in the compound's frame (NULL arrays = zero/identity) and
   per-child user data (NULL = 0). A MUTABLE compound is single-owner: the first aver_phys_body_from_shape binds
   it to that body and a second use is refused. */
AVER_PHYS_API int32_t aver_phys_shape_compound(int32_t kind, const int32_t* children, const float* posCm, const float* quatXyzw,
                                               const int32_t* childUserData, int32_t count);

/* ---- handles and introspection ---- */
AVER_PHYS_API int32_t aver_phys_shape_release(int32_t shape);
AVER_PHYS_API int32_t aver_phys_shape_count(void);
/* A new handle on the body's current shape (the caller releases it); refused for a character or a soft body. */
AVER_PHYS_API int32_t aver_phys_shape_of_body(int32_t body);

#define AVER_PHYS_SHAPE_TYPE_UNKNOWN             0
#define AVER_PHYS_SHAPE_TYPE_BOX                 1
#define AVER_PHYS_SHAPE_TYPE_SPHERE              2
#define AVER_PHYS_SHAPE_TYPE_CAPSULE             3
#define AVER_PHYS_SHAPE_TYPE_CYLINDER            4
#define AVER_PHYS_SHAPE_TYPE_TAPERED_CAPSULE     5
#define AVER_PHYS_SHAPE_TYPE_TAPERED_CYLINDER    6
#define AVER_PHYS_SHAPE_TYPE_CONVEX_HULL         7
#define AVER_PHYS_SHAPE_TYPE_TRIANGLE            8
#define AVER_PHYS_SHAPE_TYPE_PLANE               9
#define AVER_PHYS_SHAPE_TYPE_EMPTY               10
#define AVER_PHYS_SHAPE_TYPE_MESH                11
#define AVER_PHYS_SHAPE_TYPE_HEIGHTFIELD         12
#define AVER_PHYS_SHAPE_TYPE_STATIC_COMPOUND     13
#define AVER_PHYS_SHAPE_TYPE_MUTABLE_COMPOUND    14
#define AVER_PHYS_SHAPE_TYPE_ROTATED_TRANSLATED  15
#define AVER_PHYS_SHAPE_TYPE_SCALED              16
#define AVER_PHYS_SHAPE_TYPE_OFFSET_COM          17
#define AVER_PHYS_SHAPE_TYPE_SOFT_BODY           18

/* aver_phys_shape_info float block (SI): shape-local cm, kg, kg cm^2. Mass is volume times density (0 for a mesh).
   Principal inertia is in the principal frame given as a quaternion; bounds are per-axis sorted. */
#define AVER_PHYS_SI_VOLUME_CM3          0
#define AVER_PHYS_SI_MASS_KG             1
#define AVER_PHYS_SI_COM_X               2
#define AVER_PHYS_SI_COM_Y               3
#define AVER_PHYS_SI_COM_Z               4
#define AVER_PHYS_SI_BOUNDS_MIN_X        5
#define AVER_PHYS_SI_BOUNDS_MIN_Y        6
#define AVER_PHYS_SI_BOUNDS_MIN_Z        7
#define AVER_PHYS_SI_BOUNDS_MAX_X        8
#define AVER_PHYS_SI_BOUNDS_MAX_Y        9
#define AVER_PHYS_SI_BOUNDS_MAX_Z        10
#define AVER_PHYS_SI_INNER_RADIUS        11
#define AVER_PHYS_SI_INERTIA_PRINCIPAL_X 12
#define AVER_PHYS_SI_INERTIA_PRINCIPAL_Y 13
#define AVER_PHYS_SI_INERTIA_PRINCIPAL_Z 14
#define AVER_PHYS_SI_INERTIA_FRAME_X     15
#define AVER_PHYS_SI_INERTIA_FRAME_Y     16
#define AVER_PHYS_SI_INERTIA_FRAME_Z     17
#define AVER_PHYS_SI_INERTIA_FRAME_W     18
#define AVER_PHYS_SI_COUNT               19

/* aver_phys_shape_info int block (SII). */
#define AVER_PHYS_SII_TYPE               0   /* AVER_PHYS_SHAPE_TYPE_* */
#define AVER_PHYS_SII_CHILDREN           1   /* direct children of a compound, else 0 */
#define AVER_PHYS_SII_LEAVES             2   /* recursive */
#define AVER_PHYS_SII_TRIANGLES          3
#define AVER_PHYS_SII_BYTES              4   /* saturated to int32 */
#define AVER_PHYS_SII_MUST_BE_STATIC     5
#define AVER_PHYS_SII_HAS_MASS           6
#define AVER_PHYS_SII_COUNT              7

/* Writes up to min(nF, SI_COUNT) floats and min(nI, SII_COUNT) ints; either pointer may be NULL with its count 0. */
AVER_PHYS_API int32_t aver_phys_shape_info(int32_t shape, float* outF, int32_t nF, int32_t* outI, int32_t nI);

/* ---- bodies ---- */
/* Body parameter block (BP). */
#define AVER_PHYS_BP_LAYER               0   /* collision layer, default 0 */
#define AVER_PHYS_BP_SENSOR              1   /* default 0 */
#define AVER_PHYS_BP_INERTIA_FROM_BOUNDS 2   /* default 0; forced to 1 for a mass-less mesh given a mass */
#define AVER_PHYS_BP_ENHANCED_EDGES      3   /* default 0: Jolt's enhanced internal edge removal */
#define AVER_PHYS_BP_ACTIVATE            4   /* default 1 */
#define AVER_PHYS_BP_COUNT               5

/* A body from a shape, at the shape ORIGIN originCm with quatXyzw (NULL = identity). motionType is
   AVER_PHYS_MOTION_*. massKg <= 0 takes the shape's volume times density. Plane, heightfield and a mesh without
   a mass are refused as dynamic; a MESH with massKg > 0 is accepted as dynamic or kinematic: its centre of mass
   moves to the bounds centre and its inertia is a box's. A dynamic mesh collides with convex shapes only and
   falls through meshes and heightfields (Jolt has no mesh-mesh collision). aver_phys_body_position reports the
   centre of mass; aver_phys_body_origin reports the origin. */
AVER_PHYS_API int32_t aver_phys_body_from_shape(int32_t shape, const float* originCm, const float* quatXyzw,
                                                int32_t motionType, float massKg, const float* p, int32_t n);
/* Swaps the shape; the body ORIGIN stays where it was (a new centre of mass moves the centre, not the pose a model
   hangs from). keepMass keeps the old mass, otherwise the new shape's; a shape with no mass keeps it regardless.
   Refused for a character, a soft body, a vehicle chassis and a ragdoll part. */
AVER_PHYS_API int32_t aver_phys_body_set_shape(int32_t body, int32_t shape, int32_t keepMass);
/* ABSOLUTE scale relative to the unscaled shape. Mass follows volume unless keepMass. */
AVER_PHYS_API int32_t aver_phys_body_set_scale(int32_t body, const float* scaleXyz, int32_t keepMass);
AVER_PHYS_API int32_t aver_phys_body_scale(int32_t body, float* outXyz);
AVER_PHYS_API int32_t aver_phys_body_origin(int32_t body, float* outXyz);
AVER_PHYS_API int32_t aver_phys_body_set_enhanced_edges(int32_t body, int32_t on);
/* Mutable-compound edits, for a body whose shape IS a mutable compound it owns. Each re-centres the compound and
   keeps the body's mass. Adding or removing invalidates every raw sub-shape id taken before and shifts child
   indices; the last child cannot be removed. compound_set_child keeps a NULL position or rotation as it is.
   compound_add returns the new child index or -1; compound_count is -1 when the body is not a compound. */
AVER_PHYS_API int32_t aver_phys_body_compound_add(int32_t body, int32_t child, const float* posCm, const float* quatXyzw, int32_t userData);
AVER_PHYS_API int32_t aver_phys_body_compound_remove(int32_t body, int32_t index);
AVER_PHYS_API int32_t aver_phys_body_compound_set_child(int32_t body, int32_t index, const float* posCm, const float* quatXyzw);
AVER_PHYS_API int32_t aver_phys_body_compound_count(int32_t body);
/* The body's collision triangles intersecting the world box (NULL region = the body's AABB): 9 floats each in engine
   cm, one surface id per triangle (outSurface may be NULL), truncated at maxTriangles. Returns the count. */
AVER_PHYS_API int32_t aver_phys_body_triangles(int32_t body, const float* regionMin, const float* regionMax,
                                               float* outXyz, int32_t* outSurface, int32_t maxTriangles);

/* A vehicle chassis from any shape with a mass, in the vehicle origin frame; rebuilt under the same handle like
   aver_phys_vehicle_set_chassis_boxes (mass and centre of mass kept). Refused after _finish. */
AVER_PHYS_API int32_t aver_phys_vehicle_set_chassis_shape(int32_t v, int32_t shape);

#ifdef __cplusplus
}
#endif
