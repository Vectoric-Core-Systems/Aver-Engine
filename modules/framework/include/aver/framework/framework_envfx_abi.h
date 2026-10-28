#ifndef AVER_FRAMEWORK_ENVFX_ABI_H
#define AVER_FRAMEWORK_ENVFX_ABI_H

/* Environment effects C ABI (Aver.Framework, G4 R8): local fog volumes and reflection probes a script or a converted level places. Same shape as
 * framework_subview_abi.h: the script PUBLISHES, the host READS each frame. Nothing exists or runs unless a volume or probe is created.
 *
 * Own version, separate from AVER_FW_ABI_VERSION_MINOR. int32_t/float/const char* only; 1 on success and 0 on refusal. All state lives in this DLL
 * (aver/core/FogVolume.hpp, aver/core/ReflectionProbe.hpp), single game thread. Units: centimetres, engine axes (+Z up), colours linear.
 *
 * Matrices are the engine's instance matrix: 12 floats, three basis rows then the translation (row vectors; the basis carries the node's scale).
 * Half extents are along the engine's local X, Y, Z before that scale. A volume or probe is named by a KEY (the node's path in its scene); creating a key that
 * exists returns its id.
 *
 * FOG VOLUMES draw only inside Voxi's volumetric fog (RENDER.VOLFOG 1 / voxi.volumetricFog): they add extinction, scattering and emission to the froxel
 * volume. REFLECTION PROBES are recorded and answer queries (influence, box-projected direction); the renderer does not sample a capture yet. */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI */

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_ENVFX_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_envfx_abi_version(void);

#define AVER_FW_FOGVOL_MAX   64   /* volumes the registry keeps; the renderer draws the 16 nearest the camera */
#define AVER_FW_PROBE_MAX    32

/* Godot's FogVolumeShape numbers. */
#define AVER_FW_FOGVOL_ELLIPSOID 0
#define AVER_FW_FOGVOL_CONE      1
#define AVER_FW_FOGVOL_CYLINDER  2
#define AVER_FW_FOGVOL_BOX       3
#define AVER_FW_FOGVOL_WORLD     4

/* ---- fog volumes: script side ----------------------------------------------------------------------------------- */

AVER_FW_ABI int32_t aver_fw_fogvol_create(const char* key);
AVER_FW_ABI int32_t aver_fw_fogvol_destroy(int32_t id);
AVER_FW_ABI int32_t aver_fw_fogvol_find(const char* key);
/* Everything about the volume in one call. albedo3 / emission3 may be null (white / black). heightFalloff per metre, edgeFade a fraction of the half size.
 * densityPerM is Godot's FogMaterial.density (extinction per metre, negative carves). Returns 0 for a bad id, a non-finite value or a singular matrix. */
AVER_FW_ABI int32_t aver_fw_fogvol_set(int32_t id, int32_t shape, const float* world12, const float* halfExtentCm3, float densityPerM,
                                       const float* albedo3, const float* emission3, float heightFalloff, float edgeFade);
AVER_FW_ABI int32_t aver_fw_fogvol_set_enabled(int32_t id, int32_t on);
AVER_FW_ABI int32_t aver_fw_fogvol_count(void);
/* The density (extinction per cm, signed) of every volume summed at a world point: the number the shader adds to the medium. For scripts and tests. */
AVER_FW_ABI float   aver_fw_fogvol_density_at(float x, float y, float z);

/* ---- fog volumes: host side ------------------------------------------------------------------------------------- */

/* Increments whenever any volume changes (create, destroy, set, enable). The host rebuilds its block only when this moved. */
AVER_FW_ABI int64_t aver_fw_fogvol_revision(void);
/* Packs the enabled volumes nearest (x, y, z) first into the renderer's block (FogVolumeBlock: a 4-float header whose x is the count, then 16 x 6 float4).
 * outFloats must hold 4 + 16 * 6 * 4 = 388 floats. Returns the number of volumes packed (0 = none, the block header is still written). */
AVER_FW_ABI int32_t aver_fw_fogvol_pack(float x, float y, float z, float* outFloats, int32_t capFloats);

/* ---- reflection probes: script side ----------------------------------------------------------------------------- */

#define AVER_FW_PROBE_AMBIENT_DISABLED    0
#define AVER_FW_PROBE_AMBIENT_ENVIRONMENT 1
#define AVER_FW_PROBE_AMBIENT_COLOUR      2

AVER_FW_ABI int32_t aver_fw_probe_create(const char* key);
AVER_FW_ABI int32_t aver_fw_probe_destroy(int32_t id);
AVER_FW_ABI int32_t aver_fw_probe_find(const char* key);
/* Placement and look: world12, halfExtentCm3 and originOffsetCm3 as above (originOffset in the probe's local axes, null = 0). */
AVER_FW_ABI int32_t aver_fw_probe_set_box(int32_t id, const float* world12, const float* halfExtentCm3, const float* originOffsetCm3);
/* intensity, blend distance (cm), max distance (cm, 0 = unlimited), box projection, interior, update mode (0 once, 1 always), resolution (px). */
AVER_FW_ABI int32_t aver_fw_probe_set_look(int32_t id, float intensity, float blendDistanceCm, float maxDistanceCm, int32_t boxProjection,
                                           int32_t interior, int32_t updateMode, int32_t resolution);
AVER_FW_ABI int32_t aver_fw_probe_set_ambient(int32_t id, int32_t mode, const float* colour3, float energy);
AVER_FW_ABI int32_t aver_fw_probe_set_masks(int32_t id, uint32_t cullMask, uint32_t reflectionMask);
AVER_FW_ABI int32_t aver_fw_probe_set_enabled(int32_t id, int32_t on);
AVER_FW_ABI int32_t aver_fw_probe_count(void);
/* The probes that light a world point seen from (cx, cy, cz): up to 4 (index = the probe's id, weight = its share of the blend, rest = what is left for the sky).
 * Returns the count; outIds / outWeights hold at least 4 entries, outRest one. */
AVER_FW_ABI int32_t aver_fw_probe_select(float x, float y, float z, float cx, float cy, float cz, int32_t* outIds, float* outWeights, float* outRest);
/* The direction a reflection ray (dx, dy, dz) from the point should look the probe's capture up with (box projection applied). out3 receives a unit vector;
 * returns 0 for an unknown id (out3 = the input direction). */
AVER_FW_ABI int32_t aver_fw_probe_project(int32_t id, float x, float y, float z, float dx, float dy, float dz, float* out3);

/* ---- host side -------------------------------------------------------------------------------------------------- */

AVER_FW_ABI int64_t aver_fw_probe_revision(void);
/* Session boundary: forgets every volume and probe. The first call after the play state falls back to AVER_FW_PLAY_EDITOR does it by itself through
 * aver_fw_envfx_tick; a host may also call this on a level change. */
AVER_FW_ABI void    aver_fw_envfx_reset(void);
AVER_FW_ABI void    aver_fw_envfx_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* AVER_FRAMEWORK_ENVFX_ABI_H */
