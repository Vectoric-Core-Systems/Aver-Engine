// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// A drivable car in one call. Aver.Physics.Vehicle is the low-level binding (create the chassis, add each wheel, set the
// engine, finish); this wraps it with the car most games want: CarSpec.Default describes it, Vehicles.CreateCar builds it,
// pairs it with a scene entity, and keeps that entity at the vehicle's pose. Design:
// docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md section 2.2 (Vehicle).
//
// UNITS AND AXES ARE THE ENGINE'S: centimetres, kilograms, N*m, degrees, +X forward, +Y right, +Z up. A car's position is
// its ORIGIN, the bottom centre of the car where a car mesh's own origin is, not its centre of mass.

using Aver.Scene;
using AP = Aver.Physics;

namespace Aver.Framework;

/// <summary>The numbers that make a car. <see cref="Default"/> is a family saloon; change one with a <c>with</c> expression.
/// Two front wheels steer, two rear wheels are driven and carry the handbrake, and each pair is an axle (so the physics layer
/// gives the driven axle a differential and every axle an anti-roll bar).</summary>
public readonly record struct CarSpec
{
    /// <summary>Half extents of the chassis box, cm (X forward, Y right, Z up).</summary>
    public Vec3 HalfExtentsCm { get; init; }
    /// <summary>How high the chassis underside is off the ground the origin touches, cm.</summary>
    public float GroundClearanceCm { get; init; }
    /// <summary>Centre of mass relative to the origin, cm. Low keeps the car on its wheels.</summary>
    public Vec3 CentreOfMassCm { get; init; }
    /// <summary>Chassis mass, kg.</summary>
    public float MassKg { get; init; }
    /// <summary>Distance between the front and rear axles, cm.</summary>
    public float WheelbaseCm { get; init; }
    /// <summary>Distance between the left and right wheels, cm.</summary>
    public float TrackCm { get; init; }
    /// <summary>Tyre radius, cm.</summary>
    public float WheelRadiusCm { get; init; }
    /// <summary>Tyre width, cm.</summary>
    public float WheelWidthCm { get; init; }
    /// <summary>Shortest suspension length (fully compressed), cm.</summary>
    public float SuspensionMinCm { get; init; }
    /// <summary>Longest suspension length (fully drooped), cm.</summary>
    public float SuspensionMaxCm { get; init; }
    /// <summary>Suspension spring frequency, Hz.</summary>
    public float SuspensionHz { get; init; }
    /// <summary>Suspension damping ratio; 0.5 is a car, 1 is critically damped.</summary>
    public float SuspensionDamping { get; init; }
    /// <summary>How far the front wheels turn at full steering, degrees.</summary>
    public float FrontSteerDeg { get; init; }
    /// <summary>The most each wheel's brake holds, N*m.</summary>
    public float BrakeTorque { get; init; }
    /// <summary>The most each rear wheel's handbrake holds, N*m.</summary>
    public float HandBrakeTorque { get; init; }
    /// <summary>Engine peak torque, N*m.</summary>
    public float EngineTorque { get; init; }
    /// <summary>Engine idle revs per minute.</summary>
    public float EngineMinRpm { get; init; }
    /// <summary>Engine rev limit per minute.</summary>
    public float EngineMaxRpm { get; init; }
    /// <summary>Keeps the chassis' up axis within this many degrees of the world's, so the car cannot land on its roof; 0 or less turns it off.</summary>
    public float MaxPitchRollDeg { get; init; }

    /// <summary>The frozen default car: a 460 x 190 x 90 cm box, 1500 kg, 280 cm wheelbase, 160 cm track, 36 cm tyres, a 500 N*m engine.</summary>
    public static CarSpec Default => new()
    {
        HalfExtentsCm = new Vec3(230f, 95f, 45f),
        GroundClearanceCm = 30f,
        CentreOfMassCm = new Vec3(0f, 0f, -10f),
        MassKg = 1500f,
        WheelbaseCm = 280f,
        TrackCm = 160f,
        WheelRadiusCm = 36f,
        WheelWidthCm = 25f,
        SuspensionMinCm = 30f,
        SuspensionMaxCm = 50f,
        SuspensionHz = 1.6f,
        SuspensionDamping = 0.5f,
        FrontSteerDeg = 30f,
        BrakeTorque = 1500f,
        HandBrakeTorque = 4000f,
        EngineTorque = 500f,
        EngineMinRpm = 1000f,
        EngineMaxRpm = 6000f,
        MaxPitchRollDeg = 60f,
    };
}

