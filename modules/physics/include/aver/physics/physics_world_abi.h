#pragma once
// Aver.Physics world configuration: Jolt solver settings, init-time limits, build facts, stats and profiler.
// Units are the engine's (cm, cm/s, s, degrees), never Jolt's metres. Setters return 1/0; the reason is in aver_phys_last_error().
// Add settings to the table in PhysicsConfig.cpp (the name list in OcProject.cpp is checked against it).
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

#define AVER_PHYS_WORLD_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_world_abi_version(void);

#define AVER_PHYS_PRECISION_SINGLE 1
#define AVER_PHYS_PRECISION_DOUBLE 2
AVER_PHYS_API int32_t  aver_phys_precision(void);
#define AVER_PHYS_BUILD_DEBUG_RENDERER   0x01u
#define AVER_PHYS_BUILD_PROFILER         0x02u
#define AVER_PHYS_BUILD_CROSS_PLATFORM   0x04u
#define AVER_PHYS_BUILD_DETERMINISM_LOG  0x08u
#define AVER_PHYS_BUILD_STATS_TRACKING   0x10u
#define AVER_PHYS_BUILD_ASSERTS          0x20u
AVER_PHYS_API uint32_t aver_phys_build_flags(void);
/* UTF-8, NUL-terminated; returns the length needed including the NUL. Two peers compare it before exchanging state. */
AVER_PHYS_API int32_t  aver_phys_build_string(uint8_t* outUtf8, int32_t cap);

/* Process-wide: survives aver_phys_shutdown and can be set before aver_phys_init. Defaults are Jolt's, in cm.
   Units: count = VELOCITY_STEPS, POSITION_STEPS, COLLISION_STEPS (per fixed step), MAX_PAIRS_IN_FLIGHT, STEP_LISTENER_BATCH(ES_PER_JOB);
   cm = SPECULATIVE_DISTANCE, PENETRATION_SLOP, MAX_PENETRATION, MANIFOLD_TOLERANCE, PAIR_CACHE_DELTA_POS, LAMBDA_PRESERVE_DIST;
   cm/s = MIN_RESTITUTION_SPEED, SLEEP_SPEED; s = SLEEP_TIME; degrees = PAIR_CACHE_DELTA_ROT, CONTACT_NORMAL_DELTA;
   fraction = BAUMGARTE, LINEAR_CAST_THRESHOLD (of inner radius), LINEAR_CAST_MAX_PENETRATION; the rest are bool. */
