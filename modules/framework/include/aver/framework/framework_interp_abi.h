#ifndef AVER_FRAMEWORK_INTERP_ABI_H
#define AVER_FRAMEWORK_INTERP_ABI_H

/* Pose interpolation C ABI (Aver.Framework): which entities are drawn between their last two fixed steps. PHYSICS.INTERPOLATION already does
 * that for dynamic rigid bodies; this lets a script put ANY entity on the same footing (a character or platform moved in the physics tick, an
 * animated mover), take one out, switch the whole thing at run time, teleport without a blend, and read the pose that is drawn.
 *
 * The script PUBLISHES, the host READS: Runtime's BodyPoseSync captures every ON entity's world pose around each fixed step, puts it back on its
 * real pose before gameplay runs, and writes the blended pose after the frame's ticks. Move an interpolated entity in the physics tick;
 * a change made outside it is overwritten by the blend (call aver_fw_interp_reset after a teleport).
 *
 * Own version, separate from AVER_FW_ABI_VERSION_MINOR. int32_t/float only; entities are the scene's entity ids (> 0). Single game thread.
 * MODES. INHERIT (the default for every entity) follows the switch: a dynamic rigid body is blended when the switch is on, anything else is not.
 * ON blends the entity whatever the switch says. OFF never blends it, a rigid body included. */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI */

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_INTERP_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_interp_abi_version(void);

#define AVER_FW_INTERP_INHERIT 0
#define AVER_FW_INTERP_ON      1
#define AVER_FW_INTERP_OFF     2

/* The switch every INHERIT entity follows: -1 leaves it to the project (PHYSICS.INTERPOLATION), 0 forces it off, 1 forces it on. */
AVER_FW_ABI void    aver_fw_interp_set_enabled(int32_t enabled);
AVER_FW_ABI int32_t aver_fw_interp_enabled(void);                         /* the value set, -1 by default */

/* Per-entity mode (AVER_FW_INTERP_*). An unknown mode or entity <= 0 is ignored. INHERIT drops the entity's record. */
AVER_FW_ABI void    aver_fw_interp_set_mode(int32_t entity, int32_t mode);
AVER_FW_ABI int32_t aver_fw_interp_mode(int32_t entity);                  /* INHERIT for an entity never set */

/* The next pose is a teleport: the entity is drawn at it at once, with no blend from where it was. */
AVER_FW_ABI void    aver_fw_interp_reset(int32_t entity);

/* The pose last drawn for an entity the host blended: world position (3, engine cm) and rotation (x y z w). 0 when it has none (not blended, or
 * not yet drawn) and the outputs are untouched; either pointer may be NULL. */
AVER_FW_ABI int32_t aver_fw_interp_pose(int32_t entity, float* outPosition3, float* outRotation4);

/* ---- host side ---- */

/* Writes up to `cap` records, one per entity whose mode is not INHERIT: the entity, its mode and its teleport serial (it changes whenever
 * aver_fw_interp_reset is called). Returns how many records exist, which may exceed `cap`; any pointer may be NULL. */
AVER_FW_ABI int32_t aver_fw_interp_entries(int32_t* outEntity, int32_t* outMode, int32_t* outResetSerial, int32_t cap);
/* The teleport serial of an entity, 0 if it was never reset. */
AVER_FW_ABI int32_t aver_fw_interp_reset_serial(int32_t entity);
/* Stores (position != NULL) or forgets (NULL) the pose drawn for an entity. */
AVER_FW_ABI void    aver_fw_interp_publish(int32_t entity, const float* position3, const float* rotation4);
/* Drops every record and the switch, a play session's end. */
AVER_FW_ABI void    aver_fw_interp_clear(void);

#ifdef __cplusplus
}
#endif

#endif
