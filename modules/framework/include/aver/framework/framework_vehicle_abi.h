#ifndef AVER_FRAMEWORK_VEHICLE_ABI_H
#define AVER_FRAMEWORK_VEHICLE_ABI_H

/* Vehicle C ABI (Aver.Framework): spawning, driving, gears, flip, repair, damage, live tuning, telemetry and the event
 * stream, relayed to the vehicle system.
 *
 * Same conventions as framework_abi.h (int32_t/float only, 1 on success and 0 on refusal). Aver.Framework does not link the
 * vehicle module, so a composition root installs ONE provider function and every call below forwards to it; with no provider
 * every call returns 0 and nothing is written. The provider is vehicle::vehicleProvider
 * (modules/vehicle.scene/include/aver/vehicle/VehiclePhysicsSystem.hpp). Adding or removing the CVehicle component, reading its
 * OUT fields and writing its input fields (inThrottle, inSteer, inBrake, inHandbrake, the scales and flags) use the generic scene
 * ABI, as soft bodies and motion matching do; this header carries what needs arguments. Own version, separate from
 * AVER_FW_ABI_VERSION_MINOR. Semantics: docs/VEHICLES.md.
 *
 * IN THIS ABI `vehicle` IS THE ENTITY (the system maps it to the physics handle); the physics handle appears only as spawn's
 * second output, for the older Aver.Physics.Vehicle rows. The provider applies a call at once (the physics world is idle
 * between steps); telemetry reads are the last fixed step's.
 *
 * PACKING. The provider's `in` array is floats. An integer argument (an entity, a gear, a flag) travels as the BIT PATTERN of an
 * int32 (memcpy), never as a converted value: an entity id has 31 significant bits and a float keeps 24. */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI, AVER_FW_CALL */

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_VEHICLE_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_vehicle_abi_version(void);

/* Provider operations; the argument usage of each is listed with it. `text` is only used by SPAWN and TUNE. */
#define AVER_FW_VEH_OP_SPAWN       1   /* text = .ocvehicle path, preset name or inline OCVEHICLE text; in: x y z yawDeg; out i[0] = entity, i[1] = physics vehicle */
#define AVER_FW_VEH_OP_ENTER       2   /* entity = vehicle; in: driver(i) */
#define AVER_FW_VEH_OP_EXIT        3   /* out i[0] = the driver that left */
#define AVER_FW_VEH_OP_SHIFT       4   /* in: delta(i) */
#define AVER_FW_VEH_OP_SET_GEAR    5   /* in: gear(i) */
#define AVER_FW_VEH_OP_SET_MANUAL  6   /* in: manual(i) */
#define AVER_FW_VEH_OP_FLIP        7   /* in: liftCm */
#define AVER_FW_VEH_OP_REPAIR      8
#define AVER_FW_VEH_OP_DAMAGE      9   /* in: amount01 */
#define AVER_FW_VEH_OP_TUNE       10   /* text = key; in: value */
#define AVER_FW_VEH_OP_TELEMETRY  11   /* outF: AVER_FW_VEH_TF_*, outI: AVER_FW_VEH_TI_*; cap is the capacity of both */
#define AVER_FW_VEH_OP_WHEEL      12   /* in: wheel(i); outF: AVER_FW_VEH_WF_*, outI: AVER_FW_VEH_WI_*; cap is the capacity of both */
#define AVER_FW_VEH_OP_EVENTS     13   /* drains up to cap events: outI 6 ints each (kind vehicleEntity a b seq otherEntity), outF 8 floats each (point xyz normal xyz value value2); returns the count */

