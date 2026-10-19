#ifndef AVER_FRAMEWORK_SOFTFIELD_ABI_H
#define AVER_FRAMEWORK_SOFTFIELD_ABI_H

/* Soft-body field C ABI (Aver.Framework): force, damage, anchor-release, kill and sleep volumes for cloth and destructibles,
 * and the crash-damage helper, relayed to the soft-body system.
 *
 * Same conventions as framework_softbody_abi.h (int32_t/float only; every call below forwards to ONE provider function a
 * composition root installs, the provider being softbody::softFieldProvider with the SoftBodySystem as `user`; with no
 * provider every call returns 0 and nothing is written). Every op is DEFERRED to the soft-body system's next tick and
 * applied in arrival order, which is what keeps a replay deterministic. A call whose values are not finite, or whose kind
 * or shape is out of range, is refused before it reaches the provider. A field placed in a level (COMPONENT SoftField,
 * CSoftField) needs none of this: it is a component, reached through the generic scene ABI. Own version, separate from
 * AVER_FW_ABI_VERSION_MINOR. Semantics: docs/SOFTBODY.md "Fields". VEHICLES calls aver_fw_softfield_impact for crash damage. */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI, AVER_FW_CALL */

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_SOFTFIELD_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_softfield_abi_version(void);

#define AVER_FW_SF_SPEC_COUNT 24
/* spec[]: 0 kind (0 radial, 1 directional, 2 damage, 3 release anchors, 4 kill, 5 sleep), 1 shape (0 sphere, 1 box,
 * 2 capsule, 3 everywhere), 2 targets (mask: 1 cloth, 2 destructible, 4 debris; 0 = all, or debris for Kill),
 * 3 perSecond (0/1), 4-6 centre xyz (cm), 7-9 extents xyz (cm: sphere radius = x, box half extents, capsule radius = x and
 * half height = z), 10-12 yaw pitch roll (degrees), 13-15 dir xyz, 16 strength (cm/s velocity change; negative pulls),
 * 17 energyJ (damage), 18 falloff, 19 turbulence (0..1), 20 wakeCmS, 21 lifetimeS (start and spawn only), 22-23 reserved (0) */
#define AVER_FW_SF_OP_PULSE   1   /* in: spec; out i[0] = pieces in reach now */
#define AVER_FW_SF_OP_START   2   /* in: spec; out i[0] = handle (0 = refused) */
#define AVER_FW_SF_OP_STOP    3   /* in: f[0] = handle */
#define AVER_FW_SF_OP_IMPACT  4   /* in: x y z dx dy dz radiusCm energyJ kickCmS; out i[0] = pieces in reach now */
#define AVER_FW_SF_OP_SPAWN   5   /* in: spec; out i[0] = entity (a CSoftField entity that removes itself after lifetimeS) */

typedef int32_t (AVER_FW_CALL* aver_fw_softfield_fn)(int32_t op, int32_t entity, const float* in, int32_t inCount,
                                                     float* outF, int32_t* outI, int32_t cap, void* user);
/* Installs the provider. Passing null clears it. Always returns 1. */
AVER_FW_ABI int32_t aver_fw_set_softfield_provider(aver_fw_softfield_fn fn, void* user);

/* Applies the field once at the next tick (strength and energy are totals; perSecond is ignored). Returns the number of
 * pieces in reach at call time, an upper bound of what the next tick touches; 0 with no provider or on refusal. */
AVER_FW_ABI int32_t aver_fw_softfield_pulse(const float* spec, int32_t n);
/* Starts a continuous field (strength and energy become per second) that stops after spec[21] seconds. Returns a handle,
 * 0 = refused. */
AVER_FW_ABI int32_t aver_fw_softfield_start(const float* spec, int32_t n);
/* Stops a field started with start. 1 if the handle was live. */
AVER_FW_ABI int32_t aver_fw_softfield_stop(int32_t handle);
/* Damage energy in a sphere at (x, y, z), shoved along (dx, dy, dz) (zero = radial) with an extra kick of kickCmS. Returns the
 * pieces in reach at call time. */
AVER_FW_ABI int32_t aver_fw_softfield_impact(float x, float y, float z, float dx, float dy, float dz,
                                             float radiusCm, float energyJ, float kickCmS);
/* Creates an entity with a CSoftField from the spec that destroys itself after spec[21] seconds. Returns the entity, 0 = refused. */
AVER_FW_ABI int32_t aver_fw_softfield_spawn(const float* spec, int32_t n);

#ifdef __cplusplus
}
#endif

#endif /* AVER_FRAMEWORK_SOFTFIELD_ABI_H */
