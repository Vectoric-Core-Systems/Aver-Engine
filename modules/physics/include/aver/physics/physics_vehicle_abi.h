#pragma once
// Aver.Physics -- VEHICLES: a wheeled car (chassis, suspension, tyres, engine, gearbox, differentials)
// as Jolt's VehicleConstraint, behind the same plain-C ABI and the same handle convention as
// physics_abi.h. Its own header for the reason physics_character_abi.h is one: the C# parity test pairs
// headers with Native.cs, and physics_abi.h is not something to keep growing.
//
// UNITS AND AXES ARE THE ENGINE'S, exactly as in physics_abi.h: centimetres, kilograms, N*m, degrees for
// steering, +X forward, +Y right, +Z up, left-handed. No Jolt type crosses this boundary, and Jolt's own
// vehicle defaults (forward +Z, up +Y) never leak either -- they are wrong for this engine and are
// replaced inside PhysicsVehicle.cpp. 0 is always an invalid handle; a setter returns 1/0; a getter with
// a null out-pointer returns 0 rather than dereferencing it.
//
// A VEHICLE IS BUILT, NOT DESCRIBED, and out of scalars only so the C# binding and its parity test stay
// trivial: create the chassis, add the wheels one at a time, optionally set the engine, then finish.
// Nothing is simulated as a vehicle until _finish succeeds; the chassis is an ordinary body before that.
//
//     v = aver_phys_vehicle_create(...);
//     aver_phys_vehicle_add_wheel(v, ...);   // once per wheel
//     aver_phys_vehicle_finish(v, 60.0f);
//     every frame, BEFORE aver_phys_step:  aver_phys_vehicle_set_input(v, ...);
//     every frame, AFTER it:               aver_phys_vehicle_pose / _wheel_pose
//
// THE VEHICLE ORIGIN IS THE BOTTOM CENTRE OF THE CAR, which is where a car mesh's own origin is. Every
// position below is relative to it, in the chassis' own axes, and _pose reports it -- NOT the centre of
// mass that aver_phys_body_position reports for the same body. That difference is the whole reason this
// header has its own pose call: an entity whose pivot is the mesh origin must be written from the ORIGIN.
//
// THE HANDLE IS THE VEHICLE'S, NOT THE BODY'S. It is drawn from the one counter bodies and characters
// share, so it never collides with either, but it names the vehicle; aver_phys_vehicle_body gives the
// chassis' body handle, and THAT is a plain body -- a raycast hits it, contacts report it,
// aver_phys_set_entity stamps it, and a character standing on it rides it (aver_phys_step's ground carry).
//
// TEARDOWN. aver_phys_vehicle_destroy removes the vehicle and then its chassis. Removing the chassis with
// aver_phys_remove_body destroys the vehicle FIRST, so a constraint never outlives the body it is
// attached to, and the vehicle handle is dead afterwards. aver_phys_shutdown destroys every vehicle
// before it destroys the world.
//
// WHAT THE TYRES GRIP. Jolt combines a tyre's friction with the surface's as a geometric mean, and every
// collider this engine builds has Jolt's default surface friction of 0.2 -- so a stock road would give
// every car 0.49 of the grip its tyres were built for. A surface is therefore measured against that
// default instead: friction 0.2 or more gives the tyre's full grip, and a body whose friction is set
// lower scales it down in proportion (ice at 0.05 gives a quarter). A caller that wants a slippery or
// a grippy road changes the BODY's friction; nothing here needs to know.
//
// WHAT IT COSTS. Every wheel is a cylinder shape-cast against the world on every fixed step (4 casts
// per car, 60 times a second), plus the vehicle's own solver work. A road mesh must face up: a shape
// cast ignores back faces by default, so a wheel does not see a road from below.

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

// ---- Building ---------------------------------------------------------------------------------------

// Creates the chassis: a dynamic box with half extents (hx, hy, hz) centimetres whose CENTRE sits at
// (0, 0, hz + groundClearance) above the vehicle origin -- so `groundClearance` is how high the box's
// underside is off the ground the origin touches -- with the centre of mass lowered to (comX, comY, comZ),
// origin-relative, engine axes, and a mass of `massKg`. The origin is placed at (x, y, z) with rotation
// (qx, qy, qz, qw), which need not be normalised (all zero means no rotation).
//
// A LOW CENTRE OF MASS IS WHAT KEEPS A CAR ON ITS WHEELS: the box alone would put it at the box centre,
// well above the axles, and a car that tall rolls over in an ordinary turn.
//
// Returns the vehicle handle, or 0 for a non-positive extent or mass, a negative clearance, a non-finite
// number, or no world. The new vehicle has no wheels and is not simulated as a vehicle until _finish.
AVER_PHYS_API int32_t aver_phys_vehicle_create(float hx, float hy, float hz, float groundClearance,
                                               float comX, float comY, float comZ, float massKg,
                                               float x, float y, float z,
                                               float qx, float qy, float qz, float qw);

