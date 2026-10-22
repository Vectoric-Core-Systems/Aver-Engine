#pragma once
// Aver.Physics -- THE WORLD'S OWN CONFIGURATION: the solver settings Jolt exposes, the limits a world is
// built with, the build facts, and the numbers a profiler or a HUD wants (wave 11, WORLD_STATE_DEBUG).
// Additive: nothing in physics_abi.h changed, and a caller that never touches this header gets the world it
// always got (aver_phys_init() with no arguments reads the pending config below, whose defaults are the
// literals PhysicsWorld.cpp used to carry).
//
// UNITS AND AXES ARE THE ENGINE'S, as everywhere in this module: centimetres, cm/s, seconds, degrees where
// the name or the unit string says so. Jolt's metres never cross this boundary; the settings table converts.
// Setters return 1/0 and aver_phys_last_error() (physics_abi.h) is the reason channel. Main thread, between
// steps, like the rest of the ABI.
//
// ONE TABLE DRIVES EVERYTHING: the live settings below, the PHYSICS.* project keys, the phys.* console
// variables, the C# PhysicsSetting enum and the editor's settings page are all produced from the table in
// PhysicsConfig.cpp, so a setting added there appears everywhere and the name list in OcProject.cpp (which may
// not depend on this module) is checked against it by tests/game/ProjectPhysicsApplyTest.
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

/* ---- build facts: callable with no world ---- */
#define AVER_PHYS_PRECISION_SINGLE 1
#define AVER_PHYS_PRECISION_DOUBLE 2
AVER_PHYS_API int32_t  aver_phys_precision(void);
#define AVER_PHYS_BUILD_DEBUG_RENDERER   0x01u
#define AVER_PHYS_BUILD_PROFILER         0x02u   /* the external profiler hook is compiled in */
#define AVER_PHYS_BUILD_CROSS_PLATFORM   0x04u
#define AVER_PHYS_BUILD_DETERMINISM_LOG  0x08u
#define AVER_PHYS_BUILD_STATS_TRACKING   0x10u
#define AVER_PHYS_BUILD_ASSERTS          0x20u
AVER_PHYS_API uint32_t aver_phys_build_flags(void);
/* Jolt's configuration string plus " aver-phys-state N", UTF-8, NUL-terminated. Returns the length needed
   including the NUL; writes at most `cap` bytes (always terminated when cap > 0). `outUtf8` may be NULL to ask
   for the length. Two peers compare this before they exchange state. */
AVER_PHYS_API int32_t  aver_phys_build_string(uint8_t* outUtf8, int32_t cap);

/* ---- live solver settings: JPH::PhysicsSettings and the Update call ----
   Stored process-wide, so they can be set before aver_phys_init and survive aver_phys_shutdown. Defaults are
   Jolt's own, converted to centimetres; a world that never sets one steps exactly as before. A set applies to
   the live world at once. */
