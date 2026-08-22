#pragma once
// Aver.Physics — the plain-C ABI. Setters return 1/0 and 0 is always an invalid handle.
// Units and axes are the engine's, never Jolt's: centimetres, +X forward, +Y right, +Z up,
// left-handed. No Jolt type crosses this boundary.
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

// ---- World ---------------------------------------------------------------------------------------

// Start the simulation. Idempotent: a second call while running is a no-op that returns 1.
AVER_PHYS_API int32_t aver_phys_init(void);
// Destroy every body and character, then the world.
AVER_PHYS_API void    aver_phys_shutdown(void);
// 1 while a world exists.
AVER_PHYS_API int32_t aver_phys_ready(void);

// Gravity in cm/s^2, engine axes. Defaults to (0, 0, -980) -- one g, straight down +Z-up.
AVER_PHYS_API void aver_phys_set_gravity(float x, float y, float z);

// Advance by `dt` seconds of real time, in fixed steps. Leftover time carries; a long stall is
// clamped. Returns how many fixed steps actually ran.
AVER_PHYS_API int32_t aver_phys_step(float dt);

// The fixed step, in seconds (1/60 unless set).
AVER_PHYS_API float   aver_phys_fixed_step(void);
// Change the fixed step. Set it before any bodies exist.
AVER_PHYS_API int32_t aver_phys_set_fixed_step(float seconds);

// ---- Bodies --------------------------------------------------------------------------------------
// Shapes are described in the engine's units: half-extents and radii in centimetres.

// A box that never moves -- floors, walls, static level geometry.
AVER_PHYS_API int32_t aver_phys_add_static_box(float cx, float cy, float cz,
                                               float hx, float hy, float hz);

// A box that falls and collides. `massKg` <= 0 asks Jolt to derive mass from the shape's volume.
AVER_PHYS_API int32_t aver_phys_add_dynamic_box(float cx, float cy, float cz,
                                                float hx, float hy, float hz, float massKg);

// A sphere that falls and collides.
AVER_PHYS_API int32_t aver_phys_add_dynamic_sphere(float cx, float cy, float cz,
                                                   float radius, float massKg);

// Remove a body from the world and destroy it.
AVER_PHYS_API int32_t aver_phys_remove_body(int32_t body);

// Read a body's transform. `outXyz` / `outQuat` are caller-owned float[3] / float[4]. Returns 0 for
// a dead handle, leaving the outputs untouched.
AVER_PHYS_API int32_t aver_phys_body_position(int32_t body, float* outXyz);
AVER_PHYS_API int32_t aver_phys_body_rotation(int32_t body, float* outQuat);
AVER_PHYS_API int32_t aver_phys_body_velocity(int32_t body, float* outXyz);

// Teleport a body and wake it.
AVER_PHYS_API int32_t aver_phys_body_set_position(int32_t body, float x, float y, float z);
// Set a body's linear velocity, cm/s.
AVER_PHYS_API int32_t aver_phys_body_set_velocity(int32_t body, float x, float y, float z);

// How many bodies are live.
AVER_PHYS_API int32_t aver_phys_body_count(void);

// ---- Character -----------------------------------------------------------------------------------
// A capsule that walks: swept and resolved rather than simulated as a rigid body.

// Create a character capsule. `radius` and `height` in centimetres; `height` is the TOTAL capsule
// height including both caps.
AVER_PHYS_API int32_t aver_phys_character_create(float radius, float height,
                                                 float x, float y, float z);
// Destroy a character.
AVER_PHYS_API int32_t aver_phys_character_destroy(int32_t ch);

// Set the velocity the character WANTS, cm/s, engine axes. The vertical component is managed by the
// simulation unless it is set here.
AVER_PHYS_API int32_t aver_phys_character_set_velocity(int32_t ch, float vx, float vy, float vz);
// Read a character's velocity into a caller-owned float[3].
AVER_PHYS_API int32_t aver_phys_character_velocity(int32_t ch, float* outXyz);
// Read a character's position into a caller-owned float[3].
AVER_PHYS_API int32_t aver_phys_character_position(int32_t ch, float* outXyz);
// Teleport a character.
AVER_PHYS_API int32_t aver_phys_character_set_position(int32_t ch, float x, float y, float z);

// 1 while standing on ground steep enough to hold.
AVER_PHYS_API int32_t aver_phys_character_grounded(int32_t ch);

