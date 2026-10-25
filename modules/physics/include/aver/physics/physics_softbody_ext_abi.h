#pragma once
// Aver.Physics -- THE REST OF JOLT'S SOFT BODIES: inflatables, tetrahedral volumes, long range
// attachments, Cosserat rods, Jolt's own constraint generator, grab and release, contact policy, sensors,
// a collision group, and buoyancy.
//
// ADDITIVE. physics_softbody_abi.h (AVER_PHYS_SB_ABI_VERSION 2) is not edited and a body that never calls
// anything here behaves exactly as before. The builder calls extend a builder from
// aver_phys_sb_builder_begin and are staged until aver_phys_sb_builder_finish; the live calls take the
// handle finish returned. The Ext state of a builder is erased at finish or cancel.
//
// UNITS AND AXES are the engine's everywhere: centimetres, cm/s, kilopascal, +X forward, +Y right, +Z up.
// Types are int32_t, float, const float* and const int32_t* only. Every function returns 1 on success and
// 0 on refusal (aver_phys_last_error says why) unless it says it returns a count.
//
// NEVER CALL FROM INSIDE A STEP LISTENER OR A CONTACT CALLBACK; edits take effect on the next step.
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

#define AVER_PHYS_SBX_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_sbx_abi_version(void);

// ---- Extra parameters (builder_params) --------------------------------------------------------------
// A NaN entry means "leave as is" (a rebuild with replaceBody keeps the replaced body's value). Defaults
// in brackets: pressure [0] kPa at rest volume; skin enabled [1]; skin distance scale [1] (0 = hard skin);
// collision group [0]; soft / other inverse mass scale and other inverse inertia scale [1] (0 = infinite
// mass); fluid density [0] kg/m^3 (0 = no buoyancy); fluid drag [1]; buoyancy flags [1].
#define AVER_PHYS_SBX_P_PRESSURE_KPA            0
#define AVER_PHYS_SBX_P_SKIN_ENABLED            1
#define AVER_PHYS_SBX_P_SKIN_DISTANCE_SCALE     2
#define AVER_PHYS_SBX_P_COLLISION_GROUP         3
#define AVER_PHYS_SBX_P_SOFT_INV_MASS_SCALE     4
#define AVER_PHYS_SBX_P_OTHER_INV_MASS_SCALE    5
#define AVER_PHYS_SBX_P_OTHER_INV_INERTIA_SCALE 6
#define AVER_PHYS_SBX_P_FLUID_DENSITY           7
#define AVER_PHYS_SBX_P_FLUID_DRAG              8
#define AVER_PHYS_SBX_P_BUOY_FLAGS              9
#define AVER_PHYS_SBX_P_COUNT                   10

// ---- Builder ------------------------------------------------------------------------------------------

// Tetrahedra: 4 vertex indices each. The rest volume is measured from the staged rest positions at finish
// (the constraint is abs(signed volume), so winding does not matter). compliance may be null (0 =
// incompressible). Repeated calls append. A body that stages volumes, rods or auto constraints may have
// no faces: it gets one extra face over its first three vertices (so it needs at least 3 vertices).
AVER_PHYS_API int32_t aver_phys_sbx_builder_volumes(int32_t b, const int32_t* tet4, const float* compliance, int32_t count);

// Explicit long range attachments: pairs2 = {kinematic vertex, dynamic vertex} per pair, maxDistCm per
// pair. Repeated calls append.
AVER_PHYS_API int32_t aver_phys_sbx_builder_lra(int32_t b, const int32_t* pairs2, const float* maxDistCm, int32_t count);

// LRA from the closest kinematic vertex (inverse mass 0) over the staged edges and rods, for every dynamic
// vertex: mode 0 off, 1 Euclidean, 2 geodesic. distanceMultiplier >= 1 (1.01 = 1% slack).
AVER_PHYS_API int32_t aver_phys_sbx_builder_lra_auto(int32_t b, int32_t mode, float distanceMultiplier);

// Cosserat rods: ab = 2 vertices per rod; rodPairs2 = pairs of rod indices (0-based within THIS call)
// joined by a bend-twist constraint. Compliance arrays may be null (0). A rod not named in any pair is
// refused (it would spin forever). Repeated calls append rods; the indices of one call stay local to it.
AVER_PHYS_API int32_t aver_phys_sbx_builder_rods(int32_t b, const int32_t* ab, const float* stretchCompliance, int32_t count,
                                                 const int32_t* rodPairs2, const float* bendTwistCompliance, int32_t pairCount);

