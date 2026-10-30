// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// Godot's joint nodes (PinJoint3D, HingeJoint3D, SliderJoint3D, ConeTwistJoint3D, Generic6DOFJoint3D) onto the nearest Aver/Jolt joint (G2). docs/physics/RAYCAST_VEHICLE.md has
// the table of what maps exactly, approximately or not at all. (Moved here from Aver.Physics: it is Fabric-only.) The target is Godot's own Jolt module, which ignores several of these parameters too, so "not at all" here
// is usually "as in Godot". Nothing here changes a Joint: it only calls the existing factories and setters.
//
// INPUTS. Parameter values are Godot's, in Godot's units: metres, radians, newtons, newton-metres, impulses per step. Geometry is Aver's: the anchor is a world-space point in
// centimetres and every axis is a world-space Aver direction (pass a Godot direction through RaycastWheelGodot.FromGodotAxes). Godot's right-handed Y-up axes map to Jolt's by
// the identity, so joint angles and motor velocities keep their sign. No Godot default is built in: every record starts at "off / unset" and the converter fills in what the scene says.

using System;
using System.Collections.Generic;
using Aver.Physics;

namespace Aver.Fabric;

/// <summary>A joint and what could not be carried over. <see cref="Notes"/> names each parameter that was set but ignored or approximated.</summary>
public sealed class GodotJointResult
{
    public Joint Joint { get; }
    public IReadOnlyList<string> Notes { get; }
    public bool Created => Joint.IsValid;
    internal GodotJointResult(Joint joint, List<string> notes) { Joint = joint; Notes = notes; }
}

/// <summary>PinJoint3D. Jolt's point constraint has no softness, so all three are accepted and ignored (as Godot's Jolt module does).</summary>
public sealed record GodotPinParams
{
    public float? Bias { get; init; }
    public float? Damping { get; init; }
    public float? ImpulseClamp { get; init; }
}

/// <summary>HingeJoint3D. Limits in radians; <see cref="MotorMaxImpulse"/> is Godot's impulse per physics step. Bias, softness and relaxation are ignored.</summary>
public sealed record GodotHingeParams
{
    public bool UseLimit { get; init; }
    public float LimitUpper { get; init; }
    public float LimitLower { get; init; }
    public bool EnableMotor { get; init; }
    public float MotorTargetVelocity { get; init; }
    public float MotorMaxImpulse { get; init; }
    public float? LimitBias { get; init; }
    public float? LimitSoftness { get; init; }
    public float? LimitRelaxation { get; init; }
    public float? Bias { get; init; }
}

/// <summary>SliderJoint3D. Linear limits in metres (upper below lower means no limit). Everything else, including the angular limits, is ignored.</summary>
public sealed record GodotSliderParams
{
    public float LinearLimitUpper { get; init; }
    public float LinearLimitLower { get; init; }
    public float? LinearLimitSoftness { get; init; }
    public float? LinearLimitRestitution { get; init; }
    public float? LinearLimitDamping { get; init; }
    public float? AngularLimitUpper { get; init; }
    public float? AngularLimitLower { get; init; }
}

/// <summary>ConeTwistJoint3D. Spans in radians, taken as the half-angle each way (the Jolt module's reading; <see cref="SpansAreFullAngles"/> halves them if the
/// Godot version in use means the whole span). Bias, softness and relaxation are ignored.</summary>
public sealed record GodotConeTwistParams
{
    public float SwingSpan { get; init; }
    public float TwistSpan { get; init; }
    public bool SpansAreFullAngles { get; init; }
    public float? Bias { get; init; }
    public float? Softness { get; init; }
    public float? Relaxation { get; init; }
}

/// <summary>One linear axis of a Generic6DOFJoint3D (metres, newtons). Softness, restitution and damping of the limit are ignored.</summary>
public sealed record GodotLinearAxis
{
    public bool LimitEnabled { get; init; }
    public float LimitUpper { get; init; }
    public float LimitLower { get; init; }
    public bool SpringEnabled { get; init; }
    /// <summary>N/m.</summary>
    public float SpringStiffness { get; init; }
    /// <summary>N*s/m.</summary>
    public float SpringDamping { get; init; }
    /// <summary>Metres from the joint frame.</summary>
    public float SpringEquilibriumPoint { get; init; }
    public bool MotorEnabled { get; init; }
    /// <summary>m/s.</summary>
    public float MotorTargetVelocity { get; init; }
    /// <summary>N; 0 leaves the motor unlimited.</summary>
    public float MotorForceLimit { get; init; }
    public float? LimitSoftness { get; init; }
    public float? LimitRestitution { get; init; }
    public float? LimitDamping { get; init; }
}

