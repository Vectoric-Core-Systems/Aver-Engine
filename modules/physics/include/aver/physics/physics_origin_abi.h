#pragma once
// Aver.Physics -- THE FLOATING ORIGIN'S PHYSICS HALF (LARGE_WORLD, wave 11; docs/plans/wave11/LARGE_WORLD.md 2.3).
//
// Jolt stays single precision and the simulation runs in the SCENE'S frame: the origin-relative f32 centimetres every other
// ABI here already speaks. When the host rebases the floating origin by a delta (a whole number of regions, 1,638,400 cm), the
// scene's root entities move by -delta and so must every body, character and anchor in this world, in the same slot between
// two steps. That is all this header is: nothing in physics_abi.h or any other header changes meaning, there are no double
// precision twins, and a world that never calls anything here is bit-identical to one built before this header existed.
//
// delta is the ORIGIN's move (new minus old), engine axes, centimetres. Every position the world holds changes by -delta. Z is
// never shifted. A delta must be finite, nonzero, a whole number of metres (a multiple of 100 cm) on X and Y, and zero on Z.
// EXACTNESS: a multiple of 16,384 m shifts a position of 8,192 m or more in magnitude without rounding (Sterbenz); a farther
// body lands within half an ULP of its new position (0.625 mm), and nothing near the camera moves by an ULP.
//
// What a shift leaves alone: velocities, impulses, forces, shapes and the shape cache, joint local anchors, a character's ground
// carry. A SLEEPING body stays asleep and an awake one stays awake (it is moved without activation); the sleep timer of an awake
// body restarts, so a body about to sleep waits another half second.
//
// MAIN THREAD, BETWEEN STEPS, like the rest of the ABI. Calls return 1/0 (prepare returns a count or -1) and
// aver_phys_last_error() (physics_abi.h) is the reason channel.
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

#define AVER_PHYS_ORIGIN_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_origin_abi_version(void);

// Checks that a shift by this delta would be accepted and mutates nothing. -1 = refuse (no world, called from inside a step, delta
// not a finite nonzero multiple of 100 cm on X and Y, or a Z component); otherwise the number of objects a shift would move
// (bodies, characters and world anchors).
AVER_PHYS_API int32_t  aver_phys_origin_prepare(double dxCm, double dyCm, double dzCm);
// Moves every object by -delta and runs the registered hooks (physicsOriginHook in PhysicsOrigin.hpp). 1 = done, 0 = refused for
// any reason prepare refuses, with nothing changed.
AVER_PHYS_API int32_t  aver_phys_origin_shift(double dxCm, double dyCm, double dzCm);
// +1 per shift; 0 at init and after aver_phys_rebuild (a new world). 0 with no world.
AVER_PHYS_API uint32_t aver_phys_origin_generation(void);
// The sum of every shift since the world was created: this world's origin in the engine's absolute frame. Three doubles; zeros with no world.
AVER_PHYS_API void     aver_phys_origin_total(double* outXyzCm);
// The last shift, each out optional (NULL skips it): its wall time in milliseconds, then how many bodies and characters it moved,
// the world anchor bodies it moved, the pulleys it rebuilt, and how many of the bodies were static. 0 with no shift yet (and 0 with
// no world); 1 when a shift has run in this world.
AVER_PHYS_API int32_t  aver_phys_origin_stats(float* outMs, int32_t* outBodies, int32_t* outCharacters, int32_t* outAnchors,
                                              int32_t* outPulleysRebuilt, int32_t* outStaticsMoved);

#ifdef __cplusplus
}
#endif
