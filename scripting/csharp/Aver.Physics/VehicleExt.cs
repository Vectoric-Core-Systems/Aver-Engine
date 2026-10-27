// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// Vehicles v2 (physics_vehicle_ext_abi.h). Units: cm, kg, N*m, degrees for steering and slip angle, rad/s for wheel speed.
// A vehicle that never calls anything here drives exactly as under v1.

using System;

namespace Aver.Physics;

/// <summary>Wheeled (a car or truck) or tracked (a tank).</summary>
public enum VehicleKind
{
    Wheeled = 0,
    Tracked = 1,
}

/// <summary>How an axle (or the centre differential) shares torque between its sides. Jolt has no hard-locked differential: <see cref="Locked"/> is a limited-slip ratio of 1.01.</summary>
public enum DifferentialMode
{
    Open = 0,
    LimitedSlip = 1,
    Locked = 2,
}

/// <summary>The live tuning keys of <see cref="Vehicle.Tune"/>. The steering-speed curve is up to four (speed cm/s, scale) points written in order through <c>SteerCurve0</c> to <c>SteerCurve7</c>; a point with speed 0 and scale 0 ends the list.</summary>
public enum VehicleTune
{
    EngineTorqueScale = 0,
    BrakeScale = 1,
    GripScale = 2,
    SuspensionStiffnessScale = 3,
    SteerScale = 4,
    DragScale = 5,
    DownforceScale = 6,
    SteerCurve0 = 7,
    SteerCurve1 = 8,
    SteerCurve2 = 9,
    SteerCurve3 = 10,
    SteerCurve4 = 11,
    SteerCurve5 = 12,
    SteerCurve6 = 13,
    SteerCurve7 = 14,
    /// <summary>How many tune keys there are (AVER_PHYS_VU_COUNT); not a key.</summary>
    Count = 15,
}

/// <summary>What a polled vehicle event is (VEV in the header).</summary>
public enum VehiclePhysicsEventKind
{
    SkidBegin = 1,
    SkidEnd = 2,
    Land = 3,
    Impact = 4,
    Shift = 5,
    Flipped = 6,
    Assist = 7,
}

// Index blocks of the header arrays, as enums: the parity test pins every number, the code names them.
internal enum VeIndex { MaxTorque = 0, MinRpm = 1, MaxRpm = 2, Inertia = 3, AngularDamping = 4, Count = 5 }
internal enum VgIndex { SwitchTime = 0, ClutchReleaseTime = 1, SwitchLatency = 2, ShiftUpFraction = 3, ShiftDownFraction = 4, ClutchStrength = 5, Count = 6 }
internal enum VaIndex { FinalDrive = 0, DiffMode = 1, LsdRatio = 2, LeftRightSplit = 3, TorqueShare = 4, AntiRollStiffness = 5, Count = 6 }
internal enum VwIndex { Inertia = 0, AngularDamping = 1, TvLongFriction = 2, TvLatFriction = 3, BrakeBias = 4, Count = 5 }
internal enum VtIndex { Inertia = 0, AngularDamping = 1, BrakeTorque = 2, FinalDrive = 3, SteerGain = 4, PivotPower = 5, Count = 6 }
internal enum VdIndex
{
    Enabled = 0, Abs = 1, AbsSlip = 2, AbsRelease = 3, Tcs = 4, TcsSlip = 5, SteerIn = 6, SteerOut = 7, Reserved = 8,
    ThrottleIn = 9, ThrottleOut = 10, BrakeIn = 11, ReverseOnBrake = 12, ReverseSpeed = 13, HoldBrake = 14, HoldSpeed = 15,
    SkidSlip = 16, SkidAngleDeg = 17, Count = 18,
}
internal enum VsIndex
{
    Rpm = 0, EngineTorqueNm = 1, SpeedCms = 2, ForwardSpeedCms = 3, LateralSpeedCms = 4, ThrottleApplied = 5, BrakeApplied = 6,
    SteerApplied = 7, HandbrakeApplied = 8, Clutch = 9, WheelRpmAtClutch = 10, YawRateDegS = 11, LongAccelG = 12, LatAccelG = 13,
    UpZ = 14, TrackLeftCms = 15, TrackRightCms = 16, AirborneSeconds = 17, Count = 18,
}
internal enum VsiIndex
{
    Gear = 0, GearCount = 1, Manual = 2, Shifting = 3, WheelsOnGround = 4, AbsActive = 5, TcsActive = 6, Kind = 7,
    AxleCount = 8, WheelCount = 9, RevLimited = 10, Count = 11,
}
internal enum VwsIndex
{
    SlipLong = 0, SlipLatDeg = 1, LoadN = 2, CompressionCm = 3, SuspLengthCm = 4, AngVelRadS = 5, SteerDeg = 6, SpinRad = 7,
    ContactPointX = 8, ContactPointY = 9, ContactPointZ = 10, ContactNormalX = 11, ContactNormalY = 12, ContactNormalZ = 13,
    ForceLongN = 14, ForceLatN = 15, SurfaceFriction = 16, BrakeTorqueNm = 17, Count = 18,
}
internal enum VwsiIndex { Contact = 0, ContactBody = 1, ContactEntity = 2, Skidding = 3, Count = 4 }

