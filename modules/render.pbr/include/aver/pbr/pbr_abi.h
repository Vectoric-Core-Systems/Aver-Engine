#ifndef AVER_PBR_ABI_H
#define AVER_PBR_ABI_H

/* PBR C ABI — the stable surface the C# scripting layer binds to via P/Invoke. Materials are
 * instances, so everything is by handle.
 *
 * Convention: setters return 1 on success and 0 if the request was rejected; getters return the
 * current value or a documented neutral value for a stale handle. Vectors come back through a
 * float* out-param, never as a small struct by value. */

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

/* UV mode ids — must match aver::pbr::UvMode */
#define AVER_PBR_UV_MESH          0
#define AVER_PBR_UV_WORLD_ALIGNED 1

/* Alpha mode ids — must match aver::pbr::AlphaMode */
#define AVER_PBR_ALPHA_OPAQUE 0
#define AVER_PBR_ALPHA_MASK   1
#define AVER_PBR_ALPHA_BLEND  2

/* Sampler filter ids — must match aver::pbr::FxFilter (MaterialFx.hpp). 0 is the engine's own sampler. */
#define AVER_PBR_SAMPLER_ANISOTROPIC 0   /* anisotropic, mipmapped (the level is the project's RENDER.TEXTUREANISO) */
#define AVER_PBR_SAMPLER_TRILINEAR   1   /* linear, linear between mips */
#define AVER_PBR_SAMPLER_BILINEAR    2   /* linear, mip 0 only */
#define AVER_PBR_SAMPLER_POINT_MIP   3   /* nearest, nearest mip */
#define AVER_PBR_SAMPLER_POINT       4   /* nearest, mip 0 only (pixel art) */

/* Sampler address ids — must match aver::pbr::FxAddress. */
#define AVER_PBR_ADDRESS_WRAP   0
#define AVER_PBR_ADDRESS_CLAMP  1
#define AVER_PBR_ADDRESS_MIRROR 2

/* Texture channel ids — must match aver::pbr::FxChannel (MaterialFx.hpp): which channel of the metal-rough map a scalar reads. */
#define AVER_PBR_CHANNEL_R    0
#define AVER_PBR_CHANNEL_G    1   /* the engine's own packing for roughness */
#define AVER_PBR_CHANNEL_B    2   /* ... and for metallic */
#define AVER_PBR_CHANNEL_A    3
#define AVER_PBR_CHANNEL_GRAY 4   /* Rec. 709 luminance of the texel */

/* Alpha extension ids — must match aver::pbr::FxAlphaExt. Raster draws only: the coverage is discarded against noise instead of blended or scissored. */
#define AVER_PBR_ALPHAEXT_NONE       0
#define AVER_PBR_ALPHAEXT_HASH       1   /* stable noise against the alpha */
#define AVER_PBR_ALPHAEXT_ANTIALIAS  2   /* the alpha-to-coverage stand-in: noise against a coverage that follows the alpha's edge */

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
/* The first live material whose name is `name` (case-sensitive; a material loaded from "M_Wood.ocmat" is named "M_Wood"), or 0. This is how a script
 * reaches the instance a surface name draws with: every setter below pushes to the GPU on the next frame. */
AVER_PBR_ABI aver_pbr_material aver_pbr_find(const char* name);

