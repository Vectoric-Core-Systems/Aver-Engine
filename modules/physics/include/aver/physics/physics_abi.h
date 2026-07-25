#pragma once
// Aver.Physics — the plain-C ABI, following the engine's idiom: int32/int64/float/const char* cross,
// UTF-8 strings, setters return 1/0, and 0 is ALWAYS an invalid handle.
//
// Everything here is in the ENGINE's contract, never Jolt's: centimetres, +X forward, +Y right,
// +Z up, left-handed. Callers never see a Jolt type, a metre, or a +Y-up vector -- the translation
// happens once, in Convert.hpp, behind this boundary.
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
AVER_PHYS_API void    aver_phys_shutdown(void);
AVER_PHYS_API int32_t aver_phys_ready(void);

// Gravity in cm/s^2, engine axes. Defaults to (0, 0, -980) -- one g, straight down +Z-up.
AVER_PHYS_API void aver_phys_set_gravity(float x, float y, float z);

// Advance by `dt` seconds of real time.
//
// The simulation itself runs at a FIXED step regardless of what is passed here: Jolt's determinism
// guarantee is stated in terms of the same calls in the same order, and a step whose size follows
// the frame rate makes the result a function of machine speed. Leftover time is carried to the next
// call, and a long stall is clamped rather than simulated in full, so a breakpoint does not fire a
// hundred steps at once.
// Returns how many fixed steps actually ran.
AVER_PHYS_API int32_t aver_phys_step(float dt);

// The fixed step, in seconds (1/60 unless set). Set it BEFORE bodies exist; changing it mid-session
// changes the meaning of every tuned velocity in the game.
AVER_PHYS_API float   aver_phys_fixed_step(void);
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

AVER_PHYS_API int32_t aver_phys_remove_body(int32_t body);

// Read a body's transform. `outXyz` / `outQuat` are caller-owned float[3] / float[4]. Returns 0 for
// a dead handle, leaving the outputs untouched.
AVER_PHYS_API int32_t aver_phys_body_position(int32_t body, float* outXyz);
AVER_PHYS_API int32_t aver_phys_body_rotation(int32_t body, float* outQuat);
AVER_PHYS_API int32_t aver_phys_body_velocity(int32_t body, float* outXyz);

AVER_PHYS_API int32_t aver_phys_body_set_position(int32_t body, float x, float y, float z);
AVER_PHYS_API int32_t aver_phys_body_set_velocity(int32_t body, float x, float y, float z);

// How many bodies are live. Cheap, and the one number a smoke test can assert on.
AVER_PHYS_API int32_t aver_phys_body_count(void);

// ---- Character -----------------------------------------------------------------------------------
// A capsule that walks, is pushed out of geometry, and does not fall through the floor. Backed by
// Jolt's CharacterVirtual, which is not a rigid body -- it is swept and resolved, which is what makes
// a character feel controlled rather than simulated.

// `radius` and `height` in centimetres; `height` is the TOTAL capsule height including both caps, so
// a 180cm character is 180, not the cylinder part.
AVER_PHYS_API int32_t aver_phys_character_create(float radius, float height,
                                                 float x, float y, float z);
AVER_PHYS_API int32_t aver_phys_character_destroy(int32_t ch);

// The velocity the character WANTS, cm/s, engine axes. Horizontal comes from input; the vertical
// component is managed by the simulation (gravity, ground snapping) unless you set it -- which is how
// a jump is expressed.
AVER_PHYS_API int32_t aver_phys_character_set_velocity(int32_t ch, float vx, float vy, float vz);
AVER_PHYS_API int32_t aver_phys_character_velocity(int32_t ch, float* outXyz);
AVER_PHYS_API int32_t aver_phys_character_position(int32_t ch, float* outXyz);
AVER_PHYS_API int32_t aver_phys_character_set_position(int32_t ch, float x, float y, float z);

// 1 while standing on ground steep enough to hold. The thing a jump has to ask before it fires.
AVER_PHYS_API int32_t aver_phys_character_grounded(int32_t ch);

// ---- Arbitrary collision geometry ------------------------------------------------------------------
// Box, sphere and capsule cover a blockout. These cover a level.
//
// Both take raw arrays in the engine's centimetres, laid out xyz,xyz,... -- deliberately NOT a mesh
// handle: the engine has no .ocmesh loader yet, so a collider that could only be built from a loaded
// asset would be a door to nowhere. Arrays can be filled from procedural geometry, from a level file,
// or later from a loader, without this ABI changing.

// A convex hull wrapped around `count` points. The usual choice for a dynamic prop: convex shapes
// collide against anything, including each other, and are far cheaper than a mesh.
AVER_PHYS_API int32_t aver_phys_add_convex_hull(const float* pointsXyz, int32_t count,
                                                float cx, float cy, float cz,
                                                int32_t dynamic, float massKg);