// A chain: vertices [firstVertex, firstVertex + vertexCount) joined by vertexCount - 1 rods and
// vertexCount - 2 bend-twist pairs between neighbours (vertexCount >= 3).
AVER_PHYS_API int32_t aver_phys_sbx_builder_rod_chain(int32_t b, int32_t firstVertex, int32_t vertexCount,
                                                      float stretchCompliance, float bendTwistCompliance);

// Jolt's own CreateConstraints over the staged faces. attrs5 = per vertex {compliance, shearCompliance,
// bendCompliance (negative = no bends), lraMode (0 off, 1 Euclidean, 2 geodesic), lraMultiplier};
// attrCount may be less than the vertex count (the last repeats, as in Jolt). bendType 0 none, 1 distance
// (an extra edge), 2 dihedral. It makes the edges and bends itself, so finish refuses a builder that also
// staged edges or bends.
AVER_PHYS_API int32_t aver_phys_sbx_builder_auto(int32_t b, const float* attrs5, int32_t attrCount, int32_t bendType,
                                                 float angleToleranceDeg);

// p: AVER_PHYS_SBX_P_* entries (n <= AVER_PHYS_SBX_P_COUNT); NaN = leave as is. A pressure on a mesh that
// is not closed (or is inside out) is ignored with a log line at finish.
AVER_PHYS_API int32_t aver_phys_sbx_builder_params(int32_t b, const float* p, int32_t n);

// After finish: the final Jolt index of each staged item (Optimize reorders them). kind 0 volume,
// 1 lra (explicit pairs first, then lra_auto's), 2 rod, 3 rod pair (bend-twist). Writes up to maxCount
// ints into outSlots (null = count only) and returns the number of items. Valid for the body the builder made.
AVER_PHYS_API int32_t aver_phys_sbx_slots(int32_t body, int32_t kind, int32_t* outSlots, int32_t maxCount);

// ---- Live ---------------------------------------------------------------------------------------------

// Pressure in kPa at REST volume (Jolt's n R T is derived from the rest volume measured at finish). 0 = off.
// Refused (Unsupported) for a mesh that is not closed or is inside out, negative or non-finite.
AVER_PHYS_API int32_t aver_phys_sbx_set_pressure(int32_t body, float pressureKPa);
AVER_PHYS_API int32_t aver_phys_sbx_get_pressure(int32_t body, float* outKPa);
// Rest and current volume in cm^3 of the body's faces; now is negative when the shape is inside out.
AVER_PHYS_API int32_t aver_phys_sbx_volume(int32_t body, float* outRestCm3, float* outNowCm3);
// Skin constraints on or off, and the max distance multiplier (0 = hard skin).
AVER_PHYS_API int32_t aver_phys_sbx_set_skin(int32_t body, int32_t enabled, float distanceScale);
// Per-vertex inverse mass (0 = pinned). Recomputes the body's mass and the inverse mass of every rod that
// touches a vertex. The caller's own copy should be updated too if it rebuilds the body.
AVER_PHYS_API int32_t aver_phys_sbx_set_inv_mass(int32_t body, const int32_t* vertex, const float* invMass, int32_t n);

// ---- Attachments (grab and release, anchors) ----------------------------------------------------------
// The vertex becomes kinematic and is carried to the target by its velocity at the start of every fixed step
// (frame-rate independent). One way: the vertex follows, the target feels nothing. maxSpeedCmS <= 0 = no cap.
// Attaching a vertex that is already attached replaces its target and keeps the mass it had before.
AVER_PHYS_API int32_t aver_phys_sbx_attach(int32_t body, int32_t vertex, const float* targetXyzCm, float maxSpeedCmS);
// Follows a point of another body: localOffsetXyzCm is in that body's own frame (its position and rotation).
// A stale otherBody detaches the vertex (logged once).
AVER_PHYS_API int32_t aver_phys_sbx_attach_to_body(int32_t body, int32_t vertex, int32_t otherBody,
                                                   const float* localOffsetXyzCm, float maxSpeedCmS);