/* ---- factors (names match the `.ocmat` PARAM names) ---- */
AVER_PBR_ABI int32_t aver_pbr_get_base_color_factor(aver_pbr_material m, float* out4);
AVER_PBR_ABI int32_t aver_pbr_set_base_color_factor(aver_pbr_material m, float r, float g, float b, float a);
AVER_PBR_ABI int32_t aver_pbr_get_emissive_factor(aver_pbr_material m, float* out3);
AVER_PBR_ABI int32_t aver_pbr_set_emissive_factor(aver_pbr_material m, float r, float g, float b);
/* A scale on the glow, multiplied into the emissive factor where the material is packed (1 = none; 0..10000, a non-finite value is refused). */
AVER_PBR_ABI float   aver_pbr_get_emissive_energy(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_emissive_energy(aver_pbr_material m, float v);
AVER_PBR_ABI float   aver_pbr_get_metallic_factor(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_metallic_factor(aver_pbr_material m, float v);
AVER_PBR_ABI float   aver_pbr_get_roughness_factor(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_roughness_factor(aver_pbr_material m, float v);
AVER_PBR_ABI float   aver_pbr_get_normal_scale(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_normal_scale(aver_pbr_material m, float v);
AVER_PBR_ABI float   aver_pbr_get_occlusion_strength(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_occlusion_strength(aver_pbr_material m, float v);
/* Reflectance of the dielectric base and the reflectance at grazing incidence, both in [0,1]. */
AVER_PBR_ABI float   aver_pbr_get_reflectance(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_reflectance(aver_pbr_material m, float v);
AVER_PBR_ABI float   aver_pbr_get_f90(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_f90(aver_pbr_material m, float v);
/* The dielectric transmission pair. ior is the refractive index, floored at 1.0 (vacuum) and
 * ceilinged well past diamond; transmission is how optically see-through the substrate is, in [0,1].
 *
 * BOTH REACHED THE GPU LONG BEFORE THEY REACHED THIS HEADER, which is the reason they are being
 * added now rather than when they were introduced: a field with a parser, a writer and a GPU
 * packing but no ABI is one a script cannot read or write at all.
 *
 * WHAT IOR ACTUALLY DRIVES TODAY: NOTHING IN THE SHADING. This comment used to say it keyed the
 * critical angle for total internal reflection, via averTotalInternalReflection in PbrShaders.cpp.
 * That function no longer exists -- it was removed because a parallel-sided pane viewed from outside
 * CANNOT total-internally-reflect: Snell bounds the internal angle at asin(1/n), which is the
 * critical angle itself, so the test could never fire for the case it was written for. ior is
 * parsed, packed, uploaded, exposed here, and drivable from a material graph pin; it is read by no
 * shading term. Stated rather than quietly left, because a stale "this has real consumers" is worse
 * than no comment -- it is what made the claim survive in three separate files.
 *
 * NEITHER IS CLAMPED ON THIS PATH, exactly like reflectance/f90/subsurface above: commit() calls
 * touch(), not sanitise(). An out-of-range value written here survives until the next load or pack
 * clamps it. That is the existing contract of every scalar setter in this file, not a new hazard,
 * but it is worth knowing before driving ior from a slider. */
AVER_PBR_ABI float   aver_pbr_get_ior(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_ior(aver_pbr_material m, float v);
AVER_PBR_ABI float   aver_pbr_get_transmission(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_transmission(aver_pbr_material m, float v);
/* Subsurface wrap weight (0 = feature off) and back-scatter lobe width, both in [0,1]. This is a
 * cheap wrap-diffuse approximation, not a BSSRDF: no transport across the mesh, no per-texel
 * thickness, no separate scatter colour (the transmitted light is tinted by baseColorFactor). */
AVER_PBR_ABI float   aver_pbr_get_subsurface_weight(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_subsurface_weight(aver_pbr_material m, float v);
/* Lamp light: brightness at 1 metre in the sun's own units (SkyAtmosphere::sunIntensity, ~3 in the
 * editor); 0 = not a light. Read only by the ray-driven local-light pass (CSRdLocalLights) --
 * every other renderer ignores it, so a lamp still shades by its Emissive factor everywhere else. */
AVER_PBR_ABI float   aver_pbr_get_light_intensity(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_light_intensity(aver_pbr_material m, float v);
AVER_PBR_ABI float   aver_pbr_get_coat_weight(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_coat_weight(aver_pbr_material m, float v);
AVER_PBR_ABI float   aver_pbr_get_coat_roughness(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_coat_roughness(aver_pbr_material m, float v);
AVER_PBR_ABI float   aver_pbr_get_coat_f0(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_coat_f0(aver_pbr_material m, float v);
AVER_PBR_ABI float   aver_pbr_get_subsurface_radius(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_subsurface_radius(aver_pbr_material m, float v);
/* Subsurface scatter tint, sRGB like baseColorFactor: the colour light takes INSIDE the material,
 * multiplied onto the diffuse albedo for the subsurface terms. White (the default) scatters in the
 * surface's own colour; skin wants a deep red, leaves a yellow-green, wax an orange. Meaningless
 * while subsurface_weight is 0. */
AVER_PBR_ABI int32_t aver_pbr_get_subsurface_color(aver_pbr_material m, float* out3);
AVER_PBR_ABI int32_t aver_pbr_set_subsurface_color(aver_pbr_material m, float r, float g, float b);

/* ---- blending and sidedness ---- */
AVER_PBR_ABI int32_t aver_pbr_get_alpha_mode(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_alpha_mode(aver_pbr_material m, int32_t mode);
AVER_PBR_ABI float   aver_pbr_get_alpha_cutoff(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_alpha_cutoff(aver_pbr_material m, float v);
AVER_PBR_ABI int32_t aver_pbr_get_two_sided(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_two_sided(aver_pbr_material m, int32_t on);
AVER_PBR_ABI int32_t aver_pbr_get_cast_shadow(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_cast_shadow(aver_pbr_material m, int32_t on);

/* ---- texture mapping. Tiling is world CENTIMETRES per tile and is read only under
 * AVER_PBR_UV_WORLD_ALIGNED; a value <= 0 is rejected. That mode projects in the OBJECT's frame
 * despite its name, and the name is FROZEN: renaming it is a major ABI break and would invalidate
 * every .ocmat authored against the `worlduv` key. ---- */
AVER_PBR_ABI int32_t aver_pbr_get_uv_mode(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_uv_mode(aver_pbr_material m, int32_t mode);
AVER_PBR_ABI float   aver_pbr_get_uv_tiling(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_uv_tiling(aver_pbr_material m, float cmPerTile);
AVER_PBR_ABI const char* aver_pbr_uv_mode_name(int32_t mode);

/* ---- the .ocmat FX record's UV transform and sampler (raster draws only; ray hits and GI read the first 192 bytes of the material and ignore both) ----
 * `set` is 1 or 2 (the first or second UV channel). The transform is scale.x, scale.y, offset.x, offset.y; out4 gets those four floats. A material
 * that sets none of the FX features draws exactly as before; the first non-default value switches it to its own pipeline variant (compiled on first use,
 * the ordinary pipeline draws it until then). Scale and offset are clamped to +-1000 on load/pack, not here. Setters return 0 for a stale handle, a
 * `set` other than 1 or 2, a non-finite value or an unknown id. */
AVER_PBR_ABI int32_t aver_pbr_get_uv_transform(aver_pbr_material m, int32_t set, float* out4);
AVER_PBR_ABI int32_t aver_pbr_set_uv_transform(aver_pbr_material m, int32_t set, float scaleX, float scaleY, float offsetX, float offsetY);
/* Per-material sampler state: filter (AVER_PBR_SAMPLER_*) and address mode (AVER_PBR_ADDRESS_*). The anisotropy level and mip bias are project-wide
 * (RENDER.TEXTUREANISO / RENDER.TEXTUREMIPBIAS). Getters return the engine default (0) for a stale handle. */
AVER_PBR_ABI int32_t aver_pbr_get_sampler_filter(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_sampler_filter(aver_pbr_material m, int32_t filter);
AVER_PBR_ABI int32_t aver_pbr_get_sampler_address(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_sampler_address(aver_pbr_material m, int32_t address);

/* ---- texture decode and alpha (the .ocmat FX record; raster draws only: ray hits and GI read the glTF packing and the colour as stored) ----
 * Which channel of the metal-rough map roughness and metallic read (AVER_PBR_CHANNEL_*; the defaults G and B are the engine's packing), whether the mesh's
 * vertex colour is sRGB-encoded, whether the vertex colour tints the albedo at all, and the alpha extension. Getters return the default for a stale handle,
 * setters 0 for a stale handle or an unknown id. */
AVER_PBR_ABI int32_t aver_pbr_get_roughness_channel(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_roughness_channel(aver_pbr_material m, int32_t channel);
AVER_PBR_ABI int32_t aver_pbr_get_metallic_channel(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_metallic_channel(aver_pbr_material m, int32_t channel);
AVER_PBR_ABI int32_t aver_pbr_get_vertex_color(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_vertex_color(aver_pbr_material m, int32_t on);
AVER_PBR_ABI int32_t aver_pbr_get_vertex_color_srgb(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_vertex_color_srgb(aver_pbr_material m, int32_t on);
AVER_PBR_ABI int32_t aver_pbr_get_alpha_extension(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_alpha_extension(aver_pbr_material m, int32_t ext);

/* ---- texture references (path AND opaque id; this module interprets neither) ---- */
AVER_PBR_ABI const char* aver_pbr_get_texture_path(aver_pbr_material m, int32_t slot); /* "" if unset */
AVER_PBR_ABI int32_t     aver_pbr_set_texture_path(aver_pbr_material m, int32_t slot, const char* path);
AVER_PBR_ABI int64_t     aver_pbr_get_texture_id(aver_pbr_material m, int32_t slot);   /* 0 if unset */
AVER_PBR_ABI int32_t     aver_pbr_set_texture_id(aver_pbr_material m, int32_t slot, int64_t id);
AVER_PBR_ABI int32_t     aver_pbr_clear_texture(aver_pbr_material m, int32_t slot);

/* ---- the uniform registers of an embedded HLSL shader (the material's GRAPHREF names the shader). Four floats per register in the layout the
 * translator fixed; ints and bools are 32-bit patterns in the float slots. Setting registers on a material makes it an embedded-shader material (texture slot 7
 * becomes the uniform table); setting zero floats clears them. The count is a register count in the first call and a float count in the others. ---- */
AVER_PBR_ABI int32_t aver_pbr_get_embedded_register_count(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_get_embedded_registers(aver_pbr_material m, float* out, int32_t capacityFloats);          /* floats copied, -1 for a stale handle */
AVER_PBR_ABI int32_t aver_pbr_set_embedded_registers(aver_pbr_material m, const float* values, int32_t floatCount);     /* replaces all; no upload when nothing changed */
AVER_PBR_ABI int32_t aver_pbr_set_embedded_register_floats(aver_pbr_material m, int32_t firstFloat, const float* values, int32_t count); /* patches a range */

/* ---- upload bookkeeping ---- */
/* 1 when this material still owes the GPU an upload. READING IT CLEARS IT. */
AVER_PBR_ABI int32_t aver_pbr_consume_dirty(aver_pbr_material m);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AVER_PBR_ABI_H */
