#pragma once
// Aver.Physics — JOINTS. Two bodies, and a rule about how they may move relative to each other.
//
// THE BIGGEST THING THE PHYSICS ABI DID NOT HAVE. Jolt's twelve constraint classes have been compiled
// into this engine's binary all along -- Constraints/*.obj is in every build -- and none of them had an
// entry point, so a door, a lever, a rope, a chain, a crane, a piston, a hatch, a swinging sign and a
// ragdoll were all equally impossible without writing the solver yourself.
//
// A SEPARATE HEADER FROM physics_abi.h, and the reason is the C# side rather than the C side: the ABI
// parity test pairs one header with one C# file by path, so a joints header and a Joints.cs can be
// checked against each other as a unit. Including this one alone is fine -- it needs nothing from
// physics_abi.h but the handle convention, which it restates below.
//
// UNITS AND AXES ARE THE ENGINE'S, exactly as in physics_abi.h: centimetres, +X forward, +Y right,
// +Z up, left-handed, and no Jolt type crosses this boundary. Angles are RADIANS.
//
// EVERY POINT AND AXIS IS WORLD-SPACE, at the moment the joint is created. That is not a simplification
// of Jolt -- every constraint settings struct it has defaults to EConstraintSpace::WorldSpace -- and it
// is the only frame a caller can supply without first knowing where each body's centre of mass ended
// up. The joint records the two bodies' relative pose at creation and holds them to it, so the ordinary
// way to build one is: place both bodies where they belong, then join them.
//
// A JOINT HANDLE IS NOT A BODY HANDLE, and the two number spaces overlap deliberately. physics_abi.h
// draws bodies and characters from ONE counter so a single handle resolves against either; joints get
// their own, so joint 1 and body 1 both exist. That is safe because no function in either family ever
// looks in the other's table -- aver_phys_joint_* consults only the joint map -- but it IS a departure
// from the one-counter invariant, made deliberately rather than stumbled into: sharing the counter
// would mean joint creation could exhaust body handles and every joint would need a "is this actually
// a joint" check that the separate table gives for free.
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

// ---- The world as the other body -------------------------------------------------------------------
//
// Passing 0 for `bodyB` joins `bodyA` TO THE WORLD -- an immovable frame, not a body. That is how a
// door hangs on a wall that is not itself simulated, how a lever is bolted to the floor, and how a
// pendulum hangs from a fixed point. Jolt spells this Body::sFixedToWorld; here it is the handle 0
// that is already reserved as invalid everywhere else, which means "no body" and "the world" are the
// same value and cannot be confused for a real one.
#define AVER_PHYS_WORLD_BODY 0

// ---- Lifetime and common state ---------------------------------------------------------------------

// Removes a joint and destroys it. The bodies stay; only the rule between them goes. Returns 0 for a
// dead handle.
AVER_PHYS_API int32_t aver_phys_joint_remove(int32_t joint);

// How many joints are live.
AVER_PHYS_API int32_t aver_phys_joint_count(void);

// Which bodies a joint holds. Either out-pointer may be null. `outB` is 0 for a joint to the world.
AVER_PHYS_API int32_t aver_phys_joint_bodies(int32_t joint, int32_t* outBodyA, int32_t* outBodyB);

// Turn a joint off without destroying it, and on again. A disabled joint stops constraining
// immediately -- the bodies fall apart and can be re-joined by enabling it, which is what a
// breakable-but-repairable link is. Returns 0 for a dead handle.
AVER_PHYS_API int32_t aver_phys_joint_set_enabled(int32_t joint, int32_t enabled);
AVER_PHYS_API int32_t aver_phys_joint_enabled(int32_t joint);

// ---- Constraint types --------------------------------------------------------------------------------
//
// Each returns a new joint handle, or 0 if either body handle is dead, both are the world, or the
// world is not running. Both bodies being AVER_PHYS_WORLD_BODY is refused rather than silently
// creating a joint that constrains nothing.

// FIXED -- welds two bodies rigidly: no relative movement or rotation at all. Two crates glued into
// one compound object, a wheel welded to an axle that is not meant to turn.
//
// `axisX`/`axisY` are the reference frame the weld is recorded in; they only matter if the joint is
// later inspected, and (1,0,0)/(0,1,0) is the ordinary answer.
AVER_PHYS_API int32_t aver_phys_joint_fixed(int32_t bodyA, int32_t bodyB,
                                            const float pointCm[3],
                                            const float axisX[3], const float axisY[3]);

// POINT -- a ball joint: the two bodies share a point and may rotate freely about it in every
// direction. A chain link, a rope segment, a shoulder with no limits.
AVER_PHYS_API int32_t aver_phys_joint_point(int32_t bodyA, int32_t bodyB, const float pointCm[3]);