AVER_PHYS_API int32_t aver_phys_sbx_move_attachment(int32_t body, int32_t vertex, const float* targetXyzCm);   /* a plain attach's target */
// restoreInvMass < 0: what the vertex had before it was attached. Zeroes the vertex velocity and wakes the body.
AVER_PHYS_API int32_t aver_phys_sbx_detach(int32_t body, int32_t vertex, float restoreInvMass);
AVER_PHYS_API int32_t aver_phys_sbx_attachment_count(int32_t body);

// ---- Contact policy (applied when the soft body meets a rigid body) -----------------------------------
// Scales: 0 = infinite mass, 1 = unchanged, 2 = half the mass. Keep a pair's scales constant over time.
AVER_PHYS_API int32_t aver_phys_sbx_set_contact_policy(int32_t body, float softInvMassScale, float otherInvMassScale,
                                                       float otherInvInertiaScale);
#define AVER_PHYS_SBX_PAIR_NORMAL 0
#define AVER_PHYS_SBX_PAIR_SENSOR 1   /* no response; enter and exit events */
#define AVER_PHYS_SBX_PAIR_IGNORE 2   /* the contact is rejected */
AVER_PHYS_API int32_t aver_phys_sbx_set_pair_policy(int32_t body, int32_t otherBody, int32_t mode,
                                                    float softInvMassScale, float otherInvMassScale, float otherInvInertiaScale);
AVER_PHYS_API int32_t aver_phys_sbx_clear_pair_policy(int32_t body, int32_t otherBody);

// Sensor contacts, polled and cleared at the top of aver_phys_step like contacts: Jolt sensor bodies (the
// aver_phys_add_sensor_* ones) and pairs set to SENSOR. outEntered is 1 for an enter, 0 for an exit.
// A body that is asleep or deactivated reports nothing and keeps its inside set.
AVER_PHYS_API int32_t aver_phys_sbx_sensor_count(void);
AVER_PHYS_API int32_t aver_phys_sbx_sensor_get(int32_t index, int32_t* outSoftBody, int32_t* outSensorBody, int32_t* outEntered);

// ---- Collision group ----------------------------------------------------------------------------------
// 0 = none; otherwise the soft body ignores every rigid body whose group id equals this (a ragdoll's
// handle is its group id, so a cape ignores its wearer's ragdoll parts without a layer).
AVER_PHYS_API int32_t aver_phys_sbx_set_collision_group(int32_t body, int32_t group);

// ---- Buoyancy -----------------------------------------------------------------------------------------
// fluidDensityKgM3 0 = off. Hydrostatic pressure on every submerged face of a closed mesh plus quadratic
// drag on any mesh (drag 1 is a flat plate), evaluated in the fixed step. A buoyant body is kept awake while
// any face is submerged. flags: WORLD_WATER = the host water sampler, else the global water plane
// (aver_phys_set_water_plane); OWN_PLANE = the plane of aver_phys_sbx_set_water_plane, which wins.
#define AVER_PHYS_SBX_BUOY_WORLD_WATER 0x1
#define AVER_PHYS_SBX_BUOY_OWN_PLANE   0x2
AVER_PHYS_API int32_t aver_phys_sbx_set_buoyancy(int32_t body, float fluidDensityKgM3, float drag, int32_t flags);
AVER_PHYS_API int32_t aver_phys_sbx_set_water_plane(int32_t body, const float* pointXyzCm, const float* normalXyz,
                                                    const float* currentXyzCmS);
// The last step's area-weighted submerged fraction (0..1) and displaced volume (closed meshes; 0 otherwise).
AVER_PHYS_API int32_t aver_phys_sbx_submerged(int32_t body, float* outFraction01, float* outVolumeCm3);

// ---- Rods ---------------------------------------------------------------------------------------------
// Rod i in the FINAL order (see aver_phys_sbx_slots kind 2). outQuatXyzw: 4 floats per rod, the rod's world
// orientation in engine axes such that the engine +X axis rotated by it is the rod's direction (first
// vertex toward second); its twist follows the Bishop frame. outAngVelRadS: 3 floats per rod, rad/s.
// Either out may be null. Returns the number of rods written.
AVER_PHYS_API int32_t aver_phys_sbx_rod_state(int32_t body, int32_t firstRod, int32_t count, float* outQuatXyzw, float* outAngVelRadS);
AVER_PHYS_API int32_t aver_phys_sbx_rod_count(int32_t body);

#ifdef __cplusplus
} // extern "C"
#endif