/* Telemetry floats. 0..17 are the physics VS block. */
#define AVER_FW_VEH_TF_RPM                 0
#define AVER_FW_VEH_TF_ENGINE_TORQUE_NM    1
#define AVER_FW_VEH_TF_SPEED_CMS           2
#define AVER_FW_VEH_TF_FORWARD_SPEED_CMS   3
#define AVER_FW_VEH_TF_LATERAL_SPEED_CMS   4
#define AVER_FW_VEH_TF_THROTTLE            5
#define AVER_FW_VEH_TF_BRAKE               6
#define AVER_FW_VEH_TF_STEER               7
#define AVER_FW_VEH_TF_HANDBRAKE           8
#define AVER_FW_VEH_TF_CLUTCH              9
#define AVER_FW_VEH_TF_WHEEL_RPM_AT_CLUTCH 10
#define AVER_FW_VEH_TF_YAW_RATE_DEG_S      11
#define AVER_FW_VEH_TF_LONG_ACCEL_G        12
#define AVER_FW_VEH_TF_LAT_ACCEL_G         13
#define AVER_FW_VEH_TF_UP_Z                14
#define AVER_FW_VEH_TF_TRACK_LEFT_CMS      15
#define AVER_FW_VEH_TF_TRACK_RIGHT_CMS     16
#define AVER_FW_VEH_TF_AIRBORNE_SECONDS    17
#define AVER_FW_VEH_TF_HEALTH01            18
#define AVER_FW_VEH_TF_COUNT               19

/* Telemetry ints. 0..10 are the physics VSI block; STATE is CVehicle.outState, DRIVER the driving entity. */
#define AVER_FW_VEH_TI_GEAR                0
#define AVER_FW_VEH_TI_GEAR_COUNT          1
#define AVER_FW_VEH_TI_MANUAL              2
#define AVER_FW_VEH_TI_SHIFTING            3
#define AVER_FW_VEH_TI_WHEELS_ON_GROUND    4
#define AVER_FW_VEH_TI_ABS_ACTIVE          5
#define AVER_FW_VEH_TI_TCS_ACTIVE          6
#define AVER_FW_VEH_TI_KIND                7
#define AVER_FW_VEH_TI_AXLE_COUNT          8
#define AVER_FW_VEH_TI_WHEEL_COUNT         9
#define AVER_FW_VEH_TI_REV_LIMITED         10
#define AVER_FW_VEH_TI_STATE               11
#define AVER_FW_VEH_TI_DRIVER              12
#define AVER_FW_VEH_TI_COUNT               13

/* Wheel floats: the physics VWS block. */
#define AVER_FW_VEH_WF_SLIP_LONG           0
#define AVER_FW_VEH_WF_SLIP_LAT_DEG        1
#define AVER_FW_VEH_WF_LOAD_N              2
#define AVER_FW_VEH_WF_COMPRESSION_CM      3
#define AVER_FW_VEH_WF_SUSP_LENGTH_CM      4
#define AVER_FW_VEH_WF_ANG_VEL_RAD_S       5
#define AVER_FW_VEH_WF_STEER_DEG           6
#define AVER_FW_VEH_WF_SPIN_RAD            7
#define AVER_FW_VEH_WF_CONTACT_POINT_X     8
#define AVER_FW_VEH_WF_CONTACT_POINT_Y     9
#define AVER_FW_VEH_WF_CONTACT_POINT_Z     10
#define AVER_FW_VEH_WF_CONTACT_NORMAL_X    11
#define AVER_FW_VEH_WF_CONTACT_NORMAL_Y    12
#define AVER_FW_VEH_WF_CONTACT_NORMAL_Z    13
#define AVER_FW_VEH_WF_FORCE_LONG_N        14
#define AVER_FW_VEH_WF_FORCE_LAT_N         15
#define AVER_FW_VEH_WF_SURFACE_FRICTION    16
#define AVER_FW_VEH_WF_BRAKE_TORQUE_NM     17
#define AVER_FW_VEH_WF_COUNT               18

/* Wheel ints: the physics VWSI block. CONTACT_ENTITY is the scene entity stamped on the body under the wheel. */
#define AVER_FW_VEH_WI_CONTACT             0
#define AVER_FW_VEH_WI_CONTACT_BODY        1
#define AVER_FW_VEH_WI_CONTACT_ENTITY      2
#define AVER_FW_VEH_WI_SKIDDING            3
#define AVER_FW_VEH_WI_COUNT               4

