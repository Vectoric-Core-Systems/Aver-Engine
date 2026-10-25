#pragma once
// Aver.Physics -- STATE RECORDING: snapshots of the whole simulation, exact stepping and the state hash.
// A world that never calls anything here steps exactly as before (the only cost is one boolean
// test per fixed step for the hash log).
//
// WHAT A SNAPSHOT IS. A byte blob holding everything the simulation DERIVES between inputs: Jolt's bodies, contact cache and
// constraints (joints, ragdolls, vehicle wheels, engine and gearbox), every character's CharacterVirtual state, and the
// engine-side state Jolt cannot see (the fixed-step accumulator, vehicle driver-aid state, a character's ground carry).
// INPUTS ARE NOT STATE and are never in a snapshot: pedals set this frame, forces, impulses, velocities a game writes, kinematic
// targets, tune values, ragdoll mode and strength, wind. A rollback layer re-issues them for each resimulated tick.
// Statics are left out unless asked for (AVER_PHYS_SNAP_STATICS).
//
// RESTORE IS ALL-OR-NOTHING. It validates first: every captured body must still exist with the same Jolt id, every captured
// character, vehicle, joint and ragdoll must still exist, and no constraint may have been added or removed since (Jolt restores
// constraints by array index and would silently restore the wrong one). Only then does it call Jolt. A refusal changes nothing
// and says why through aver_phys_snapshot_last_result(). Bodies ADDED since the capture are allowed by default and keep their
// current state (AVER_PHYS_RESTORE_STRICT refuses them).
//
// A SNAPSHOT IS NOT A SAVE FILE. Handles change when a level reloads, so a snapshot only restores into a world built the same
// way; aver_phys_snapshot_read / _import exist for transport (a netcode peer, a test), not for saving a game. A snapshot from a
// single-precision build is refused by a double-precision one (BUILD_MISMATCH); aver_phys_build_string is what two peers compare.
//
// UNITS AND CONVENTIONS are the module's: all calls are main-thread and between steps; returns are 1/0 with
// aver_phys_last_error() as the reason channel, except the restore family, whose richer result has its own getter and never
// uses negative returns (the inversion trap ErrorCodes.hpp documents).
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
#define AVER_PHYS_SNAP_BODIES       0x01u   /* dynamic and kinematic bodies, a soft body's vertices included */
#define AVER_PHYS_SNAP_CONTACTS     0x02u   /* the contact cache: without it the next step re-detects contacts and loses its warm start */
#define AVER_PHYS_SNAP_CONSTRAINTS  0x04u   /* joints (a broken one stays broken or un-breaks), ragdolls, vehicle wheels / engine / gearbox */
#define AVER_PHYS_SNAP_CHARACTERS   0x08u   /* CharacterVirtual state, which Jolt's own SaveState does not include */
#define AVER_PHYS_SNAP_ENGINE       0x10u   /* the accumulator, vehicle driver-aid state and a character's ground carry */
#define AVER_PHYS_SNAP_STATICS      0x20u   /* also static bodies (off by default: ~48 bytes each, 65k statics = 3 MB per snapshot) */
#define AVER_PHYS_SNAP_ALL          0x1Fu

/* restore options (aver_phys_snapshot_restore's `options`) */
#define AVER_PHYS_RESTORE_STRICT    0x01u   /* also refuse when bodies, characters, vehicles or joints were ADDED since the capture */

/* restore results (aver_phys_snapshot_last_result: the outcome of the last capture, restore or import) */
#define AVER_PHYS_RESTORE_OK              0
#define AVER_PHYS_RESTORE_BAD_HANDLE     -1   /* no such snapshot, or it belongs to a world that is gone */
#define AVER_PHYS_RESTORE_BAD_HEADER     -2   /* magic, ABI layout or size wrong: not a snapshot, or truncated */
#define AVER_PHYS_RESTORE_BUILD_MISMATCH -3   /* precision, Jolt version, state layout version or determinism build differ */
#define AVER_PHYS_RESTORE_TOPOLOGY       -4   /* a captured body, character, vehicle, joint or ragdoll is gone, or the constraint set changed */
#define AVER_PHYS_RESTORE_CORRUPT        -5   /* the payload does not match its checksum, or Jolt refused the stream */

