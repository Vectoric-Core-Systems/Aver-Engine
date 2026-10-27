#pragma once
// Aver.Physics: the rest of Jolt's soft bodies (inflatables, tetrahedral volumes, LRA, Cosserat rods, grab and release, contact
// policy, sensors, collision group, buoyancy). Additive: builder calls are staged until aver_phys_sb_builder_finish.
// Units: cm, cm/s, kPa, +Z up. Never call from a step listener or contact callback; edits take effect on the next step.
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

// Extra parameters (builder_params). NaN = leave as is. Defaults: pressure 0, skin on, skin scale 1, group 0, inverse scales 1, density 0 (no buoyancy), drag 1, flags 1.
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

// Tetrahedra: 4 vertex indices each. The rest volume is measured at finish. compliance may be null (0 = incompressible).
AVER_PHYS_API int32_t aver_phys_sbx_builder_volumes(int32_t b, const int32_t* tet4, const float* compliance, int32_t count);

// Explicit LRA: pairs2 = {kinematic vertex, dynamic vertex} per pair, maxDistCm per pair. Repeated calls append.
AVER_PHYS_API int32_t aver_phys_sbx_builder_lra(int32_t b, const int32_t* pairs2, const float* maxDistCm, int32_t count);

// LRA from the closest kinematic vertex over the staged edges and rods: mode 0 off, 1 Euclidean, 2 geodesic. distanceMultiplier >= 1.
AVER_PHYS_API int32_t aver_phys_sbx_builder_lra_auto(int32_t b, int32_t mode, float distanceMultiplier);

// Cosserat rods: ab = 2 vertices per rod; rodPairs2 joins rods (0-based within this call) by bend-twist. A rod in no pair is refused.
AVER_PHYS_API int32_t aver_phys_sbx_builder_rods(int32_t b, const int32_t* ab, const float* stretchCompliance, int32_t count,
                                                 const int32_t* rodPairs2, const float* bendTwistCompliance, int32_t pairCount);

// A chain of vertexCount >= 3 vertices from firstVertex: rods between neighbours, bend-twist pairs between them.
AVER_PHYS_API int32_t aver_phys_sbx_builder_rod_chain(int32_t b, int32_t firstVertex, int32_t vertexCount,
                                                      float stretchCompliance, float bendTwistCompliance);

// Jolt's own CreateConstraints over the staged faces. attrs5 per vertex: {compliance, shear, bend (negative = no bends), lraMode, lraMultiplier}.
// bendType 0 none, 1 distance, 2 dihedral. Finish refuses a builder that also staged edges or bends.
AVER_PHYS_API int32_t aver_phys_sbx_builder_auto(int32_t b, const float* attrs5, int32_t attrCount, int32_t bendType,
                                                 float angleToleranceDeg);

// A pressure on a mesh that is not closed (or is inside out) is ignored with a log line at finish.
AVER_PHYS_API int32_t aver_phys_sbx_builder_params(int32_t b, const float* p, int32_t n);

// After finish: the final Jolt index of each staged item (Optimize reorders them). kind 0 volume, 1 lra, 2 rod, 3 rod pair.
AVER_PHYS_API int32_t aver_phys_sbx_slots(int32_t body, int32_t kind, int32_t* outSlots, int32_t maxCount);

// Pressure in kPa at REST volume; 0 = off. Refused for a mesh that is not closed or is inside out, negative or non-finite.
AVER_PHYS_API int32_t aver_phys_sbx_set_pressure(int32_t body, float pressureKPa);
AVER_PHYS_API int32_t aver_phys_sbx_get_pressure(int32_t body, float* outKPa);
// Rest and current volume in cm^3; now is negative when the shape is inside out.
AVER_PHYS_API int32_t aver_phys_sbx_volume(int32_t body, float* outRestCm3, float* outNowCm3);
AVER_PHYS_API int32_t aver_phys_sbx_set_skin(int32_t body, int32_t enabled, float distanceScale);
// Per-vertex inverse mass (0 = pinned). Recomputes the body's mass and the rods touching each vertex.
AVER_PHYS_API int32_t aver_phys_sbx_set_inv_mass(int32_t body, const int32_t* vertex, const float* invMass, int32_t n);