/// <summary>One wheel as <see cref="Vehicles.CreateCar"/> adds it: the Aver.Physics.Vehicle.AddWheel arguments, named.</summary>
/// <param name="AttachmentCm">Top of the suspension travel, relative to the vehicle origin, cm.</param>
/// <param name="RadiusCm">Tyre radius, cm.</param>
/// <param name="WidthCm">Tyre width, cm.</param>
/// <param name="SuspensionMinCm">Shortest suspension length, cm.</param>
/// <param name="SuspensionMaxCm">Longest suspension length, cm.</param>
/// <param name="SuspensionHz">Spring frequency, Hz.</param>
/// <param name="SuspensionDamping">Damping ratio.</param>
/// <param name="MaxSteerDeg">Steering at full input, degrees; 0 is fixed.</param>
/// <param name="BrakeTorque">Brake torque, N*m.</param>
/// <param name="HandBrakeTorque">Handbrake torque, N*m.</param>
/// <param name="Driven">Whether the engine drives this wheel.</param>
public readonly record struct WheelPlan(Vec3 AttachmentCm, float RadiusCm, float WidthCm, float SuspensionMinCm,
                                        float SuspensionMaxCm, float SuspensionHz, float SuspensionDamping,
                                        float MaxSteerDeg, float BrakeTorque, float HandBrakeTorque, bool Driven);

/// <summary>A car's speed, position and heading, read from the physics step.</summary>
/// <param name="ForwardSpeedCmS">Speed along the chassis' own +X, cm/s; negative when reversing.</param>
/// <param name="Position">The vehicle origin, cm.</param>
/// <param name="YawDegrees">Heading about +Z, degrees.</param>
public readonly record struct CarState(float ForwardSpeedCmS, Vec3 Position, float YawDegrees);

/// <summary>A car made by <see cref="Vehicles.CreateCar"/>: the physics vehicle and the scene entity that follows it.
/// A handle, not an object; <c>default</c> is no car.</summary>
public readonly struct Car : IEquatable<Car>
{
    /// <summary>The physics vehicle.</summary>
    public AP.Vehicle Vehicle { get; }
    /// <summary>The scene entity kept at the vehicle's pose; invalid if none could be made.</summary>
    public Entity Entity { get; }

    internal Car(AP.Vehicle vehicle, Entity entity) { Vehicle = vehicle; Entity = entity; }

    /// <summary>No car; also what a refused <see cref="Vehicles.CreateCar"/> returns.</summary>
    public static Car None => default;

    /// <summary>The raw vehicle handle, 0 for no car. This is the value Aver Node graphs carry.</summary>
    public int Handle => Vehicle.Handle;

    /// <summary>True when the physics layer accepted the car. Says nothing about whether it still exists.</summary>
    public bool IsValid => Vehicle.IsValid;

    /// <summary>The driver's input, which holds until set again: throttle -1..1 (negative reverses), steer -1..1 (+1 toward the
    /// car's right), brake and handbrake 0..1. False for a dead car.</summary>
    public bool SetInput(float throttle, float steer, float brake = 0f, float handbrake = 0f) =>
        Vehicle.SetInput(throttle, steer, brake, handbrake);

    /// <summary>Speed, origin position and heading. False for a dead car.</summary>
    public bool TryGetState(out CarState state)
    {
        state = default;
        if (!Vehicle.TryGetPose(out AP.Float3 p, out AP.Quaternion q)) return false;
        state = new CarState(Vehicle.ForwardSpeed, new Vec3(p.X, p.Y, p.Z), new Quat(q.X, q.Y, q.Z, q.W).YawDegrees());
        return true;
    }

    /// <summary>Puts the car's origin at <paramref name="position"/> facing <paramref name="yawDegrees"/>, stops it dead and wakes it.</summary>
    public bool SetPose(Vec3 position, float yawDegrees)
    {
        Quat q = new Rot(yawDegrees, 0f, 0f).ToQuat();
        if (!Vehicle.SetPose(new AP.Float3(position.X, position.Y, position.Z), new AP.Quaternion(q.X, q.Y, q.Z, q.W))) return false;
        if (Entity.IsValid && Entity.IsAlive)
        {
            Entity.SetLocalPosition(position);
            Entity.SetLocalRotation(q);
        }
        return true;
    }

    /// <summary>True when wheel <paramref name="index"/> touched ground on the last step.</summary>
    public bool WheelContact(int index) => Vehicle.WheelContact(index);

    /// <summary>Removes the vehicle, its chassis and its entity.</summary>
    public bool Destroy()
    {
        bool ok = Vehicle.Destroy();
        Vehicles.Forget(Handle);
        if (Entity.IsValid && Entity.IsAlive) Entity.Destroy();
        return ok;
    }

    /// <inheritdoc/>
    public bool Equals(Car other) => Handle == other.Handle;
    /// <inheritdoc/>
    public override bool Equals(object? obj) => obj is Car c && Equals(c);
    /// <inheritdoc/>
    public override int GetHashCode() => Handle;
    /// <inheritdoc/>
    public override string ToString() => IsValid ? $"Car#{Handle}" : "Car.None";
}

