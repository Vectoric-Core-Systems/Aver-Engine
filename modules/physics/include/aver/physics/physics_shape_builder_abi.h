#pragma once
// Aver.Physics: generic shape handles, one table for every Jolt collision shape. A shape handle is NOT a body handle.
// Builders and release are safe from any thread; body functions are main thread only. Units are the engine's (cm, +Z up, left-handed).
// Parameter blocks (`const float* p, int32_t n`): any prefix; unset entries keep the default; a negative value means the shape's own default.
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

#define AVER_PHYS_SHAPE_ABI_VERSION 2   /* 2: shape_separation_ray, body_set_shape_disabled, body_shape_disabled (all additive) */
AVER_PHYS_API int32_t aver_phys_shape_abi_version(void);

/* shape parameter block (SP): convex radius cm (5), density kg/m^3 (1000), hull tolerance cm (0.1), mesh active-edge angle deg (5;
   negative = every edge active), mesh build quality (0 runtime, 1 build speed), mesh tris per leaf 1..8 (8). */
#define AVER_PHYS_SP_CONVEX_RADIUS    0
#define AVER_PHYS_SP_DENSITY          1
#define AVER_PHYS_SP_HULL_TOLERANCE   2
#define AVER_PHYS_SP_EDGE_ANGLE_DEG   3
#define AVER_PHYS_SP_BUILD_QUALITY    4
#define AVER_PHYS_SP_MAX_TRIS_PER_LEAF 5
#define AVER_PHYS_SP_COUNT            6

/* `surface` is a physical-surface id (0 = none), see physics_surface_abi.h. */
AVER_PHYS_API int32_t aver_phys_shape_box(float hx, float hy, float hz, int32_t surface, const float* p, int32_t n);
AVER_PHYS_API int32_t aver_phys_shape_sphere(float radius, int32_t surface, const float* p, int32_t n);
AVER_PHYS_API int32_t aver_phys_shape_capsule(float radius, float height, int32_t surface, const float* p, int32_t n);
AVER_PHYS_API int32_t aver_phys_shape_cylinder(float radius, float height, int32_t surface, const float* p, int32_t n);
AVER_PHYS_API int32_t aver_phys_shape_tapered_capsule(float topRadius, float bottomRadius, float height, int32_t surface, const float* p, int32_t n);
AVER_PHYS_API int32_t aver_phys_shape_tapered_cylinder(float topRadius, float bottomRadius, float height, int32_t surface, const float* p, int32_t n);
/* One-sided: bodies touch the face (B - A) x (C - A) points to and pass through the back. */
AVER_PHYS_API int32_t aver_phys_shape_triangle(const float* abcXyz, int32_t surface, const float* p, int32_t n);
/* Points with n . x = offsetCm form the plane; the side opposite the normal is solid. Static or kinematic bodies only. */
AVER_PHYS_API int32_t aver_phys_shape_plane(const float* normalXyz, float offsetCm, float halfExtentCm, int32_t surface);
/* No volume, collides with nothing: a placeholder, or a kinematic anchor for a joint. */
AVER_PHYS_API int32_t aver_phys_shape_empty(const float* comXyz);
/* Godot SeparationRayShape3D, added in ABI version 2: an APPROXIMATION. Jolt has no ray shape that pushes bodies apart, so this is a thin
   capsule from the shape origin along dirXyz (any length, normalised here; NULL is refused) for lengthCm, the far cap's tip exactly at lengthCm.
   radiusCm <= 0 takes 0.1. It is solid, so a body whose compound holds it is stopped with the ray's tip on the surface (the end point
   "moves to the collision point"), along the CONTACT normal: Godot's slide_on_slope = true behaviour; slide_on_slope = false (push along
   the ray whatever the slope) is not provided. Its density is 0.001 kg/m^3, so it adds no mass or centre-of-mass shift worth the name.
   `surface` is a physical-surface id. Put it in a compound (or a body) like any shape, and toggle it with aver_phys_body_set_shape_disabled. */
AVER_PHYS_API int32_t aver_phys_shape_separation_ray(float lengthCm, const float* dirXyz, float radiusCm, int32_t surface);
AVER_PHYS_API int32_t aver_phys_shape_convex_hull(const float* pointsXyz, int32_t count, int32_t surface, const float* p, int32_t n);
/* palette: up to 32 surface ids; triPaletteIndex one byte per triangle (NULL = 0). Bad-index triangles are dropped from all arrays.
   triUserData (NULL = none) costs about 25% more memory. */
AVER_PHYS_API int32_t aver_phys_shape_mesh(const float* verticesXyz, int32_t vertexCount, const int32_t* indices, int32_t indexCount,
                                           const uint8_t* triPaletteIndex, const int32_t* palette, int32_t paletteCount,
                                           const int32_t* triUserData, const float* p, int32_t n);

/* decorators and compounds (children are shape handles) */
#define AVER_PHYS_COMPOUND_STATIC  0
#define AVER_PHYS_COMPOUND_MUTABLE 1
AVER_PHYS_API int32_t aver_phys_shape_rotated_translated(int32_t inner, const float* posCm, const float* quatXyzw);
/* The centre of mass moves by offsetCm; the geometry does not. */
AVER_PHYS_API int32_t aver_phys_shape_offset_com(int32_t inner, const float* offsetCm);
/* Baked when Jolt can wrap the scale, refused when it cannot. Zero components are refused. */
AVER_PHYS_API int32_t aver_phys_shape_scaled(int32_t inner, const float* scaleXyz);
/* A MUTABLE compound is single-owner: the first aver_phys_body_from_shape binds it; a second use is refused. */
AVER_PHYS_API int32_t aver_phys_shape_compound(int32_t kind, const int32_t* children, const float* posCm, const float* quatXyzw,
                                               const int32_t* childUserData, int32_t count);

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

