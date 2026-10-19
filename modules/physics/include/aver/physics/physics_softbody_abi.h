#pragma once
// Aver.Physics -- SOFT BODIES THAT CAN DENT, TEAR AND BE REBUILT (cloth and destruction).
//
// WHAT THIS ADDS TO aver_phys_softbody_*. That surface (physics_abi.h) builds a fixed triangle shell and
// only skins it. This one is builder style: the caller stages vertices, faces, edges (with their own rest
// lengths), dihedral bends and skin entries, and `finish` makes the Jolt body. A body made here keeps its
// shared settings in a registry record, so between aver_phys_step calls the rest length of an edge
// (a DENT) and the rest angle of a bend can be rewritten in place, and a TEAR rebuilds the body with the
// SAME handle, carrying positions and velocities across. Both are driven by Aver.SoftBody.Scene.
//
// The existing aver_phys_softbody_* functions are untouched (Fluids uses them).
//
// UNITS AND AXES are the engine's everywhere: centimetres, cm/s, +X forward, +Y right, +Z up. Types are
// int32_t, float, const float* and const int32_t* only. Every function returns 1 on success and 0 on
// refusal (aver_phys_last_error says why) unless it says it returns a handle or a count.
//
// NEVER CALL FROM INSIDE A STEP LISTENER OR A CONTACT CALLBACK. The host is single threaded here: the
// system ticks after aver_phys_step has returned. Edits take effect on the next step.
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

// The version of this surface: 1 = the wave-9 builder surface (which had no macro), 2 adds aver_phys_sb_add_velocity
// and this function.
#define AVER_PHYS_SB_ABI_VERSION 2
AVER_PHYS_API int32_t aver_phys_sb_abi_version(void);

// ---- Parameter indices (builder_params, set_params) --------------------------------------------------
// A NaN entry means "leave as is". Defaults: iterations 5, damping 0.1, friction 0.2, restitution 0,
// gravity scale 1, vertex radius 0, max speed 50000 cm/s, sleeping allowed, single sided, layer 0,
// entity 0, impacts off.
#define AVER_PHYS_SB_P_ITERATIONS       0
#define AVER_PHYS_SB_P_DAMPING          1
#define AVER_PHYS_SB_P_FRICTION         2
#define AVER_PHYS_SB_P_RESTITUTION      3
#define AVER_PHYS_SB_P_GRAVITY_SCALE    4
#define AVER_PHYS_SB_P_VERTEX_RADIUS_CM 5
#define AVER_PHYS_SB_P_MAX_SPEED_CMS    6
#define AVER_PHYS_SB_P_ALLOW_SLEEP      7
#define AVER_PHYS_SB_P_DOUBLE_SIDED     8
#define AVER_PHYS_SB_P_LAYER            9    /* user collision layer 0..15 */
#define AVER_PHYS_SB_P_ENTITY           10   /* aver_phys_set_entity user data */
#define AVER_PHYS_SB_P_IMPACTS          11   /* 1 = report impacts (aver_phys_sb_impact_*) */
#define AVER_PHYS_SB_P_COUNT            12

// ---- Builder -----------------------------------------------------------------------------------------
// Stages arrays; nothing is a body until finish. Calls may come in any order after begin. The builder id
// is dead after finish or cancel.

// A builder for `vertexCount` particles (1..65536). 0 = refused.
AVER_PHYS_API int32_t aver_phys_sb_builder_begin(int32_t vertexCount);

// restXyz: world cm, vertexCount * 3; the basis of edge geometry and of the skin bind pose.
// invMass: optional (null = 1 each); 0 = kinematic, which is how a pin is expressed.
// statePosXyz / stateVelXyz: optional starting state in world cm and cm/s (null = rest / zero).
AVER_PHYS_API int32_t aver_phys_sb_builder_vertices(int32_t b, const float* restXyz, const float* invMass,
                                                    const float* statePosXyz, const float* stateVelXyz);

// tri3: triCount * 3 vertex indices, wound straight through (as aver_phys_softbody_create does).
AVER_PHYS_API int32_t aver_phys_sb_builder_faces(int32_t b, const int32_t* tri3, int32_t triCount);