/* aver_phys_snapshot_info slots */
#define AVER_PHYS_SI_FLAGS             0   /* the AVER_PHYS_SNAP_* parts held */
#define AVER_PHYS_SI_BODIES            1   /* bodies Jolt saved (character inner bodies included) */
#define AVER_PHYS_SI_CHARACTERS        2
#define AVER_PHYS_SI_VEHICLES          3
#define AVER_PHYS_SI_JOINTS            4
#define AVER_PHYS_SI_RAGDOLLS          5
#define AVER_PHYS_SI_TOPOLOGY_EPOCH    6
#define AVER_PHYS_SI_CONSTRAINT_EPOCH  7
#define AVER_PHYS_SI_PRECISION         8   /* AVER_PHYS_PRECISION_* of the build that captured it */
#define AVER_PHYS_SI_STEPS_LOW         9   /* fixed steps since the world was created, at the capture: low 32 bits ... */
#define AVER_PHYS_SI_STEPS_HIGH        10  /* ... and the high 32 */
#define AVER_PHYS_SI_BYTES             11  /* the blob size (header included) */
#define AVER_PHYS_SI_COUNT             12

/* ---- snapshots ---- */

/* Captures the world. `reuse` of a live snapshot overwrites it in place (the byte buffer keeps its capacity, so a ring of N
   snapshots allocates N buffers once) and returns the same handle; 0 or a dead handle allocates a new one. `flags` 0 means
   AVER_PHYS_SNAP_ALL. Returns the handle, or 0 (no world, or a flag outside the known set: aver_phys_last_error says which). */
AVER_PHYS_API int32_t  aver_phys_snapshot_capture(int32_t reuse, uint32_t flags);
/* Puts the world back as it was. 1 on success; 0 and aver_phys_snapshot_last_result() say why, and nothing was applied.
   On success the event queues (contacts, overlaps, vehicle events, joint breaks, soft-body impacts) are cleared, because the
   resimulated steps will produce them again, and aver_phys_step_total() returns the step count of the capture. */
AVER_PHYS_API int32_t  aver_phys_snapshot_restore(int32_t snapshot, uint32_t options);
AVER_PHYS_API int32_t  aver_phys_snapshot_last_result(void);
AVER_PHYS_API int32_t  aver_phys_snapshot_release(int32_t snapshot);               /* 1; 0 for a dead handle */
/* Releases every snapshot of the current world and returns how many there were. The hosts call it when a level is unloaded: a
   snapshot names the level's bodies, which are about to go, so none of them could be restored afterwards anyway. */
AVER_PHYS_API int32_t  aver_phys_snapshot_release_all(void);
AVER_PHYS_API int32_t  aver_phys_snapshot_count(void);                              /* live snapshots in the current world */
AVER_PHYS_API int32_t  aver_phys_snapshot_bytes(int32_t snapshot);                  /* the blob size; 0 for a dead handle */
/* Copies the blob (header and payload) for disk or network. Call with outData == NULL for the size; otherwise at most `cap`
   bytes are written and the count is returned (0 when `cap` is smaller than the blob: it is never cut short). */
AVER_PHYS_API int32_t  aver_phys_snapshot_read(int32_t snapshot, uint8_t* outData, int32_t cap);
/* Validates a blob (header, build, checksum) and returns a handle usable with restore; 0 on failure, with last_result saying
   BAD_HEADER, BUILD_MISMATCH or CORRUPT. Needs a world: a snapshot belongs to the world that is alive when it is imported. */
AVER_PHYS_API int32_t  aver_phys_snapshot_import(const uint8_t* data, int32_t size);
AVER_PHYS_API int32_t  aver_phys_snapshot_info(int32_t snapshot, int32_t* outValues, int32_t n);   /* AVER_PHYS_SI_*; min(n, COUNT) written; returns the count written, 0 for a dead handle */
/* FNV-1a 64 of the payload (the blob without its header, so the capture time and the step count are not in it); 0 for a dead handle. */
AVER_PHYS_API uint64_t aver_phys_snapshot_hash(int32_t snapshot);
/* The offset of the first payload byte at which two snapshots differ, -1 when they are equal, -2 for a dead handle. When the
   sizes differ and one is a prefix of the other, the shorter size. Finding WHERE two runs diverge. */
AVER_PHYS_API int32_t  aver_phys_snapshot_diff(int32_t a, int32_t b);

/* ---- hash and replay ---- */