// ---- Entity association ---------------------------------------------------------------------------
// A body or character is an opaque handle with no notion of "which scene entity this is" -- this is
// the one seam that ties one back to the other, for anything (Raycast, above all) that must report
// not just THAT something was hit but WHAT. Works on a body OR a character handle: the two families
// are drawn from one counter and never collide, so a single call covers both.

// Stamps `handle` (a body OR a character) with `entity`, a scene entity id. 0 clears it back to
// "unowned". Returns 0 for a dead handle. Backed by Jolt's own per-body user-data field, so it needs
// no side table and nothing to invalidate when the body or character is destroyed.
AVER_PHYS_API int32_t aver_phys_set_entity(int32_t handle, int32_t entity);

// ---- Arbitrary collision geometry ------------------------------------------------------------------
// Raw arrays in the engine's centimetres, laid out xyz,xyz,... -- not a mesh handle.

// A convex hull wrapped around `count` points.
AVER_PHYS_API int32_t aver_phys_add_convex_hull(const float* pointsXyz, int32_t count,
                                                float cx, float cy, float cz,
                                                int32_t dynamic, float massKg);

// A triangle mesh, STATIC ONLY. `indices` is 3 per triangle.
AVER_PHYS_API int32_t aver_phys_add_mesh(const float* verticesXyz, int32_t vertexCount,
                                         const int32_t* indices, int32_t indexCount,
                                         float cx, float cy, float cz);

// A static heightfield: `samples` is a row-major sampleCount x sampleCount grid of heights in
// centimetres, spaced `spacingCm` apart.
// Sample (column x, row y) lands at engine (cx - y*spacingCm, cy + x*spacingCm, cz + height).
AVER_PHYS_API int32_t aver_phys_add_heightfield(const float* samples, int32_t sampleCount,
                                                float spacingCm,
                                                float cx, float cy, float cz);

// ---- Sensors (triggers) --------------------------------------------------------------------------
// A sensor detects overlap without pushing anything, and reports through the overlap queue below.

// A box-shaped trigger volume.
AVER_PHYS_API int32_t aver_phys_add_sensor_box(float cx, float cy, float cz,
                                               float hx, float hy, float hz);
// A sphere-shaped trigger volume.
AVER_PHYS_API int32_t aver_phys_add_sensor_sphere(float cx, float cy, float cz, float radius);

// ---- Contact and overlap events ------------------------------------------------------------------
// POLLED, not called back. Both queues are cleared at the start of each aver_phys_step.

// How many contacts between two solid bodies began this step.
AVER_PHYS_API int32_t aver_phys_contact_count(void);
// Read one contact. `outPoint` / `outNormal` are caller-owned float[3]. 0 for an out-of-range index.
AVER_PHYS_API int32_t aver_phys_contact_get(int32_t index, int32_t* outBodyA, int32_t* outBodyB,
                                            float* outPoint, float* outNormal);

// How many sensor overlaps started or stopped this step.
AVER_PHYS_API int32_t aver_phys_overlap_count(void);
// Read one overlap. `outEntered` is 1 for an enter, 0 for an exit.
AVER_PHYS_API int32_t aver_phys_overlap_get(int32_t index, int32_t* outSensor, int32_t* outBody,
                                            int32_t* outEntered);

// ---- Soft bodies ------------------------------------------------------------------------------------
// A deformable mesh: particles held together by distance constraints, colliding with the world.
//
// A SOFT BODY IS A BODY. It is drawn from the same handle counter as everything above, so
// aver_phys_remove_body, aver_phys_set_entity and aver_phys_raycast all work on one unchanged --
// there is no parallel family to keep in step.
//
// WHY JOLT AND NOT A SOLVER OF OUR OWN: Jolt 5.6 ships this, it is already vendored and already
// compiled into this build, and it brings the one thing a hand-written cage solver does not get for
// free -- collision against the real world. The engine's own contribution is the seam: engine units
// and axes on this side, Jolt's on the other, and skinning driven from Aver's animation palette.
//
// WHAT IT DOES NOT DO, so it is not discovered later: Jolt soft bodies are purely ELASTIC. There is
// no plastic deformation, no permanent set, no material yield and no break/tear. A dent that stays
// is a different solver (see docs/recon/softbody-solver.md), not a parameter here.

// Build a soft body from a triangle mesh. Positions are engine centimetres, xyz,xyz,...; `indices`
// is 3 per triangle and gives the FACES, from which the edge constraints are generated.
//
// `invMasses` is optional (NULL = every particle at 1). A particle at inverse mass 0 is PINNED and
// is how a flag stays attached to its pole.
//
// `compliance` is inverse stiffness: 0 is inextensible, larger is stretchier. `pressure` inflates a
// closed mesh from within; 0 for cloth.
AVER_PHYS_API int32_t aver_phys_softbody_create(const float* verticesXyz, int32_t vertexCount,
                                                const int32_t* indices, int32_t indexCount,
                                                const float* invMasses,
                                                float cx, float cy, float cz,
                                                float compliance, float pressure);