/// <summary>Builds cars. Needs the physics library; without it <see cref="CreateCar"/> answers <see cref="Car.None"/>.</summary>
/// <remarks>Cars made here are tracked by their vehicle handle (<see cref="TryGet"/>), which is how Aver Node graphs hold one.
/// A repeating 60 Hz timer copies each car's pose onto its entity, because a physics body stamped with an entity does not
/// move that entity by itself; call <see cref="SyncEntities"/> to do it on demand. An entity destroyed during play takes its
/// car with it.</remarks>
public static class Vehicles
{
    private const float Gravity = 981f;                 // cm/s^2
    private const float SyncInterval = 1f / 60f;

    private static readonly Dictionary<int, Car> s_cars = new();
    private static TimerHandle s_sync;

    /// <summary>How far a spring of <paramref name="hz"/> hertz sags under a quarter of a car's weight, cm: g / (2 pi f)^2.
    /// The wheel attachment points are lowered by this so the tyres rest on the ground the origin touches.</summary>
    public static float SpringSagCm(float hz) => hz > 0f ? Gravity / MathF.Pow(2f * MathF.PI * hz, 2f) : 0f;

    /// <summary>The four wheels of <paramref name="spec"/>: front left, front right, rear left, rear right. The front pair steers and
    /// the rear pair is driven and carries the handbrake. Attachment points sit at tyre radius plus longest suspension minus the spring sag.</summary>
    public static WheelPlan[] PlanWheels(CarSpec spec)
    {
        float z = spec.WheelRadiusCm + spec.SuspensionMaxCm - SpringSagCm(spec.SuspensionHz);
        float x = spec.WheelbaseCm * 0.5f;
        float y = spec.TrackCm * 0.5f;
        WheelPlan Wheel(float wx, float wy, bool front) =>
            new(new Vec3(wx, wy, z), spec.WheelRadiusCm, spec.WheelWidthCm, spec.SuspensionMinCm, spec.SuspensionMaxCm,
                spec.SuspensionHz, spec.SuspensionDamping, front ? spec.FrontSteerDeg : 0f, spec.BrakeTorque,
                front ? 0f : spec.HandBrakeTorque, !front);
        return new[] { Wheel(x, -y, true), Wheel(x, y, true), Wheel(-x, -y, false), Wheel(-x, y, false) };
    }

    /// <summary>Which axle each wheel belongs to, by the physics layer's rule: two wheels whose attachment points agree in X to within
    /// 5 cm and lie on opposite sides of Y = 0 form an axle; a wheel with no partner is its own axle.</summary>
    public static int[] AxleOf(IReadOnlyList<WheelPlan> wheels)
    {
        var axle = new int[wheels.Count];
        Array.Fill(axle, -1);
        int next = 0;
        for (int i = 0; i < wheels.Count; i++)
        {
            if (axle[i] >= 0) continue;
            axle[i] = next;
            for (int j = i + 1; j < wheels.Count; j++)
            {
                Vec3 a = wheels[i].AttachmentCm, b = wheels[j].AttachmentCm;
                if (axle[j] < 0 && MathF.Abs(a.X - b.X) <= 5f && a.Y * b.Y < 0f) { axle[j] = next; break; }
            }
            next++;
        }
        return axle;
    }

    /// <summary>How many axles the wheels form (see <see cref="AxleOf"/>).</summary>
    public static int CountAxles(IReadOnlyList<WheelPlan> wheels) => wheels.Count == 0 ? 0 : AxleOf(wheels).Max() + 1;

