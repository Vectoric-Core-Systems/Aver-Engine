// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// VehicleBody3D and VehicleWheel3D on the G2 raycast vehicle (docs/physics/RAYCAST_VEHICLE.md): a chassis body with a ray per wheel,
// engine force, brake and steering spread over the wheels the way Godot's VehicleBody3D does it. Engine side; not run headless.

using Aver.Scripting;
using PhysBody = Aver.Physics.Body;
using PhysFloat3 = Aver.Physics.Float3;

namespace Aver.Fabric;

/// <summary>One wheel of a <see cref="VehicleBody3D"/> (Godot's VehicleWheel3D). Place it as a child of the body at the wheel's position; its
/// -Y axis is the suspension direction and its X axis the axle. The body moves this node with the suspension and the spin.</summary>
public class VehicleWheel3D : Node3D
{
    public bool UseAsTraction { get; set; }
    public bool UseAsSteering { get; set; }
    public float WheelRollInfluence { get; set; } = 0.1f;
    public float WheelRadius { get; set; } = 0.5f;
    public float WheelRestLength { get; set; } = 0.15f;
    public float WheelFrictionSlip { get; set; } = 10.5f;
    public float SuspensionTravel { get; set; } = 0.2f;
    public float SuspensionStiffness { get; set; } = 5.88f;
    public float SuspensionMaxForce { get; set; } = 6000f;
    public float DampingCompression { get; set; } = 0.83f;
    public float DampingRelaxation { get; set; } = 0.88f;

    private float _engineForce, _brake, _steering;

    /// <summary>Force driving this wheel in newtons (a body's EngineForce sets it on its traction wheels).</summary>
    public float EngineForce { get => _engineForce; set { _engineForce = value; Push(); } }

    public float Brake { get => _brake; set { _brake = value; Push(); } }

    /// <summary>Steering angle in radians, positive to the left.</summary>
    public float Steering { get => _steering; set { _steering = value; Push(); } }

    internal int Index = -1;
    internal Aver.Physics.RaycastVehicle Vehicle;
    internal Transform3D RestTransform = Transform3D.Identity;

    private void Push()
    {
        if (Index < 0 || !Vehicle.IsValid) return;
        try
        {
            Vehicle.SetEngineForce(Index, RaycastWheelGodot.EngineForceFromGodot(_engineForce));
            Vehicle.SetBrake(Index, RaycastWheelGodot.BrakeFromGodot(_brake, 1f / PhysicsDelta));
            Vehicle.SetSteering(Index, RaycastWheelGodot.SteeringFromGodot(_steering));
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { }
    }

    internal void PushDrive() => Push();

    private Aver.Physics.RaycastWheelState? State() => Index >= 0 && Vehicle.IsValid ? Vehicle.GetWheelState(Index) : null;

    public bool IsInContact() => State()?.InContact ?? false;
    public float GetRpm() => State()?.Rpm ?? 0f;
    /// <summary>1 when the tyre grips, less when it slides.</summary>
    public float GetSkidinfo() => State()?.SkidInfo ?? 1f;
    public Node3D? GetContactBody() => State() is { InContact: true } s ? CollisionObject3D.NodeOfBody(s.Ground) as Node3D : null;
    public Vector3 GetContactPoint() => State() is { } s ? FabricPhysics.FromPhysPosition(s.ContactPoint) : Vector3.Zero;
}

/// <summary>A car or truck (Godot's VehicleBody3D): a rigid body whose child <see cref="VehicleWheel3D"/> nodes hold it up and drive it.</summary>
public class VehicleBody3D : RigidBody3D
{
    private float _engineForce, _brake, _steering;
    private Aver.Physics.RaycastVehicle _vehicle;
    private readonly List<VehicleWheel3D> _wheels = new();

    public VehicleBody3D() { Mass = 40f; }

