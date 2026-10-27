#pragma once
// Aver.Physics: the raycast vehicle (G2, Godot's VehicleBody3D / VehicleWheel3D). Docs: docs/physics/GODOT_VEHICLE.md.
//
// A SECOND vehicle model next to Jolt's wheeled controller (physics_vehicle_abi.h), which is untouched and stays the default. A raycast vehicle
// is an ordinary dynamic body (the chassis) plus N wheels that are not bodies: each fixed step, before the solver, every wheel casts one ray along
// its suspension, applies a spring and damper force at the contact, and applies friction impulses limited by friction_slip. It is the behaviour of a
// btRaycastVehicle-style model, written from its description. A world that never calls anything here steps exactly as before.
//
// Per-wheel drive is set by the caller each frame: engine force, brake and steering (Godot's VehicleBody3D spreads its own engine_force, brake and
// steering over the wheels flagged TRACTION and STEERING; here that is the caller's job, and the two flags only GATE a wheel's engine force and steering).
// Units: cm for lengths, kg*cm/s^2 for forces (aver_phys_body_add_force's unit), radians, kg, +X forward, +Y right, +Z up, left-handed. Vectors in a
// wheel block are CHASSIS-LOCAL (the body's origin and rotation). Positive steering turns the wheel's forward toward its axle side (+Y for the default
// axle: a right turn). Godot's metres, brake impulses and left-positive steering are the converter's to convert (C# RaycastWheel.FromGodot does it).
// Returns 1/0 with aver_phys_last_error unless stated. Everything here is configuration or per-step input; physics snapshots do not capture it
// (the forces are a pure function of the chassis state and these inputs, so a restore replays exactly; docs/physics/GODOT_VEHICLE.md).
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

#define AVER_PHYS_RAYVEH_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_rayveh_abi_version(void);

/* ---- wheel parameter block (AVER_PHYS_RAYW_*, floats; `index NAME (unit) = default`) ----
   The defaults are Aver's own neutral numbers; none is a Godot default. */
#define AVER_PHYS_RAYW_CONNECT_X            0    /* connection point in chassis-local cm: where the suspension ray starts (0) */
#define AVER_PHYS_RAYW_CONNECT_Y            1    /* (0) */
#define AVER_PHYS_RAYW_CONNECT_Z            2    /* (0) */
#define AVER_PHYS_RAYW_DIR_X                3    /* suspension direction, chassis-local, from the chassis toward the ground; normalised here (0) */
#define AVER_PHYS_RAYW_DIR_Y                4    /* (0) */
#define AVER_PHYS_RAYW_DIR_Z                5    /* (-1) */
#define AVER_PHYS_RAYW_AXLE_X               6    /* the wheel's axle, chassis-local: the direction its side friction acts along; normalised here (0) */
#define AVER_PHYS_RAYW_AXLE_Y               7    /* (1) */
#define AVER_PHYS_RAYW_AXLE_Z               8    /* (0) */
#define AVER_PHYS_RAYW_REST_LENGTH          9    /* cm >= 0: suspension length with no load; the ray reaches rest + radius (30) */
#define AVER_PHYS_RAYW_MAX_TRAVEL           10   /* cm >= 0: how far it can compress below the rest length (30) */
#define AVER_PHYS_RAYW_STIFFNESS            11   /* >= 0, MASS-NORMALISED (1/s^2): force = stiffness x compression x CHASSIS mass per wheel; unit-free, same in cm or m (20) */
#define AVER_PHYS_RAYW_DAMP_COMPRESSION     12   /* >= 0 (1/s), mass-normalised like stiffness: resists the suspension compressing (2) */
#define AVER_PHYS_RAYW_DAMP_RELAXATION      13   /* >= 0 (1/s): resists it extending (3) */
#define AVER_PHYS_RAYW_MAX_FORCE            14   /* kg*cm/s^2 >= 0: the most the suspension pushes; 0 = no limit (0) */
#define AVER_PHYS_RAYW_RADIUS               15   /* cm > 0 (35) */
#define AVER_PHYS_RAYW_FRICTION_SLIP        16   /* >= 0: friction coefficient; the wheel's grip is this times its suspension load (1) */
#define AVER_PHYS_RAYW_ROLL_INFLUENCE       17   /* >= 0: scales the height (along world up) at which side friction acts; 0 = at the centre of mass, no roll (1) */
#define AVER_PHYS_RAYW_TRACTION             18   /* 0/1: engine force counts (1) */
#define AVER_PHYS_RAYW_STEERING             19   /* 0/1: steering counts (0) */
#define AVER_PHYS_RAYW_COUNT                20

/* ---- wheel state block (AVER_PHYS_RAYS_*, floats written by aver_phys_rayveh_wheel_state) ----
   As of the last fixed step's pre-solver phase. A vector member's index is the index of its X. */
