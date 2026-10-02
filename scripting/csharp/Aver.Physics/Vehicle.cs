// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
//
// A wheeled car: a chassis body plus suspension, tyres, engine, gearbox and differentials, simulated by
// Jolt's vehicle constraint. Built wheel by wheel -- Create, AddWheel for each, Finish -- because the C
// ABI describes it in scalars (physics_vehicle_abi.h has the full contract and the reasoning).
//
// THE HANDLE IS THE VEHICLE'S, not the body's: it comes from the same counter as Body and CharacterBody
// so it never collides with either, but <see cref="Body"/> is the chassis' own handle, and THAT is an
// ordinary body -- a raycast hits it, SetEntity stamps it, a character standing on it rides it.
//
// Poses are of the vehicle ORIGIN (the bottom centre of the car, where a car mesh's own origin is), not
// of the centre of mass that <see cref="Body.Position"/> reports for the chassis.

using System;

namespace Aver.Physics;

/// <summary>A wheeled vehicle. A handle, not an object: 0 is invalid.</summary>
public readonly struct Vehicle : IEquatable<Vehicle>
{
    public int Handle { get; }
    public Vehicle(int handle) { Handle = handle; }

    public static Vehicle None => new(0);
    public bool IsValid => Handle != 0;

    // ---- Building -------------------------------------------------------------------------------------

    /// <summary>Creates the chassis: a dynamic box of the given half extents (cm) whose centre sits
    /// <c>halfExtentsCm.Z + groundClearanceCm</c> above the vehicle origin, with its centre of mass at
    /// <paramref name="centreOfMassCm"/> (origin-relative, engine axes) and <paramref name="massKg"/> of mass.
    /// The origin is placed at <paramref name="position"/> with <paramref name="rotation"/>. Returns
    /// <see cref="None"/> for a non-positive extent or mass or a non-finite number. Not simulated as a
    /// vehicle until <see cref="Finish"/>.</summary>
    public static Vehicle Create(Float3 halfExtentsCm, float groundClearanceCm, Float3 centreOfMassCm,
                                 float massKg, Float3 position, Quaternion rotation) =>
        new(Native.aver_phys_vehicle_create(halfExtentsCm.X, halfExtentsCm.Y, halfExtentsCm.Z, groundClearanceCm,
                                            centreOfMassCm.X, centreOfMassCm.Y, centreOfMassCm.Z, massKg,
                                            position.X, position.Y, position.Z,
                                            rotation.X, rotation.Y, rotation.Z, rotation.W));

    /// <summary>Adds a wheel before <see cref="Finish"/> and returns its index, or -1. The attachment point
    /// is the TOP of the suspension travel, origin-relative, cm; the wheel centre hangs below it by the
    /// suspension length, which the springs settle between the two lengths given. Steering is in degrees
    /// (0 = fixed), brake torques in N*m. Two wheels at the same x on opposite sides of y = 0 form an axle,
    /// which gets a differential if either is driven and an anti-roll bar.</summary>
    public int AddWheel(Float3 attachmentCm, float radiusCm, float widthCm, float suspensionMinCm,
                        float suspensionMaxCm, float suspensionHz, float suspensionDamping,
                        float maxSteerDeg, float maxBrakeTorque, float maxHandBrakeTorque, bool driven) =>
        Native.aver_phys_vehicle_add_wheel(Handle, attachmentCm.X, attachmentCm.Y, attachmentCm.Z, radiusCm,
                                           widthCm, suspensionMinCm, suspensionMaxCm, suspensionHz,
                                           suspensionDamping, maxSteerDeg, maxBrakeTorque, maxHandBrakeTorque,
                                           driven ? 1 : 0);

    /// <summary>The engine, before <see cref="Finish"/>: peak torque (N*m) and the idle and limit revs.
    /// Never called, a vehicle gets 500, 1000 and 6000.</summary>
    public bool SetEngine(float maxTorque, float minRpm, float maxRpm) =>
        Native.aver_phys_vehicle_set_engine(Handle, maxTorque, minRpm, maxRpm) != 0;

    /// <summary>Builds the simulation. <paramref name="maxPitchRollDeg"/> keeps the chassis' up axis within
    /// that many degrees of the world's, so a car cannot be tipped onto its roof; 0 or less, or 180 or more,
    /// leaves it off. False for no wheels, no driven wheel, or a vehicle already finished.</summary>
    public bool Finish(float maxPitchRollDeg = 60f) => Native.aver_phys_vehicle_finish(Handle, maxPitchRollDeg) != 0;

    /// <summary>Removes the vehicle and then its chassis. Both handles are dead afterwards.</summary>
    public bool Destroy() => Native.aver_phys_vehicle_destroy(Handle) != 0;

    /// <summary>The chassis, an ordinary body; the invalid body for a dead vehicle.</summary>
    public Body Body => new(Native.aver_phys_vehicle_body(Handle));

    // ---- Driving --------------------------------------------------------------------------------------

    /// <summary>The driver's input, which holds until set again: <paramref name="forward"/> -1..1 (negative
    /// reverses), <paramref name="right"/> -1..1 (+1 steers toward engine +Y), and brake and handbrake 0..1.
    /// Call it before each physics step. A non-zero forward wakes a sleeping vehicle. False for a dead or
    /// unfinished vehicle.</summary>
    public bool SetInput(float forward, float right, float brake = 0f, float handbrake = 0f) =>
        Native.aver_phys_vehicle_set_input(Handle, forward, right, brake, handbrake) != 0;

    // ---- Reading it back ------------------------------------------------------------------------------

    /// <summary>The vehicle ORIGIN's position and rotation, engine axes. False for a dead vehicle.</summary>
    public bool TryGetPose(out Float3 position, out Quaternion rotation)
    {
        var p = new float[3];
        var q = new float[4];
        if (Native.aver_phys_vehicle_pose(Handle, p, q) == 0)
        {
            position = Float3.Zero;
            rotation = Quaternion.Identity;
            return false;
        }
        position = new Float3(p[0], p[1], p[2]);
        rotation = new Quaternion(q[0], q[1], q[2], q[3]);
        return true;
    }

    /// <summary>The origin's position, cm, or zero for a dead vehicle. NOT the centre of mass.</summary>
    public Float3 Position => TryGetPose(out var p, out _) ? p : Float3.Zero;
    /// <summary>The origin's rotation, or the identity for a dead vehicle.</summary>
    public Quaternion Rotation => TryGetPose(out _, out var r) ? r : Quaternion.Identity;

    /// <summary>The chassis' linear velocity, cm/s, world space.</summary>
    public Float3 Velocity
    {
        get { var v = new float[3]; return Native.aver_phys_vehicle_velocity(Handle, v) != 0 ? new Float3(v[0], v[1], v[2]) : Float3.Zero; }
    }

    /// <summary>The speed along the chassis' own +X, cm/s; negative when reversing.</summary>
    public float ForwardSpeed => Native.aver_phys_vehicle_forward_speed(Handle);

    /// <summary>How many wheels the vehicle has.</summary>
    public int WheelCount => Native.aver_phys_vehicle_wheel_count(Handle);

    /// <summary>Wheel <paramref name="index"/>'s centre and rotation RELATIVE TO THE VEHICLE ORIGIN, engine
    /// axes, for a wheel mesh whose axle is engine Y: the steering turn and the spin rolled through. False
    /// for a dead or unfinished vehicle or a bad index.</summary>
    public bool TryGetWheelPose(int index, out Float3 position, out Quaternion rotation)
    {
        var p = new float[3];
        var q = new float[4];
        if (Native.aver_phys_vehicle_wheel_pose(Handle, index, p, q) == 0)
        {
            position = Float3.Zero;
            rotation = Quaternion.Identity;
            return false;
        }
        position = new Float3(p[0], p[1], p[2]);
        rotation = new Quaternion(q[0], q[1], q[2], q[3]);
        return true;
    }

    /// <summary>True when wheel <paramref name="index"/> touched ground on the last step.</summary>
    public bool WheelContact(int index) => Native.aver_phys_vehicle_wheel_contact(Handle, index) != 0;

    // ---- Teleport -------------------------------------------------------------------------------------

    /// <summary>Puts the vehicle ORIGIN at <paramref name="position"/> with <paramref name="rotation"/>, stops
    /// it dead (the wheels and engine too) and wakes it. For a reset after a crash or a lane jump.</summary>
    public bool SetPose(Float3 position, Quaternion rotation) =>
        Native.aver_phys_vehicle_set_pose(Handle, position.X, position.Y, position.Z,
                                          rotation.X, rotation.Y, rotation.Z, rotation.W) != 0;

    public bool Equals(Vehicle other) => Handle == other.Handle;
    public override bool Equals(object? obj) => obj is Vehicle v && Equals(v);
    public override int GetHashCode() => Handle;
    public static bool operator ==(Vehicle a, Vehicle b) => a.Equals(b);
    public static bool operator !=(Vehicle a, Vehicle b) => !a.Equals(b);
    public override string ToString() => IsValid ? $"Vehicle#{Handle}" : "Vehicle.None";
}