// The same, plus SKINNED constraints -- soft body on a skeletal mesh.
//
// Each vertex is tethered to where ordinary bone skinning would have put it, free to move up to
// `maxDistanceCm` away from it and no further. That single number is the whole dial between "this
// is just skinning" (0) and "this is a free-floating cloth that happens to be near a skeleton"
// (large): jiggle, squash and drape all live in between.
//
// `jointIndices` and `jointWeights` are `vertexCount * influences` long, matching the mesh's own
// skinning data. `backStopDistanceCm` keeps a vertex from sinking back through the surface it hangs
// off -- negative disables it.
//
// PASS THE BIND POSE as `verticesXyz`, because that is what the skinning is defined against. The
// body is then driven each frame by aver_phys_softbody_skin.
AVER_PHYS_API int32_t aver_phys_softbody_create_skinned(const float* verticesXyz, int32_t vertexCount,
                                                        const int32_t* indices, int32_t indexCount,
                                                        const float* invMasses,
                                                        const int32_t* jointIndices,
                                                        const float* jointWeights,
                                                        int32_t influences, int32_t jointCount,
                                                        float maxDistanceCm, float backStopDistanceCm,
                                                        float cx, float cy, float cz,
                                                        float compliance);

// Drive a skinned soft body from an animated pose. `jointMatrices` is `jointCount` matrices of 16
// floats, row-major, in the engine's own convention -- exactly what aver::anim::poseToSkinning
// produces, handed over unchanged.
//
// `hardSkin` snaps every particle onto its skinned position instead of constraining toward it: what
// to pass on the first frame, and after a teleport, so the body starts on the character rather than
// flying in from wherever it was.
//
// Call it BEFORE aver_phys_step each frame. Returns 0 for a handle that is not a skinned soft body.
AVER_PHYS_API int32_t aver_phys_softbody_skin(int32_t body, const float* jointMatrices,
                                              int32_t jointCount, int32_t hardSkin);

// How many particles a soft body has, or 0 for any other handle.
AVER_PHYS_API int32_t aver_phys_softbody_vertex_count(int32_t body);

// Read the deformed particle positions into a caller-owned float[maxVertices*3], as engine
// centimetres in WORLD space. Returns how many were written. This is what the renderer draws.
AVER_PHYS_API int32_t aver_phys_softbody_vertices(int32_t body, float* outXyz, int32_t maxVertices);

// ---- Queries -------------------------------------------------------------------------------------

// Cast a ray from `o` along `d` for `maxDistCm`. Returns the hit body handle, or 0 for a miss.
// `outPoint` / `outNormal` / `outEntity` are only written on a hit -- check the RETURN VALUE for
// hit/miss, never `*outEntity` alone. `outEntity` is whatever aver_phys_set_entity last stamped the
// hit handle with, or 0 when nothing ever did: that is a REAL hit against something no entity owns
// (a landscape heightfield today), not a miss, and the two are told apart by the return value being
// non-zero either way.
// A live character IS visible to this query: aver_phys_character_create gives it a broadphase body
// for exactly this reason, so a raycast can identify a character the same way it identifies any
// other body, through this same `handle`/`outEntity` pair.
// Jolt broadphase queries are not deterministic between equidistant bodies.
AVER_PHYS_API int32_t aver_phys_raycast(float ox, float oy, float oz,
                                        float dx, float dy, float dz,
                                        float maxDistCm, float* outPoint, float* outNormal,
                                        int32_t* outEntity);

// Every body whose shape overlaps a sphere. Writes up to `maxBodies` handles into `outBodies` and
// returns how many were written.
AVER_PHYS_API int32_t aver_phys_overlap_sphere(float x, float y, float z, float radius,
                                               int32_t* outBodies, int32_t maxBodies);

// Sweep a sphere along a direction and report the first thing it touches. Returns the hit body
// handle, or 0 for a clear sweep.
AVER_PHYS_API int32_t aver_phys_sphere_cast(float ox, float oy, float oz,
                                            float dx, float dy, float dz,
                                            float maxDistCm, float radius,
                                            float* outPoint, float* outNormal);

#ifdef __cplusplus
}
#endif
