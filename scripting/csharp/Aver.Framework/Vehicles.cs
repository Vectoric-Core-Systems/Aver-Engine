// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// A drivable car in one call. Aver.Physics.Vehicle is the low-level binding (create the chassis, add each wheel, set the
// engine, finish); this wraps it with the car most games want: CarSpec.Default describes it, Vehicles.CreateCar builds it
// and pairs it with a scene entity. The car is made by the vehicle SYSTEM from CarSpec.ToOcVehicle() (one parser, one builder),
// and the system writes the entity's pose every frame in C++. Design: docs/VEHICLES.md and
// docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md section 2.2 (Vehicle).
//
// UNITS AND AXES ARE THE ENGINE'S: centimetres, kilograms, N*m, degrees, +X forward, +Y right, +Z up. A car's position is
// its ORIGIN, the bottom centre of the car where a car mesh's own origin is, not its centre of mass.

using System.Globalization;
using System.Text;
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

    /// <summary>This car as .ocvehicle text: the four-wheel plan <see cref="Vehicles.PlanWheels"/> computes (front pair steered, rear pair
    /// driven with the handbrake), the engine and the chassis, with every other record at the physics layer's defaults, so the car drives
    /// exactly as it did before .ocvehicle existed. The wheel hubs are the SETTLED wheel centres; the builder lowers the attachment points
    /// by the spring sag.</summary>
    public string ToOcVehicle()
    {
        static string N(float v) => v.ToString("R", CultureInfo.InvariantCulture);
        float x = WheelbaseCm * 0.5f, y = TrackCm * 0.5f, r = WheelRadiusCm, steer = FrontSteerDeg;
        var sb = new StringBuilder();
        sb.Append("OCVEHICLE 1\n");
        sb.Append("NAME CarSpec\n");
        sb.Append("KIND wheeled\n");
        sb.Append("BODY half ").Append(N(HalfExtentsCm.X)).Append(' ').Append(N(HalfExtentsCm.Y)).Append(' ').Append(N(HalfExtentsCm.Z))
          .Append(" clearance ").Append(N(GroundClearanceCm))
          .Append(" com ").Append(N(CentreOfMassCm.X)).Append(' ').Append(N(CentreOfMassCm.Y)).Append(' ').Append(N(CentreOfMassCm.Z))
          .Append(" mass ").Append(N(MassKg)).Append(" pitchroll ").Append(N(MaxPitchRollDeg)).Append('\n');
        sb.Append("ENGINE torque ").Append(N(EngineTorque)).Append(" minrpm ").Append(N(EngineMinRpm)).Append(" maxrpm ").Append(N(EngineMaxRpm)).Append('\n');
        sb.Append("WHEELCLASS wheel radius ").Append(N(r)).Append(" width ").Append(N(WheelWidthCm))
          .Append(" susp ").Append(N(SuspensionMinCm)).Append(' ').Append(N(SuspensionMaxCm))
          .Append(" hz ").Append(N(SuspensionHz)).Append(" damping ").Append(N(SuspensionDamping))
          .Append(" brake ").Append(N(BrakeTorque)).Append(" handbrake ").Append(N(HandBrakeTorque)).Append('\n');
        void Wheel(string name, float wx, float wy, bool front) =>
            sb.Append("WHEEL ").Append(name).Append(" class wheel hub ").Append(N(wx)).Append(' ').Append(N(wy)).Append(' ').Append(N(r))
              .Append(" steer ").Append(N(front ? steer : 0f)).Append(" driven ").Append(front ? '0' : '1')
              .Append(" handbrake ").Append(front ? '0' : '1').Append('\n');
        Wheel("FL", x, -y, true);
        Wheel("FR", x, y, true);
        Wheel("RL", -x, -y, false);
        Wheel("RR", -x, y, false);
        return sb.ToString();
    }
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
    /// car's right), brake and handbrake 0..1. It goes through the Vehicle component the system reads, so the aids and a driver's
    /// actions see one source. False for a dead car.</summary>
    public bool SetInput(float throttle, float steer, float brake = 0f, float handbrake = 0f) =>
        Entity.IsValid ? Entity.SetVehicleInput(throttle, steer, brake, handbrake) : Vehicle.SetInput(throttle, steer, brake, handbrake);

    /// <summary>The car's telemetry as of the last physics step. False for a dead car.</summary>
    public bool TryGetTelemetry(out VehicleTelemetry telemetry) { telemetry = default; return Entity.IsValid && Entity.TryGetVehicleTelemetry(out telemetry); }

    /// <summary>Manual gearbox: one gear up (+1) or down (-1).</summary>
    public bool Shift(int delta) => Entity.IsValid && Entity.ShiftVehicle(delta);

    /// <summary>Puts the car upright about its heading, stopped, <paramref name="liftCm"/> higher.</summary>
    public bool Flip(float liftCm = 50f) => Entity.IsValid && Entity.FlipVehicle(liftCm);

    /// <summary>Puts <paramref name="driver"/> in the seat (see <see cref="Entity.EnterVehicle"/>).</summary>
    public bool Enter(Entity driver) => Entity.IsValid && Entity.EnterVehicle(driver);

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
        Vehicles.Forget(Handle);
        if (Entity.IsValid && Entity.IsAlive) Entity.Destroy();   // the system destroys the vehicle with its entity at its next tick ...
        return Vehicle.Destroy();                                  // ... and this does it now
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