// DISTANCE -- holds two points at a distance between min and max, and is free otherwise. A rope
// (min 0, max its length) or a rigid strut (min == max). Distances in centimetres; a NEGATIVE max
// means "whatever they are apart right now", which is Jolt's own default and usually what is wanted.
AVER_PHYS_API int32_t aver_phys_joint_distance(int32_t bodyA, int32_t bodyB,
                                               const float pointACm[3], const float pointBCm[3],
                                               float minDistanceCm, float maxDistanceCm);

// HINGE -- one axis of rotation, everything else locked. A door, a hatch, a lever, a wheel on an axle.
//
// `hingeAxis` is what it turns about and `normalAxis` is the zero-angle reference the limits are
// measured from; the two must be perpendicular. `minAngle`/`maxAngle` are radians, and passing
// -PI/+PI (or anything wider) means unlimited -- a wheel rather than a door.
//
// A RANGE OF ZERO IS REFUSED (returns 0). min >= 0 >= max collapses, after the clamp this applies, to
// a hinge locked at one angle, and Jolt declines to build that: HingeConstraint.cpp:83 asserts the
// limits differ, saying "Better use a fixed constraint in this case". Use aver_phys_joint_fixed.
AVER_PHYS_API int32_t aver_phys_joint_hinge(int32_t bodyA, int32_t bodyB,
                                            const float pointCm[3],
                                            const float hingeAxis[3], const float normalAxis[3],
                                            float minAngleRad, float maxAngleRad);

// SLIDER -- one axis of translation, everything else locked. A piston, a drawer, a lift, a sliding
// door. Limits in centimetres along `sliderAxis`; `normalAxis` must be perpendicular to it.
//
// A RANGE OF ZERO IS REFUSED (returns 0), for the same reason the hinge above refuses one:
// SliderConstraint.cpp:159 asserts the limits differ. Use aver_phys_joint_fixed to hold two bodies
// together with no relative motion at all.
AVER_PHYS_API int32_t aver_phys_joint_slider(int32_t bodyA, int32_t bodyB,
                                             const float pointCm[3],
                                             const float sliderAxis[3], const float normalAxis[3],
                                             float minCm, float maxCm);

// CONE -- free rotation within a cone about `twistAxis`, twist itself unlimited. A tentacle segment,
// a loosely mounted camera. `halfConeAngleRad` of 0 locks it to the axis.
AVER_PHYS_API int32_t aver_phys_joint_cone(int32_t bodyA, int32_t bodyB,
                                           const float pointCm[3], const float twistAxis[3],
                                           float halfConeAngleRad);

// SWING-TWIST -- a cone with an ELLIPTICAL cross-section and a separately limited twist. This is the
// ragdoll joint: a shoulder swings further forward than sideways and can only rotate so far about the
// upper arm, and those are three different numbers.
AVER_PHYS_API int32_t aver_phys_joint_swing_twist(int32_t bodyA, int32_t bodyB,
                                                  const float pointCm[3],
                                                  const float twistAxis[3], const float planeAxis[3],
                                                  float normalHalfConeRad, float planeHalfConeRad,
                                                  float twistMinRad, float twistMaxRad);

// SIX DOF -- every other joint above, expressed as limits on six independent axes. Reach for this when
// none of the named joints is the shape you want: a drawer that also rotates, a joystick, a suspension
// strut.
//
// `limitMin`/`limitMax` are six floats each, in AVER_PHYS_DOF order below: the three translations in
// CENTIMETRES, the three rotations in RADIANS. min > max LOCKS that axis; a min of -1e30 with a max of
// +1e30 frees it. Both arrays are required -- there is no "unset" that means anything useful.
#define AVER_PHYS_DOF_TRANSLATION_X 0
#define AVER_PHYS_DOF_TRANSLATION_Y 1
#define AVER_PHYS_DOF_TRANSLATION_Z 2
#define AVER_PHYS_DOF_ROTATION_X    3
#define AVER_PHYS_DOF_ROTATION_Y    4
#define AVER_PHYS_DOF_ROTATION_Z    5
#define AVER_PHYS_DOF_COUNT         6
AVER_PHYS_API int32_t aver_phys_joint_six_dof(int32_t bodyA, int32_t bodyB,
                                              const float pointCm[3],
                                              const float axisX[3], const float axisY[3],
                                              const float limitMin[6], const float limitMax[6]);

// GEAR -- couples the ROTATION of two bodies at a fixed ratio. Two meshed cogs, a hand-crank driving a
// drum. Each body must already have a hinge of its own; this adds the relationship between them, it
// does not constrain either body on its own.
//
// `ratio` is teeth2/teeth1: 2.0 means body B turns half as fast as body A, and NEGATIVE reverses the
// direction, which is what meshed (as opposed to belt-driven) gears actually do.
AVER_PHYS_API int32_t aver_phys_joint_gear(int32_t bodyA, int32_t bodyB,
                                           const float hingeAxisA[3], const float hingeAxisB[3],
                                           float ratio);