/// <summary>One angular axis of a Generic6DOFJoint3D (radians, newton-metres). Softness, restitution, force limit and erp of the limit are ignored.</summary>
public sealed record GodotAngularAxis
{
    public bool LimitEnabled { get; init; }
    public float LimitUpper { get; init; }
    public float LimitLower { get; init; }
    public bool SpringEnabled { get; init; }
    /// <summary>N*m/rad.</summary>
    public float SpringStiffness { get; init; }
    /// <summary>N*m*s/rad.</summary>
    public float SpringDamping { get; init; }
    public float SpringEquilibriumPoint { get; init; }
    public bool MotorEnabled { get; init; }
    /// <summary>rad/s.</summary>
    public float MotorTargetVelocity { get; init; }
    /// <summary>N*m; 0 leaves the motor unlimited.</summary>
    public float MotorForceLimit { get; init; }
    public float? LimitSoftness { get; init; }
    public float? LimitRestitution { get; init; }
    public float? LimitForceLimit { get; init; }
    public float? LimitErp { get; init; }
}

/// <summary>Generic6DOFJoint3D: the frame's X, Y and Z are the three axes. A spring needs the mass (linear) or the inertia (angular) it acts on to become a frequency;
/// leave the angular inertia null and an angular spring is skipped with a note.</summary>
public sealed record GodotGeneric6DofParams
{
    public GodotLinearAxis LinearX { get; init; } = new();
    public GodotLinearAxis LinearY { get; init; } = new();
    public GodotLinearAxis LinearZ { get; init; } = new();
    public GodotAngularAxis AngularX { get; init; } = new();
    public GodotAngularAxis AngularY { get; init; } = new();
    public GodotAngularAxis AngularZ { get; init; } = new();
    /// <summary>kg*m^2 about the joint axes, for the angular springs (the body's inertia, or the pair's reduced inertia).</summary>
    public float? EffectiveInertiaKgM2 { get; init; }
}

public static class GodotJoints
{
    const float Cm = 100f;
    const float Free = 1e30f;

    /// <summary>A spring in Godot's terms (stiffness k, damping c, acting on an effective mass or inertia m) as Jolt's frequency (Hz) and damping ratio.</summary>
    public static (float FrequencyHz, float DampingRatio) SpringToHz(float stiffness, float damping, float effectiveMass)
    {
        if (stiffness <= 0f || effectiveMass <= 0f) return (0f, 0f);
        float omega = MathF.Sqrt(stiffness / effectiveMass);
        return (omega / (2f * MathF.PI), damping / (2f * MathF.Sqrt(stiffness * effectiveMass)));
    }

    /// <summary>The reduced mass of a pair: both bodies' mass if both move, the moving body's if the other is static or the world.</summary>
    public static float ReducedMass(Body a, Body b)
    {
        float ma = a.Mass, mb = b.Mass;
        if (ma > 0f && mb > 0f) return ma * mb / (ma + mb);
        return ma > 0f ? ma : mb;
    }