// Adds a wheel BEFORE _finish and returns its index (0, 1, 2, ... in the order added), or -1.
//
// (px, py, pz) is the wheel's ATTACHMENT point -- the top of its suspension travel -- relative to the
// vehicle origin, engine axes, cm. The wheel centre hangs below it by the suspension length, which the
// springs settle between `suspensionMin` (fully compressed) and `suspensionMax` (fully drooped), cm.
// `radius` and `width` are the tyre's, cm. The suspension is a spring of `suspensionHz` hertz with a
// `suspensionDamping` ratio (0.5 is a car, 1 is critically damped). `maxSteerDeg` is how far the wheel
// turns at full steering input, degrees, 0 = fixed. `maxBrakeTorque` and `maxHandBrakeTorque` are the
// most the brake and the handbrake can hold THIS wheel with, N*m. `driven` 1 = the engine drives it.
//
// RIDE HEIGHT: Jolt rests the spring at `suspensionMax`, so the car's weight compresses it by the
// spring's static sag -- about g / (2 pi f)^2 for a spring of f hertz, 6 cm at 2 Hz, and it falls with
// the square of the frequency -- and the origin settles that far BELOW where a fully drooped
// suspension would hold it. A mesh whose origin is the bottom of its tyres therefore sinks that far
// into the road unless the attachment points are LOWERED by the same amount: with pz = radius +
// suspensionMax - sag the wheel centre settles one radius above the origin, tyres on the ground the
// origin touches. (Jolt sizes the spring from an effective mass that is a little under the quarter of
// the car a wheel carries -- worked by hand from VehicleConstraint.cpp for a four-wheeler, not
// measured -- so the real sag is typically 15 to 20 percent more than that formula gives and the origin
// still settles about a centimetre under the ground.)
//
// AXLES ARE FOUND, NOT DECLARED. Two wheels whose attachment points agree in x to within 5 cm and lie on
// opposite sides of y = 0 form an axle: _finish gives each driven axle a differential and each complete
// axle an anti-roll bar. A wheel with no partner is its own axle.
//
// Returns -1 once _finish has been called, for a dead handle, past 16 wheels, and for a wheel that is
// not physically a wheel: radius or width not positive, suspensionMax not above suspensionMin, a
// negative suspensionMin, hz not positive, a negative damping or torque, or a non-finite number.
// `maxSteerDeg` is clamped to [0, 89].
AVER_PHYS_API int32_t aver_phys_vehicle_add_wheel(int32_t v, float px, float py, float pz,
                                                  float radius, float width,
                                                  float suspensionMin, float suspensionMax,
                                                  float suspensionHz, float suspensionDamping,
                                                  float maxSteerDeg, float maxBrakeTorque,
                                                  float maxHandBrakeTorque, int32_t driven);

// The engine, BEFORE _finish: peak torque in N*m and the revolutions per minute it idles at and is
// limited to. Never called, a vehicle gets 500, 1000 and 6000. The automatic gearbox shifts up at 60% and
// down at 20% of the way through that rev range, and the clutch and engine inertia scale with the
// torque, so a 2000 N*m bus behaves like a 500 N*m car with more of it rather than like an engine that
// outruns its own clutch.
//
// Returns 0 after _finish, for a dead handle, for a torque that is not positive, a negative minRpm, or a
// maxRpm not above minRpm.
AVER_PHYS_API int32_t aver_phys_vehicle_set_engine(int32_t v, float maxTorque, float minRpm, float maxRpm);