// A triangle mesh: exact geometry, and STATIC ONLY -- that is Jolt's rule, not a shortcut here. A
// mesh has no interior, so there is nothing to resolve a penetration against; level geometry is what
// it is for. `indices` is 3 per triangle.
AVER_PHYS_API int32_t aver_phys_add_mesh(const float* verticesXyz, int32_t vertexCount,
                                         const int32_t* indices, int32_t indexCount,
                                         float cx, float cy, float cz);

// A heightfield: `samples` is a row-major sampleCount x sampleCount grid of heights in centimetres,
// spaced `spacingCm` apart, with its corner at (cx, cy, cz). Static, like a mesh. sampleCount must be
// a power of two plus nothing -- Jolt requires a multiple of its block size, so it is rounded down.
AVER_PHYS_API int32_t aver_phys_add_heightfield(const float* samples, int32_t sampleCount,
                                                float spacingCm,
                                                float cx, float cy, float cz);

// ---- Sensors (triggers) --------------------------------------------------------------------------
// A sensor is a body that DETECTS overlap without pushing anything: a pickup volume, a level exit, a
// damage zone. It is a real body in the broad phase, so it costs what a body costs, and it reports
// through the overlap event queue below rather than by blocking movement.

AVER_PHYS_API int32_t aver_phys_add_sensor_box(float cx, float cy, float cz,
                                               float hx, float hy, float hz);
AVER_PHYS_API int32_t aver_phys_add_sensor_sphere(float cx, float cy, float cz, float radius);

// ---- Contact and overlap events ------------------------------------------------------------------
// POLLED, not called back, and that is a deliberate design choice rather than a shortcut.
//
// Jolt invokes its contact listener from SEVERAL WORKER THREADS during the step, in an order it
// explicitly documents as non-deterministic. Calling managed code from there would mean marshalling
// into the CLR from threads it has never seen, mid-simulation, with gameplay then free to mutate the
// very world being stepped. Recording events into a buffer and letting the game drain them after the
// step keeps every gameplay reaction on the main thread, in a fixed order, at a point where the world
// is safe to touch.
//
// Both queues are cleared at the START of each aver_phys_step, so what you read describes the step
// that just ran. Drain them in a PostPhysics tick.

// Contacts between two solid bodies that began touching this step.
AVER_PHYS_API int32_t aver_phys_contact_count(void);
// `outPoint` / `outNormal` are caller-owned float[3]. Returns 0 for an out-of-range index.
AVER_PHYS_API int32_t aver_phys_contact_get(int32_t index, int32_t* outBodyA, int32_t* outBodyB,
                                            float* outPoint, float* outNormal);

// Sensor overlaps that STARTED or STOPPED this step. `outEntered` is 1 for an enter, 0 for an exit.
AVER_PHYS_API int32_t aver_phys_overlap_count(void);
AVER_PHYS_API int32_t aver_phys_overlap_get(int32_t index, int32_t* outSensor, int32_t* outBody,
                                            int32_t* outEntered);

// ---- Queries -------------------------------------------------------------------------------------

// Cast a ray from `o` along `d` for `maxDistCm`. Returns the hit body handle, or 0 for a miss.
// `outPoint` / `outNormal` are float[3] and are only written on a hit.
//
// NOTE: Jolt documents broadphase queries as NOT deterministic -- the broad phase can be modified
// from several threads -- so a gate may assert on a hit's existence and position, but must not
// depend on WHICH of several equidistant bodies comes back.
AVER_PHYS_API int32_t aver_phys_raycast(float ox, float oy, float oz,
                                        float dx, float dy, float dz,
                                        float maxDistCm, float* outPoint, float* outNormal);

// Every body whose shape overlaps a sphere. Writes up to `maxBodies` handles into `outBodies` and
// returns how many were WRITTEN -- so a result equal to maxBodies means the list was truncated and
// the caller should ask again with a bigger buffer rather than assume it saw everything.
//
// The natural query for "what is within blast radius" or "what is standing on this plate".
AVER_PHYS_API int32_t aver_phys_overlap_sphere(float x, float y, float z, float radius,
                                               int32_t* outBodies, int32_t maxBodies);

// Sweep a sphere along a direction and report the first thing it touches. Unlike a ray, this has
// THICKNESS: it is what a projectile, a camera boom or a step-up probe actually needs, because a ray
// slips through gaps a moving object could never fit through.
// Returns the hit body handle, or 0 for a clear sweep.
AVER_PHYS_API int32_t aver_phys_sphere_cast(float ox, float oy, float oz,
                                            float dx, float dy, float dz,
                                            float maxDistCm, float radius,
                                            float* outPoint, float* outNormal);

#ifdef __cplusplus
}
#endif