// ab: edgeCount * 2 vertex indices, restCm: rest length per edge, compliance: Jolt compliance (m/N, 0 =
// inextensible) per edge or null for 0. No shear edges are created: a shear edge across a tear would
// keep it shut.
AVER_PHYS_API int32_t aver_phys_sb_builder_edges(int32_t b, const int32_t* ab, const float* restCm,
                                                 const float* compliance, int32_t edgeCount);

// quad4: bendCount * 4 vertex indices, the Jolt DihedralBend layout (the first two are the shared edge).
// compliance: per bend or null for 0. The rest angle is taken from the rest positions.
AVER_PHYS_API int32_t aver_phys_sb_builder_bends(int32_t b, const int32_t* quad4, const float* compliance,
                                                 int32_t bendCount);

// Skin entries. joints4/weights4: 4 per vertex (a zero weight ends a vertex's list). reachCm: per vertex,
// < 0 = no skinned constraint, 0 = hard skin, else the max distance from the skinned position.
// backstopCm < 0 = off. jointCount: how many joint matrices skin_world will be given.
AVER_PHYS_API int32_t aver_phys_sb_builder_skin(int32_t b, const int32_t* joints4, const float* weights4,
                                                const float* reachCm, float backstopCm, int32_t jointCount);

// p: AVER_PHYS_SB_P_* entries (n <= AVER_PHYS_SB_P_COUNT).
AVER_PHYS_API int32_t aver_phys_sb_builder_params(int32_t b, const float* p, int32_t n);

// Makes the body; returns its handle (an ordinary body handle) or 0 on refusal.
//   cx,cy,cz: the body's centre (world cm); vertices are stored relative to it.
//   replaceBody != 0: the SAME handle is kept. The new body takes the old one's registry params (the
//     builder's own entries win), layer, entity and last palette; the old body is removed. A refused finish
//     leaves the old body intact. replaceBody == 0 returns a new handle.
//   stateSrcBody != 0: the starting state comes from that body's vertices through stateMap (new vertex ->
//     old vertex, -1 = rest). It may name replaceBody. The old and new bodies may have different centres.
//   outEdgeSlot / outBendSlot: optional; the final Jolt index of each input edge / bend, for set_edge_rest
//     and rest_bends_from_pose. Refusals: no world, a bad builder, an index out of range, a duplicate vertex
//     in a bend, more than 65536 vertices, the body limit.
AVER_PHYS_API int32_t aver_phys_sb_builder_finish(int32_t b, float cx, float cy, float cz,
                                                  int32_t replaceBody, int32_t stateSrcBody,
                                                  const int32_t* stateMap,
                                                  int32_t* outEdgeSlot, int32_t* outBendSlot);

AVER_PHYS_API int32_t aver_phys_sb_builder_cancel(int32_t b);

// ---- State and in-place edits ------------------------------------------------------------------------

// Vertex positions (world cm) and velocities (cm/s), vertex order of the builder. Either out may be null.
// Returns the vertices written, 0 for a body this ABI did not build.
AVER_PHYS_API int32_t aver_phys_sb_state(int32_t body, float* outPosXyz, float* outVelXyz, int32_t maxVertices);

// How many vertices a built body has (0 = not one of ours).
AVER_PHYS_API int32_t aver_phys_sb_vertex_count(int32_t body);

// A DENT: edgeSlot[i]'s rest length := restCm[i]. Wakes the body.
AVER_PHYS_API int32_t aver_phys_sb_set_edge_rest(int32_t body, const int32_t* edgeSlot, const float* restCm, int32_t n);

// bendSlot[i]'s rest angle := the current angle, so the constraint residual is zero in the current pose.
// Without it a dented sheet springs flat again. Wakes the body.
AVER_PHYS_API int32_t aver_phys_sb_rest_bends_from_pose(int32_t body, const int32_t* bendSlot, int32_t n);

