#ifndef AVER_DECAL_ABI_H
#define AVER_DECAL_ABI_H

/* Decal C ABI -- the pooled gameplay-decal spawn API the C# layer binds to (Aver.Scene/Decals.cs).
 * Lives in Aver.Scene.dll beside scene_abi.h and shares its export macro. Authored decals are plain
 * CDecal components, reachable through the generic scene ABI; this is only the runtime spawn side.
 * docs/rendering/DECALS.md. */

#include "aver/scene/scene_abi.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_DECAL_ABI_VERSION 1

/* Mirrors CDecal's kDecal* flag bits (Components.hpp); checked in DecalAbi.cpp. */
#define AVER_DECAL_NO_COLOUR    0x2
#define AVER_DECAL_NO_NORMAL    0x4
#define AVER_DECAL_NO_ROUGHNESS 0x8

/* One gameplay decal. Zero in a field means "default", exactly as on CDecal (see Components.hpp).
 * The pose is `position` plus EITHER `normal` (the surface normal the decal sits on; it faces into the
 * surface, rolled by rollRad about the projection axis, up hint `up` or +Z) when normal is non-zero,
 * OR the `rotation` quaternion (x y z w) otherwise. The decal projects along its local +X.
 * Field order is part of the ABI: 8-byte members first so there is no padding on any compiler. */
typedef struct AverDecalSpawnDesc {
    int64_t baseTexture;      /* ObjectId (fnv1a64 of the content-relative path); 0 = none */
    int64_t normalTexture;
    int64_t ormTexture;       /* green roughness, blue metallic */
    float   position[3];
    float   normal[3];
    float   up[3];
    float   rotation[4];
    float   rollRad;
    float   sizeCm[3];        /* full box: x depth, y width, z height */
    float   tint[3];          /* linear; all 0 = white */
    float   transparency;     /* 0 opaque .. 1 invisible */
    float   normalStrength;
    float   roughness;
    float   metallic;
    float   edgeFade;
    float   angleFadeStartDeg;
    float   angleFadeEndDeg;
    float   fadeDistanceCm;
    float   uvScale[2];
    float   uvOffset[2];
    float   lifetimeSec;      /* 0 = until recycled */
    float   fadeOutSec;
    int32_t sortOrder;
    uint32_t flags;           /* AVER_DECAL_NO_* */
} AverDecalSpawnDesc;

/* Returns AVER_DECAL_ABI_VERSION as the DLL was built with. */
AVER_SCENE_ABI int32_t aver_decal_abi_version(void);

/* Sets how many pooled decals may be alive at once (default 256), dropping the old pool. 0 disables
 * spawning. Returns the capacity in force; a clamped request records InvalidArgument on
 * aver_scene_last_error. */
AVER_SCENE_ABI int32_t aver_decal_pool_set_capacity(int32_t capacity);

/* Spawns a decal, recycling the oldest when the pool is full. Returns its entity (> 0), or 0 when
 * desc is null, a position/size/orientation value is not finite, the pool is disabled, or the world
 * refused an entity.
 * Reason on aver_scene_last_error (same DLL); see ErrorCodes.hpp. */
AVER_SCENE_ABI int32_t aver_decal_spawn(const AverDecalSpawnDesc* desc);

/* Gives one decal back now. 1 when `entity` was an active pooled decal.
 * Reason on aver_scene_last_error (same DLL); see ErrorCodes.hpp. */
AVER_SCENE_ABI int32_t aver_decal_release(int32_t entity);

/* Releases every pooled decal. */
AVER_SCENE_ABI void aver_decal_clear(void);

/* Advances lifetimes by dt seconds; call once per game frame. */
AVER_SCENE_ABI void aver_decal_tick(float dt);

/* Pooled decals currently visible, and the pool's capacity. */
AVER_SCENE_ABI int32_t aver_decal_active_count(void);
AVER_SCENE_ABI int32_t aver_decal_capacity(void);

#ifdef __cplusplus
}
#endif

#endif