#define AVER_PHYS_WS_VELOCITY_STEPS              0   /* below 2 friction stops working */
#define AVER_PHYS_WS_POSITION_STEPS              1
#define AVER_PHYS_WS_COLLISION_STEPS             2
#define AVER_PHYS_WS_BAUMGARTE                   3
#define AVER_PHYS_WS_SPECULATIVE_DISTANCE        4
#define AVER_PHYS_WS_PENETRATION_SLOP            5
#define AVER_PHYS_WS_MAX_PENETRATION             6
#define AVER_PHYS_WS_MANIFOLD_TOLERANCE          7
#define AVER_PHYS_WS_LINEAR_CAST_THRESHOLD       8
#define AVER_PHYS_WS_LINEAR_CAST_MAX_PENETRATION 9
#define AVER_PHYS_WS_MIN_RESTITUTION_SPEED       10
#define AVER_PHYS_WS_SLEEP_TIME                  11
#define AVER_PHYS_WS_SLEEP_SPEED                 12
#define AVER_PHYS_WS_ALLOW_SLEEPING              13
#define AVER_PHYS_WS_DETERMINISTIC               14  /* off is faster and BREAKS snapshots and replay */
#define AVER_PHYS_WS_WARM_START                  15
#define AVER_PHYS_WS_BODY_PAIR_CACHE             16
#define AVER_PHYS_WS_MANIFOLD_REDUCTION          17
#define AVER_PHYS_WS_LARGE_ISLAND_SPLITTER       18
#define AVER_PHYS_WS_CHECK_ACTIVE_EDGES          19
#define AVER_PHYS_WS_MAX_PAIRS_IN_FLIGHT         20
#define AVER_PHYS_WS_STEP_LISTENER_BATCH         21
#define AVER_PHYS_WS_STEP_LISTENER_BATCHES_PER_JOB 22
#define AVER_PHYS_WS_PAIR_CACHE_DELTA_POS        23
#define AVER_PHYS_WS_PAIR_CACHE_DELTA_ROT        24
#define AVER_PHYS_WS_CONTACT_NORMAL_DELTA        25
#define AVER_PHYS_WS_LAMBDA_PRESERVE_DIST        26
#define AVER_PHYS_WS_COUNT                       27
AVER_PHYS_API int32_t     aver_phys_setting_count(void);
AVER_PHYS_API const char* aver_phys_setting_name(int32_t key);
AVER_PHYS_API const char* aver_phys_setting_unit(int32_t key);                        /* "count", "cm", "cm/s", "s", "degrees", "fraction", "bool"; NULL if out of range */
AVER_PHYS_API int32_t     aver_phys_setting_find(const char* name);                   /* case-insensitive; -1 if unknown */
AVER_PHYS_API int32_t     aver_phys_setting_range(int32_t key, float* outMin, float* outMax, float* outDefault);   /* outs may be NULL; 0 for a bad key */
AVER_PHYS_API int32_t     aver_phys_setting_set(int32_t key, float value);            /* clamps into range; NaN and a bad key are refused (0) */
AVER_PHYS_API int32_t     aver_phys_setting_get(int32_t key, float* outValue);
AVER_PHYS_API int32_t     aver_phys_setting_reset(int32_t key);                       /* -1 = every setting */
AVER_PHYS_API int32_t     aver_phys_setting_get_all(float* outValues, int32_t n);     /* min(n, COUNT) written; returns the count written */

/* Pending config, read by the next aver_phys_init and aver_phys_rebuild. Fixed for a world's lifetime. */
#define AVER_PHYS_CF_MAX_BODIES      0     /* Jolt's BodyID limit is 0x7fffff */
#define AVER_PHYS_CF_MAX_BODY_PAIRS  1
#define AVER_PHYS_CF_MAX_CONTACTS    2
#define AVER_PHYS_CF_TEMP_MB         3     /* contact scratch; Jolt aborts if a frame needs more */
#define AVER_PHYS_CF_THREADS         4     /* -1 auto; 0 = single-threaded; N = N workers */
#define AVER_PHYS_CF_COUNT           5
AVER_PHYS_API const char* aver_phys_config_name(int32_t key);                         /* NULL if out of range */
AVER_PHYS_API int32_t     aver_phys_config_range(int32_t key, float* outMin, float* outMax, float* outDefault);
AVER_PHYS_API int32_t     aver_phys_config_set(int32_t key, float value);             /* pending value, with or without a world; clamps; NaN refused */
AVER_PHYS_API int32_t     aver_phys_config_get(int32_t key, float* outValue);         /* pending value: what the NEXT world uses */
AVER_PHYS_API int32_t     aver_phys_config_active(int32_t key, float* outValue);      /* what the LIVE world was built with; 0 with no world */
AVER_PHYS_API int32_t     aver_phys_config_reset(int32_t key);                        /* pending back to default; -1 = every key */
AVER_PHYS_API int32_t     aver_phys_config_differs(void);                             /* 1 when a world exists and pending != active (editor restart badge) */
/* Shutdown and init with the pending config. Refused (0, logged) unless the world is empty; see aver_phys_world_empty.
   Settings and shape handles survive; per-world tables do not. */
AVER_PHYS_API int32_t     aver_phys_rebuild(void);
/* 1 when aver_phys_rebuild would be accepted (no world, or an empty one). */
AVER_PHYS_API int32_t     aver_phys_world_empty(void);