/// <summary>The gearbox timings of <see cref="Vehicle.SetGearbox"/>. An all-zero value (<c>default</c>) leaves them at Jolt's own.</summary>
public readonly record struct GearboxTiming(float SwitchTime, float ClutchReleaseTime, float SwitchLatency,
                                            float ShiftUpFraction, float ShiftDownFraction, float ClutchStrength)
{
    /// <summary>Jolt's own timings (the clutch strength 0 means "scale with the engine's torque").</summary>
    public static GearboxTiming Jolt => new(0.5f, 0.3f, 0.5f, 0.6f, 0.2f, 0f);
    internal bool IsUnset => this == default;
}

/// <summary>The driver aids of <see cref="Vehicle.SetAssists"/> (VD in the header). They run in the fixed step; <see cref="Enabled"/> off is the v1 pass-through (raw pedals straight to the controller).</summary>
public readonly record struct DriverAids(
    bool Enabled, bool Abs, float AbsSlip, float AbsRelease, bool Tcs, float TcsSlip, float SteerIn, float SteerOut,
    float ThrottleIn, float ThrottleOut, float BrakeIn, bool ReverseOnBrake, float ReverseSpeed, float HoldBrake,
    float HoldSpeed, float SkidSlip, float SkidAngleDeg)
{
    /// <summary>The defaults of the header, aids on but ABS and traction control off.</summary>
    public static DriverAids Default => new(true, false, 0.25f, 8f, false, 0.35f, 3f, 5f, 6f, 8f, 8f, true, 50f, 0.3f, 30f, 0.45f, 12f);

    internal float[] ToBlock() => new[]
    {
        Enabled ? 1f : 0f, Abs ? 1f : 0f, AbsSlip, AbsRelease, Tcs ? 1f : 0f, TcsSlip, SteerIn, SteerOut, 0f,
        ThrottleIn, ThrottleOut, BrakeIn, ReverseOnBrake ? 1f : 0f, ReverseSpeed, HoldBrake, HoldSpeed, SkidSlip, SkidAngleDeg,
    };
}

/// <summary>One vehicle's telemetry as of the last fixed step (VS and VSI in the header).</summary>
public readonly record struct VehicleState(
    float Rpm, float EngineTorqueNm, float SpeedCmS, float ForwardSpeedCmS, float LateralSpeedCmS, float Throttle, float Brake,
    float Steer, float Handbrake, float Clutch, float WheelRpmAtClutch, float YawRateDegS, float LongAccelG, float LatAccelG,
    float UpZ, float TrackLeftCmS, float TrackRightCmS, float AirborneSeconds,
    int Gear, int GearCount, bool Manual, bool Shifting, int WheelsOnGround, bool AbsActive, bool TcsActive,
    VehicleKind Kind, int AxleCount, int WheelCount, bool RevLimited);

/// <summary>One wheel's telemetry as of the last fixed step (VWS and VWSI in the header). Longitudinal slip is a signed ratio: positive when the tyre turns faster than the road moves, negative when it drags.</summary>
public readonly record struct WheelState(
    bool Contact, float SlipLongitudinal, float SlipLateralDeg, float LoadN, float CompressionCm, float SuspensionLengthCm,
    float AngularVelocityRadS, float SteerDeg, float SpinRad, Float3 ContactPoint, Float3 ContactNormal,
    float ForceLongitudinalN, float ForceLateralN, float SurfaceFriction, float BrakeTorqueNm,
    int ContactBody, int ContactEntity, bool Skidding);

