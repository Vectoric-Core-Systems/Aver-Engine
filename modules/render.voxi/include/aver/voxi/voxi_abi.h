#ifndef AVER_VOXI_ABI_H
#define AVER_VOXI_ABI_H

/* Voxi C ABI — the stable surface the C# scripting layer binds to via P/Invoke.
 *
 * Deliberately plain C: only int32_t / float / const char* cross the boundary, so the same
 * header works for C, C++, C# DllImport and any other FFI. Feature and quality values match
 * the aver::voxi enums one-for-one.
 *
 * Error convention: setters return 1 on success, 0 if the request was rejected (unsupported
 * feature or bad argument). Getters return the current value.
 */

#include <stdint.h>

#if defined(_WIN32)
#  if defined(AVER_VOXI_BUILD)
#    define AVER_VOXI_ABI __declspec(dllexport)
#  else
#    define AVER_VOXI_ABI __declspec(dllimport)
#  endif
#else
#  define AVER_VOXI_ABI
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Feature ids — must match aver::voxi::Feature */
#define AVER_VOXI_FEATURE_MSAA                0
#define AVER_VOXI_FEATURE_GLOBAL_ILLUMINATION 1
#define AVER_VOXI_FEATURE_RAY_TRACING         2
#define AVER_VOXI_FEATURE_PATH_TRACING        3
#define AVER_VOXI_FEATURE_COUNT               4

/* Status ids — must match aver::voxi::Status */
#define AVER_VOXI_STATUS_READY           0
#define AVER_VOXI_STATUS_NOT_IMPLEMENTED 1
#define AVER_VOXI_STATUS_UNSUPPORTED     2

/* Quality ids — must match aver::voxi::Quality */
#define AVER_VOXI_QUALITY_OFF    0
#define AVER_VOXI_QUALITY_LOW    1
#define AVER_VOXI_QUALITY_MEDIUM 2
#define AVER_VOXI_QUALITY_HIGH   3
#define AVER_VOXI_QUALITY_EPIC   4

/* ---- feature introspection ---- */
AVER_VOXI_ABI int32_t     aver_voxi_feature_count(void);
AVER_VOXI_ABI const char* aver_voxi_feature_name(int32_t feature);
AVER_VOXI_ABI int32_t     aver_voxi_feature_status(int32_t feature);
AVER_VOXI_ABI const char* aver_voxi_feature_status_text(int32_t feature);

/* ---- anti-aliasing ---- */
AVER_VOXI_ABI int32_t aver_voxi_get_msaa(void);         /* sample count: 1,2,4,8 */
AVER_VOXI_ABI int32_t aver_voxi_set_msaa(int32_t samples);
AVER_VOXI_ABI int32_t aver_voxi_msaa_mask(void);        /* bit N set => N samples supported */

/* ---- quality-ladder features (GI / ray tracing / path tracing) ---- */
AVER_VOXI_ABI int32_t aver_voxi_get_quality(int32_t feature);
AVER_VOXI_ABI int32_t aver_voxi_set_quality(int32_t feature, int32_t quality);

/* ---- global illumination tunables ---- */
AVER_VOXI_ABI int32_t aver_voxi_get_voxel_resolution(void);
AVER_VOXI_ABI int32_t aver_voxi_set_voxel_resolution(int32_t res);
AVER_VOXI_ABI float   aver_voxi_get_gi_intensity(void);
AVER_VOXI_ABI int32_t aver_voxi_set_gi_intensity(float v);
AVER_VOXI_ABI float   aver_voxi_get_gi_max_distance(void);
AVER_VOXI_ABI int32_t aver_voxi_set_gi_max_distance(float cm);

/* ---- device capabilities (read-only mirror of what the GPU reports) ---- */
AVER_VOXI_ABI int32_t aver_voxi_ray_tracing_tier(void);  /* 0 none, 10 DXR 1.0, 11 DXR 1.1 */
AVER_VOXI_ABI int32_t aver_voxi_max_msaa(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AVER_VOXI_ABI_H */
