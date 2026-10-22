#ifndef AVER_FRAMEWORK_HAIR_ABI_H
#define AVER_FRAMEWORK_HAIR_ABI_H

/* Strand hair C ABI (Aver.Framework): teleport, script colliders and a state read, relayed to the hair system
 * (docs/plans/wave11/SOFTBODY_HAIR.md 2.20, docs/HAIR.md).
 *
 * Same conventions as framework_vehicle_abi.h (int32_t/float/const char* only, 1 on success and 0 on refusal). Aver.Framework does not
 * link the hair module, so a composition root installs ONE provider function and every call below forwards to it; with no provider every
 * call returns 0 and nothing is written. The provider is hair::hairProvider (modules/hair.scene/include/aver/hair/HairSystem.hpp), whose
 * `user` is the HairSystem. Adding the CHair component and writing its fields (asset, material, flags, widthScale, densityScale, lodBias,
 * windScale, stiffnessScale, teleportCm) and reading its OUT fields use the generic scene ABI (aver_scene_add_component by name,
 * aver_scene_field), as CCloth does; this header carries what needs more than a field. The world wind is the soft bodies' (aver_fw_softbody_
 * set_wind): the hair system takes it from the same place. The ops are between-step calls and apply at once, in arrival order. Own version,
 * separate from AVER_FW_ABI_VERSION_MINOR. Semantics: docs/HAIR.md.
 *
 * PACKING. The provider's `in` array is floats; the shape code travels as a small integer in a float slot (exact). `text` is the bone name. */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI, AVER_FW_CALL */

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_HAIR_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_hair_abi_version(void);

/* Provider operations; `entity` is the entity carrying the CHair. */
#define AVER_FW_HAIR_OP_TELEPORT         1   /* the default pose at the next update, wherever the head now is */
#define AVER_FW_HAIR_OP_ADD_COLLIDER     2   /* text = bone; in: shape sx sy sz ox oy oz; out i[0] = the entity's script collider count after */
#define AVER_FW_HAIR_OP_CLEAR_COLLIDERS  3   /* removes every collider a script added to the entity */
#define AVER_FW_HAIR_OP_STATE            4   /* outI: AVER_FW_HAIR_TI_*; cap must be at least AVER_FW_HAIR_TI_COUNT */

/* Collider shapes (the .ochair PROXY shapes). Sizes in centimetres, in the owner's model space: a sphere reads sx (radius), a capsule
 * sx (radius) and sy (half height of the straight part, along engine +Z), a box sx sy sz (half extents). The offset is in the bone's frame. */
#define AVER_FW_HAIR_SHAPE_SPHERE   0
#define AVER_FW_HAIR_SHAPE_CAPSULE  1
#define AVER_FW_HAIR_SHAPE_BOX      2

/* State ints. STATE is CHair::outState & 0xFF (bit0 simulating, bit1 frozen by distance, bit2 inert, bit3 building); LOD is the strand level 0..3. */
#define AVER_FW_HAIR_TI_STATE            0
#define AVER_FW_HAIR_TI_LOD              1
#define AVER_FW_HAIR_TI_RENDER_STRANDS   2
#define AVER_FW_HAIR_TI_SIM_STRANDS      3
#define AVER_FW_HAIR_TI_COLLIDERS        4   /* script colliders asked for, including ones still waiting for the groom to build */
#define AVER_FW_HAIR_TI_BUILT            5   /* 1 once the groom is built and a physics hair exists */
#define AVER_FW_HAIR_TI_COUNT            6

typedef int32_t (AVER_FW_CALL* aver_fw_hair_fn)(int32_t op, int32_t entity, const float* in, int32_t inCount,
                                                const char* text, float* outF, int32_t* outI, int32_t cap, void* user);
/* Installs the provider. Passing null clears it. Always returns 1. */
AVER_FW_ABI int32_t aver_fw_set_hair_provider(aver_fw_hair_fn fn, void* user);

/* The default pose at the next update (after a cutscene cut, a respawn). 0 when the entity has no built hair. */
AVER_FW_ABI int32_t aver_fw_hair_teleport(int32_t entity);
/* Adds a kinematic convex-hull collider that follows `bone` of the entity's skeleton on the hair's collision layer. Applied once the groom is
 * built. outCount (may be null) receives the entity's script collider count. An unknown bone is not detected here (the hull is made at a later
 * tick, which logs and drops it). Returns 0 for no CHair, a bad shape, a size that is not positive, an empty bone or more than 16 colliders. */
AVER_FW_ABI int32_t aver_fw_hair_add_collider(int32_t entity, int32_t shape, float sx, float sy, float sz, float ox, float oy, float oz,
                                              const char* bone, int32_t* outCount);
AVER_FW_ABI int32_t aver_fw_hair_clear_colliders(int32_t entity);
/* outI6: the AVER_FW_HAIR_TI_* ints. 0 when the entity has no CHair. */
AVER_FW_ABI int32_t aver_fw_hair_state(int32_t entity, int32_t* outI6);

#ifdef __cplusplus
}
#endif

#endif /* AVER_FRAMEWORK_HAIR_ABI_H */