    /// <summary>How many differentials the physics layer builds: one per axle that has a driven wheel.</summary>
    public static int CountDifferentials(IReadOnlyList<WheelPlan> wheels)
    {
        int[] axle = AxleOf(wheels);
        var driven = new HashSet<int>();
        for (int i = 0; i < axle.Length; i++)
            if (wheels[i].Driven) driven.Add(axle[i]);
        return driven.Count;
    }

    /// <summary>Builds a default car at <paramref name="position"/> facing <paramref name="yawDegrees"/>.</summary>
    public static Car CreateCar(Vec3 position, float yawDegrees = 0f) => CreateCar(position, yawDegrees, CarSpec.Default);

    /// <summary>Builds the chassis, four wheels, the engine and the simulation, then a scene entity kept at the car's pose.
    /// <see cref="Car.None"/> when the physics layer refuses the numbers or is not loaded.</summary>
    public static Car CreateCar(Vec3 position, float yawDegrees, CarSpec spec)
    {
        try
        {
            Quat rot = new Rot(yawDegrees, 0f, 0f).ToQuat();
            AP.Vehicle v = AP.Vehicle.Create(F3(spec.HalfExtentsCm), spec.GroundClearanceCm, F3(spec.CentreOfMassCm), spec.MassKg,
                                             F3(position), new AP.Quaternion(rot.X, rot.Y, rot.Z, rot.W));
            if (!v.IsValid) return Car.None;
            foreach (WheelPlan w in PlanWheels(spec))
            {
                if (v.AddWheel(F3(w.AttachmentCm), w.RadiusCm, w.WidthCm, w.SuspensionMinCm, w.SuspensionMaxCm, w.SuspensionHz,
                               w.SuspensionDamping, w.MaxSteerDeg, w.BrakeTorque, w.HandBrakeTorque, w.Driven) >= 0) continue;
                v.Destroy();
                return Car.None;
            }
            if (!v.SetEngine(spec.EngineTorque, spec.EngineMinRpm, spec.EngineMaxRpm) || !v.Finish(spec.MaxPitchRollDeg))
            {
                v.Destroy();
                return Car.None;
            }

            Entity e = Entity.Create("Car");
            if (e.IsValid)
            {
                e.SetLocalPosition(position);
                e.SetLocalRotation(rot);
                v.Body.SetEntity(e.Handle);      // raycasts and contacts on the chassis name the entity
            }
            var car = new Car(v, e);
            s_cars[v.Handle] = car;
            EnsureSync();
            return car;
        }
        catch (DllNotFoundException) { return Car.None; }
        catch (EntryPointNotFoundException) { return Car.None; }   // a physics DLL without the vehicle ABI
    }

    /// <summary>The car made by <see cref="CreateCar"/> with this vehicle handle, if it still exists.</summary>
    public static bool TryGet(int vehicleHandle, out Car car) => s_cars.TryGetValue(vehicleHandle, out car);

    /// <summary>Copies each car's pose onto its entity now. Runs by itself every sixtieth of a second while a car exists.</summary>
    public static void SyncEntities()
    {
        if (s_cars.Count == 0) return;
        List<int>? gone = null;
        foreach (var (handle, car) in s_cars)
        {
            if (car.Entity.IsValid && !car.Entity.IsAlive)
            {
                car.Vehicle.Destroy();           // the entity died during play: its car goes with it
                (gone ??= new()).Add(handle);
                continue;
            }
            if (!car.Vehicle.TryGetPose(out AP.Float3 p, out AP.Quaternion q))
            {
                (gone ??= new()).Add(handle);    // the physics world was reset under it
                continue;
            }
            if (!car.Entity.IsValid) continue;
            car.Entity.SetLocalPosition(new Vec3(p.X, p.Y, p.Z));
            car.Entity.SetLocalRotation(new Quat(q.X, q.Y, q.Z, q.W));
        }
        if (gone != null)
            foreach (int h in gone) s_cars.Remove(h);
        if (s_cars.Count == 0 && s_sync.IsValid) { Timers.Clear(s_sync); s_sync = default; }
    }

    internal static void Forget(int vehicleHandle) => s_cars.Remove(vehicleHandle);

    private static void EnsureSync()
    {
        if (s_sync.IsValid && Timers.IsActive(s_sync)) return;
        s_sync = Timers.Set(SyncInterval, () => SyncEntities(), repeat: true, flags: TimerFlags.IgnoreDilation);
    }

    private static AP.Float3 F3(Vec3 v) => new(v.X, v.Y, v.Z);
}