/* Event ints per event, and floats per event. */
#define AVER_FW_VEH_EVENT_INTS             6
#define AVER_FW_VEH_EVENT_FLOATS           8

typedef int32_t (AVER_FW_CALL* aver_fw_vehicle_fn)(int32_t op, int32_t entity, const float* in, int32_t inCount,
                                                   const char* text, float* outF, int32_t* outI, int32_t cap, void* user);
/* Installs the provider. Passing null clears it. Always returns 1. */
AVER_FW_ABI int32_t aver_fw_set_vehicle_provider(aver_fw_vehicle_fn fn, void* user);

/* Makes an entity with a Vehicle component at the pose and builds it now. `assetOrPresetOrInlineText` is a content path of an
 * .ocvehicle, a preset name (car van truck bus sports tank), or inline .ocvehicle text (it begins OCVEHICLE). A preset gets a
 * default size and needs no mesh. outEntity / outVehicle (may be null) receive the entity and the physics vehicle handle. */
AVER_FW_ABI int32_t aver_fw_vehicle_spawn(const char* assetOrPresetOrInlineText, float x, float y, float z, float yawDeg,
                                          int32_t* outEntity, int32_t* outVehicle);
/* Puts `driver` in the seat: reads the Drive.* actions, hides and parks the driver, creates the camera rigs. 0 if taken or not a vehicle. */
AVER_FW_ABI int32_t aver_fw_vehicle_enter(int32_t vehicle, int32_t driver);
/* Gets the driver out beside the vehicle. outDriver (may be null) receives the entity that left. */
AVER_FW_ABI int32_t aver_fw_vehicle_exit(int32_t vehicle, int32_t* outDriver);
/* Manual gearbox: one gear up (+1) or down (-1), or a gear (-1 reverse, 0 neutral). */
AVER_FW_ABI int32_t aver_fw_vehicle_shift(int32_t vehicle, int32_t delta);
AVER_FW_ABI int32_t aver_fw_vehicle_set_gear(int32_t vehicle, int32_t gear);
AVER_FW_ABI int32_t aver_fw_vehicle_set_manual(int32_t vehicle, int32_t manual);
/* Upright about its heading, stopped, set down liftCm higher. */
AVER_FW_ABI int32_t aver_fw_vehicle_flip(int32_t vehicle, float liftCm);
AVER_FW_ABI int32_t aver_fw_vehicle_repair(int32_t vehicle);
/* Health lost, as a fraction of the spec's DAMAGE health. */
AVER_FW_ABI int32_t aver_fw_vehicle_damage(int32_t vehicle, float amount01);
/* Live tuning by key: power brake grip suspension steer drag downforce abs tcs manual collisionperiod. */
AVER_FW_ABI int32_t aver_fw_vehicle_tune(int32_t vehicle, const char* key, float value);
/* Writes min(nF, AVER_FW_VEH_TF_COUNT) floats and min(nI, AVER_FW_VEH_TI_COUNT) ints (either array may be null). 0 for a vehicle that is not built. */
AVER_FW_ABI int32_t aver_fw_vehicle_telemetry(int32_t vehicle, float* outF, int32_t nF, int32_t* outI, int32_t nI);
/* The same for one wheel: AVER_FW_VEH_WF_* and AVER_FW_VEH_WI_*. */
AVER_FW_ABI int32_t aver_fw_vehicle_wheel(int32_t vehicle, int32_t wheel, float* outF, int32_t nF, int32_t* outI, int32_t nI);
/* Up to `max` events: outI gets AVER_FW_VEH_EVENT_INTS ints per event, outF AVER_FW_VEH_EVENT_FLOATS floats. Returns the count. */
AVER_FW_ABI int32_t aver_fw_vehicle_drain_events(int32_t* outI, float* outF, int32_t max);

#ifdef __cplusplus
}
#endif

#endif /* AVER_FRAMEWORK_VEHICLE_ABI_H */