// Builds the simulation. Jolt's wheeled-vehicle controller with an automatic gearbox, a differential on
// every axle that has a driven wheel (sharing the engine's torque equally), an anti-roll bar on every
// complete axle, and a cylinder shape-cast per wheel against everything on the default layer, then
// registers it with the world so every aver_phys_step runs it.
//
// `maxPitchRollDeg` keeps the chassis' up axis within that many degrees of the world's, which is what
// stops traffic ending up on its roof. Jolt does it with a velocity and a position constraint that only
// push TOWARD upright, so a hard hit can carry the car a little past the limit for a step before it is
// pulled back, and a car on its side comes to rest pressed AGAINST the limit rather than beyond it: to
// test for "tipped over", compare the up axis with cos(maxPitchRollDeg), not with something smaller.
// 0 or less, or 180 or more, leaves it off.
//
// Returns 1, or 0 for a dead handle, one already finished, no wheels, or no wheel flagged `driven`
// (the engine would be connected to nothing).
AVER_PHYS_API int32_t aver_phys_vehicle_finish(int32_t v, float maxPitchRollDeg);

// Removes the vehicle and then its chassis. The vehicle handle AND the chassis handle are dead afterwards.
AVER_PHYS_API int32_t aver_phys_vehicle_destroy(int32_t v);

// The chassis' body handle, or 0 for a dead handle. Valid before _finish too.
AVER_PHYS_API int32_t aver_phys_vehicle_body(int32_t v);

// ---- Driving ---------------------------------------------------------------------------------------

// The driver's input, which holds until it is set again and applies on every fixed step in between:
// `forward` -1..1 (negative reverses), `right` -1..1 (+1 steers toward engine +Y, the car's right),
// `brake` 0..1, `handbrake` 0..1. Out-of-range values are clamped and a non-finite one is 0.
//
// A VEHICLE ASLEEP IS WOKEN BY THROTTLE. The body is allowed to sleep while the input is idle, and
// a non-zero `forward` wakes it; brake and steering alone do not, because they cannot make a stationary
// car move.
//
// Returns 0 for a dead handle and for a vehicle that has not been finished.
AVER_PHYS_API int32_t aver_phys_vehicle_set_input(int32_t v, float forward, float right,
                                                  float brake, float handbrake);

// ---- Reading it back ----------------------------------------------------------------------------------

// The vehicle ORIGIN's pose, NOT the centre of mass: engine centimetres and an (x, y, z, w) quaternion in
// engine axes, into caller-owned float[3] and float[4]. Valid before _finish too.
AVER_PHYS_API int32_t aver_phys_vehicle_pose(int32_t v, float* outXyz, float* outQuat);
// The chassis' linear velocity, cm/s, world space, into a caller-owned float[3].
AVER_PHYS_API int32_t aver_phys_vehicle_velocity(int32_t v, float* outXyz);
// The speed along the chassis' own +X, cm/s, negative when reversing. 0 for a dead handle.
AVER_PHYS_API float   aver_phys_vehicle_forward_speed(int32_t v);

// How many wheels the vehicle has (added so far, before _finish). 0 for a dead handle.
AVER_PHYS_API int32_t aver_phys_vehicle_wheel_count(int32_t v);
// Wheel `i`'s centre position (cm) and rotation, RELATIVE TO THE VEHICLE ORIGIN, engine axes, into
// caller-owned float[3] and float[4]. The rotation is the steering turn and the spin the wheel has rolled
// through, for a wheel mesh whose axle is engine Y and whose forward is engine X -- it is the identity for
// an unsteered wheel at rest, and its steering sign is the vehicle's: full right input turns a front wheel
// toward +Y. Compose it with the origin's pose for the wheel's world transform.
// Returns 0 for a dead or unfinished vehicle, a bad index, or a null out-pointer.
AVER_PHYS_API int32_t aver_phys_vehicle_wheel_pose(int32_t v, int32_t i, float* outXyz, float* outQuat);
// 1 when wheel `i` touched ground on the last step. 0 for no contact, and also for a dead or unfinished
// vehicle or a bad index.
AVER_PHYS_API int32_t aver_phys_vehicle_wheel_contact(int32_t v, int32_t i);

// ---- Teleport ---------------------------------------------------------------------------------------

// Puts the vehicle ORIGIN at (x, y, z) with rotation (qx, qy, qz, qw), stops it dead -- linear and angular
// velocity zero, every wheel and the engine back to rest, the gearbox in neutral (it engages first gear
// on the next throttle) -- and wakes it. For a reset after a crash or a lane jump. Valid before _finish
// too.
AVER_PHYS_API int32_t aver_phys_vehicle_set_pose(int32_t v, float x, float y, float z,
                                                 float qx, float qy, float qz, float qw);

#ifdef __cplusplus
}
#endif
