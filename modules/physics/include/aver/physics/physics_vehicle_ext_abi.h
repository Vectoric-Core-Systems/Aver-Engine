#pragma once
// Aver.Physics -- VEHICLES v2: what a car, a truck and a tank need beyond physics_vehicle_abi.h. Its own
// header for the reason that one is: the C# parity test pairs headers with binding files, and the 14 v1
// functions are untouched. A vehicle that never calls anything here behaves exactly as it does under v1
// (Jolt's engine curve, gearbox, tyre curves, limited-slip differentials, raw pedals).
//
// UNITS AND AXES ARE THE ENGINE'S, as in physics_vehicle_abi.h: cm, kg, N*m, degrees for steering and slip
// angle, rad/s for wheel speed. Every function returns 1 or 0 (aver_phys_last_error says why) except where
// stated. A PARAMETER BLOCK MAY BE SHORTER THAN ITS DEFINED LENGTH (the rest keep their values) and is
// refused when longer. Builder calls are refused after _finish; live calls are refused before it.
//
// THE DRIVER AIDS RUN IN THE FIXED STEP, not in the frame: with VD_ENABLED set, aver_phys_vehicle_set_input
// stores the pedals and the step loop turns them into what Jolt sees (steering rate and speed-sensitive
// steering, brake-to-reverse, hold at standstill, ABS, traction control, drag and downforce). With it off
// (the default) set_input forwards to Jolt immediately, exactly as v1 does.
//
// WHAT JOLT CANNOT DO is said in the places it bites: there is no hard-locked differential (mode 2 writes a
// limited-slip ratio of 1.01), and the steering sign, the wheel poses and the origin convention are v1's.

#include <stdint.h>

#if defined(_WIN32)
#  if defined(AVER_PHYS_BUILD)
#    define AVER_PHYS_API __declspec(dllexport)
#  else
#    define AVER_PHYS_API __declspec(dllimport)
#  endif
#else
#  define AVER_PHYS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_PHYS_VEHICLE_ABI_VERSION 2

#define AVER_PHYS_VEH_KIND_WHEELED 0
#define AVER_PHYS_VEH_KIND_TRACKED 1

// ---- Parameter blocks: index name (unit) = default -------------------------------------------------

/* VE engine: 0 MAX_TORQUE (N*m) 500, 1 MIN_RPM 1000 (idle), 2 MAX_RPM 6000, 3 INERTIA (kg m^2) 0 = 0.5 x torque scale as v1,
   4 ANGULAR_DAMPING 0.2 (engine braking) */
#define AVER_PHYS_VE_MAX_TORQUE      0
#define AVER_PHYS_VE_MIN_RPM         1
#define AVER_PHYS_VE_MAX_RPM         2
#define AVER_PHYS_VE_INERTIA         3
#define AVER_PHYS_VE_ANGULAR_DAMPING 4
#define AVER_PHYS_VE_COUNT           5

/* VG gearbox: 0 SWITCH_TIME 0.5, 1 CLUTCH_RELEASE_TIME 0.3, 2 SWITCH_LATENCY 0.5, 3 SHIFT_UP_FRACTION 0.6, 4 SHIFT_DOWN_FRACTION 0.2
   (of the rev range), 5 CLUTCH_STRENGTH 0 = 10 x torque scale as v1 */
#define AVER_PHYS_VG_SWITCH_TIME          0
#define AVER_PHYS_VG_CLUTCH_RELEASE_TIME  1
#define AVER_PHYS_VG_SWITCH_LATENCY       2
#define AVER_PHYS_VG_SHIFT_UP_FRACTION    3
#define AVER_PHYS_VG_SHIFT_DOWN_FRACTION  4
#define AVER_PHYS_VG_CLUTCH_STRENGTH      5
#define AVER_PHYS_VG_COUNT                6

/* VA axle: 0 FINAL_DRIVE 3.42, 1 DIFF_MODE 1 (0 open, 1 limited slip, 2 locked; 1 at 1.4 is Jolt's own default), 2 LSD_RATIO 1.4,
   3 LEFT_RIGHT_SPLIT 0.5, 4 TORQUE_SHARE 0 (0 = equal among driven axles), 5 ANTIROLL_STIFFNESS (N/m) 1000, 0 removes the bar */