// RACK AND PINION -- couples a body's ROTATION to another body's TRANSLATION. A pinion turning a rack,
// a screw jack, a rotating handle that drives a sliding door. Same prerequisite as gear: body A needs
// its hinge and body B its slider.
//
// `ratio` is radians of A per centimetre of B.
AVER_PHYS_API int32_t aver_phys_joint_rack_and_pinion(int32_t bodyA, int32_t bodyB,
                                                      const float hingeAxisA[3],
                                                      const float sliderAxisB[3],
                                                      float ratioRadPerCm);

// PULLEY -- a rope over two fixed points: as one body descends the other rises. A counterweight, a
// drawbridge, a lift. `bodyPointA`/`bodyPointB` are where the rope attaches to each body and
// `fixedPointA`/`fixedPointB` are the pulley wheels it runs over, all world-space centimetres.
//
// `ratio` is how much B moves per unit of A -- a block and tackle. Lengths in centimetres; a NEGATIVE
// `maxLengthCm` means "however long the rope is right now", matching Jolt's own default.
AVER_PHYS_API int32_t aver_phys_joint_pulley(int32_t bodyA, int32_t bodyB,
                                             const float bodyPointACm[3], const float fixedPointACm[3],
                                             const float bodyPointBCm[3], const float fixedPointBCm[3],
                                             float ratio, float minLengthCm, float maxLengthCm);

// PATH -- constrains a body to slide along an arbitrary polyline. A roller coaster car, a cable car, a
// camera on a dolly track.
//
// `pointsCm` is `pointCount` world-space positions, xyz,xyz,... -- at least two. `closed` joins the
// last back to the first, for a loop. `maxSlideCm` limits how far along the path the body may travel
// (negative for the whole path).
//
// THE ONE CONSTRAINT HERE THAT OWNS AN OBJECT RATHER THAN JUST SETTINGS: Jolt models the path itself
// as a separate reference-counted PathConstraintPath. It is created from these points and owned by the
// joint, so aver_phys_joint_remove disposes of both and a caller never sees it.
AVER_PHYS_API int32_t aver_phys_joint_path(int32_t bodyA, int32_t bodyB,
                                           const float* pointsCm, int32_t pointCount,
                                           int32_t closed, float maxSlideCm);

// ---- Motors and limits -------------------------------------------------------------------------------
//
// A JOINT THAT ONLY RESISTS IS HALF A JOINT. A hinge with no motor is a door that swings; a hinge with
// one is a door that CLOSES ITSELF, a powered winch, a turret that turns to face you, a lift that
// rises. This is what separates a physics toy from a mechanism.
//
// HINGE, SLIDER AND SIX-DOF ONLY. The other constraint types have no motor in Jolt, and these calls
// return 0 on them rather than pretending -- a silently ignored motor is a mechanism that does not
// move with nothing to say why.

// What a motor is doing. Off is free (the joint still limits); Velocity drives toward a target SPEED;
// Position drives toward a target ANGLE or OFFSET and holds there.
#define AVER_PHYS_MOTOR_OFF      0
#define AVER_PHYS_MOTOR_VELOCITY 1
#define AVER_PHYS_MOTOR_POSITION 2

// Set the motor state and its target. For a hinge the target is radians (position) or radians/second
// (velocity); for a slider, centimetres or centimetres/second. For a six-DOF joint, `axis` picks which
// of the six the call is about; every other joint type ignores `axis` and may be passed 0.
AVER_PHYS_API int32_t aver_phys_joint_set_motor(int32_t joint, int32_t axis,
                                                int32_t state, float target);

// How much the motor may exert: force in kg*cm/s^2 for a slider axis, torque in kg*cm^2/s^2 for a
// hinge or rotation axis. A motor left at Jolt's default limits is effectively unlimited, which makes
// a winch that lifts anything regardless of weight; setting this is what makes it able to STALL.
AVER_PHYS_API int32_t aver_phys_joint_set_motor_strength(int32_t joint, int32_t axis,
                                                         float maxForceOrTorque);

// Change a joint's limits after creation, in the same units its creator took: radians for a hinge,
// centimetres for a slider, and for six-DOF whichever `axis` names. Returns 0 on a joint type with no
// limits to change.
AVER_PHYS_API int32_t aver_phys_joint_set_limits(int32_t joint, int32_t axis,
                                                 float minimum, float maximum);

// ---- Reading a joint back ------------------------------------------------------------------------------

// A hinge's current angle in radians, or a slider's current offset in centimetres, written to `outValue`.
// Returns 0 for a dead handle or a joint type with no single scalar to report -- which is every type
// except hinge and slider, and is why this is one function rather than a family.
//
// WHAT IT IS FOR: driving anything that has to KNOW where the mechanism is -- a door's animation, a
// dial's readout, a lift's floor indicator, a script waiting for a drawer to be fully open.
AVER_PHYS_API int32_t aver_phys_joint_value(int32_t joint, float* outValue);

#ifdef __cplusplus
}
#endif
