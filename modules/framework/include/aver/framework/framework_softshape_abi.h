#ifndef AVER_FRAMEWORK_SOFTSHAPE_ABI_H
#define AVER_FRAMEWORK_SOFTSHAPE_ABI_H

/* Jelly, ropes, grab and release, soft-body buoyancy and contact policy C ABI (Aver.Framework), relayed to the soft shape
 * system (docs/plans/wave11/SOFTBODY_HAIR.md 2.4 and 2.6).
 *
 * Same conventions as framework_softbody_abi.h (int32_t/float only, 1 on success and 0 on refusal unless a call says it returns an
 * id). Aver.Framework does not link the soft-body module, so a composition root installs ONE provider function and every call
 * below forwards to it; with no provider every call returns 0 and nothing is written. The provider is softbody::softShapeProvider
 * (modules/softbody.scene/include/aver/softbody/SoftShapeSystem.hpp). Adding the components (CSoftVolume, CSoftRope) and reading
 * their fields uses the generic scene ABI (aver_scene_add_component by name, aver_scene_field), as CCloth does; what HAPPENS
 * (grab, buoyancy, a cut, pressure) goes through here. The ops are between-step physics calls and apply at once, in arrival
 * order. Own version, separate from AVER_FW_ABI_VERSION_MINOR. Semantics: docs/SOFTBODY.md. */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI, AVER_FW_CALL */

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_SOFTSHAPE_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_softshape_abi_version(void);

/* Provider operations; the argument usage of each is listed with it. `entity` is the soft entity (cloth, jelly or rope). */
#define AVER_FW_SS_OP_PRESSURE       1   /* in: kPa at rest volume (a closed cloth or a jelly; 0 deflates) */
#define AVER_FW_SS_OP_VOLUME         2   /* out f: restCm3 nowCm3 */
#define AVER_FW_SS_OP_GRAB           3   /* in: x y z reachCm targetX targetY targetZ; out i[0] = grab id */
#define AVER_FW_SS_OP_MOVE_GRAB      4   /* in: grab targetX targetY targetZ */
#define AVER_FW_SS_OP_RELEASE_GRAB   5   /* in: grab */
#define AVER_FW_SS_OP_BUOYANCY       6   /* in: fluidDensityKgM3 drag */
#define AVER_FW_SS_OP_SUBMERGED      7   /* out f: fraction volumeCm3 */
#define AVER_FW_SS_OP_CONTACT        8   /* in: softInvMassScale otherInvMassScale */
#define AVER_FW_SS_OP_PAIR_MODE      9   /* in: otherEntity mode (0 normal, 1 sensor, 2 ignore) */
#define AVER_FW_SS_OP_CUT_ROPE      10   /* in: x y z; out i[0] = the new entity that took the far side of the cut */
#define AVER_FW_SS_OP_CREATE_ROPE   11   /* entity unused; in: x1 y1 z1 x2 y2 z2 lengthCm radiusCm; out i[0] = the rope entity */
#define AVER_FW_SS_OP_SENSOR_EVENTS 12   /* drains up to cap events: i = softEntity sensorEntity entered (3 each); returns the count */

typedef int32_t (AVER_FW_CALL* aver_fw_softshape_fn)(int32_t op, int32_t entity, const float* in, int32_t inCount,
                                                     float* outF, int32_t* outI, int32_t cap, void* user);
/* Installs the provider. Passing null clears it. Always returns 1. */
AVER_FW_ABI int32_t aver_fw_set_softshape_provider(aver_fw_softshape_fn fn, void* user);

/* The inflation pressure of a closed cloth or a jelly, kPa at rest volume (0 deflates). 0 for an entity with no such body, an
 * open mesh or a negative value. */
AVER_FW_ABI int32_t aver_fw_softshape_set_pressure(int32_t entity, float pressureKPa);
/* outRestNowCm3_2: rest volume and current volume in cm^3 (the current one is negative when the shape is inside out). */
AVER_FW_ABI int32_t aver_fw_softshape_volume(int32_t entity, float* outRestNowCm3_2);
/* Attaches the soft body's vertex nearest (x, y, z) within reachCm (<= 0: any) to the target and carries it there each physics
 * step. Returns the grab id (> 0), 0 when nothing is in reach. */
AVER_FW_ABI int32_t aver_fw_softshape_grab(int32_t entity, float x, float y, float z, float reachCm, float targetX, float targetY, float targetZ);
AVER_FW_ABI int32_t aver_fw_softshape_move_grab(int32_t entity, int32_t grab, float targetX, float targetY, float targetZ);
AVER_FW_ABI int32_t aver_fw_softshape_release_grab(int32_t entity, int32_t grab);
/* fluidDensityKgM3 0 = off. The water is the world's (the host water sampler or the global plane). */
AVER_FW_ABI int32_t aver_fw_softshape_set_buoyancy(int32_t entity, float fluidDensityKgM3, float drag);
/* outFractionVolume2: the last step's submerged fraction 0..1 and displaced volume in cm^3. */
AVER_FW_ABI int32_t aver_fw_softshape_submerged(int32_t entity, float* outFractionVolume2);
/* 0 = infinite mass, 1 = unchanged. */
AVER_FW_ABI int32_t aver_fw_softshape_set_contact(int32_t entity, float softInvMassScale, float otherInvMassScale);
AVER_FW_ABI int32_t aver_fw_softshape_set_pair_mode(int32_t entity, int32_t otherEntity, int32_t mode);
/* Splits the rope at its node nearest the point. Returns the new entity (> 0) that took the far side, 0 on refusal. */
AVER_FW_ABI int32_t aver_fw_softshape_cut_rope(int32_t entity, float x, float y, float z);
/* Makes a rope entity between two world points. Returns the entity (> 0), 0 on refusal. */
AVER_FW_ABI int32_t aver_fw_softshape_create_rope(float x1, float y1, float z1, float x2, float y2, float z2, float lengthCm, float radiusCm);
/* Up to `max` sensor enter / exit events of the last tick: outI gets 3 ints per event (softEntity sensorEntity entered). Returns the count. */
AVER_FW_ABI int32_t aver_fw_softshape_drain_sensor_events(int32_t* outI, int32_t max);

#ifdef __cplusplus
}
#endif

#endif /* AVER_FRAMEWORK_SOFTSHAPE_ABI_H */