// Adds velocity to the vertices within radiusCm of centreCm (world cm): speedCmS * (1 - d/r)^falloffPower
// along dirUnit, blended toward radial (0 directional .. 1 radial). Kinematic vertices are skipped. Wakes.
AVER_PHYS_API int32_t aver_phys_sb_impulse(int32_t body, const float* centreCm, float radiusCm, const float* dirUnit,
                                           float speedCmS, float radialBlend, float falloffPower);

// Adds dvXyz[i] (world cm/s, 3 floats each) to vertex[i] of a body this ABI built: the indexed velocity add behind
// soft-body force fields. Kinematic vertices (inverse mass 0) are skipped, an index out of range or a non-finite
// value refuses the whole call (nothing is written), and the body is activated only when some component applied
// is non-zero. Clears the record's frozen flag like impulse does. n == 0 succeeds and does nothing.
AVER_PHYS_API int32_t aver_phys_sb_add_velocity(int32_t body, const int32_t* vertex, const float* dvXyz, int32_t n);

// Pushes a skin palette (jointCount row-major 4x4 matrices, engine order) composed with the entity's world
// matrix: M' = T(com) * skin[j] * entityWorld * T(-com), so the cloth simulates in a world-aligned frame and
// the entity's rotation and scale reach the skinned constraints. entityWorld16 null = identity. hardSkin
// snaps every skinned vertex (first frame, a teleport). The body keeps the palette for rebuilds.
AVER_PHYS_API int32_t aver_phys_sb_skin_world(int32_t body, const float* jointMatrices, int32_t jointCount,
                                              const float* entityWorld16, int32_t hardSkin);

// Wakes or deactivates; a deactivated body keeps its pose and costs nothing.
AVER_PHYS_API int32_t aver_phys_sb_set_active(int32_t body, int32_t active);
AVER_PHYS_API int32_t aver_phys_sb_is_active(int32_t body);

// Applies AVER_PHYS_SB_P_* live (NaN = unchanged).
AVER_PHYS_API int32_t aver_phys_sb_set_params(int32_t body, const float* p, int32_t n);

// Bodies in this ABI's registry (a test reads it to prove shutdown leaves none).
AVER_PHYS_API int32_t aver_phys_sb_count(void);

// ---- Aero (wind and drag) ----------------------------------------------------------------------------
// Evaluated inside the fixed loop before the solver, next to buoyancy, so it is frame-rate independent.
// Per triangle: v_rel = wind(p, step) - mean(v); F = 0.5 * 1.2 * A * (drag * |v_n| v_n n + lift * (v_rel -
// v_n n) |v_rel|), shared over the corners by inverse mass; kinematic vertices are skipped.

// drag 0 = off (0..2). flags bit0: keep the body awake even with no wind.
AVER_PHYS_API int32_t aver_phys_sb_set_aero(int32_t body, float drag, float lift, int32_t flags);

// The world's wind (cm/s): base + gustSpeed * sin(2 pi gustHz t + phase(p)) along the base direction +
// turbulence (cm/s) from an integer-hash noise of the 100 cm cell and the step. No clock, no rand: the
// same run twice is bit identical. A body with aero and non-zero wind is kept awake.
AVER_PHYS_API void aver_phys_sb_set_wind(float vx, float vy, float vz, float gustSpeed, float gustHz, float turbulence);

// ---- Impacts -----------------------------------------------------------------------------------------
// Polled, cleared at the top of aver_phys_step like contacts. Only bodies with AVER_PHYS_SB_P_IMPACTS set
// report. One record per (soft body, other body) per step; below 1 cm/s closing is dropped.

AVER_PHYS_API int32_t aver_phys_sb_impact_count(void);
// outOtherMassKg is 0 for a static or kinematic other. Any out pointer may be null.
AVER_PHYS_API int32_t aver_phys_sb_impact_get(int32_t index, int32_t* outSoftBody, int32_t* outOtherBody,
                                              float* outPoint3, float* outNormal3, float* outClosingCmS,
                                              float* outOtherMassKg, int32_t* outContactVertices);

#ifdef __cplusplus
} // extern "C"
#endif