    static Float3 Normalise(Float3 v)
    {
        float l = MathF.Sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z);
        return l > 1e-9f ? new Float3(v.X / l, v.Y / l, v.Z / l) : v;
    }

    static Float3 Perpendicular(Float3 a)
    {
        a = Normalise(a);
        var t = MathF.Abs(a.Z) < 0.9f ? new Float3(0, 0, 1) : new Float3(1, 0, 0);
        var c = new Float3(a.Y * t.Z - a.Z * t.Y, a.Z * t.X - a.X * t.Z, a.X * t.Y - a.Y * t.X);
        return Normalise(c);
    }

    static void Note(List<string> notes, float? v, string what) { if (v.HasValue) notes.Add(what + " is ignored"); }

    /// <summary>PinJoint3D -> a point (ball) joint at the anchor. The three parameters have no Jolt counterpart.</summary>
    public static GodotJointResult Pin(Body a, Body b, Float3 anchorCm, GodotPinParams? p = null)
    {
        var notes = new List<string>();
        p ??= new GodotPinParams();
        Note(notes, p.Bias, "PinJoint3D bias (Jolt's point constraint has no softness)");
        Note(notes, p.Damping, "PinJoint3D damping");
        Note(notes, p.ImpulseClamp, "PinJoint3D impulse_clamp");
        return new GodotJointResult(Joint.Point(a, b, anchorCm), notes);
    }

    /// <summary>HingeJoint3D -> a hinge about <paramref name="axis"/>. Limits are measured from the pose the bodies have when this is called, and a range that does
    /// not contain 0 is widened to include it. The motor is a velocity motor; Godot's max impulse becomes a torque of impulse x ticks per second.</summary>
    public static GodotJointResult Hinge(Body a, Body b, Float3 anchorCm, Float3 axis, GodotHingeParams p, float physicsTicksPerSecond)
    {
        var notes = new List<string>();
        Note(notes, p.LimitBias, "HingeJoint3D limit/bias");
        Note(notes, p.LimitSoftness, "HingeJoint3D limit/softness");
        Note(notes, p.LimitRelaxation, "HingeJoint3D limit/relaxation");
        Note(notes, p.Bias, "HingeJoint3D bias");
        float lo = -MathF.PI, hi = MathF.PI;
        if (p.UseLimit)
        {
            lo = p.LimitLower; hi = p.LimitUpper;
            if (lo > 0f || hi < 0f) notes.Add("HingeJoint3D limits do not contain the starting angle 0; widened to include it");
            if (lo >= hi) { notes.Add("HingeJoint3D limit_lower >= limit_upper: built as a fixed joint"); return new GodotJointResult(Joint.Fixed(a, b, anchorCm, Normalise(axis), Perpendicular(axis)), notes); }
        }
        var axisN = Normalise(axis);
        var j = Joint.Hinge(a, b, anchorCm, axisN, Perpendicular(axisN), lo, hi);
        if (j.IsValid && p.EnableMotor)
        {
            j.SetMotor(MotorState.Velocity, p.MotorTargetVelocity);
            if (p.MotorMaxImpulse > 0f) j.SetMotorStrength(p.MotorMaxImpulse * physicsTicksPerSecond * Cm * Cm);
        }
        return new GodotJointResult(j, notes);
    }

    /// <summary>SliderJoint3D -> a slider along <paramref name="axis"/>. Only the linear limits carry over (upper below lower = unlimited). Limits are measured from the
    /// bodies' pose at creation, not from the joint frame, and a range that does not contain 0 is widened to include it.</summary>
    public static GodotJointResult Slider(Body a, Body b, Float3 anchorCm, Float3 axis, GodotSliderParams p)
    {
        var notes = new List<string>();
        Note(notes, p.LinearLimitSoftness, "SliderJoint3D linear_limit/softness");
        Note(notes, p.LinearLimitRestitution, "SliderJoint3D linear_limit/restitution");
        Note(notes, p.LinearLimitDamping, "SliderJoint3D linear_limit/damping");
        Note(notes, p.AngularLimitUpper, "SliderJoint3D angular_limit/upper_angle (Jolt's slider locks rotation)");
        Note(notes, p.AngularLimitLower, "SliderJoint3D angular_limit/lower_angle (Jolt's slider locks rotation)");
        float lo = -Free, hi = Free;
        if (p.LinearLimitUpper >= p.LinearLimitLower)
        {
            lo = p.LinearLimitLower * Cm; hi = p.LinearLimitUpper * Cm;
            if (lo > 0f || hi < 0f) notes.Add("SliderJoint3D limits do not contain the starting offset 0; widened to include it");
            if (lo >= hi) { notes.Add("SliderJoint3D limits are equal: built as a fixed joint"); return new GodotJointResult(Joint.Fixed(a, b, anchorCm, Normalise(axis), Perpendicular(axis)), notes); }
        }
        var axisN = Normalise(axis);
        return new GodotJointResult(Joint.Slider(a, b, anchorCm, axisN, Perpendicular(axisN), lo, hi), notes);
    }

    /// <summary>ConeTwistJoint3D -> a swing-twist joint about <paramref name="twistAxis"/>: a round swing cone of swing_span and a twist range of +-twist_span.</summary>
    public static GodotJointResult ConeTwist(Body a, Body b, Float3 anchorCm, Float3 twistAxis, GodotConeTwistParams p)
    {
        var notes = new List<string>();
        Note(notes, p.Bias, "ConeTwistJoint3D bias");
        Note(notes, p.Softness, "ConeTwistJoint3D softness");
        Note(notes, p.Relaxation, "ConeTwistJoint3D relaxation");
        float k = p.SpansAreFullAngles ? 0.5f : 1f;
        float swing = Math.Clamp(p.SwingSpan * k, 0f, MathF.PI), twist = Math.Clamp(p.TwistSpan * k, 0f, MathF.PI);
        var t = Normalise(twistAxis);
        return new GodotJointResult(Joint.SwingTwist(a, b, anchorCm, t, Perpendicular(t), swing, swing, -twist, twist), notes);
    }

    /// <summary>Generic6DOFJoint3D -> a six-DOF joint. Disabled limits are free; enabled ones are the range (equal bounds lock the axis). A spring becomes a position
    /// motor toward the equilibrium point with a frequency and damping ratio worked out from the stiffness and the effective mass, or <paramref name="p"/>'s inertia
    /// for the angular axes; a motor on the same axis replaces the spring. Jolt limits rotation about its Y and Z axes as a swing, which differs slightly from Godot's
    /// Euler limits at large angles.</summary>
    public static GodotJointResult Generic6Dof(Body a, Body b, Float3 anchorCm, Float3 axisX, Float3 axisY, GodotGeneric6DofParams p)
    {
        var notes = new List<string>();
        var lin = new[] { p.LinearX, p.LinearY, p.LinearZ };
        var ang = new[] { p.AngularX, p.AngularY, p.AngularZ };
        var mn = new float[6]; var mx = new float[6];
        for (int i = 0; i < 3; i++)
        {
            var l = lin[i];
            string ax = "XYZ"[i].ToString();
            Note(notes, l.LimitSoftness, "Generic6DOFJoint3D linear " + ax + " limit softness");
            Note(notes, l.LimitRestitution, "Generic6DOFJoint3D linear " + ax + " limit restitution");
            Note(notes, l.LimitDamping, "Generic6DOFJoint3D linear " + ax + " limit damping");
            if (l.LimitEnabled)
            {
                if (l.LimitLower > l.LimitUpper) { mn[i] = 1f; mx[i] = -1f; notes.Add("Generic6DOFJoint3D linear " + ax + " lower > upper: axis locked"); }
                else { mn[i] = l.LimitLower * Cm; mx[i] = l.LimitUpper * Cm; }
            }
            else { mn[i] = -Free; mx[i] = Free; }

            var g = ang[i];
            Note(notes, g.LimitSoftness, "Generic6DOFJoint3D angular " + ax + " limit softness");
            Note(notes, g.LimitRestitution, "Generic6DOFJoint3D angular " + ax + " limit restitution");
            Note(notes, g.LimitForceLimit, "Generic6DOFJoint3D angular " + ax + " limit force_limit");
            Note(notes, g.LimitErp, "Generic6DOFJoint3D angular " + ax + " limit erp");
            if (g.LimitEnabled)
            {
                if (g.LimitLower > g.LimitUpper) { mn[3 + i] = 1f; mx[3 + i] = -1f; notes.Add("Generic6DOFJoint3D angular " + ax + " lower > upper: axis locked"); }
                else { mn[3 + i] = g.LimitLower; mx[3 + i] = g.LimitUpper; }
            }
            else { mn[3 + i] = -Free; mx[3 + i] = Free; }
        }
        var x = Normalise(axisX);
        var j = Joint.SixDof(a, b, anchorCm, x, Normalise(axisY), mn, mx);
        if (!j.IsValid) return new GodotJointResult(j, notes);

        float mass = ReducedMass(a, b);
        for (int i = 0; i < 6; i++)
        {
            var axis = (SixDofAxis)i;
            bool angular = i >= 3;
            string name = (angular ? "angular " : "linear ") + "XYZ"[i % 3];
            bool springOn = angular ? ang[i - 3].SpringEnabled : lin[i].SpringEnabled;
            bool motorOn = angular ? ang[i - 3].MotorEnabled : lin[i].MotorEnabled;
            if (motorOn)
            {
                float target = angular ? ang[i - 3].MotorTargetVelocity : lin[i].MotorTargetVelocity * Cm;
                float limit = angular ? ang[i - 3].MotorForceLimit * Cm * Cm : lin[i].MotorForceLimit * Cm;
                j.SetMotor(axis, MotorState.Velocity, target);
                if (limit > 0f) j.SetMotorStrength(axis, limit);
                if (springOn) notes.Add("Generic6DOFJoint3D " + name + ": motor and spring both set; the motor wins");
            }
            else if (springOn)
            {
                float k = angular ? ang[i - 3].SpringStiffness : lin[i].SpringStiffness;
                float c = angular ? ang[i - 3].SpringDamping : lin[i].SpringDamping;
                float m = angular ? (p.EffectiveInertiaKgM2 ?? 0f) : mass;
                if (m <= 0f)
                {
                    notes.Add("Generic6DOFJoint3D " + name + " spring skipped: " + (angular ? "set EffectiveInertiaKgM2" : "both bodies are static"));
                    continue;
                }
                var (hz, ratio) = SpringToHz(k, c, m);
                if (hz <= 0f) continue;
                float eq = angular ? ang[i - 3].SpringEquilibriumPoint : lin[i].SpringEquilibriumPoint * Cm;
                j.SetMotor(axis, MotorState.Position, eq);
                j.SetMotorSpring(axis, hz, ratio);
            }
        }
        return new GodotJointResult(j, notes);
    }
}