/* Snapshot of the last fixed steps, cheap enough for a HUD. */
#define AVER_PHYS_ST_BODIES              0
#define AVER_PHYS_ST_MAX_BODIES          1
#define AVER_PHYS_ST_STATIC              2
#define AVER_PHYS_ST_DYNAMIC             3
#define AVER_PHYS_ST_ACTIVE_DYNAMIC      4
#define AVER_PHYS_ST_KINEMATIC           5
#define AVER_PHYS_ST_ACTIVE_KINEMATIC    6
#define AVER_PHYS_ST_SOFT                7
#define AVER_PHYS_ST_ACTIVE_SOFT         8
#define AVER_PHYS_ST_CONSTRAINTS         9
#define AVER_PHYS_ST_CHARACTERS          10
#define AVER_PHYS_ST_VEHICLES            11
#define AVER_PHYS_ST_STEPS_LAST_CALL     12
#define AVER_PHYS_ST_STEPS_TOTAL         13
#define AVER_PHYS_ST_STEP_MS_LAST        14
#define AVER_PHYS_ST_STEP_MS_AVG         15  /* EMA, 0.5 s time constant */
#define AVER_PHYS_ST_STEP_MS_MAX         16  /* window since aver_phys_stats_reset_window */
#define AVER_PHYS_ST_CALL_MS_LAST        17
#define AVER_PHYS_ST_CONTACTS_LAST_CALL  18
#define AVER_PHYS_ST_UPDATE_ERRORS       19  /* bit set of EPhysicsUpdateError (1 manifold cache, 2 body pair cache, 4 contact constraints); cumulative */
#define AVER_PHYS_ST_UPDATE_ERROR_STEPS  20
#define AVER_PHYS_ST_TEMP_PEAK_BYTES     21
#define AVER_PHYS_ST_TEMP_SIZE_BYTES     22
#define AVER_PHYS_ST_WORKER_THREADS      23
#define AVER_PHYS_ST_ACCUMULATOR_S       24
#define AVER_PHYS_ST_TOPOLOGY_EPOCH      25
#define AVER_PHYS_ST_COUNT               26
AVER_PHYS_API int32_t aver_phys_stats(double* outValues, int32_t n);          /* min(n, COUNT) written; zeros with no world */
AVER_PHYS_API int32_t aver_phys_stats_reset_window(void);                      /* clears max, temp peak and update-error fields */
/* Diagnostics preset (AVER_PHYSICS_DIAGNOSTICS). kind: 1 broad-phase, 2 per-body, 3 narrow-phase stats (engine log),
   4 Jolt profiler as HTML in the working directory. Returns 1 when produced; 0 otherwise (Unsupported in a build
   without the preset, NotInitialised with no world, OutOfRange for another kind). Always exported for the C# parity test. */
AVER_PHYS_API int32_t aver_phys_diag_report(int32_t kind);
/* Directory for Jolt's detlog.txt (determinism log, preset only). NULL or "" = working directory. Applies to the next world. */
AVER_PHYS_API int32_t aver_phys_diag_log_dir(const char* dirUtf8);

/* Jolt JPH_PROFILE scopes plus fixed-step stages as one tree. Off until enabled; needs AVER_PHYS_BUILD_PROFILER.
   Rows publish every 60 fixed steps, depth first. Worker-job time is summed CPU time. Survives a rebuild; freezes when disabled. */
AVER_PHYS_API int32_t aver_phys_profile_enable(int32_t on);                       /* 1 on success; 0 with no hook (Unsupported) */
AVER_PHYS_API int32_t aver_phys_profile_is_enabled(void);
AVER_PHYS_API int32_t aver_phys_profile_count(void);
/* outValues (4 doubles): inclusive ms per step, exclusive ms per step, max per-step inclusive ms, calls per step.
   outParentDepth (2 ints): parent row (-1 = root), depth. Any out may be NULL. 0 for a bad index. */
AVER_PHYS_API int32_t aver_phys_profile_get(int32_t index, uint8_t* outNameUtf8, int32_t nameCap, double* outValues, int32_t* outParentDepth);
AVER_PHYS_API int32_t aver_phys_profile_reset(void);                              /* drops the table and restarts the window */
/* "Name 1.2 ms | Name 0.8 ms | Name 0.5 ms": three largest exclusive scopes of the last window. Length incl. NUL. */
AVER_PHYS_API int32_t aver_phys_profile_summary(uint8_t* outUtf8, int32_t cap);

#ifdef __cplusplus
}
#endif