/// <summary>A polled vehicle event (see VEV in the header for what A, B, Value and Value2 mean per kind).</summary>
public readonly record struct VehiclePhysicsEvent(VehiclePhysicsEventKind Kind, int Vehicle, int A, int B,
                                                  Float3 Point, Float3 Normal, float Value, float Value2);

public readonly partial struct Vehicle
{
    /// <summary>The ABI version the loaded physics library reports: 2 with the extended vehicle ABI, 1 without.</summary>
    public static int AbiVersion
    {
        get
        {
            try { return Native.aver_phys_vehicle_abi_version(); }
            catch (EntryPointNotFoundException) { return 1; }
        }
    }

    /// <summary>Wheeled (the default) or tracked. Before the first <see cref="AddWheel"/>.</summary>
    public bool SetKind(VehicleKind kind) => Native.aver_phys_vehicle_set_kind(Handle, (int)kind) != 0;

    /// <summary>Replaces the single chassis box with one to eight: <c>cx cy cz hx hy hz</c> per box, origin-relative cm. Mass and centre of mass are unchanged.</summary>
    public bool SetChassisBoxes(ReadOnlySpan<float> boxes) =>
        boxes.Length >= 6 && boxes.Length % 6 == 0 && Native.aver_phys_vehicle_set_chassis_boxes(Handle, boxes.ToArray(), boxes.Length / 6) != 0;

    /// <summary>Replaces the chassis with any <see cref="CollisionShape"/> that has a mass, in the vehicle's origin frame (bottom centre of the car). Before <c>Finish</c>.</summary>
    public bool SetChassisShape(CollisionShape shape) => Native.aver_phys_vehicle_set_chassis_shape(Handle, shape.Handle) != 0;

    /// <summary>The engine: peak torque (N*m), idle and limit revs, rotational inertia (0 = scaled to the torque as the plain engine is) and damping (engine braking).</summary>
    public bool SetEngine(float maxTorque, float minRpm, float maxRpm, float inertia = 0f, float damping = 0.2f) =>
        Native.aver_phys_vehicle_set_engine_ex(Handle, new[] { maxTorque, minRpm, maxRpm, inertia, damping }, (int)VeIndex.Count) != 0;

    /// <summary>The torque curve: x is the fraction of the limit revs (ascending, 0..1), y the fraction of peak torque. Two to 32 points.</summary>
    public bool SetTorqueCurve(ReadOnlySpan<float> rpmFraction, ReadOnlySpan<float> torqueFraction) =>
        rpmFraction.Length == torqueFraction.Length &&
        Native.aver_phys_vehicle_set_torque_curve(Handle, rpmFraction.ToArray(), torqueFraction.ToArray(), rpmFraction.Length) != 0;

    /// <summary>Forward ratios (positive), reverse ratios (positive magnitudes), automatic or manual, and the timings. An empty list keeps Jolt's ratios.</summary>
    public bool SetGearbox(bool manual, ReadOnlySpan<float> forward, ReadOnlySpan<float> reverse, GearboxTiming timing = default)
    {
        float[]? block = timing.IsUnset ? null : new[]
        {
            timing.SwitchTime, timing.ClutchReleaseTime, timing.SwitchLatency, timing.ShiftUpFraction, timing.ShiftDownFraction, timing.ClutchStrength,
        };
        return Native.aver_phys_vehicle_set_gearbox(Handle, manual ? 1 : 0, forward.ToArray(), forward.Length, reverse.ToArray(), reverse.Length,
                                                    block, block?.Length ?? 0) != 0;
    }

    /// <summary>How many axles the wheels form (two wheels at the same x on opposite sides), front first.</summary>
    public int AxleCount => Native.aver_phys_vehicle_axle_count(Handle);

    /// <summary>The axle (front is 0) wheel <paramref name="wheel"/> sits on, or -1.</summary>
    public int AxleOf(int wheel) => Native.aver_phys_vehicle_wheel_axle(Handle, wheel);

    /// <summary>An axle's final drive ratio, differential, torque share between driven axles (0 = equal) and anti-roll bar (N/m, 0 = none).</summary>
    public bool SetAxle(int axle, float finalDrive, DifferentialMode mode, float lsdRatio = 1.4f, float split = 0.5f,
                        float torqueShare = 0f, float antiRollNmPerM = 1000f) =>
        Native.aver_phys_vehicle_set_axle(Handle, axle, new[] { finalDrive, (float)(int)mode, lsdRatio, split, torqueShare, antiRollNmPerM }, (int)VaIndex.Count) != 0;

    /// <summary>The differential between the driven axles of an all-wheel-drive vehicle.</summary>
    public bool SetCenterDifferential(DifferentialMode mode, float lsdRatio = 1.4f) =>
        Native.aver_phys_vehicle_set_center_diff(Handle, (int)mode, lsdRatio) != 0;

    /// <summary>A wheel's rotational inertia and damping, its share of the brake (1 = the class's torque), and the friction of a tracked wheel.</summary>
    public bool SetWheel(int wheel, float inertia = 0.9f, float angularDamping = 0.2f, float brakeBias = 1f,
                         float trackLongitudinalFriction = 4f, float trackLateralFriction = 2f) =>
        Native.aver_phys_vehicle_set_wheel_ex(Handle, wheel, new[] { inertia, angularDamping, trackLongitudinalFriction, trackLateralFriction, brakeBias }, (int)VwIndex.Count) != 0;

    /// <summary>The tyre's friction curves: longitudinal (slip ratio, mu) and lateral (slip degrees, mu), two to 16 points each. Wheel -1 sets them all.</summary>
    public bool SetWheelCurves(int wheel, ReadOnlySpan<float> longSlip, ReadOnlySpan<float> longMu, ReadOnlySpan<float> latDeg, ReadOnlySpan<float> latMu) =>
        longSlip.Length == longMu.Length && latDeg.Length == latMu.Length &&
        Native.aver_phys_vehicle_set_wheel_curves(Handle, wheel, longSlip.ToArray(), longMu.ToArray(), longSlip.Length,
                                                  latDeg.ToArray(), latMu.ToArray(), latDeg.Length) != 0;

    /// <summary>A tracked vehicle's track: side 0 left, 1 right, the road wheels of that side, the driven (sprocket) wheel among them.</summary>
    public bool SetTrack(int side, ReadOnlySpan<int> wheels, int drivenWheel, float inertia = 10f, float damping = 0.5f,
                         float brakeNm = 15000f, float finalDrive = 6f, float steerGain = 1.6f, float pivotPower = 0.6f) =>
        Native.aver_phys_vehicle_set_track(Handle, side, wheels.ToArray(), wheels.Length, drivenWheel,
                                           new[] { inertia, damping, brakeNm, finalDrive, steerGain, pivotPower }, (int)VtIndex.Count) != 0;

    /// <summary>The driver aids. Run in the fixed step, so they do not depend on the frame rate.</summary>
    public bool SetAssists(in DriverAids aids) => Native.aver_phys_vehicle_set_assists(Handle, aids.ToBlock(), (int)VdIndex.Count) != 0;

    /// <summary>Aerodynamic drag and downforce, N per (m/s)^2, applied at the centre of mass.</summary>
    public bool SetAero(float drag, float downforce) => Native.aver_phys_vehicle_set_aero(Handle, drag, downforce) != 0;

    /// <summary>Runs the wheel shape casts every Nth fixed step (1 to 8): the lever that makes distant vehicles cheaper.</summary>
    public bool SetCollisionPeriod(int everyNSteps) => Native.aver_phys_vehicle_set_collision_period(Handle, everyNSteps) != 0;

    /// <summary>Live tuning: engine torque, brake, grip, suspension stiffness, steering, drag and downforce scales.</summary>
    public bool Tune(VehicleTune key, float value) => Native.aver_phys_vehicle_tune(Handle, (int)key, value) != 0;

    /// <summary>Switches between an automatic and a manual gearbox.</summary>
    public bool SetManualGearbox(bool manual) => Native.aver_phys_vehicle_set_gearbox_mode(Handle, manual ? 1 : 0) != 0;

    /// <summary>Manual box only: -1 reverse, 0 neutral, 1 and up. False when out of range.</summary>
    public bool SetGear(int gear) => Native.aver_phys_vehicle_set_gear(Handle, gear) != 0;

    /// <summary>Manual box only: one gear up (+1) or down (-1), clamped, through a timed clutch.</summary>
    public bool Shift(int delta) => Native.aver_phys_vehicle_shift(Handle, delta) != 0;

    /// <summary>Puts the vehicle upright about its current heading, stops it dead and sets it down <paramref name="liftCm"/> higher.</summary>
    public bool Flip(float liftCm = 50f) => Native.aver_phys_vehicle_flip(Handle, liftCm) != 0;

    /// <summary>The vehicle's telemetry as of the last fixed step. False for a dead or unfinished vehicle.</summary>
    public bool TryGetState(out VehicleState state)
    {
        var f = new float[(int)VsIndex.Count];
        var i = new int[(int)VsiIndex.Count];
        if (Native.aver_phys_vehicle_state(Handle, f, f.Length, i, i.Length) == 0)
        {
            state = default;
            return false;
        }
        state = new VehicleState(
            f[(int)VsIndex.Rpm], f[(int)VsIndex.EngineTorqueNm], f[(int)VsIndex.SpeedCms], f[(int)VsIndex.ForwardSpeedCms],
            f[(int)VsIndex.LateralSpeedCms], f[(int)VsIndex.ThrottleApplied], f[(int)VsIndex.BrakeApplied], f[(int)VsIndex.SteerApplied],
            f[(int)VsIndex.HandbrakeApplied], f[(int)VsIndex.Clutch], f[(int)VsIndex.WheelRpmAtClutch], f[(int)VsIndex.YawRateDegS],
            f[(int)VsIndex.LongAccelG], f[(int)VsIndex.LatAccelG], f[(int)VsIndex.UpZ], f[(int)VsIndex.TrackLeftCms],
            f[(int)VsIndex.TrackRightCms], f[(int)VsIndex.AirborneSeconds],
            i[(int)VsiIndex.Gear], i[(int)VsiIndex.GearCount], i[(int)VsiIndex.Manual] != 0, i[(int)VsiIndex.Shifting] != 0,
            i[(int)VsiIndex.WheelsOnGround], i[(int)VsiIndex.AbsActive] != 0, i[(int)VsiIndex.TcsActive] != 0,
            (VehicleKind)i[(int)VsiIndex.Kind], i[(int)VsiIndex.AxleCount], i[(int)VsiIndex.WheelCount], i[(int)VsiIndex.RevLimited] != 0);
        return true;
    }

    /// <summary>One wheel's telemetry as of the last fixed step. False for a bad index or an unfinished vehicle.</summary>
    public bool TryGetWheelState(int wheel, out WheelState state)
    {
        var f = new float[(int)VwsIndex.Count];
        var i = new int[(int)VwsiIndex.Count];
        if (Native.aver_phys_vehicle_wheel_state(Handle, wheel, f, f.Length, i, i.Length) == 0)
        {
            state = default;
            return false;
        }
        state = new WheelState(
            i[(int)VwsiIndex.Contact] != 0, f[(int)VwsIndex.SlipLong], f[(int)VwsIndex.SlipLatDeg], f[(int)VwsIndex.LoadN],
            f[(int)VwsIndex.CompressionCm], f[(int)VwsIndex.SuspLengthCm], f[(int)VwsIndex.AngVelRadS], f[(int)VwsIndex.SteerDeg],
            f[(int)VwsIndex.SpinRad],
            new Float3(f[(int)VwsIndex.ContactPointX], f[(int)VwsIndex.ContactPointY], f[(int)VwsIndex.ContactPointZ]),
            new Float3(f[(int)VwsIndex.ContactNormalX], f[(int)VwsIndex.ContactNormalY], f[(int)VwsIndex.ContactNormalZ]),
            f[(int)VwsIndex.ForceLongN], f[(int)VwsIndex.ForceLatN], f[(int)VwsIndex.SurfaceFriction], f[(int)VwsIndex.BrakeTorqueNm],
            i[(int)VwsiIndex.ContactBody], i[(int)VwsiIndex.ContactEntity], i[(int)VwsiIndex.Skidding] != 0);
        return true;
    }

    /// <summary>Copies this step's vehicle events (for every vehicle in the world) into <paramref name="buffer"/> and returns how many were written.</summary>
    public static int DrainEvents(Span<VehiclePhysicsEvent> buffer)
    {
        int count = Native.aver_phys_vehicle_event_count();
        int n = Math.Min(count, buffer.Length);
        var i4 = new int[4];
        var f8 = new float[8];
        for (int k = 0; k < n; k++)
        {
            if (Native.aver_phys_vehicle_event_get(k, i4, f8) == 0) return k;
            buffer[k] = new VehiclePhysicsEvent((VehiclePhysicsEventKind)i4[0], i4[1], i4[2], i4[3],
                new Float3(f8[0], f8[1], f8[2]), new Float3(f8[3], f8[4], f8[5]), f8[6], f8[7]);
        }
        return n;
    }
}
