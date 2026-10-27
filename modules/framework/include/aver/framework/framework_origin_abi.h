#ifndef AVER_FRAMEWORK_ORIGIN_ABI_H
#define AVER_FRAMEWORK_ORIGIN_ABI_H

/* Floating-origin C ABI (Aver.Framework): the origin O, conversions between origin-relative f32 and absolute positions, and WorldPos.
 * Design: docs/plans/wave11/LARGE_WORLD.md 2.5.
 * RULE: every position crossing any OTHER ABI stays origin-relative f32 centimetres. An absolute is always f64 or a WorldPos, never a bare f32.
 * O is a whole number of regions (1,638,400 cm = 16.384 km) in x and y; Z is never shifted. A local `p` and absolute `a` satisfy a.xy = p.xy + O.xy, a.z = p.z.
 * A position held across frames goes a region off after a rebase: store an absolute or WorldPos, or listen for "world.origin_changed".
 * The host installs ONE provider; Aver.Framework has no link dependency on Aver.World. With no provider O is (0, 0) and the origin is not enabled.
 * Event "world.origin_changed" is POSTED after every committed shift. Payload ints: [0] dxCm, [1] dyCm (every local position changes by minus this), [2] generation, [3] originX, [4] originY. Handlers run at the next aver_fw_events_flush.
 * int32_t / uint32_t / int64_t / float / double only; single game thread. 1 = success, 0 = refused, unless stated. */

#include "aver/framework/framework_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_ORIGIN_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_origin_abi_version(void);

/* Provider the host installs. `size` is sizeof(aver_fw_origin_provider), so the struct can grow at the end; a smaller size than this header's is refused. Every member is required. */
typedef struct aver_fw_origin_provider {
    int32_t  size;
    void*    user;
    int32_t  (AVER_FW_CALL* get)(void* user, int64_t* outXyCm);                              /* writes O.x, O.y; returns 1 */
    uint32_t (AVER_FW_CALL* generation)(void* user);                                         /* 0 at start, +1 per committed shift or level reset */
    int32_t  (AVER_FW_CALL* enabled)(void* user);                                            /* 1 when the origin can shift */
    int32_t  (AVER_FW_CALL* request_shift)(void* user, int32_t dxRegions, int32_t dyRegions);/* 1 = queued for the next frame's slot S0 */
} aver_fw_origin_provider;

/* Copies the provider. NULL clears it. 0 for a malformed struct; the old provider stays. */
AVER_FW_ABI int32_t aver_fw_set_origin_provider(const aver_fw_origin_provider* provider);

/* Writes O (x, y) in centimetres to outXyCm[2]. Returns 1 when a provider answered, 0 (and writes 0, 0) with none. */
AVER_FW_ABI int32_t  aver_fw_origin_get(int64_t* outXyCm);
AVER_FW_ABI uint32_t aver_fw_origin_generation(void);
AVER_FW_ABI int32_t  aver_fw_origin_enabled(void);

/* Absolute centimetres to origin-relative f32: the subtraction is in f64, the one rounding is the narrowing. */
AVER_FW_ABI void aver_fw_origin_to_local(double ax, double ay, double az, float* outXyz);
/* Origin-relative f32 to absolute centimetres: the add is in f64 (exact for every f32 and every O). */
AVER_FW_ABI void aver_fw_origin_to_abs(float x, float y, float z, double* outXyz);

/* WorldPos: chunk (i32 x3, 16 m chunks, region 0 centred on the origin) + local offset in [0, 1600) cm. */
AVER_FW_ABI void aver_fw_worldpos_from_abs(double ax, double ay, double az, int32_t* outChunk3, float* outLocal3);
AVER_FW_ABI void aver_fw_worldpos_to_abs(const int32_t* chunk3, const float* local3, double* outXyz);

/* Console and test seam: asks for a shift of dxRegions, dyRegions whole regions at the next frame's slot S0 (the same place a
 * camera-driven shift runs, with the same deferral rules). Refused (0) unless the project states WORLD.ALLOWSHIFTREQUEST 1, the
 * origin is not enabled, there is no provider, or both numbers are zero. 1 means queued, not done: watch the generation. Refused
 * shifts are dropped, not retried. Under the default follow-camera policy the next frame rebases back to the camera, so use this
 * with the Manual policy (the editor's world.policy) or when tests also move the camera. */
AVER_FW_ABI int32_t aver_fw_origin_request_shift(int32_t dxRegions, int32_t dyRegions);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AVER_FRAMEWORK_ORIGIN_ABI_H */