#define AVER_PHYS_WS_VELOCITY_STEPS              0   /* count     2..64     10   below 2 friction stops working (Jolt) */
#define AVER_PHYS_WS_POSITION_STEPS              1   /* count     1..32     2 */
#define AVER_PHYS_WS_COLLISION_STEPS             2   /* count per fixed step 1..8   1   the Update argument */
#define AVER_PHYS_WS_BAUMGARTE                   3   /* fraction  0..1      0.2 */
#define AVER_PHYS_WS_SPECULATIVE_DISTANCE        4   /* cm        0.1..20   2 */
#define AVER_PHYS_WS_PENETRATION_SLOP            5   /* cm        0.01..10  2 */
#define AVER_PHYS_WS_MAX_PENETRATION             6   /* cm        1..200    20 */
#define AVER_PHYS_WS_MANIFOLD_TOLERANCE          7   /* cm        0.001..2  0.1 */
#define AVER_PHYS_WS_LINEAR_CAST_THRESHOLD       8   /* fraction of inner radius 0.05..1  0.75 */
#define AVER_PHYS_WS_LINEAR_CAST_MAX_PENETRATION 9   /* fraction  0..1      0.25 */
#define AVER_PHYS_WS_MIN_RESTITUTION_SPEED       10  /* cm/s      0..1000   100 */
#define AVER_PHYS_WS_SLEEP_TIME                  11  /* s         0.05..60  0.5 */
#define AVER_PHYS_WS_SLEEP_SPEED                 12  /* cm/s      0.1..100  3 */
#define AVER_PHYS_WS_ALLOW_SLEEPING              13  /* bool      1 */
#define AVER_PHYS_WS_DETERMINISTIC               14  /* bool      1   off is faster and BREAKS snapshots and replay */
#define AVER_PHYS_WS_WARM_START                  15  /* bool      1 */
#define AVER_PHYS_WS_BODY_PAIR_CACHE             16  /* bool      1 */
#define AVER_PHYS_WS_MANIFOLD_REDUCTION          17  /* bool      1 */
#define AVER_PHYS_WS_LARGE_ISLAND_SPLITTER       18  /* bool      1 */
#define AVER_PHYS_WS_CHECK_ACTIVE_EDGES          19  /* bool      1 */
#define AVER_PHYS_WS_MAX_PAIRS_IN_FLIGHT         20  /* count     256..262144  16384 */
#define AVER_PHYS_WS_STEP_LISTENER_BATCH         21  /* count     1..64     8 */
#define AVER_PHYS_WS_STEP_LISTENER_BATCHES_PER_JOB 22 /* count    1..1000000 1 */
#define AVER_PHYS_WS_PAIR_CACHE_DELTA_POS        23  /* cm        0.001..10 0.1 */
#define AVER_PHYS_WS_PAIR_CACHE_DELTA_ROT        24  /* degrees   0.01..30  2 */
#define AVER_PHYS_WS_CONTACT_NORMAL_DELTA        25  /* degrees   0.1..45   5 */
#define AVER_PHYS_WS_LAMBDA_PRESERVE_DIST        26  /* cm        0.01..50  1 */
#define AVER_PHYS_WS_COUNT                       27
AVER_PHYS_API int32_t     aver_phys_setting_count(void);                              /* AVER_PHYS_WS_COUNT */
AVER_PHYS_API const char* aver_phys_setting_name(int32_t key);                        /* "velocitySteps"; NULL if out of range */
AVER_PHYS_API const char* aver_phys_setting_unit(int32_t key);                        /* "count", "cm", "cm/s", "s", "degrees", "fraction", "bool"; NULL if out of range */
AVER_PHYS_API int32_t     aver_phys_setting_find(const char* name);                   /* case-insensitive; -1 if unknown */
AVER_PHYS_API int32_t     aver_phys_setting_range(int32_t key, float* outMin, float* outMax, float* outDefault);   /* any out may be NULL; 0 for a bad key */
AVER_PHYS_API int32_t     aver_phys_setting_set(int32_t key, float value);            /* clamps into range; NaN and a bad key are refused (0) */
AVER_PHYS_API int32_t     aver_phys_setting_get(int32_t key, float* outValue);
AVER_PHYS_API int32_t     aver_phys_setting_reset(int32_t key);                       /* -1 = every setting */
AVER_PHYS_API int32_t     aver_phys_setting_get_all(float* outValues, int32_t n);     /* min(n, COUNT) written; returns the count written */

/* ---- init-time config: fixed for a world's lifetime ----
   Stored as PENDING config, which the next aver_phys_init (and aver_phys_rebuild) reads. The defaults are the
   literals PhysicsWorld.cpp used to carry. */
#define AVER_PHYS_CF_MAX_BODIES      0     /* 65536   [1024, 8388607]   Jolt's BodyID limit is 0x7fffff */
#define AVER_PHYS_CF_MAX_BODY_PAIRS  1     /* 65536   [1024, 4194304] */
#define AVER_PHYS_CF_MAX_CONTACTS    2     /* 16384   [1024, 1048576] */
#define AVER_PHYS_CF_TEMP_MB         3     /* 10      [1, 1024]   contact scratch; Jolt aborts if a frame needs more */
#define AVER_PHYS_CF_THREADS         4     /* -1 auto (hardware_concurrency - 2, floor 1); 0 = single-threaded; N = N workers  [-1, 64] */
#define AVER_PHYS_CF_COUNT           5
AVER_PHYS_API const char* aver_phys_config_name(int32_t key);                         /* "maxBodies"; NULL if out of range */
AVER_PHYS_API int32_t     aver_phys_config_range(int32_t key, float* outMin, float* outMax, float* outDefault);
AVER_PHYS_API int32_t     aver_phys_config_set(int32_t key, float value);             /* stores the PENDING value, with or without a world; clamps; NaN refused */
AVER_PHYS_API int32_t     aver_phys_config_get(int32_t key, float* outValue);         /* the pending value: what the NEXT world will use */
AVER_PHYS_API int32_t     aver_phys_config_active(int32_t key, float* outValue);      /* what the LIVE world was built with; 0 with no world */
AVER_PHYS_API int32_t     aver_phys_config_reset(int32_t key);                        /* pending back to the default; -1 = every key */
AVER_PHYS_API int32_t     aver_phys_config_differs(void);                             /* 1 when a world exists and pending != active (the editor's "restart" badge) */
/* Shutdown + init with the pending config. With no world it is a plain init. REFUSED (0, reason logged) unless
   the world is EMPTY: no bodies, characters, joints, ragdolls or vehicles. Settings and shape handles
   survive; collision-layer matrix, water volumes and every other per-world table do not (they are empty anyway). */
