#pragma once
// Aver.Physics: state recording. Snapshots of the whole simulation, exact stepping and the state hash. A world that never calls these steps as before.
// A snapshot holds what the simulation derives between inputs (bodies, contact cache, constraints, characters, engine-side state).
// Inputs (pedals, forces, velocities, kinematic targets, tune values, wind) are not state; a rollback layer re-issues them each resimulated tick.
// Restore is all-or-nothing: it validates first (same bodies, characters, vehicles, joints and ragdolls; no constraint added or removed),
// then calls Jolt. A refusal changes nothing and aver_phys_snapshot_last_result says why.
// A snapshot is not a save file: handles change on a level reload. Read and import are for transport (a netcode peer, a test).
// All calls are main-thread, between steps. Returns 1/0 with aver_phys_last_error, except the restore family (its own result getter).
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

#define AVER_PHYS_STATE_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_state_abi_version(void);

/* what a snapshot holds (aver_phys_snapshot_capture's flags; 0 means AVER_PHYS_SNAP_ALL) */
#define AVER_PHYS_SNAP_BODIES       0x01u
#define AVER_PHYS_SNAP_CONTACTS     0x02u   /* without it the next step re-detects contacts and loses its warm start */
#define AVER_PHYS_SNAP_CONSTRAINTS  0x04u
#define AVER_PHYS_SNAP_CHARACTERS   0x08u   /* CharacterVirtual state, which Jolt's own SaveState does not include */
#define AVER_PHYS_SNAP_ENGINE       0x10u
#define AVER_PHYS_SNAP_STATICS      0x20u   /* off by default: ~48 bytes each, 65k statics = 3 MB per snapshot */
#define AVER_PHYS_SNAP_ALL          0x1Fu

/* restore options (aver_phys_snapshot_restore's `options`) */
#define AVER_PHYS_RESTORE_STRICT    0x01u   /* also refuse when bodies, characters, vehicles or joints were ADDED since the capture */

/* restore results (aver_phys_snapshot_last_result: the outcome of the last capture, restore or import) */
#define AVER_PHYS_RESTORE_OK              0
#define AVER_PHYS_RESTORE_BAD_HANDLE     -1
#define AVER_PHYS_RESTORE_BAD_HEADER     -2
#define AVER_PHYS_RESTORE_BUILD_MISMATCH -3
#define AVER_PHYS_RESTORE_TOPOLOGY       -4
#define AVER_PHYS_RESTORE_CORRUPT        -5   /* the payload does not match its checksum, or Jolt refused the stream */

/* aver_phys_snapshot_info slots */
#define AVER_PHYS_SI_FLAGS             0
#define AVER_PHYS_SI_BODIES            1
#define AVER_PHYS_SI_CHARACTERS        2
#define AVER_PHYS_SI_VEHICLES          3
#define AVER_PHYS_SI_JOINTS            4
#define AVER_PHYS_SI_RAGDOLLS          5
#define AVER_PHYS_SI_TOPOLOGY_EPOCH    6
#define AVER_PHYS_SI_CONSTRAINT_EPOCH  7
#define AVER_PHYS_SI_PRECISION         8
#define AVER_PHYS_SI_STEPS_LOW         9   /* fixed steps since the world was created, at the capture: low 32 bits ... */
#define AVER_PHYS_SI_STEPS_HIGH        10  /* ... and the high 32 */
#define AVER_PHYS_SI_BYTES             11
#define AVER_PHYS_SI_COUNT             12

/* ---- snapshots ---- */

/* `reuse` of a live snapshot overwrites it in place (its buffer keeps its capacity, so a ring of N snapshots allocates N buffers once).
   0 or a dead handle allocates a new one. Returns the handle, or 0. */
AVER_PHYS_API int32_t  aver_phys_snapshot_capture(int32_t reuse, uint32_t flags);
/* 1 on success; 0 and aver_phys_snapshot_last_result() say why, and nothing was applied. On success the event queues are cleared and
   aver_phys_step_total() returns the capture's step count. */