    /// <summary>Force in newtons on every wheel that is used as traction.</summary>
    public float EngineForce
    {
        get => _engineForce;
        set { _engineForce = value; foreach (VehicleWheel3D w in Wheels()) if (w.UseAsTraction) w.EngineForce = value; }
    }

    /// <summary>Braking, as an impulse per step, on every wheel.</summary>
    public float Brake
    {
        get => _brake;
        set { _brake = value; foreach (VehicleWheel3D w in Wheels()) w.Brake = value; }
    }

    /// <summary>Steering angle in radians (positive to the left) on every wheel that is used for steering.</summary>
    public float Steering
    {
        get => _steering;
        set { _steering = value; foreach (VehicleWheel3D w in Wheels()) if (w.UseAsSteering) w.Steering = value; }
    }

    private IEnumerable<VehicleWheel3D> Wheels() => ChildrenSnapshot().OfType<VehicleWheel3D>();

    internal override void OnBodyBuilt(PhysBody body)
    {
        base.OnBodyBuilt(body);
        _wheels.Clear();
        _vehicle = Aver.Physics.RaycastVehicle.Create(body);
        if (!_vehicle.IsValid)
        {
            Log.Warn($"[Fabric] VehicleBody3D '{Name}': the physics world refused a raycast vehicle (the body must be dynamic and have a shape)");
            return;
        }
        foreach (VehicleWheel3D w in Wheels())
        {
            Transform3D t = w.Transform;
            // Godot: the suspension runs along the wheel's -Y, the axle along its X, both in the body's frame.
            Vector3 down = -t.Basis.Y, axle = t.Basis.X;
            Aver.Physics.RaycastWheelSettings s = RaycastWheelGodot.FromGodot(
                ToFloat3(t.Origin), new PhysFloat3(down.X, down.Y, down.Z), new PhysFloat3(axle.X, axle.Y, axle.Z),
                w.WheelRestLength, w.SuspensionTravel, w.SuspensionStiffness,
                Aver.Physics.RaycastWheelSettings.DampingFromRatio(w.DampingCompression, w.SuspensionStiffness),
                Aver.Physics.RaycastWheelSettings.DampingFromRatio(w.DampingRelaxation, w.SuspensionStiffness),
                w.SuspensionMaxForce, w.WheelRadius, w.WheelFrictionSlip, w.WheelRollInfluence, w.UseAsTraction, w.UseAsSteering, CollisionMask);
            int index = _vehicle.AddWheel(s);
            if (index < 0) continue;
            w.Index = index;
            w.Vehicle = _vehicle;
            w.RestTransform = t;
            _wheels.Add(w);
            w.PushDrive();
        }
    }

    // The G2 mapper takes Godot's own axes (it does the axis map itself).
    private static PhysFloat3 ToFloat3(Vector3 v) => new(v.X, v.Y, v.Z);

    internal override void OnBodyDestroying(PhysBody body)
    {
        foreach (VehicleWheel3D w in _wheels) { w.Index = -1; w.Vehicle = default; }
        _wheels.Clear();
        _vehicle = default;
        base.OnBodyDestroying(body);
    }

    // Before the step the wheels get the last step's pose, so a mesh under a wheel node follows the suspension and the spin.
    internal override void OnPhysicsStep(float dt)
    {
        base.OnPhysicsStep(dt);
        if (!_vehicle.IsValid) return;
        Transform3D chassis = GlobalTransform.AffineInverse();
        foreach (VehicleWheel3D w in _wheels)
        {
            if (!_vehicle.TryGetWheelPose(w.Index, out PhysFloat3 pos, out Aver.Physics.Quaternion rot)) continue;
            Vector3 world = FabricPhysics.FromPhysPosition(pos);
            Quaternion q = FabricSpace.FromAverRotation(new Aver.Scene.Quat(rot.X, rot.Y, rot.Z, rot.W));
            Transform3D local = chassis * new Transform3D(new Basis(q), world);
            w.Transform = new Transform3D(local.Basis * w.RestTransform.Basis, local.Origin);
        }
    }
}
