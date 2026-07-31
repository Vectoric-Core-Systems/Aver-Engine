#pragma once
// Aver.Physics — the plain-C ABI. Setters return 1/0 and 0 is always an invalid handle.
//
// Everything here is in the ENGINE's contract, never Jolt's: centimetres, +X forward, +Y right,
// +Z up, left-handed. No Jolt type crosses this boundary.
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
// Change the fixed step. Set it BEFORE bodies exist; it changes what every tuned velocity means.
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
// A capsule that walks, is pushed out of geometry, and does not fall through the floor. Swept and
// resolved rather than simulated as a rigid body.

// Create a character capsule. `radius` and `height` in centimetres; `height` is the TOTAL capsule
// height including both caps, so a 180cm character is 180, not the cylinder part.
AVER_PHYS_API int32_t aver_phys_character_create(float radius, float height,
                                                 float x, float y, float z);
// Destroy a character.
AVER_PHYS_API int32_t aver_phys_character_destroy(int32_t ch);

// Set the velocity the character WANTS, cm/s, engine axes. Horizontal comes from input; the vertical
// component is managed by the simulation unless you set it -- which is how a jump is expressed.
AVER_PHYS_API int32_t aver_phys_character_set_velocity(int32_t ch, float vx, float vy, float vz);
// Read a character's velocity into a caller-owned float[3].
AVER_PHYS_API int32_t aver_phys_character_velocity(int32_t ch, float* outXyz);
// Read a character's position into a caller-owned float[3].
AVER_PHYS_API int32_t aver_phys_character_position(int32_t ch, float* outXyz);
// Teleport a character.
AVER_PHYS_API int32_t aver_phys_character_set_position(int32_t ch, float x, float y, float z);

// 1 while standing on ground steep enough to hold. The thing a jump has to ask before it fires.
AVER_PHYS_API int32_t aver_phys_character_grounded(int32_t ch);

// ---- Arbitrary collision geometry ------------------------------------------------------------------
// Raw arrays in the engine's centimetres, laid out xyz,xyz,... -- not a mesh handle.

// A convex hull wrapped around `count` points. The usual choice for a dynamic prop.
AVER_PHYS_API int32_t aver_phys_add_convex_hull(const float* pointsXyz, int32_t count,
                                                float cx, float cy, float cz,
                                                int32_t dynamic, float massKg);

// A triangle mesh: exact geometry, and STATIC ONLY -- that is Jolt's rule. `indices` is 3 per triangle.
AVER_PHYS_API int32_t aver_phys_add_mesh(const float* verticesXyz, int32_t vertexCount,
                                         const int32_t* indices, int32_t indexCount,
                                         float cx, float cy, float cz);

// A heightfield: `samples` is a row-major sampleCount x sampleCount grid of heights in centimetres,
// spaced `spacingCm` apart. Static, like a mesh. Every sample is used; sampleCount has no alignment
// requirement.
//
// GRID PLACEMENT: sample (column x, row y) lands at engine (cx - y*spacingCm, cy + x*spacingCm,
// cz + height). A column runs along engine +Y and a row along engine -X, so (cx, cy, cz) names the
// field's +X corner, not its minimum corner. Use modules/landscape/PhysicsBridge.hpp rather than
// re-deriving the transpose and flip a column->+X, row->+Y grid needs.
AVER_PHYS_API int32_t aver_phys_add_heightfield(const float* samples, int32_t sampleCount,
                                                float spacingCm,
                                                float cx, float cy, float cz);

// ---- Sensors (triggers) --------------------------------------------------------------------------
// A sensor is a body that DETECTS overlap without pushing anything, and reports through the overlap
// event queue below rather than by blocking movement.

// A box-shaped trigger volume.
AVER_PHYS_API int32_t aver_phys_add_sensor_box(float cx, float cy, float cz,
                                               float hx, float hy, float hz);
// A sphere-shaped trigger volume.
AVER_PHYS_API int32_t aver_phys_add_sensor_sphere(float cx, float cy, float cz, float radius);

// ---- Contact and overlap events ------------------------------------------------------------------
// POLLED, not called back. Both queues are cleared at the START of each aver_phys_step, so what you
// read describes the step that just ran. Drain them in a PostPhysics tick.

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

// ---- Queries -------------------------------------------------------------------------------------

// Cast a ray from `o` along `d` for `maxDistCm`. Returns the hit body handle, or 0 for a miss.
// `outPoint` / `outNormal` are float[3] and are only written on a hit.
// Jolt documents broadphase queries as NOT deterministic between equidistant bodies.
AVER_PHYS_API int32_t aver_phys_raycast(float ox, float oy, float oz,
                                        float dx, float dy, float dz,
                                        float maxDistCm, float* outPoint, float* outNormal);

// Every body whose shape overlaps a sphere. Writes up to `maxBodies` handles into `outBodies` and
// returns how many were WRITTEN, so a result equal to maxBodies means the list was truncated.
AVER_PHYS_API int32_t aver_phys_overlap_sphere(float x, float y, float z, float radius,
                                               int32_t* outBodies, int32_t maxBodies);

// Sweep a sphere along a direction and report the first thing it touches. Unlike a ray, this has
// THICKNESS. Returns the hit body handle, or 0 for a clear sweep.
AVER_PHYS_API int32_t aver_phys_sphere_cast(float ox, float oy, float oz,
                                            float dx, float dy, float dz,
                                            float maxDistCm, float radius,
                                            float* outPoint, float* outNormal);

#ifdef __cplusplus
}
#endif
