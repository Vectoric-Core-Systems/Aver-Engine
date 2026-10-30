// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// A raycast vehicle. The native side is physics_raycast_vehicle_abi.h; docs/physics/RAYCAST_VEHICLE.md has the model and
// the limits. A second vehicle model beside Jolt's wheeled one (Vehicle), which is unchanged. The defaults of RaycastWheelSettings are Aver's own neutral numbers;
// values in other units (metres, left-positive steering, brake impulses) are converted by the importer.

using System;

namespace Aver.Physics;

// The numbers of the C blocks, as enums so the parity test pins them. A vector member is the index of its X.
internal enum RaycastWheelParam
{
    ConnectX = 0, ConnectY = 1, ConnectZ = 2, DirX = 3, DirY = 4, DirZ = 5, AxleX = 6, AxleY = 7, AxleZ = 8,
    RestLength = 9, MaxTravel = 10, Stiffness = 11, DampCompression = 12, DampRelaxation = 13, MaxForce = 14, Radius = 15,
    FrictionSlip = 16, RollInfluence = 17, Traction = 18, Steering = 19, Count = 20,
}
internal enum RaycastWheelStateIndex
{
    InContact = 0, ContactPoint = 1, ContactNormal = 4, SuspLength = 7, SuspForce = 8, SkidInfo = 9, Spin = 10, Rotation = 11, Rpm = 12,
    ForwardSpeed = 13, SideSpeed = 14, ContactBody = 15, SteerApplied = 16, Count = 17,
}

/// <summary>Everything a wheel is built from. Chassis-local vectors, centimetres, kg*cm/s^2. The defaults are Aver's own.</summary>
public sealed record RaycastWheelSettings
{
    /// <summary>Where the suspension ray starts, in the chassis body's local space (cm).</summary>
    public Float3 ConnectionPoint { get; init; } = Float3.Zero;
    /// <summary>The suspension direction, from the chassis toward the ground (normalised natively).</summary>
    public Float3 SuspensionDirection { get; init; } = new(0, 0, -1);
    /// <summary>The wheel's axle: the direction its side friction acts along. +Y is a right-hand wheel of a +X-forward car.</summary>
    public Float3 Axle { get; init; } = new(0, 1, 0);
    /// <summary>Suspension length with no load (cm); the ray reaches this plus the radius.</summary>
    public float RestLength { get; init; } = 30f;
    /// <summary>How far the suspension can compress below the rest length (cm).</summary>
    public float MaxTravel { get; init; } = 30f;
    /// <summary>Mass-normalised spring (1/s^2): the force is this times the compression times the chassis mass, for every wheel. The sag under load is g / (wheels x stiffness).</summary>
    public float Stiffness { get; init; } = 20f;
    /// <summary>Mass-normalised damping while the suspension compresses (1/s).</summary>
    public float DampingCompression { get; init; } = 2f;
    /// <summary>Mass-normalised damping while it extends (1/s).</summary>
    public float DampingRelaxation { get; init; } = 3f;
    /// <summary>The most the suspension pushes (kg*cm/s^2); 0 = no limit.</summary>
    public float MaxForce { get; init; }
    /// <summary>Wheel radius (cm).</summary>
    public float Radius { get; init; } = 35f;
    /// <summary>Friction coefficient: the wheel's grip is this times its suspension load.</summary>
    public float FrictionSlip { get; init; } = 1f;
    /// <summary>Scales the height at which side friction acts: 1 at the contact, 0 at the centre of mass (no roll from tyres).</summary>
    public float RollInfluence { get; init; } = 1f;
    /// <summary>Engine force counts for this wheel.</summary>
    public bool UseAsTraction { get; init; } = true;
    /// <summary>Steering counts for this wheel.</summary>
    public bool UseAsSteering { get; init; }
    /// <summary>Layer bits the wheel's ray hits (default every layer).</summary>
    public uint CollisionMask { get; init; } = 0xFFFFFFFFu;