/// <summary>Builds cars. Needs the physics library and the host's vehicle system; without them <see cref="CreateCar"/> answers
/// <see cref="Car.None"/>.</summary>
/// <remarks>Cars made here are tracked by their vehicle handle (<see cref="TryGet"/>), which is how Aver Node graphs hold one. The
/// vehicle system writes each car's pose onto its entity every frame in C++ (a physics body stamped with an entity does not move that
/// entity by itself, so something has to; it is no longer a script timer). An entity destroyed during play takes its car with it.</remarks>
public static class Vehicles
{
    private const float Gravity = 981f;                 // cm/s^2

    private static readonly Dictionary<int, Car> s_cars = new();

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

    /// <summary>Makes the car as .ocvehicle text (<see cref="CarSpec.ToOcVehicle"/>) through the vehicle system, which builds the chassis, four
    /// wheels, engine and simulation and keeps the entity at the car's pose. <see cref="Car.None"/> when the host has no vehicle system, the
    /// physics layer refuses the numbers or it is not loaded.</summary>
    public static Car CreateCar(Vec3 position, float yawDegrees, CarSpec spec)
    {
        Entity e = SpawnFromText(spec.ToOcVehicle(), position, yawDegrees, out int vehicle);
        return e.IsValid && s_cars.TryGetValue(vehicle, out Car car) ? car : Car.None;
    }

    /// <summary>Makes a vehicle entity from an .ocvehicle content path or a built-in preset name ("car", "van", "truck", "bus", "sports",
    /// "tank") at a pose, built now. The entity has no mesh: give it children for its wheels and a mesh for its body, or use a level
    /// placement with a Vehicle component. Needs the vehicle system in the host; <c>default</c> when it is missing or the spec is refused.</summary>
    public static Entity Spawn(string assetOrPreset, Vec3 position, float yawDegrees = 0f) =>
        SpawnCore(assetOrPreset, position, yawDegrees, out _);

    /// <summary>The same from inline .ocvehicle text (it begins <c>OCVEHICLE 1</c>).</summary>
    public static Entity SpawnFromText(string ocvehicleText, Vec3 position, float yawDegrees = 0f) =>
        SpawnCore(ocvehicleText, position, yawDegrees, out _);

    internal static Entity SpawnFromText(string ocvehicleText, Vec3 position, float yawDegrees, out int vehicle) =>
        SpawnCore(ocvehicleText, position, yawDegrees, out vehicle);

    private static Entity SpawnCore(string text, Vec3 position, float yawDegrees, out int vehicle)
    {
        vehicle = 0;
        if (string.IsNullOrEmpty(text)) return default;
        try
        {
            if (FwVehicle.aver_fw_vehicle_spawn(text, position.X, position.Y, position.Z, yawDegrees, out int entity, out vehicle) == 0) return default;
            var e = new Entity(entity);
            // So the older rows, which hold a vehicle by its physics handle, find it.
            s_cars[vehicle] = new Car(new AP.Vehicle(vehicle), e);
            return e;
        }
        catch (Exception ex) when (IsMissingNative(ex)) { return default; }
    }

    /// <summary>Takes up to <paramref name="buffer"/>.Length pending vehicle events (skids, landings, impacts, shifts, crashes, flips,
    /// getting in and out) off the queue, oldest first, and returns how many. One consumer should own the queue.</summary>
    public static int DrainEvents(Span<VehicleEvent> buffer)
    {
        if (buffer.Length == 0) return 0;
        var ints = new int[buffer.Length * FwVehicle.EventInts];
        var floats = new float[buffer.Length * FwVehicle.EventFloats];
        int n;
        try { n = FwVehicle.aver_fw_vehicle_drain_events(ints, floats, buffer.Length); }
        catch (Exception ex) when (IsMissingNative(ex)) { return 0; }
        n = Math.Clamp(n, 0, buffer.Length);
        for (int k = 0; k < n; k++)
        {
            int i = k * FwVehicle.EventInts, f = k * FwVehicle.EventFloats;
            buffer[k] = new VehicleEvent((VehicleEventKind)ints[i], new Entity(ints[i + 1]), ints[i + 2], ints[i + 3],
                new Vec3(floats[f], floats[f + 1], floats[f + 2]), new Vec3(floats[f + 3], floats[f + 4], floats[f + 5]),
                floats[f + 6], floats[f + 7], ints[i + 4]);
        }
        return n;
    }

    /// <summary>The car made by <see cref="CreateCar"/> (or <see cref="Spawn"/>) with this vehicle handle, if it still exists.</summary>
    public static bool TryGet(int vehicleHandle, out Car car) => s_cars.TryGetValue(vehicleHandle, out car);

    /// <summary>Forgets cars whose entity died or whose physics world was reset. The system now keeps the entities at their vehicles'
    /// poses, so this no longer writes anything; it is kept for scripts that called it.</summary>
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
            if (!car.Vehicle.TryGetPose(out _, out _)) (gone ??= new()).Add(handle);   // the physics world was reset under it
        }
        if (gone != null)
            foreach (int h in gone) s_cars.Remove(h);
    }

    internal static void Forget(int vehicleHandle) => s_cars.Remove(vehicleHandle);

    /// <summary>Makes a vehicle the system built for a level placement findable by its physics handle, which is how the older rows hold one.</summary>
    internal static void Adopt(int vehicleHandle, Entity entity)
    {
        if (vehicleHandle != 0 && !s_cars.ContainsKey(vehicleHandle)) s_cars[vehicleHandle] = new Car(new AP.Vehicle(vehicleHandle), entity);
    }

    // No native library, or no entry point in it (an older build): the calls answer their empty result.
    private static bool IsMissingNative(Exception e) => e is DllNotFoundException or EntryPointNotFoundException;
}
