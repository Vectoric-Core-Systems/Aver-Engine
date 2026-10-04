// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// Joints: two bodies and a rule about how they may move relative to each other. See
// physics_joints_abi.h's own file comment for why every point and axis is WORLD-SPACE at the moment
// the joint is created (place both bodies where they belong, then join them), why a joint handle is
// never a body handle even though the two numeric ranges overlap, and why passing Joint.WorldBody for
// bodyB joins bodyA to an immovable frame rather than to nothing.

using System;

namespace Aver.Physics;

/// <summary>A constraint between two bodies. A handle, not an object: 0 is invalid.</summary>
public readonly struct Joint : IEquatable<Joint>
{
    public int Handle { get; }
    public Joint(int handle) { Handle = handle; }

    public static Joint None => new(0);
    public bool IsValid => Handle != 0;

    /// <summary>Passing this as bodyB joins bodyA to an immovable frame instead of another body -- how
    /// a door hangs on a wall that is not itself simulated. The same handle (0) that is invalid
    /// everywhere else, so "no body" and "the world" can never be confused for a real body.</summary>
    public static Body WorldBody => Body.None;

    /// <summary>How many joints are live.</summary>
    public static int Count => Native.aver_phys_joint_count();

    // ---- Constraint types ---------------------------------------------------------------------------
    // Each factory returns Joint.None if either body handle is dead, both are the world, or the world
    // is not running.

    /// <summary>Welds two bodies rigidly: no relative movement or rotation at all. <paramref name="axisX"/>/
    /// <paramref name="axisY"/> are the reference frame the weld is recorded in and only matter if the
    /// joint is later inspected -- (1,0,0)/(0,1,0) is the ordinary answer.</summary>
    public static Joint Fixed(Body bodyA, Body bodyB, Float3 point, Float3 axisX, Float3 axisY) =>
        new(Native.aver_phys_joint_fixed(bodyA.Handle, bodyB.Handle, point.ToArray(), axisX.ToArray(), axisY.ToArray()));

    /// <summary>A ball joint: the two bodies share a point and rotate freely about it in every direction.</summary>
    public static Joint Point(Body bodyA, Body bodyB, Float3 point) =>
        new(Native.aver_phys_joint_point(bodyA.Handle, bodyB.Handle, point.ToArray()));

    /// <summary>Holds two points at a distance between min and max, free otherwise. A rope (min 0, max
    /// its length) or a rigid strut (min == max). A negative <paramref name="maxDistanceCm"/> means
    /// "whatever they are apart right now".</summary>
    public static Joint Distance(Body bodyA, Body bodyB, Float3 pointA, Float3 pointB, float minDistanceCm, float maxDistanceCm = -1f) =>
        new(Native.aver_phys_joint_distance(bodyA.Handle, bodyB.Handle, pointA.ToArray(), pointB.ToArray(), minDistanceCm, maxDistanceCm));

    /// <summary>One axis of rotation, everything else locked. A door, a hatch, a lever, a wheel.
    /// <paramref name="hingeAxis"/> is what it turns about; <paramref name="normalAxis"/> is the
    /// zero-angle reference the limits are measured from and must be perpendicular to it.
    /// -PI/+PI (or wider) means unlimited -- a wheel rather than a door.</summary>
    public static Joint Hinge(Body bodyA, Body bodyB, Float3 point, Float3 hingeAxis, Float3 normalAxis, float minAngleRad, float maxAngleRad) =>
        new(Native.aver_phys_joint_hinge(bodyA.Handle, bodyB.Handle, point.ToArray(), hingeAxis.ToArray(), normalAxis.ToArray(), minAngleRad, maxAngleRad));

    /// <summary>One axis of translation, everything else locked. A piston, a drawer, a lift.
    /// <paramref name="normalAxis"/> must be perpendicular to <paramref name="sliderAxis"/>.</summary>
    public static Joint Slider(Body bodyA, Body bodyB, Float3 point, Float3 sliderAxis, Float3 normalAxis, float minCm, float maxCm) =>
        new(Native.aver_phys_joint_slider(bodyA.Handle, bodyB.Handle, point.ToArray(), sliderAxis.ToArray(), normalAxis.ToArray(), minCm, maxCm));

    /// <summary>Free rotation within a cone about <paramref name="twistAxis"/>, twist itself unlimited.
    /// <paramref name="halfConeAngleRad"/> of 0 locks it to the axis.</summary>
    public static Joint Cone(Body bodyA, Body bodyB, Float3 point, Float3 twistAxis, float halfConeAngleRad) =>
        new(Native.aver_phys_joint_cone(bodyA.Handle, bodyB.Handle, point.ToArray(), twistAxis.ToArray(), halfConeAngleRad));

    /// <summary>A cone with an ELLIPTICAL cross-section and a separately limited twist -- the ragdoll
    /// joint: a shoulder swings further forward than sideways and twists about the upper arm by a third,
    /// independent amount.</summary>
    public static Joint SwingTwist(Body bodyA, Body bodyB, Float3 point, Float3 twistAxis, Float3 planeAxis,
                                    float normalHalfConeRad, float planeHalfConeRad, float twistMinRad, float twistMaxRad) =>
        new(Native.aver_phys_joint_swing_twist(bodyA.Handle, bodyB.Handle, point.ToArray(), twistAxis.ToArray(), planeAxis.ToArray(),
                                                normalHalfConeRad, planeHalfConeRad, twistMinRad, twistMaxRad));

    /// <summary>Every other joint above, expressed as limits on six independent axes -- reach for this
    /// when none of the named joints is the shape wanted: a drawer that also rotates, a joystick, a
    /// suspension strut. <paramref name="limitMin"/>/<paramref name="limitMax"/> are six floats each, in
    /// <see cref="SixDofAxis"/> order: the three translations in CENTIMETRES, the three rotations in
    /// RADIANS. min &gt; max LOCKS that axis; -1e30/+1e30 frees it. Both arrays must have exactly 6
    /// elements -- there is no "unset" that means anything useful.</summary>
    public static Joint SixDof(Body bodyA, Body bodyB, Float3 point, Float3 axisX, Float3 axisY, float[] limitMin, float[] limitMax)
    {
        if (limitMin.Length != 6 || limitMax.Length != 6)
            throw new ArgumentException("limitMin and limitMax must each have exactly 6 elements, one per SixDofAxis.");
        return new(Native.aver_phys_joint_six_dof(bodyA.Handle, bodyB.Handle, point.ToArray(), axisX.ToArray(), axisY.ToArray(), limitMin, limitMax));
    }

    /// <summary>Couples the ROTATION of two bodies at a fixed ratio -- two meshed cogs. Each body must
    /// already have a hinge of its own; this adds the relationship, it does not constrain either body on
    /// its own. <paramref name="ratio"/> is teeth2/teeth1: 2.0 means bodyB turns half as fast as bodyA,
    /// and negative reverses direction, matching real meshed gears.</summary>
    public static Joint Gear(Body bodyA, Body bodyB, Float3 hingeAxisA, Float3 hingeAxisB, float ratio) =>
        new(Native.aver_phys_joint_gear(bodyA.Handle, bodyB.Handle, hingeAxisA.ToArray(), hingeAxisB.ToArray(), ratio));

    /// <summary>Couples a body's ROTATION to another body's TRANSLATION -- a pinion turning a rack, a
    /// screw jack. bodyA needs its own hinge, bodyB its own slider. <paramref name="ratioRadPerCm"/> is
    /// radians of A per centimetre of B.</summary>
    public static Joint RackAndPinion(Body bodyA, Body bodyB, Float3 hingeAxisA, Float3 sliderAxisB, float ratioRadPerCm) =>
        new(Native.aver_phys_joint_rack_and_pinion(bodyA.Handle, bodyB.Handle, hingeAxisA.ToArray(), sliderAxisB.ToArray(), ratioRadPerCm));

    /// <summary>A rope over two fixed points: as one body descends the other rises. <paramref name="bodyPointA"/>/
    /// <paramref name="bodyPointB"/> are where the rope attaches to each body; <paramref name="fixedPointA"/>/
    /// <paramref name="fixedPointB"/> are the pulley wheels it runs over. <paramref name="ratio"/> is how
    /// much B moves per unit of A -- a block and tackle. A negative <paramref name="maxLengthCm"/> means
    /// "however long the rope is right now".</summary>
    public static Joint Pulley(Body bodyA, Body bodyB, Float3 bodyPointA, Float3 fixedPointA, Float3 bodyPointB, Float3 fixedPointB,
                                float ratio, float minLengthCm, float maxLengthCm = -1f) =>
        new(Native.aver_phys_joint_pulley(bodyA.Handle, bodyB.Handle, bodyPointA.ToArray(), fixedPointA.ToArray(),
                                           bodyPointB.ToArray(), fixedPointB.ToArray(), ratio, minLengthCm, maxLengthCm));

    /// <summary>Constrains a body to slide along an arbitrary polyline -- a roller coaster car, a cable
    /// car, a camera on a dolly track. <paramref name="points"/> needs at least two; <paramref name="closed"/>
    /// joins the last point back to the first. A negative <paramref name="maxSlideCm"/> means the whole
    /// path. Jolt owns the path itself as a separate object; <see cref="Remove"/> disposes of both.</summary>
    public static Joint Path(Body bodyA, Body bodyB, Float3[] points, bool closed, float maxSlideCm = -1f)
    {
        var flat = new float[points.Length * 3];
        for (int i = 0; i < points.Length; i++) { flat[i * 3] = points[i].X; flat[i * 3 + 1] = points[i].Y; flat[i * 3 + 2] = points[i].Z; }
        return new(Native.aver_phys_joint_path(bodyA.Handle, bodyB.Handle, flat, points.Length, closed ? 1 : 0, maxSlideCm));
    }

    // ---- Lifetime and common state ---------------------------------------------------------------------

    /// <summary>Removes the joint and destroys it. The bodies stay; only the rule between them goes.</summary>
    public bool Remove() => Native.aver_phys_joint_remove(Handle) != 0;

    /// <summary>Which bodies this joint holds. B is <see cref="Body.None"/> for a joint to the world.</summary>
    public (Body A, Body B) Bodies()
    {
        var a = new int[1]; var b = new int[1];
        Native.aver_phys_joint_bodies(Handle, a, b);
        return (new Body(a[0]), new Body(b[0]));
    }

    /// <summary>A disabled joint stops constraining immediately -- the bodies fall apart and can be
    /// re-joined by enabling it again, which is what a breakable-but-repairable link is.</summary>
    public bool Enabled
    {
        get => Native.aver_phys_joint_enabled(Handle) != 0;
        set => Native.aver_phys_joint_set_enabled(Handle, value ? 1 : 0);
    }

    // ---- Motors and limits -------------------------------------------------------------------------------
    // Hinge, slider and six-DOF only. Every other constraint type has no motor in Jolt and these calls
    // return false on them rather than pretending.

    /// <summary>Sets the motor state and target for a hinge or slider (radians/centimetres, or their
    /// per-second rate for Velocity). Ignores <paramref name="axis"/>.</summary>
    public bool SetMotor(MotorState state, float target) => SetMotor(0, state, target);
    /// <summary>As <see cref="SetMotor(MotorState, float)"/>, for the named axis of a six-DOF joint.</summary>
    public bool SetMotor(SixDofAxis axis, MotorState state, float target) => SetMotor((int)axis, state, target);
    private bool SetMotor(int axis, MotorState state, float target) => Native.aver_phys_joint_set_motor(Handle, axis, (int)state, target) != 0;

    /// <summary>The most force (slider) or torque (hinge/rotation axis) the motor may exert. Left at
    /// Jolt's default the motor is effectively unlimited; setting this is what lets it STALL.</summary>
    public bool SetMotorStrength(float maxForceOrTorque) => SetMotorStrength(0, maxForceOrTorque);
    public bool SetMotorStrength(SixDofAxis axis, float maxForceOrTorque) => SetMotorStrength((int)axis, maxForceOrTorque);
    private bool SetMotorStrength(int axis, float maxForceOrTorque) => Native.aver_phys_joint_set_motor_strength(Handle, axis, maxForceOrTorque) != 0;

    /// <summary>Changes a hinge's (radians) or slider's (centimetres) limits after creation. False on a
    /// joint type with no limits to change.</summary>
    public bool SetLimits(float minimum, float maximum) => SetLimits(0, minimum, maximum);
    /// <summary>As <see cref="SetLimits(float, float)"/>, for the named axis of a six-DOF joint.</summary>
    public bool SetLimits(SixDofAxis axis, float minimum, float maximum) => SetLimits((int)axis, minimum, maximum);
    private bool SetLimits(int axis, float minimum, float maximum) => Native.aver_phys_joint_set_limits(Handle, axis, minimum, maximum) != 0;

    // ---- Reading a joint back ------------------------------------------------------------------------------

    /// <summary>A hinge's current angle in radians, or a slider's current offset in centimetres. Null
    /// for a dead handle or a joint type with no single scalar to report -- every type except hinge and
    /// slider.</summary>
    public float? Value()
    {
        var v = new float[1];
        return Native.aver_phys_joint_value(Handle, v) != 0 ? v[0] : null;
    }

    public bool Equals(Joint other) => Handle == other.Handle;
    public override bool Equals(object? obj) => obj is Joint j && Equals(j);
    public override int GetHashCode() => Handle;
    public static bool operator ==(Joint a, Joint b) => a.Equals(b);
    public static bool operator !=(Joint a, Joint b) => !a.Equals(b);
    public override string ToString() => IsValid ? $"Joint#{Handle}" : "Joint.None";
}