#define AVER_PHYS_VA_FINAL_DRIVE         0
#define AVER_PHYS_VA_DIFF_MODE           1
#define AVER_PHYS_VA_LSD_RATIO           2
#define AVER_PHYS_VA_LEFT_RIGHT_SPLIT    3
#define AVER_PHYS_VA_TORQUE_SHARE        4
#define AVER_PHYS_VA_ANTIROLL_STIFFNESS  5
#define AVER_PHYS_VA_COUNT               6

/* VW wheel: 0 INERTIA 0.9, 1 ANGULAR_DAMPING 0.2, 2 TV_LONG_FRICTION 4 and 3 TV_LAT_FRICTION 2 (tracked wheels), 4 BRAKE_BIAS 1 */
#define AVER_PHYS_VW_INERTIA          0
#define AVER_PHYS_VW_ANGULAR_DAMPING  1
#define AVER_PHYS_VW_TV_LONG_FRICTION 2
#define AVER_PHYS_VW_TV_LAT_FRICTION  3
#define AVER_PHYS_VW_BRAKE_BIAS       4
#define AVER_PHYS_VW_COUNT            5

/* VT track: 0 INERTIA 10, 1 ANGULAR_DAMPING 0.5, 2 BRAKE_TORQUE (N*m) 15000, 3 FINAL_DRIVE 6, 4 STEER_GAIN 1.6, 5 PIVOT_POWER 0.6 */
#define AVER_PHYS_VT_INERTIA          0
#define AVER_PHYS_VT_ANGULAR_DAMPING  1
#define AVER_PHYS_VT_BRAKE_TORQUE     2
#define AVER_PHYS_VT_FINAL_DRIVE      3
#define AVER_PHYS_VT_STEER_GAIN       4
#define AVER_PHYS_VT_PIVOT_POWER      5
#define AVER_PHYS_VT_COUNT            6

/* VD driver aids. 0 ENABLED 0 (off = v1 pass-through), 1 ABS 0, 2 ABS_SLIP 0.25, 3 ABS_RELEASE (per second) 8, 4 TCS 0, 5 TCS_SLIP 0.35,
   6 STEER_IN (full locks per second, away from centre) 3, 7 STEER_OUT (back to centre) 5, 8 reserved, 9 THROTTLE_IN 6, 10 THROTTLE_OUT 8,
   11 BRAKE_IN 8, 12 REVERSE_ON_BRAKE 1, 13 REVERSE_SPEED (cm/s) 50, 14 HOLD_BRAKE (0..1 at standstill with no pedal) 0.3, 15 HOLD_SPEED (cm/s) 30,
   16 SKID_SLIP 0.45, 17 SKID_ANGLE_DEG 12 */
#define AVER_PHYS_VD_ENABLED          0
#define AVER_PHYS_VD_ABS              1
#define AVER_PHYS_VD_ABS_SLIP         2
#define AVER_PHYS_VD_ABS_RELEASE      3
#define AVER_PHYS_VD_TCS              4
#define AVER_PHYS_VD_TCS_SLIP         5
#define AVER_PHYS_VD_STEER_IN         6
#define AVER_PHYS_VD_STEER_OUT        7
#define AVER_PHYS_VD_RESERVED         8
#define AVER_PHYS_VD_THROTTLE_IN      9
#define AVER_PHYS_VD_THROTTLE_OUT     10
#define AVER_PHYS_VD_BRAKE_IN         11
#define AVER_PHYS_VD_REVERSE_ON_BRAKE 12
#define AVER_PHYS_VD_REVERSE_SPEED    13
#define AVER_PHYS_VD_HOLD_BRAKE       14
#define AVER_PHYS_VD_HOLD_SPEED       15
#define AVER_PHYS_VD_SKID_SLIP        16
#define AVER_PHYS_VD_SKID_ANGLE_DEG   17
#define AVER_PHYS_VD_COUNT            18