    internal float[] ToBlock() => new[]
    {
        ConnectionPoint.X, ConnectionPoint.Y, ConnectionPoint.Z, SuspensionDirection.X, SuspensionDirection.Y, SuspensionDirection.Z,
        Axle.X, Axle.Y, Axle.Z, RestLength, MaxTravel, Stiffness, DampingCompression, DampingRelaxation, MaxForce, Radius,
        FrictionSlip, RollInfluence, UseAsTraction ? 1f : 0f, UseAsSteering ? 1f : 0f,
    };

    /// <summary>A damping ratio (1 = critical for one wheel's share of the spring) to the mass-normalised damping (1/s): ratio x 2 x sqrt(stiffness).</summary>
    public static float DampingFromRatio(float ratio, float stiffness) => ratio * 2f * MathF.Sqrt(stiffness);
}

/// <summary>What a wheel did in the last step's pre-solver phase. Cm, cm/s, kg*cm/s^2, radians.</summary>
public readonly struct RaycastWheelState
{
    public bool InContact { get; init; }
    /// <summary>World space; the ray's end when not in contact.</summary>
    public Float3 ContactPoint { get; init; }
    public Float3 ContactNormal { get; init; }
    public float SuspensionLength { get; init; }
    public float SuspensionForce { get; init; }
    /// <summary>1 = gripping; below 1 the friction limit scaled the wheel's impulses to this fraction.</summary>
    public float SkidInfo { get; init; }
    /// <summary>rad/s, positive rolling forward.</summary>
    public float Spin { get; init; }
    public float Rotation { get; init; }
    public float Rpm { get; init; }
    public float ForwardSpeed { get; init; }
    public float SideSpeed { get; init; }
    /// <summary>The ground body, or <see cref="Body.None"/>.</summary>
    public Body Ground { get; init; }
    public float SteerApplied { get; init; }
}

