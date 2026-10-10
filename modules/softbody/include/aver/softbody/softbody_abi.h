#pragma once
// Aver.SoftBody -- the plain-C ABI. Handles are positive int32, 0 is invalid; calls return 1/0 (or a
// count) and never throw. Units are the engine's: centimetres, Newtons, +Z up. No C++ type crosses.
//
// This is the PLASTIC solver (permanent dents, tearing). Elastic soft bodies and cloth are Jolt's:
// aver_phys_softbody_* in physics_abi.h. The two never share a handle.
//
// A statically linked library: the symbols are plain extern "C" with no import/export decoration.
// To expose them to C# through Aver.Physics.dll, re-export them from there; see docs/SOFTBODY.md.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AverSbMaterial {
    float stiffness;          // 0..1
    float axialStiffness;     // N/cm
    float bendForceN;         // yield force
    float breakForceN;
    float plasticStiffness;   // N/cm
    float maxBend;            // cm
    float bendAbsorb;
    float breakAbsorb;
    float hardening;
    float breakStrain;        // 0 = off
    int32_t behavior;         // 0 Deform, 1 Fracture, 2 Shatter
} AverSbMaterial;

typedef struct AverSbConfig {
    float dt;                 // seconds per step; the async worker overrides it with 1/hz
    int32_t substeps;
    int32_t iterations;
    float velocityDamping;
    float maxNodeSpeed;       // cm/s
    float damageRate;
    float breakKick;
    int32_t enableDamage;
    float gravityX, gravityY, gravityZ;
    int32_t gravityAll;
    float settleThresholdCm;
} AverSbConfig;

// Why the last call on this thread failed: an aver::AbiError (core/ErrorCodes.hpp) -- 0 Ok, -1 BadHandle,
// -2 NullPointer, -3 NotInitialised (not built), -4 OutOfRange, -5 Unsupported (frozen topology, async
// running), -6 InvalidArgument. Thread-local and set on success too. The slot is Aver.Core's, so a DLL that
// re-exports this library (Aver.Physics.dll) shares it with its own *_last_error.
int32_t aver_sb_last_error(void);

// A new empty cage with default config. 0 on failure.
int32_t aver_sb_create(void);
void    aver_sb_destroy(int32_t h);

// ---- building (before aver_sb_build) --------------------------------------------------------------
// Fills a material / config with the defaults.
void    aver_sb_default_material(AverSbMaterial* out);
void    aver_sb_default_config(AverSbConfig* out);

// Returns the material index, or -1.
int32_t aver_sb_add_material(int32_t h, const AverSbMaterial* m);
// Returns the particle index, or -1.
int32_t aver_sb_add_particle(int32_t h, float x, float y, float z, int32_t pinned);
// Returns the beam index, or -1.
int32_t aver_sb_add_beam(int32_t h, int32_t a, int32_t b, int32_t material);
// Returns the triangle index, or -1. Missing edges get a beam of `material`.
int32_t aver_sb_add_triangle(int32_t h, int32_t a, int32_t b, int32_t c, int32_t material);
// Re-assigns an existing beam's material (seams, weak lines). 1/0.
int32_t aver_sb_set_beam_material(int32_t h, int32_t beam, int32_t material);
// Freezes the topology. 1/0.
int32_t aver_sb_build(int32_t h);

int32_t aver_sb_set_config(int32_t h, const AverSbConfig* cfg);

// ---- synchronous driving --------------------------------------------------------------------------
// One fixed step. Bit 0 set: something broke. Bit 1 set: settled. -1: bad handle or not built.
int32_t aver_sb_step(int32_t h);
// Moves a pinned (kinematic) particle. 1/0.
int32_t aver_sb_move_particle(int32_t h, int32_t particle, float x, float y, float z);
// A dent: depthCm through the crush curve, along (dx,dy,dz), within radiusCm of (px,py,pz). 1/0.
int32_t aver_sb_impact(int32_t h, float px, float py, float pz, float dx, float dy, float dz,
                       float depthCm, float radiusCm);
// Cuts a beam. 1/0.
int32_t aver_sb_break_beam(int32_t h, int32_t beam);
// Undo all plastic change and tears. 1/0.
int32_t aver_sb_repair(int32_t h);

// ---- readback -------------------------------------------------------------------------------------
int32_t aver_sb_particle_count(int32_t h);
// Writes up to maxParticles xyz triples; returns how many. When the async worker runs, this is the
// last snapshot aver_sb_async_poll took.
int32_t aver_sb_positions(int32_t h, float* outXyz, int32_t maxParticles);
int32_t aver_sb_broken_beams(int32_t h);
int32_t aver_sb_pieces(int32_t h);

// ---- async worker ---------------------------------------------------------------------------------
// Copies the built cage to a worker running at `hz` (15..240). aver_sb_impact / move / break / repair
// are then routed to the worker, and aver_sb_step is refused. 1/0.
int32_t aver_sb_async_start(int32_t h, float hz);
void    aver_sb_async_stop(int32_t h);
// Takes the newest snapshot if there is one: returns the particle count, or -1 when nothing is new.
// `settledOut` (optional) receives 1 when the worker has gone idle.
int32_t aver_sb_async_poll(int32_t h, float* outXyz, int32_t maxParticles, int32_t* settledOut);

#ifdef __cplusplus
}
#endif