AVER_PHYS_API int32_t  aver_phys_snapshot_restore(int32_t snapshot, uint32_t options);
AVER_PHYS_API int32_t  aver_phys_snapshot_last_result(void);
AVER_PHYS_API int32_t  aver_phys_snapshot_release(int32_t snapshot);               /* 1; 0 for a dead handle */
/* Releases every snapshot of the current world and returns how many there were. Called when a level is unloaded. */
AVER_PHYS_API int32_t  aver_phys_snapshot_release_all(void);
AVER_PHYS_API int32_t  aver_phys_snapshot_count(void);                              /* live snapshots in the current world */
AVER_PHYS_API int32_t  aver_phys_snapshot_bytes(int32_t snapshot);                  /* the blob size; 0 for a dead handle */
/* Copies the blob for disk or network. outData == NULL returns the size; a too-small cap returns 0 (never cut short). */
AVER_PHYS_API int32_t  aver_phys_snapshot_read(int32_t snapshot, uint8_t* outData, int32_t cap);
/* Validates a blob and returns a handle usable with restore; 0 on failure. Needs a world: the snapshot belongs to the world alive at import. */
AVER_PHYS_API int32_t  aver_phys_snapshot_import(const uint8_t* data, int32_t size);
AVER_PHYS_API int32_t  aver_phys_snapshot_info(int32_t snapshot, int32_t* outValues, int32_t n);   /* AVER_PHYS_SI_*; min(n, COUNT) written; returns the count written, 0 for a dead handle */
/* FNV-1a 64 of the payload (the blob without its header, so the capture time and step count are not in it); 0 for a dead handle. */
AVER_PHYS_API uint64_t aver_phys_snapshot_hash(int32_t snapshot);
/* The offset of the first differing payload byte, -1 when equal, -2 for a dead handle. Finds WHERE two runs diverge. */
AVER_PHYS_API int32_t  aver_phys_snapshot_diff(int32_t a, int32_t b);

/* ---- hash and replay ---- */

/* The hash a capture made now with `flags` would have (0 = ALL), without keeping the capture. 0 with no world. */
AVER_PHYS_API uint64_t aver_phys_state_hash(uint32_t flags);
/* Non-NULL: creates (truncating) the file and writes one "step hash" line per fixed step (decimal step number, then 16 hex digits of the
   ALL-parts hash). NULL: stop. Costs about a snapshot per fixed step: a diagnostic, off by default. Closes at world shutdown. */
AVER_PHYS_API int32_t  aver_phys_hash_log(const char* pathOrNull);

/* Runs exactly `steps` (1..600) fixed steps, ignoring and leaving the accumulator. The replay primitive for a rollback layer. */
AVER_PHYS_API int32_t  aver_phys_step_exact(int32_t steps);
AVER_PHYS_API int64_t  aver_phys_step_total(void);                                 /* fixed steps since this world was created (restore sets it back); 0 with no world */
AVER_PHYS_API int32_t  aver_phys_accumulator_set(float seconds);                   /* clamped into [0, 8 fixed steps]; 1/0 */
AVER_PHYS_API float    aver_phys_accumulator_get(void);

/* ---- shape binary cache ----
   A mesh or heightfield shape is mostly its BVH, which costs far more to build than to read back. These calls turn a shape handle into a
   byte blob and back, so a host can keep it on disk. A restore is always a miss (0), never a failure, whatever the mismatch: the caller
   then builds the shape the slow way. Only shapes with no physical-surface materials are saved (a palette names live surface ids). A
   restored heightfield is not editable. The KEY is the caller's: it is stored in the blob and compared on restore. */
#define AVER_PHYS_SHAPECACHE_VERSION 1u
AVER_PHYS_API uint32_t aver_phys_shape_cache_version(void);   /* bumped by any change to unit conversion, winding or the blob layout */
/* 1 when the host should use the cache (default 0: off until measured on real levels). Process-wide; the calls below work whatever it says. */
AVER_PHYS_API int32_t  aver_phys_shape_cache_enable(int32_t on);
AVER_PHYS_API int32_t  aver_phys_shape_cache_enabled(void);
/* aver_phys_add_heightfield's construction minus the body: a shape handle, placed by aver_phys_add_mesh_shape_body. 0 on failure. */
AVER_PHYS_API int32_t  aver_phys_create_heightfield_shape(const float* samples, int32_t sampleCount, float spacingCm);
/* Writes the blob for `shape` under `key`. Returns the bytes written, or with outData == NULL the bytes needed; 0 when the handle is not a
   saveable mesh or heightfield, or cap is smaller than the blob. */
AVER_PHYS_API int32_t  aver_phys_shape_save(int32_t shape, uint64_t key, uint8_t* outData, int32_t cap);
/* Rebuilds a shape from a blob; the new shape handle, or 0 for a miss. Needs a world. */
AVER_PHYS_API int32_t  aver_phys_shape_restore(const uint8_t* data, int32_t size, uint64_t key);

#ifdef __cplusplus
}
#endif