/* The hash a capture made now with `flags` would have (0 = ALL), without keeping the capture. 0 with no world. */
AVER_PHYS_API uint64_t aver_phys_state_hash(uint32_t flags);
/* Non-NULL: creates (truncating) the file and, from the next fixed step on, writes one "step hash" line to it (the decimal
   fixed-step number, then 16 hex digits of the ALL-parts hash taken at the end of the step), so two runs diff directly. NULL:
   stop. 1 on success; 0 when the file cannot be opened or there is no world. The log closes when the world is shut down. The
   hash costs about a snapshot per fixed step: this is a diagnostic, off by default. */
AVER_PHYS_API int32_t  aver_phys_hash_log(const char* pathOrNull);

/* Runs exactly `steps` (1..600) fixed steps, ignoring the accumulator and leaving it as it was. Returns the steps run, 0 for a
   count outside the range or no world. The replay primitive: a rollback layer re-simulates ticks with it. */
AVER_PHYS_API int32_t  aver_phys_step_exact(int32_t steps);
AVER_PHYS_API int64_t  aver_phys_step_total(void);                                 /* fixed steps since this world was created (restore sets it back); 0 with no world */
AVER_PHYS_API int32_t  aver_phys_accumulator_set(float seconds);                   /* for a replay that must restore a mid-frame position; clamped into [0, 8 fixed steps]; 1/0 */
AVER_PHYS_API float    aver_phys_accumulator_get(void);

/* ---- shape binary cache ----
   A mesh or heightfield shape is mostly its BVH, which costs far more to build than to read back. These calls turn a shape
   handle (physics_shape_builder_abi.h's table, the one aver_phys_create_mesh_shape fills) into a byte blob and back, so a host can
   keep the blob on disk (modules/world/include/aver/world/PhysicsShapeCache.hpp wraps it in an AVR1 "PSHP" file under
   <project>/Saved/DerivedDataCache/Physics) and skip the build next time. A restore is always a MISS, never a failure: a wrong key,
   cache version, Jolt version, precision, a truncated or damaged blob all answer 0 and the caller builds the shape the slow way.

   WHAT CAN BE SAVED. A mesh built by aver_phys_create_mesh_shape (no per-triangle user data) and a heightfield built by
   aver_phys_create_heightfield_shape / aver_phys_heightfield_shape / aver_phys_add_heightfield's builder, in both cases ONLY when
   the shape carries no physical-surface materials (a palette names live surface ids, which a file cannot hold). Anything else
   answers 0 from save. A restored heightfield is a plain collision shape: it is not editable (aver_phys_heightfield_set_heights
   refuses it), because the edit metadata lives with the process that built the field.

   THE KEY is the caller's: a hash of everything the shape was built from (the host uses mesh path, size, mtime and the
   collision cache version). It is stored in the blob and compared on restore, so a blob for another mesh or an older source is
   a miss even if the file name collides. */
#define AVER_PHYS_SHAPECACHE_VERSION 1u  /* what aver_phys_shape_cache_version() answers today */
AVER_PHYS_API uint32_t aver_phys_shape_cache_version(void);   /* bumped by any change to unit conversion, winding or the blob layout */
/* 1 when the host should use the cache (default 0: off until measured on real levels; PHYSICS.SHAPECACHE in the project, or the
   AVER_PHYS_SHAPE_CACHE=1 environment variable for tools). Process-wide; the cache calls below work whatever it says. */
AVER_PHYS_API int32_t  aver_phys_shape_cache_enable(int32_t on);
AVER_PHYS_API int32_t  aver_phys_shape_cache_enabled(void);
/* aver_phys_add_heightfield's construction (a square sampleCount * sampleCount grid, row-major, the v1 physics order, cm) minus the
   body: a shape handle in the same table as aver_phys_create_mesh_shape, placed by aver_phys_add_mesh_shape_body. 0 on failure. */
AVER_PHYS_API int32_t  aver_phys_create_heightfield_shape(const float* samples, int32_t sampleCount, float spacingCm);
/* Writes the blob for `shape` under `key`. Returns the bytes written, or with outData == NULL the bytes needed; 0 when the handle
   is not a saveable mesh or heightfield, or `cap` is smaller than the blob (never cut short). */
AVER_PHYS_API int32_t  aver_phys_shape_save(int32_t shape, uint64_t key, uint8_t* outData, int32_t cap);
/* Rebuilds a shape from a blob; the new shape handle, or 0 for a miss (wrong key, version, build or precision, or a truncated or
   damaged blob). The shape is the same bits the original was: queries and bodies behave identically. Needs a world. */
AVER_PHYS_API int32_t  aver_phys_shape_restore(const uint8_t* data, int32_t size, uint64_t key);

#ifdef __cplusplus
}
#endif