/* VU live tune keys for aver_phys_vehicle_tune. 7..14 are the steering-speed curve: up to four (speed cm/s, scale) points in order, a point with
   speed 0 and scale 0 ends the list; scale 1 below the first point, linear between, the last point's scale beyond. Default curve: one point (0, 1). */
#define AVER_PHYS_VU_ENGINE_TORQUE_SCALE          0
#define AVER_PHYS_VU_BRAKE_SCALE                  1
#define AVER_PHYS_VU_GRIP_SCALE                   2
#define AVER_PHYS_VU_SUSPENSION_STIFFNESS_SCALE   3
#define AVER_PHYS_VU_STEER_SCALE                  4
#define AVER_PHYS_VU_DRAG_SCALE                   5
#define AVER_PHYS_VU_DOWNFORCE_SCALE              6
#define AVER_PHYS_VU_STEER_CURVE_0                7
#define AVER_PHYS_VU_STEER_CURVE_1                8
#define AVER_PHYS_VU_STEER_CURVE_2                9
#define AVER_PHYS_VU_STEER_CURVE_3                10
#define AVER_PHYS_VU_STEER_CURVE_4                11
#define AVER_PHYS_VU_STEER_CURVE_5                12
#define AVER_PHYS_VU_STEER_CURVE_6                13
#define AVER_PHYS_VU_STEER_CURVE_7                14
#define AVER_PHYS_VU_COUNT                        15

/* VS vehicle floats (aver_phys_vehicle_state outF) */
#define AVER_PHYS_VS_RPM                 0
#define AVER_PHYS_VS_ENGINE_TORQUE_NM    1
#define AVER_PHYS_VS_SPEED_CMS           2
#define AVER_PHYS_VS_FORWARD_SPEED_CMS   3
#define AVER_PHYS_VS_LATERAL_SPEED_CMS   4
#define AVER_PHYS_VS_THROTTLE_APPLIED    5
#define AVER_PHYS_VS_BRAKE_APPLIED       6
#define AVER_PHYS_VS_STEER_APPLIED       7
#define AVER_PHYS_VS_HANDBRAKE_APPLIED   8
#define AVER_PHYS_VS_CLUTCH              9
#define AVER_PHYS_VS_WHEEL_RPM_AT_CLUTCH 10
#define AVER_PHYS_VS_YAW_RATE_DEG_S      11
#define AVER_PHYS_VS_LONG_ACCEL_G        12
#define AVER_PHYS_VS_LAT_ACCEL_G         13
#define AVER_PHYS_VS_UP_Z                14
#define AVER_PHYS_VS_TRACK_LEFT_CMS      15
#define AVER_PHYS_VS_TRACK_RIGHT_CMS     16
#define AVER_PHYS_VS_AIRBORNE_SECONDS    17
#define AVER_PHYS_VS_COUNT               18

/* VSI vehicle ints (aver_phys_vehicle_state outI) */
#define AVER_PHYS_VSI_GEAR            0
#define AVER_PHYS_VSI_GEAR_COUNT      1
#define AVER_PHYS_VSI_MANUAL          2
#define AVER_PHYS_VSI_SHIFTING        3
#define AVER_PHYS_VSI_WHEELS_ON_GROUND 4
#define AVER_PHYS_VSI_ABS_ACTIVE      5
#define AVER_PHYS_VSI_TCS_ACTIVE      6
#define AVER_PHYS_VSI_KIND            7
#define AVER_PHYS_VSI_AXLE_COUNT      8
#define AVER_PHYS_VSI_WHEEL_COUNT     9
#define AVER_PHYS_VSI_REV_LIMITED     10
#define AVER_PHYS_VSI_COUNT           11

/* VWS wheel floats (aver_phys_vehicle_wheel_state outF). SLIP_LONG is a signed ratio: positive = spinning faster than the ground, negative =
   locking. CONTACT_POINT is world cm, CONTACT_NORMAL a unit vector. */