AVER_PHYS_API int32_t     aver_phys_rebuild(void);
/* 1 when aver_phys_rebuild would be accepted: no world at all, or a world with no bodies, characters, joints,
   ragdolls or vehicles. The hosts ask this before they rebuild for a project's limits, and the editor's page
   enables its "Rebuild world" button on it. */
AVER_PHYS_API int32_t     aver_phys_world_empty(void);

/* ---- stats: a snapshot of the last fixed steps, cheap enough for a HUD ---- */
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
#define AVER_PHYS_ST_STEP_MS_LAST        14  /* wall time of the last system.Update */
#define AVER_PHYS_ST_STEP_MS_AVG         15  /* EMA, 0.5 s time constant */
#define AVER_PHYS_ST_STEP_MS_MAX         16  /* window since aver_phys_stats_reset_window */
#define AVER_PHYS_ST_CALL_MS_LAST        17  /* the whole aver_phys_step call */
#define AVER_PHYS_ST_CONTACTS_LAST_CALL  18
#define AVER_PHYS_ST_UPDATE_ERRORS       19  /* bit set of EPhysicsUpdateError: 1 manifold cache, 2 body pair cache, 4 contact constraints; cumulative since reset */
#define AVER_PHYS_ST_UPDATE_ERROR_STEPS  20
#define AVER_PHYS_ST_TEMP_PEAK_BYTES     21
#define AVER_PHYS_ST_TEMP_SIZE_BYTES     22
#define AVER_PHYS_ST_WORKER_THREADS      23  /* 0 = single-threaded */
#define AVER_PHYS_ST_ACCUMULATOR_S       24
#define AVER_PHYS_ST_TOPOLOGY_EPOCH      25
#define AVER_PHYS_ST_COUNT               26
AVER_PHYS_API int32_t aver_phys_stats(double* outValues, int32_t n);          /* min(n, COUNT) written; returns the count; zeros with no world */
AVER_PHYS_API int32_t aver_phys_stats_reset_window(void);                      /* clears the max, the temp peak and the update-error fields; 0 with no world */
/* The diagnostics preset (AVER_PHYSICS_DIAGNOSTICS): 1 = broad-phase stats, 2 = per-body simulation stats, 3 = narrow-phase
   stats (all written to the engine log through Jolt's Trace), 4 = Jolt's internal profiler dumped as HTML to the working
   directory. Returns 1 when the report was produced; 0 (last_error Unsupported, and a log line) in any build without the preset,
   NotInitialised with no world, OutOfRange for another kind. Always exported so the C# parity test sees no conditional ABI. */
AVER_PHYS_API int32_t aver_phys_diag_report(int32_t kind);
/* Where Jolt writes detlog.txt (its determinism log; the preset's AVER_PHYS_BUILD_DETERMINISM_LOG): a directory, or NULL or ""
   for the working directory. Takes effect when the next world is created (the log is opened then). 1 in the preset, 0 elsewhere. */
AVER_PHYS_API int32_t aver_phys_diag_log_dir(const char* dirUtf8);

/* ---- profiler table (phase B): Jolt's own JPH_PROFILE scopes plus the engine's fixed-step stages, as one tree ----
   Needs AVER_PHYS_BUILD_PROFILER; OFF at run time until enabled (a disabled hook costs one relaxed load and a branch
   per scope). Rows are published every 60 fixed steps (the first after 5), depth first so a reader indents by depth.
   Jolt's job scopes run on worker threads: they appear under one "Worker jobs" row whose milliseconds are CPU time
   summed over the workers, not wall time. The table survives a world rebuild; it freezes (is not cleared) when disabled. */
AVER_PHYS_API int32_t aver_phys_profile_enable(int32_t on);                       /* 1 on success; 0 when the build has no hook (last_error Unsupported) */
AVER_PHYS_API int32_t aver_phys_profile_is_enabled(void);
AVER_PHYS_API int32_t aver_phys_profile_count(void);                              /* rows in the last published window */
/* outValues (4 doubles): inclusive ms per fixed step (window average), exclusive ms per step, the largest per-step inclusive ms of
   any call in the window, calls per step. outParentDepth (2 ints): parent row (-1 = root) and depth. Any out may be NULL. 0 for a bad index. */
AVER_PHYS_API int32_t aver_phys_profile_get(int32_t index, uint8_t* outNameUtf8, int32_t nameCap, double* outValues, int32_t* outParentDepth);
AVER_PHYS_API int32_t aver_phys_profile_reset(void);                              /* drops the table and restarts the window */
/* "Name 1.2 ms | Name 0.8 ms | Name 0.5 ms": the three largest exclusive scopes of the last window, for a hitch log line. Returns the
   length needed including the NUL (1 for an empty table); writes at most `cap` bytes, always terminated when cap > 0. outUtf8 may be NULL. */
AVER_PHYS_API int32_t aver_phys_profile_summary(uint8_t* outUtf8, int32_t cap);

#ifdef __cplusplus
}
#endif