#define AVER_PHYS_RAYS_IN_CONTACT           0    /* 1 when the ray hit something, else 0 */
#define AVER_PHYS_RAYS_CONTACT_POINT        1    /* 3: world cm (the ray's end when not in contact) */
#define AVER_PHYS_RAYS_CONTACT_NORMAL       4    /* 3: unit, world (the up of the suspension when not in contact) */
#define AVER_PHYS_RAYS_SUSP_LENGTH          7    /* cm: the suspension length now, rest length when not in contact */
#define AVER_PHYS_RAYS_SUSP_FORCE           8    /* kg*cm/s^2 the suspension pushed with */
#define AVER_PHYS_RAYS_SKID_INFO            9    /* 1 = gripping; below 1 = the friction limit scaled the wheel's impulses to this fraction (a skid) */
#define AVER_PHYS_RAYS_SPIN                 10   /* rad/s about the axle, positive rolling forward */
#define AVER_PHYS_RAYS_ROTATION             11   /* rad, accumulated spin (wraps at +-2*pi) */
#define AVER_PHYS_RAYS_RPM                  12   /* spin in revolutions per minute */
#define AVER_PHYS_RAYS_FORWARD_SPEED        13   /* cm/s: the chassis' speed at the contact along the wheel's forward, relative to what it rolls on */
#define AVER_PHYS_RAYS_SIDE_SPEED           14   /* cm/s: the same along the axle (what side friction fights) */
#define AVER_PHYS_RAYS_CONTACT_BODY         15   /* body handle of the ground (an integer in a float); 0 when not in contact */
#define AVER_PHYS_RAYS_STEER_APPLIED        16   /* rad: the steering angle used (0 for a wheel without STEERING) */
#define AVER_PHYS_RAYS_COUNT                17

/* ---- vehicles ----
   A vehicle handle is NOT a body handle. The chassis must be a dynamic, non-sensor body that is not a Jolt vehicle's chassis and not already a raycast
   chassis. Removing the chassis body destroys the vehicle. Wheels are added once and never removed; an index is stable (0.. in add order, max 64). */
AVER_PHYS_API int32_t aver_phys_rayveh_create(int32_t chassisBody);      /* the new vehicle handle, or 0 */
AVER_PHYS_API int32_t aver_phys_rayveh_destroy(int32_t veh);
AVER_PHYS_API int32_t aver_phys_rayveh_exists(int32_t veh);
AVER_PHYS_API int32_t aver_phys_rayveh_count(void);
AVER_PHYS_API int32_t aver_phys_rayveh_chassis(int32_t veh);              /* the chassis body handle, 0 for a dead vehicle */

/* Adds a wheel from a parameter block; a block shorter than COUNT leaves the rest at the defaults, a longer one is refused. Returns the wheel index,
   or -1 (bad handle, bad number: non-finite, a zero DIR or AXLE, radius <= 0, a negative length / stiffness / damping / force / slip / roll). */
AVER_PHYS_API int32_t aver_phys_rayveh_add_wheel(int32_t veh, const float* p, int32_t n);
AVER_PHYS_API int32_t aver_phys_rayveh_wheel_count(int32_t veh);          /* -1 for a dead vehicle */
AVER_PHYS_API int32_t aver_phys_rayveh_set_wheel_param(int32_t veh, int32_t wheel, int32_t key, float value);   /* live, between steps */
AVER_PHYS_API int32_t aver_phys_rayveh_wheel_param(int32_t veh, int32_t wheel, int32_t key, float* outValue);
/* The wheel's ray collides with bodies whose Godot layer bits (aver_phys_body_set_collision_filter, else the bit of the user layer) meet this mask.
   Default 0xFFFFFFFF. The chassis itself and sensors are never hit. */
AVER_PHYS_API int32_t aver_phys_rayveh_set_wheel_mask(int32_t veh, int32_t wheel, uint32_t mask);
AVER_PHYS_API int32_t aver_phys_rayveh_wheel_mask(int32_t veh, int32_t wheel, uint32_t* outMask);

/* ---- drive, per wheel (wheel -1 = every wheel); they stay in force until changed ----
   engineForce (kg*cm/s^2): thrust along the wheel's forward at the contact, only while it touches ground, only with TRACTION, limited by the wheel's
   friction. While it is non-zero the brake is ignored for that wheel. A non-zero engine force wakes a sleeping chassis.
   brake (kg*cm/s^2 >= 0): the most braking force the wheel may apply to stop it rolling relative to the ground. Not frame-rate dependent: Godot's
   brake is an impulse per step, so the converter multiplies it by the step rate and 100.
   steering (rad): the wheel turns about the suspension axis, only with STEERING. */
AVER_PHYS_API int32_t aver_phys_rayveh_set_engine_force(int32_t veh, int32_t wheel, float engineForce);
AVER_PHYS_API int32_t aver_phys_rayveh_set_brake(int32_t veh, int32_t wheel, float brake);
AVER_PHYS_API int32_t aver_phys_rayveh_set_steering(int32_t veh, int32_t wheel, float steeringRad);

/* ---- readback ---- */
/* Writes min(n, AVER_PHYS_RAYS_COUNT) floats of the state block. */
AVER_PHYS_API int32_t aver_phys_rayveh_wheel_state(int32_t veh, int32_t wheel, float* outState, int32_t n);
/* The wheel's centre (world cm) and orientation (x, y, z, w) for a mesh: forward -> X, axle -> Y, up -> Z at rest, steered and spun. From the chassis'
   CURRENT pose and the last suspension length. Either out pointer may be null. */
AVER_PHYS_API int32_t aver_phys_rayveh_wheel_pose(int32_t veh, int32_t wheel, float* outPositionCm, float* outRotation);

#ifdef __cplusplus
}
#endif