#define AVER_PHYS_VWS_SLIP_LONG        0
#define AVER_PHYS_VWS_SLIP_LAT_DEG     1
#define AVER_PHYS_VWS_LOAD_N           2
#define AVER_PHYS_VWS_COMPRESSION_CM   3
#define AVER_PHYS_VWS_SUSP_LENGTH_CM   4
#define AVER_PHYS_VWS_ANG_VEL_RAD_S    5
#define AVER_PHYS_VWS_STEER_DEG        6
#define AVER_PHYS_VWS_SPIN_RAD         7
#define AVER_PHYS_VWS_CONTACT_POINT_X  8
#define AVER_PHYS_VWS_CONTACT_POINT_Y  9
#define AVER_PHYS_VWS_CONTACT_POINT_Z  10
#define AVER_PHYS_VWS_CONTACT_NORMAL_X 11
#define AVER_PHYS_VWS_CONTACT_NORMAL_Y 12
#define AVER_PHYS_VWS_CONTACT_NORMAL_Z 13
#define AVER_PHYS_VWS_FORCE_LONG_N     14
#define AVER_PHYS_VWS_FORCE_LAT_N      15
#define AVER_PHYS_VWS_SURFACE_FRICTION 16
#define AVER_PHYS_VWS_BRAKE_TORQUE_NM  17
#define AVER_PHYS_VWS_COUNT            18

/* VWSI wheel ints. CONTACT_ENTITY is what aver_phys_set_entity stamped on the body under the wheel, 0 if none. */
#define AVER_PHYS_VWSI_CONTACT         0
#define AVER_PHYS_VWSI_CONTACT_BODY    1
#define AVER_PHYS_VWSI_CONTACT_ENTITY  2
#define AVER_PHYS_VWSI_SKIDDING        3
#define AVER_PHYS_VWSI_COUNT           4

/* Events, polled with aver_phys_vehicle_event_get: outI4 = kind, vehicle, a, b; outF8 = point xyz, normal xyz, value, value2.
   SKID_BEGIN / SKID_END: a wheel index, value peak slip (ratio or degrees), value2 slide speed cm/s.
   LAND: a wheel index, value downward speed cm/s, value2 airborne seconds.
   IMPACT: a other body handle, value closing speed cm/s along the normal, value2 other mass kg (0 static).
   SHIFT: a from gear, b to gear, value rpm at the shift.
   FLIPPED: value up.z, value2 seconds on its side.
   ASSIST: a 0 ABS / 1 TCS, b 1 on / 0 off, value wheel index. */
#define AVER_PHYS_VEV_SKID_BEGIN  1
#define AVER_PHYS_VEV_SKID_END    2
#define AVER_PHYS_VEV_LAND        3
#define AVER_PHYS_VEV_IMPACT      4
#define AVER_PHYS_VEV_SHIFT       5
#define AVER_PHYS_VEV_FLIPPED     6
#define AVER_PHYS_VEV_ASSIST      7

/* ---- before _finish -------------------------------------------------------------------------------- */

/* The ABI version this library implements: 2. */
AVER_PHYS_API int32_t aver_phys_vehicle_abi_version(void);

/* Wheeled (0, the default) or tracked (1). Refused once a wheel has been added. A tracked vehicle ignores each wheel's steer angle, handbrake torque
   and driven flag, takes its brakes from the tracks, and refuses the axle calls. */
AVER_PHYS_API int32_t aver_phys_vehicle_set_kind(int32_t v, int32_t kind);

/* 1..8 boxes, cx cy cz hx hy hz each, origin-relative cm, replacing create()'s single box; mass and centre of mass are unchanged. */
AVER_PHYS_API int32_t aver_phys_vehicle_set_chassis_boxes(int32_t v, const float* boxes6, int32_t count);

/* The engine block (VE), the torque curve (2..32 points: x = rpm / maxRpm ascending in 0..1, y = fraction of peak torque) and the gearbox (VG).
   manual 1 = the box holds its gear until told otherwise. Forward ratios are positive; reverse ratios are POSITIVE MAGNITUDES, negated inside. */
AVER_PHYS_API int32_t aver_phys_vehicle_set_engine_ex(int32_t v, const float* p, int32_t n);
AVER_PHYS_API int32_t aver_phys_vehicle_set_torque_curve(int32_t v, const float* rpmFraction, const float* torqueFraction, int32_t n);
AVER_PHYS_API int32_t aver_phys_vehicle_set_gearbox(int32_t v, int32_t manual, const float* fwd, int32_t nFwd,
                                                    const float* rev, int32_t nRev, const float* p, int32_t n);