// Attachments. The vertex becomes kinematic and is carried to the target by its velocity at the start of every fixed step. One way:
// the target feels nothing. maxSpeedCmS <= 0 = no cap. Attaching an attached vertex keeps the mass it had before.
AVER_PHYS_API int32_t aver_phys_sbx_attach(int32_t body, int32_t vertex, const float* targetXyzCm, float maxSpeedCmS);
// Follows a point of another body, localOffsetXyzCm in that body's frame. A stale otherBody detaches the vertex (logged once).
AVER_PHYS_API int32_t aver_phys_sbx_attach_to_body(int32_t body, int32_t vertex, int32_t otherBody,
                                                   const float* localOffsetXyzCm, float maxSpeedCmS);
AVER_PHYS_API int32_t aver_phys_sbx_move_attachment(int32_t body, int32_t vertex, const float* targetXyzCm);
// restoreInvMass < 0: what the vertex had before it was attached. Zeroes the vertex velocity and wakes the body.
AVER_PHYS_API int32_t aver_phys_sbx_detach(int32_t body, int32_t vertex, float restoreInvMass);
AVER_PHYS_API int32_t aver_phys_sbx_attachment_count(int32_t body);

// Contact policy (soft body vs rigid body). Scales: 0 = infinite mass, 1 = unchanged, 2 = half the mass. Keep them constant over time.
AVER_PHYS_API int32_t aver_phys_sbx_set_contact_policy(int32_t body, float softInvMassScale, float otherInvMassScale,
                                                       float otherInvInertiaScale);
/* PAIR_SENSOR: no response, enter and exit events. PAIR_IGNORE: the contact is rejected. */
#define AVER_PHYS_SBX_PAIR_NORMAL 0
#define AVER_PHYS_SBX_PAIR_SENSOR 1
#define AVER_PHYS_SBX_PAIR_IGNORE 2
AVER_PHYS_API int32_t aver_phys_sbx_set_pair_policy(int32_t body, int32_t otherBody, int32_t mode,
                                                    float softInvMassScale, float otherInvMassScale, float otherInvInertiaScale);
AVER_PHYS_API int32_t aver_phys_sbx_clear_pair_policy(int32_t body, int32_t otherBody);

// Sensor contacts, polled at the top of aver_phys_step. outEntered is 1 for an enter, 0 for an exit. An asleep or deactivated body reports nothing.
AVER_PHYS_API int32_t aver_phys_sbx_sensor_count(void);
AVER_PHYS_API int32_t aver_phys_sbx_sensor_get(int32_t index, int32_t* outSoftBody, int32_t* outSensorBody, int32_t* outEntered);

// 0 = none; otherwise the soft body ignores every rigid body whose group id equals this (a ragdoll's handle is its group id).
AVER_PHYS_API int32_t aver_phys_sbx_set_collision_group(int32_t body, int32_t group);

// Buoyancy, evaluated in the fixed step: hydrostatic pressure on submerged faces of a closed mesh plus quadratic drag on any mesh.
// flags: WORLD_WATER = the host water sampler, else the global water plane (aver_phys_set_water_plane); OWN_PLANE = the plane of
// aver_phys_sbx_set_water_plane, which wins.
#define AVER_PHYS_SBX_BUOY_WORLD_WATER 0x1
#define AVER_PHYS_SBX_BUOY_OWN_PLANE   0x2
AVER_PHYS_API int32_t aver_phys_sbx_set_buoyancy(int32_t body, float fluidDensityKgM3, float drag, int32_t flags);
AVER_PHYS_API int32_t aver_phys_sbx_set_water_plane(int32_t body, const float* pointXyzCm, const float* normalXyz,
                                                    const float* currentXyzCmS);
// The last step's area-weighted submerged fraction (0..1) and displaced volume (closed meshes; 0 otherwise).
AVER_PHYS_API int32_t aver_phys_sbx_submerged(int32_t body, float* outFraction01, float* outVolumeCm3);

// Rod i in the FINAL order (see aver_phys_sbx_slots kind 2). outQuatXyzw: 4 floats per rod, engine axes, engine +X rotated by it gives the
// rod's direction. outAngVelRadS: 3 floats per rod, rad/s. Either may be null.
AVER_PHYS_API int32_t aver_phys_sbx_rod_state(int32_t body, int32_t firstRod, int32_t count, float* outQuatXyzw, float* outAngVelRadS);
AVER_PHYS_API int32_t aver_phys_sbx_rod_count(int32_t body);

#ifdef __cplusplus
} // extern "C"
#endif
