#ifndef AVER_FRAMEWORK_SOFTBODY_ABI_H
#define AVER_FRAMEWORK_SOFTBODY_ABI_H

/* Cloth and destruction C ABI (Aver.Framework): damage, explosions, cuts, repair, release, wind, budgets and the
 * event stream, relayed to the soft-body system.
 *
 * Same conventions as framework_abi.h (int32_t/float only, 1 on success and 0 on refusal). Aver.Framework does not
 * link the soft-body module, so a composition root installs ONE provider function and every call below forwards to
 * it; with no provider every call returns 0 and nothing is written. The provider is softbody::softBodyProvider
 * (modules/softbody.scene/include/aver/softbody/SoftBodySystem.hpp). Every op is DEFERRED to the system's next tick
 * and applied in arrival order, which is what keeps a replay deterministic. Adding or removing the components
 * (CCloth, CDestructible) and reading their fields uses the generic scene ABI (aver_scene_add_component by name,
 * aver_scene_field), as motion matching does. Own version, separate from AVER_FW_ABI_VERSION_MINOR. Semantics:
 * docs/SOFTBODY.md. */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI, AVER_FW_CALL */

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_SOFTBODY_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_softbody_abi_version(void);

/* Provider operations; the argument usage of each is listed with it. */
#define AVER_FW_SB_OP_DAMAGE   1   /* in: x y z dirx diry dirz radius energy kind  (entity or 0) */
#define AVER_FW_SB_OP_EXPLODE  2   /* in: x y z radius impulse energy; out i[0] = entities touched */
#define AVER_FW_SB_OP_TEAR     3   /* in: x y z radius; out i[0] = beams a cut of that radius breaks now */
#define AVER_FW_SB_OP_REPAIR   4
#define AVER_FW_SB_OP_RELEASE  5
#define AVER_FW_SB_OP_WIND     6   /* in: vx vy vz gust gustHz turbulence */
#define AVER_FW_SB_OP_BUDGET   7   /* in: maxPieces maxParticles maxRebuilds lifetime */
#define AVER_FW_SB_OP_PRESET   8   /* in: preset (changes the preset of the entity's component) */
#define AVER_FW_SB_OP_STATE    9   /* out f: damage01 maxStrain; out i: pieces brokenEdges state (bit0 awake, bit1 frozen) */
#define AVER_FW_SB_OP_EVENTS  10   /* drains up to cap events: i = kind entity piece beam seq (5 each); f = x y z value (4 each); returns the count */

typedef int32_t (AVER_FW_CALL* aver_fw_softbody_fn)(int32_t op, int32_t entity, const float* in, int32_t inCount,
                                                    float* outF, int32_t* outI, int32_t cap, void* user);
/* Installs the provider. Passing null clears it. Always returns 1. */
AVER_FW_ABI int32_t aver_fw_set_softbody_provider(aver_fw_softbody_fn fn, void* user);

/* Hurts the soft body on `entity` (0 = every one with a piece in reach): `energyJ` spread over the edges within
 * `radiusCm` of the point, along `dir` (zero = radial). kind: 0 impact, 1 explosion, 2 gameplay. */
AVER_FW_ABI int32_t aver_fw_softbody_damage(int32_t entity, float x, float y, float z, float dx, float dy, float dz,
                                            float radiusCm, float energyJ, int32_t kind);
/* A radial shove and damage to everything in reach. Returns how many entities are in reach, 0 with no provider. */
AVER_FW_ABI int32_t aver_fw_softbody_explode(float x, float y, float z, float radiusCm, float impulseCmS, float energyJ);
/* A scripted cut. */
AVER_FW_ABI int32_t aver_fw_softbody_tear(int32_t entity, float x, float y, float z, float radiusCm);
/* Back to intact: pieces gone, dents out, tears closed. */
AVER_FW_ABI int32_t aver_fw_softbody_repair(int32_t entity);
/* Unpins a cloth: a cape falls off. */
AVER_FW_ABI int32_t aver_fw_softbody_release(int32_t entity);
/* The world wind, cm/s (independent of the sky's cloud drift). */
AVER_FW_ABI int32_t aver_fw_softbody_set_wind(float vx, float vy, float vz, float gustSpeed, float gustHz, float turbulence);
AVER_FW_ABI int32_t aver_fw_softbody_set_budget(int32_t maxPieces, int32_t maxParticles, int32_t maxRebuilds, float debrisLifetimeS);
AVER_FW_ABI int32_t aver_fw_softbody_set_preset(int32_t entity, int32_t preset);
/* outDamageMaxStrain2: damage01, maxStrain. outPiecesBrokenState3: pieces, brokenEdges, state. 0 when the entity has no soft body. */
AVER_FW_ABI int32_t aver_fw_softbody_state(int32_t entity, float* outDamageMaxStrain2, int32_t* outPiecesBrokenState3);
/* Up to `max` events: outI gets 5 ints per event (kind entity piece beam seq), outF 4 floats (x y z value). Returns the count. */
AVER_FW_ABI int32_t aver_fw_softbody_drain_events(int32_t* outI, float* outF, int32_t max);

#ifdef __cplusplus
}
#endif

#endif /* AVER_FRAMEWORK_SOFTBODY_ABI_H */
