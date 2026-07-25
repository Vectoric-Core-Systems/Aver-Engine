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

#ifdef __cplusplus
}
#endif
