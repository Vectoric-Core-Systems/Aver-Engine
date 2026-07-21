#ifndef AVER_PBR_ABI_H
#define AVER_PBR_ABI_H

/* PBR C ABI — the stable surface the C# scripting layer binds to via P/Invoke.
 *
 * Deliberately plain C, in the same idiom as voxi_abi.h: only int32_t / float / const char* cross
 * the boundary, so the same header works for C, C++, C# DllImport and any other FFI. Enum values
 * match the aver::pbr enums one-for-one.
 *
 * ONE deviation from that idiom, and it is deliberate: a texture's opaque asset id is int64_t. It
 * is forward-compatible with an ObjectId or an `.octex` GUID, neither of which fits in 32 bits, and
 * splitting it into halves would put the burden of reassembling an identifier on every binding.
 * int64_t is blittable in C# (long) exactly as int32_t is.
 *
 * Vectors are returned through a float* OUT-PARAM, never as a small struct by value: the
 * by-value-small-struct return convention differs between compilers and marshallers, and getting it
 * wrong corrupts the return register rather than failing to link.
 *
 * Materials are INSTANCES, so unlike Voxi's global-settings shape everything here is by HANDLE:
 * create/destroy, and set-by-handle.
 *
 * Error convention: setters return 1 on success, 0 if the request was rejected (stale handle, bad
 * slot, or a value out of range). Getters return the current value, or a documented neutral value
 * for a stale handle.
 */

#include <stdint.h>

#if defined(_WIN32)
#  if defined(AVER_PBR_BUILD)
#    define AVER_PBR_ABI __declspec(dllexport)
#  else
#    define AVER_PBR_ABI __declspec(dllimport)
#  endif
#else
#  define AVER_PBR_ABI
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Feature ids — must match aver::pbr::Feature */
#define AVER_PBR_FEATURE_FACTORS         0
#define AVER_PBR_FEATURE_BASE_COLOR_MAP  1
#define AVER_PBR_FEATURE_METAL_ROUGH_MAP 2
#define AVER_PBR_FEATURE_NORMAL_MAP      3
#define AVER_PBR_FEATURE_OCCLUSION_MAP   4
#define AVER_PBR_FEATURE_EMISSIVE_MAP    5
#define AVER_PBR_FEATURE_ALPHA_MASK      6
#define AVER_PBR_FEATURE_ALPHA_BLEND     7
#define AVER_PBR_FEATURE_COUNT           8

/* Status ids — must match aver::pbr::Status */
#define AVER_PBR_STATUS_READY           0
#define AVER_PBR_STATUS_NOT_IMPLEMENTED 1
#define AVER_PBR_STATUS_UNSUPPORTED     2

/* Texture slot ids — must match aver::pbr::TextureSlot */
#define AVER_PBR_TEX_BASE_COLOR  0
#define AVER_PBR_TEX_METAL_ROUGH 1
#define AVER_PBR_TEX_NORMAL      2
#define AVER_PBR_TEX_OCCLUSION   3
#define AVER_PBR_TEX_EMISSIVE    4
#define AVER_PBR_TEX_COUNT       5

/* Alpha mode ids — must match aver::pbr::AlphaMode */
#define AVER_PBR_ALPHA_OPAQUE 0
#define AVER_PBR_ALPHA_MASK   1
#define AVER_PBR_ALPHA_BLEND  2

/* A material handle. 0 is invalid; a valid one is always positive. */
typedef int32_t aver_pbr_material;

/* ---- feature introspection ---- */
AVER_PBR_ABI int32_t     aver_pbr_feature_count(void);
AVER_PBR_ABI const char* aver_pbr_feature_name(int32_t feature);
AVER_PBR_ABI int32_t     aver_pbr_status(int32_t feature);
AVER_PBR_ABI const char* aver_pbr_status_text(int32_t feature);
AVER_PBR_ABI const char* aver_pbr_texture_slot_name(int32_t slot);
AVER_PBR_ABI const char* aver_pbr_alpha_mode_name(int32_t mode);

/* ---- lifetime ---- */
/* Creates a material with the glTF default surface. Returns 0 if none could be created. */
AVER_PBR_ABI aver_pbr_material aver_pbr_create(const char* name);
AVER_PBR_ABI int32_t           aver_pbr_destroy(aver_pbr_material m);
AVER_PBR_ABI int32_t           aver_pbr_valid(aver_pbr_material m);

/* ---- enumeration (indices are dense over LIVE materials and shift on destroy) ---- */
AVER_PBR_ABI int32_t           aver_pbr_count(void);
AVER_PBR_ABI aver_pbr_material aver_pbr_at(int32_t index);

/* ---- identity ---- */
AVER_PBR_ABI const char* aver_pbr_get_name(aver_pbr_material m);   /* "" for a stale handle */
AVER_PBR_ABI int32_t     aver_pbr_set_name(aver_pbr_material m, const char* name);

/* ---- factors (names match the `.ocmat` PARAM names) ---- */
AVER_PBR_ABI int32_t aver_pbr_get_base_color_factor(aver_pbr_material m, float* out4);
AVER_PBR_ABI int32_t aver_pbr_set_base_color_factor(aver_pbr_material m, float r, float g, float b, float a);
AVER_PBR_ABI int32_t aver_pbr_get_emissive_factor(aver_pbr_material m, float* out3);
AVER_PBR_ABI int32_t aver_pbr_set_emissive_factor(aver_pbr_material m, float r, float g, float b);
AVER_PBR_ABI float   aver_pbr_get_metallic_factor(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_metallic_factor(aver_pbr_material m, float v);
AVER_PBR_ABI float   aver_pbr_get_roughness_factor(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_roughness_factor(aver_pbr_material m, float v);
AVER_PBR_ABI float   aver_pbr_get_normal_scale(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_normal_scale(aver_pbr_material m, float v);
AVER_PBR_ABI float   aver_pbr_get_occlusion_strength(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_occlusion_strength(aver_pbr_material m, float v);

/* ---- blending and sidedness ---- */
AVER_PBR_ABI int32_t aver_pbr_get_alpha_mode(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_alpha_mode(aver_pbr_material m, int32_t mode);
AVER_PBR_ABI float   aver_pbr_get_alpha_cutoff(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_alpha_cutoff(aver_pbr_material m, float v);
AVER_PBR_ABI int32_t aver_pbr_get_two_sided(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_two_sided(aver_pbr_material m, int32_t on);
AVER_PBR_ABI int32_t aver_pbr_get_cast_shadow(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_cast_shadow(aver_pbr_material m, int32_t on);

/* ---- texture references (path AND opaque id; this module interprets neither) ---- */
AVER_PBR_ABI const char* aver_pbr_get_texture_path(aver_pbr_material m, int32_t slot); /* "" if unset */
AVER_PBR_ABI int32_t     aver_pbr_set_texture_path(aver_pbr_material m, int32_t slot, const char* path);
AVER_PBR_ABI int64_t     aver_pbr_get_texture_id(aver_pbr_material m, int32_t slot);   /* 0 if unset */
AVER_PBR_ABI int32_t     aver_pbr_set_texture_id(aver_pbr_material m, int32_t slot, int64_t id);
AVER_PBR_ABI int32_t     aver_pbr_clear_texture(aver_pbr_material m, int32_t slot);

/* ---- upload bookkeeping ---- */
/* 1 when this material still owes the GPU an upload. READING IT CLEARS IT, so exactly one consumer
 * acts on each change — same contract as aver_voxi's msaa dirty flag. */
AVER_PBR_ABI int32_t aver_pbr_consume_dirty(aver_pbr_material m);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AVER_PBR_ABI_H */