/* shape info float block (SI): shape-local cm, kg, kg cm^2. Principal inertia is in the frame given by the quaternion. Bounds are sorted per axis. */
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

/* shape info int block (SII). */
#define AVER_PHYS_SII_TYPE               0
#define AVER_PHYS_SII_CHILDREN           1
#define AVER_PHYS_SII_LEAVES             2
#define AVER_PHYS_SII_TRIANGLES          3
#define AVER_PHYS_SII_BYTES              4
#define AVER_PHYS_SII_MUST_BE_STATIC     5
#define AVER_PHYS_SII_HAS_MASS           6
#define AVER_PHYS_SII_COUNT              7

/* Writes up to min(nF, SI_COUNT) floats and min(nI, SII_COUNT) ints; either pointer may be NULL with its count 0. */
AVER_PHYS_API int32_t aver_phys_shape_info(int32_t shape, float* outF, int32_t nF, int32_t* outI, int32_t nI);

/* body parameter block (BP). INERTIA_FROM_BOUNDS is forced to 1 for a mass-less mesh given a mass. */
#define AVER_PHYS_BP_LAYER               0
#define AVER_PHYS_BP_SENSOR              1
#define AVER_PHYS_BP_INERTIA_FROM_BOUNDS 2
#define AVER_PHYS_BP_ENHANCED_EDGES      3
#define AVER_PHYS_BP_ACTIVATE            4
#define AVER_PHYS_BP_COUNT               5

/* Body at the shape ORIGIN originCm (NULL quat = identity). massKg <= 0 takes volume times density. Plane, heightfield and mass-less mesh
   are refused as dynamic. A mesh with mass is dynamic or kinematic (its COM moves to the bounds centre). A dynamic mesh collides with
   convex shapes only. aver_phys_body_position reports the COM; aver_phys_body_origin the origin. */
AVER_PHYS_API int32_t aver_phys_body_from_shape(int32_t shape, const float* originCm, const float* quatXyzw,
                                                int32_t motionType, float massKg, const float* p, int32_t n);
/* The body ORIGIN stays where it was. keepMass keeps the old mass (a shape with no mass keeps it regardless). Refused for a character,
   a soft body, a vehicle chassis and a ragdoll part. */
AVER_PHYS_API int32_t aver_phys_body_set_shape(int32_t body, int32_t shape, int32_t keepMass);
/* ABSOLUTE scale relative to the unscaled shape. Mass follows volume unless keepMass. */
AVER_PHYS_API int32_t aver_phys_body_set_scale(int32_t body, const float* scaleXyz, int32_t keepMass);
AVER_PHYS_API int32_t aver_phys_body_scale(int32_t body, float* outXyz);
AVER_PHYS_API int32_t aver_phys_body_origin(int32_t body, float* outXyz);
AVER_PHYS_API int32_t aver_phys_body_set_enhanced_edges(int32_t body, int32_t on);
/* Mutable-compound edits (the body's shape must be a mutable compound it owns). Each keeps the mass. Add and remove invalidate raw
   sub-shape ids and shift child indices; the last child cannot be removed. compound_add returns the index or -1; compound_count -1 = not a compound. */
AVER_PHYS_API int32_t aver_phys_body_compound_add(int32_t body, int32_t child, const float* posCm, const float* quatXyzw, int32_t userData);
AVER_PHYS_API int32_t aver_phys_body_compound_remove(int32_t body, int32_t index);
AVER_PHYS_API int32_t aver_phys_body_compound_set_child(int32_t body, int32_t index, const float* posCm, const float* quatXyzw);
AVER_PHYS_API int32_t aver_phys_body_compound_count(int32_t body);
/* Per-shape disable (Godot CollisionShape3D.disabled), added in ABI version 2. The shape stays where it is and keeps its index and user data;
   it just stops colliding, being hit by queries and weighing on the inertia (the body's MASS is kept). index >= 0 is a child of a MUTABLE
   compound this body owns (a static compound cannot be edited: build the body from a mutable one); index < 0, or 0 on a body whose shape is
   not a compound, disables the body's whole shape (it is swapped for an empty one with the same centre of mass, and aver_phys_shape_of_body
   then reports that empty shape; set_shape and set_scale are refused until it is enabled again). Idempotent. A disabled child moves with
   compound_set_child; compound_remove shifts the disabled set with the indices; set_shape forgets it. Not captured by snapshots. */
AVER_PHYS_API int32_t aver_phys_body_set_shape_disabled(int32_t body, int32_t index, int32_t disabled);
AVER_PHYS_API int32_t aver_phys_body_shape_disabled(int32_t body, int32_t index, int32_t* outDisabled);
/* Triangles in the world box (NULL region = the body's AABB): 9 floats each in engine cm, one surface id each. */
AVER_PHYS_API int32_t aver_phys_body_triangles(int32_t body, const float* regionMin, const float* regionMax,
                                               float* outXyz, int32_t* outSurface, int32_t maxTriangles);

/* Vehicle chassis from any shape with a mass, in the vehicle origin frame. Refused after _finish. */
AVER_PHYS_API int32_t aver_phys_vehicle_set_chassis_shape(int32_t v, int32_t shape);

#ifdef __cplusplus
}
#endif