/* Axles are found by the v1 geometry rule and numbered front (largest x) first. */
AVER_PHYS_API int32_t aver_phys_vehicle_axle_count(int32_t v);
AVER_PHYS_API int32_t aver_phys_vehicle_wheel_axle(int32_t v, int32_t wheel);
AVER_PHYS_API int32_t aver_phys_vehicle_set_axle(int32_t v, int32_t axle, const float* p, int32_t n);
/* mode 0 open, 1 limited slip at slipRatio (> 1), 2 locked (a ratio of 1.01: Jolt has no hard lock). Between the driven axles; needs two. */
AVER_PHYS_API int32_t aver_phys_vehicle_set_center_diff(int32_t v, int32_t mode, float slipRatio);
AVER_PHYS_API int32_t aver_phys_vehicle_set_wheel_ex(int32_t v, int32_t wheel, const float* p, int32_t n);
/* wheel -1 = all wheels. Two to 16 points each: longitudinal (slip ratio, mu) and lateral (slip degrees, mu). */
AVER_PHYS_API int32_t aver_phys_vehicle_set_wheel_curves(int32_t v, int32_t wheel,
                                                         const float* longSlip, const float* longMu, int32_t nLong,
                                                         const float* latDeg, const float* latMu, int32_t nLat);
/* side 0 left, 1 right; the road wheels of that side, the driven (sprocket) wheel among them, and the VT block. */
AVER_PHYS_API int32_t aver_phys_vehicle_set_track(int32_t v, int32_t side, const int32_t* wheels, int32_t n,
                                                  int32_t drivenWheel, const float* p, int32_t np);

/* ---- before or after _finish (live) ------------------------------------------------------------------- */

AVER_PHYS_API int32_t aver_phys_vehicle_set_assists(int32_t v, const float* p, int32_t n);
/* N per (m/s)^2, applied at the centre of mass in the fixed step. */
AVER_PHYS_API int32_t aver_phys_vehicle_set_aero(int32_t v, float dragNs2Pm2, float downforceNs2Pm2);
/* 1..8: the wheel shape casts run every Nth step (round robin). 1 is every step. */
AVER_PHYS_API int32_t aver_phys_vehicle_set_collision_period(int32_t v, int32_t everyNSteps);
AVER_PHYS_API int32_t aver_phys_vehicle_tune(int32_t v, int32_t key, float value);

/* ---- after _finish ------------------------------------------------------------------------------------ */

AVER_PHYS_API int32_t aver_phys_vehicle_set_gearbox_mode(int32_t v, int32_t manual);
/* Manual box only: -1 reverse, 0 neutral, 1..N. 0 when out of range. */
AVER_PHYS_API int32_t aver_phys_vehicle_set_gear(int32_t v, int32_t gear);
/* Manual box only: +1 or -1 from the current gear, clamped. */
AVER_PHYS_API int32_t aver_phys_vehicle_shift(int32_t v, int32_t delta);
/* Upright about the current yaw, stopped dead, set down liftCm higher. */
AVER_PHYS_API int32_t aver_phys_vehicle_flip(int32_t v, float liftCm);
/* Write min(n, defined) entries into each array (either may be null) and return the number of floats written; 0 for a dead or unfinished vehicle. */
AVER_PHYS_API int32_t aver_phys_vehicle_state(int32_t v, float* outF, int32_t nF, int32_t* outI, int32_t nI);
AVER_PHYS_API int32_t aver_phys_vehicle_wheel_state(int32_t v, int32_t wheel, float* outF, int32_t nF, int32_t* outI, int32_t nI);

/* Polled and cleared at the top of aver_phys_step, like contacts. event_get writes four ints and eight floats. */
AVER_PHYS_API int32_t aver_phys_vehicle_event_count(void);
AVER_PHYS_API int32_t aver_phys_vehicle_event_get(int32_t index, int32_t* outI4, float* outF8);

#ifdef __cplusplus
}
#endif