/// <summary>A raycast vehicle on a dynamic body. A handle (not a body handle); 0 is invalid. Removing the chassis body destroys it.</summary>
public readonly struct RaycastVehicle : IEquatable<RaycastVehicle>
{
    /// <summary>Pass as a wheel index to set every wheel.</summary>
    public const int AllWheels = -1;

    public int Handle { get; }
    public RaycastVehicle(int handle) { Handle = handle; }
    public static RaycastVehicle None => new(0);

    public bool IsValid => Native.aver_phys_rayveh_exists(Handle) != 0;
    public static int Count => Native.aver_phys_rayveh_count();
    public Body Chassis => new(Native.aver_phys_rayveh_chassis(Handle));
    public int WheelCount => Native.aver_phys_rayveh_wheel_count(Handle);

    /// <summary>Makes <paramref name="chassis"/> (a dynamic, non-sensor body that is not a Jolt vehicle's chassis) a raycast vehicle; <see cref="None"/> if refused.</summary>
    public static RaycastVehicle Create(Body chassis) => new(Native.aver_phys_rayveh_create(chassis.Handle));
    public bool Destroy() => Native.aver_phys_rayveh_destroy(Handle) != 0;

    /// <summary>Adds a wheel; its index (add order), or -1 if the numbers are refused.</summary>
    public int AddWheel(RaycastWheelSettings settings)
    {
        float[] block = settings.ToBlock();
        int index = Native.aver_phys_rayveh_add_wheel(Handle, block, block.Length);
        if (index >= 0 && settings.CollisionMask != 0xFFFFFFFFu) Native.aver_phys_rayveh_set_wheel_mask(Handle, index, settings.CollisionMask);
        return index;
    }

    /// <summary>Changes one number of a wheel live (between steps).</summary>
    internal bool SetWheelParam(int wheel, RaycastWheelParam key, float value) => Native.aver_phys_rayveh_set_wheel_param(Handle, wheel, (int)key, value) != 0;
    internal float? WheelParam(int wheel, RaycastWheelParam key) => Native.aver_phys_rayveh_wheel_param(Handle, wheel, (int)key, out float v) != 0 ? v : null;

    public bool SetFrictionSlip(int wheel, float value) => SetWheelParam(wheel, RaycastWheelParam.FrictionSlip, value);
    public bool SetRollInfluence(int wheel, float value) => SetWheelParam(wheel, RaycastWheelParam.RollInfluence, value);
    public bool SetSuspension(int wheel, float stiffness, float dampingCompression, float dampingRelaxation)
        => SetWheelParam(wheel, RaycastWheelParam.Stiffness, stiffness) && SetWheelParam(wheel, RaycastWheelParam.DampCompression, dampingCompression)
           && SetWheelParam(wheel, RaycastWheelParam.DampRelaxation, dampingRelaxation);
    public bool SetWheelMask(int wheel, uint mask) => Native.aver_phys_rayveh_set_wheel_mask(Handle, wheel, mask) != 0;
    public uint? WheelMask(int wheel) => Native.aver_phys_rayveh_wheel_mask(Handle, wheel, out uint m) != 0 ? m : null;

    /// <summary>Engine force (kg*cm/s^2) along the wheel's forward; only a wheel with UseAsTraction, only on the ground. A non-zero force wakes the chassis.</summary>
    public bool SetEngineForce(int wheel, float force) => Native.aver_phys_rayveh_set_engine_force(Handle, wheel, force) != 0;
    /// <summary>The most braking force (kg*cm/s^2, not per step) the wheel may apply.</summary>
    public bool SetBrake(int wheel, float brake) => Native.aver_phys_rayveh_set_brake(Handle, wheel, brake) != 0;
    /// <summary>Steering in radians, positive toward the axle side; only a wheel with UseAsSteering.</summary>
    public bool SetSteering(int wheel, float radians) => Native.aver_phys_rayveh_set_steering(Handle, wheel, radians) != 0;

    public RaycastWheelState? GetWheelState(int wheel)
    {
        var s = new float[(int)RaycastWheelStateIndex.Count];
        if (Native.aver_phys_rayveh_wheel_state(Handle, wheel, s, s.Length) == 0) return null;
        return new RaycastWheelState
        {
            InContact = s[(int)RaycastWheelStateIndex.InContact] != 0f,
            ContactPoint = new Float3(s[1], s[2], s[3]),
            ContactNormal = new Float3(s[4], s[5], s[6]),
            SuspensionLength = s[(int)RaycastWheelStateIndex.SuspLength],
            SuspensionForce = s[(int)RaycastWheelStateIndex.SuspForce],
            SkidInfo = s[(int)RaycastWheelStateIndex.SkidInfo],
            Spin = s[(int)RaycastWheelStateIndex.Spin],
            Rotation = s[(int)RaycastWheelStateIndex.Rotation],
            Rpm = s[(int)RaycastWheelStateIndex.Rpm],
            ForwardSpeed = s[(int)RaycastWheelStateIndex.ForwardSpeed],
            SideSpeed = s[(int)RaycastWheelStateIndex.SideSpeed],
            Ground = new Body((int)s[(int)RaycastWheelStateIndex.ContactBody]),
            SteerApplied = s[(int)RaycastWheelStateIndex.SteerApplied],
        };
    }

    /// <summary>The wheel's centre (world, cm) and rotation for a mesh: at rest its forward is +X, its axle +Y, its up +Z.</summary>
    public bool TryGetWheelPose(int wheel, out Float3 position, out Quaternion rotation)
    {
        var p = new float[3]; var q = new float[4];
        bool ok = Native.aver_phys_rayveh_wheel_pose(Handle, wheel, p, q) != 0;
        position = ok ? new Float3(p[0], p[1], p[2]) : Float3.Zero;
        rotation = ok ? new Quaternion(q[0], q[1], q[2], q[3]) : Quaternion.Identity;
        return ok;
    }

    public bool Equals(RaycastVehicle other) => Handle == other.Handle;
    public override bool Equals(object? obj) => obj is RaycastVehicle v && Equals(v);
    public override int GetHashCode() => Handle;
    public static bool operator ==(RaycastVehicle a, RaycastVehicle b) => a.Equals(b);
    public static bool operator !=(RaycastVehicle a, RaycastVehicle b) => !a.Equals(b);
    public override string ToString() => Handle != 0 ? $"RaycastVehicle#{Handle}" : "RaycastVehicle.None";
}
