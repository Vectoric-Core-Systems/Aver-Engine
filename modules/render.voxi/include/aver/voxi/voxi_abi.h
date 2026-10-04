#ifndef AVER_VOXI_ABI_H
#define AVER_VOXI_ABI_H

/* Voxi C ABI: the stable C surface the C# scripting layer binds to via P/Invoke.
 * Plain C only, so any FFI can use it. Setters return 1 on success, 0 if rejected.
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

/* Must match aver::voxi::Feature. */
#define AVER_VOXI_FEATURE_MSAA                0
#define AVER_VOXI_FEATURE_GLOBAL_ILLUMINATION 1
#define AVER_VOXI_FEATURE_RAY_TRACING         2
#define AVER_VOXI_FEATURE_PATH_TRACING        3
#define AVER_VOXI_FEATURE_MESH_SHADERS        4
#define AVER_VOXI_FEATURE_COUNT               5

/* Must match aver::voxi::Status. */
#define AVER_VOXI_STATUS_READY           0
#define AVER_VOXI_STATUS_NOT_IMPLEMENTED 1
#define AVER_VOXI_STATUS_UNSUPPORTED     2

/* Must match aver::voxi::Quality. */
#define AVER_VOXI_QUALITY_OFF    0
#define AVER_VOXI_QUALITY_LOW    1
#define AVER_VOXI_QUALITY_MEDIUM 2
#define AVER_VOXI_QUALITY_HIGH   3
#define AVER_VOXI_QUALITY_EPIC   4

/* Returns the number of features. */
AVER_VOXI_ABI int32_t     aver_voxi_feature_count(void);
/* Returns a feature's display name. */
AVER_VOXI_ABI const char* aver_voxi_feature_name(int32_t feature);
/* Returns a feature's status id. */
AVER_VOXI_ABI int32_t     aver_voxi_feature_status(int32_t feature);
/* Returns a feature's status as readable text. */
AVER_VOXI_ABI const char* aver_voxi_feature_status_text(int32_t feature);

/* Returns the current MSAA sample count: 1, 2, 4 or 8. */
AVER_VOXI_ABI int32_t aver_voxi_get_msaa(void);
/* Sets the MSAA sample count. */
AVER_VOXI_ABI int32_t aver_voxi_set_msaa(int32_t samples);
/* Returns the supported sample counts: bit N set => N samples supported. */
AVER_VOXI_ABI int32_t aver_voxi_msaa_mask(void);

/* Returns a feature's quality level. */
AVER_VOXI_ABI int32_t aver_voxi_get_quality(int32_t feature);
/* Sets a feature's quality level. */
AVER_VOXI_ABI int32_t aver_voxi_set_quality(int32_t feature, int32_t quality);

/* Returns the cubic voxel grid edge. */
AVER_VOXI_ABI int32_t aver_voxi_get_voxel_resolution(void);
/* Sets the cubic voxel grid edge. */
AVER_VOXI_ABI int32_t aver_voxi_set_voxel_resolution(int32_t res);
/* Returns the indirect bounce multiplier. */
AVER_VOXI_ABI float   aver_voxi_get_gi_intensity(void);
/* Sets the indirect bounce multiplier. */
AVER_VOXI_ABI int32_t aver_voxi_set_gi_intensity(float v);
/* Returns the cone trace range in centimetres. */
AVER_VOXI_ABI float   aver_voxi_get_gi_max_distance(void);
/* Sets the cone trace range, in centimetres. */
AVER_VOXI_ABI int32_t aver_voxi_set_gi_max_distance(float cm);
/* Returns how many frames apart the GI volume is re-voxelised (1 = every frame). */
AVER_VOXI_ABI int32_t aver_voxi_get_gi_update_interval(void);
/* Sets the GI revoxelise interval, in frames; clamped [1, 8]. */
AVER_VOXI_ABI int32_t aver_voxi_set_gi_update_interval(int32_t frames);

/* Returns the device's ray tracing tier: 0 none, 10 DXR 1.0, 11 DXR 1.1. */
AVER_VOXI_ABI int32_t aver_voxi_ray_tracing_tier(void);
/* Returns the device's highest supported MSAA sample count. */
AVER_VOXI_ABI int32_t aver_voxi_max_msaa(void);
/* Returns the device's mesh shader tier: 0 none, 1 Tier 1. */
AVER_VOXI_ABI int32_t aver_voxi_mesh_shader_tier(void);
/* Returns the device's shader model: 60 = SM 6.0, 65 = SM 6.5. */
AVER_VOXI_ABI int32_t aver_voxi_shader_model(void);

/* Returns 1 when the mesh shader submission path is on. */
AVER_VOXI_ABI int32_t aver_voxi_get_mesh_shaders(void);
/* Turns the mesh shader submission path on or off. */
AVER_VOXI_ABI int32_t aver_voxi_set_mesh_shaders(int32_t on);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AVER_VOXI_ABI_H */
