#ifndef AVER_FRAMEWORK_ORIGIN_ABI_H
#define AVER_FRAMEWORK_ORIGIN_ABI_H

/* Floating-origin C ABI (Aver.Framework): where the origin O is, how a script converts between the origin-relative f32 frame
 * every other ABI speaks and absolute positions, and the WorldPos split (a global chunk index plus a small float offset:
 * 1.2 micrometres at any distance). Design: docs/plans/wave11/LARGE_WORLD.md 2.5.
 *
 * RULE: every position crossing any OTHER ABI stays origin-relative f32 centimetres. Absolute values appear
 * only through the calls below, and an absolute is always f64 or a WorldPos, never a bare f32.
 *
 * O is the pair (x, y) in centimetres, always a whole number of regions (1,638,400 cm = 16.384 km); Z is never shifted. A
 * local position `p` and the absolute `a` it names satisfy a.xy = p.xy + O.xy, a.z = p.z. The host shifts O at the top of the
 * frame and moves every local position by -delta in the same slot, so a script that stores a `Position` across frames holds a
 * value that is a region off after a rebase: store an absolute (or a WorldPos) instead, or listen for "world.origin_changed".
 *
 * The state lives in the host: it installs ONE provider (aver_fw_set_origin_provider) and the origin calls forward to it, so
 * Aver.Framework gains no link dependency on Aver.World. With no provider O is (0, 0), the generation is 0, the origin is not
 * enabled and request_shift refuses; the conversions are still exact (they are plain arithmetic about O).
 *
 * Event: "world.origin_changed" is POSTED through aver_fw_event_post after every committed shift (sender 0, target 0) with the
 * payload ints [0] dxCm, [1] dyCm (the shift: every local position changed by minus this), [2] generation (after the shift),
 * [3] originX, [4] originY (the new origin). Handlers run at the next aver_fw_events_flush, once the shift is complete.
 *
 * int32_t / uint32_t / int64_t / float / double only; single game thread. 1 = success and 0 = refused unless stated. */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI, AVER_FW_CALL */

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_ORIGIN_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_origin_abi_version(void);

/* What the host installs. `size` is sizeof(aver_fw_origin_provider) so the struct can grow at the end; a smaller size than this
 * header's is refused. Every member is required. */
typedef struct aver_fw_origin_provider {
    int32_t  size;
    void*    user;
    int32_t  (AVER_FW_CALL* get)(void* user, int64_t* outXyCm);                              /* writes O.x, O.y; returns 1 */
    uint32_t (AVER_FW_CALL* generation)(void* user);                                         /* 0 at start, +1 per committed shift or level reset */
    int32_t  (AVER_FW_CALL* enabled)(void* user);                                            /* 1 when the origin can shift */
    int32_t  (AVER_FW_CALL* request_shift)(void* user, int32_t dxRegions, int32_t dyRegions);/* 1 = queued for the next frame's slot S0 */
} aver_fw_origin_provider;

/* Installs the provider (copied). NULL clears it. 1 installed or cleared, 0 for a malformed struct (the old provider stays). */
AVER_FW_ABI int32_t aver_fw_set_origin_provider(const aver_fw_origin_provider* provider);

/* ---- the origin ------------------------------------------------------------------------------------------------- */
/* Writes O (x, y) in centimetres to outXyCm[2]. Returns 1 when a provider answered, 0 (and writes 0, 0) with none. */
AVER_FW_ABI int32_t  aver_fw_origin_get(int64_t* outXyCm);
AVER_FW_ABI uint32_t aver_fw_origin_generation(void);
AVER_FW_ABI int32_t  aver_fw_origin_enabled(void);

/* ---- conversions (exact arithmetic about O; need no provider) ---------------------------------------------------- */
/* Absolute centimetres to origin-relative f32: the subtraction is in f64, the one rounding is the narrowing. */
AVER_FW_ABI void aver_fw_origin_to_local(double ax, double ay, double az, float* outXyz);
/* Origin-relative f32 to absolute centimetres: the add is in f64 (exact for every f32 and every O). */
AVER_FW_ABI void aver_fw_origin_to_abs(float x, float y, float z, double* outXyz);

/* ---- WorldPos: chunk (i32 x3, 16 m chunks, region 0 centred on the origin) + local offset in [0, 1600) cm -------- */
AVER_FW_ABI void aver_fw_worldpos_from_abs(double ax, double ay, double az, int32_t* outChunk3, float* outLocal3);
AVER_FW_ABI void aver_fw_worldpos_to_abs(const int32_t* chunk3, const float* local3, double* outXyz);

/* Console and test seam: asks for a shift of dxRegions, dyRegions whole regions at the next frame's slot S0 (the same place a
 * camera-driven shift runs, with the same deferral rules). Refused (0) unless the project states WORLD.ALLOWSHIFTREQUEST 1, the
 * origin is not enabled, there is no provider, or both numbers are zero. 1 means queued, not done: watch the generation. A shift
 * the deferral rules refuse (a level loading, a gesture, a flat streamed level) is dropped, not retried. Under the default
 * follow-camera policy the next frame rebases back to the camera, so this is for tests and tools that also move the camera, or run
 * the origin in the Manual policy (the editor's world.policy). */
AVER_FW_ABI int32_t aver_fw_origin_request_shift(int32_t dxRegions, int32_t dyRegions);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AVER_FRAMEWORK_ORIGIN_ABI_H */
