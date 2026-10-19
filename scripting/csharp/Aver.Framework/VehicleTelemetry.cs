// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The value types of the drivable-vehicle surface: presets, telemetry, wheel telemetry and events. Design: docs/VEHICLES.md.

using Aver.Scene;

namespace Aver.Framework;

/// <summary>The built-in vehicles a preset-only <c>Vehicle</c> component (or <see cref="Vehicles.Spawn"/>) can be: the five traffic
/// presets, sized from the mesh, and the tank. Pinned to <c>CVehicle::preset</c>.</summary>
public enum VehiclePreset { Car = 0, Van = 1, Truck = 2, Bus = 3, Sports = 4, Tank = 5 }

/// <summary>What a vehicle event is. 1 to 7 are the physics layer's (SkidBegin .. Assist); 8 and up are the system's own.</summary>
public enum VehicleEventKind
{
    SkidBegin = 1,
    SkidEnd = 2,
    Land = 3,
    Impact = 4,
    Shift = 5,
    Flipped = 6,
    Assist = 7,
    Entered = 8,
    Exited = 9,
    Crash = 10,
    Destroyed = 11,
}

/// <summary>A vehicle's state as of the last fixed physics step. Speeds are cm/s, angles degrees, accelerations in g.</summary>
public readonly record struct VehicleTelemetry(
    float SpeedCmS, float ForwardSpeedCmS, float Rpm, int Gear, int GearCount, bool Manual, bool Shifting,
    float Throttle, float Brake, float Steer, float Handbrake, int WheelsOnGround, bool AbsActive, bool TcsActive, bool Flipped, bool Destroyed,
    float Health01, float AirborneSeconds, float LongAccelG, float LatAccelG, float TrackLeftCmS, float TrackRightCmS);

/// <summary>One wheel's state as of the last fixed physics step. Longitudinal slip is a signed ratio: positive when the tyre turns
/// faster than the road moves under it, negative when it drags.</summary>
public readonly record struct WheelTelemetry(
    bool Contact, float SlipLongitudinal, float SlipLateralDeg, float LoadN, float CompressionCm, float AngularVelocityRadS, float SteerDeg,
    float SpinRad, Vec3 ContactPoint, Vec3 ContactNormal, float ForceLongitudinalN, float ForceLateralN, float SurfaceFriction,
    Entity SurfaceEntity, bool Skidding);

/// <summary>Something that happened to a vehicle. <paramref name="A"/> and <paramref name="B"/> mean what the physics event says
/// (a wheel index, gears from and to ...). <paramref name="Seq"/> is a per-system counter, the replication key.</summary>
public readonly record struct VehicleEvent(VehicleEventKind Kind, Entity Vehicle, int A, int B, Vec3 Point, Vec3 Normal, float Value, float Value2, int Seq);
