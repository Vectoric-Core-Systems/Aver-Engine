// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The collision node classes: CollisionObject3D and its bodies and Area3D, on the G2 physics layer (docs/physics/GODOT_*.md).
// The body is built when the node becomes ready, from its CollisionShape3D children. Numbers at the script boundary are Godot's
// (metres, kilograms, newtons); FabricPhysics converts. The engine-facing parts are not run by the headless tests (the native physics
// library cannot be loaded beside the managed binding of the same name); the signal routing is (FabricPhysics.Dispatch*).

using Aver.Scripting;
using PhysBody = Aver.Physics.Body;
using PhysFloat3 = Aver.Physics.Float3;

namespace Aver.Fabric;

/// <summary>Friction, bounce and the combine flags of a body's surface (Godot's PhysicsMaterial).</summary>
public class PhysicsMaterial : Resource
{
    public float Friction { get; set; } = 1f;
    public bool Rough { get; set; }
    public float Bounce { get; set; }
    public bool Absorbent { get; set; }
}

/// <summary>Base of every node that takes part in physics (Godot's CollisionObject3D).</summary>
public class CollisionObject3D : Node3D
{
    public new class SignalName : Node3D.SignalName
    {
        public static readonly StringName InputEvent = "input_event";
        public static readonly StringName MouseEntered = "mouse_entered";
        public static readonly StringName MouseExited = "mouse_exited";
    }

    private PhysBody _body;
    private uint _layer = 1, _mask = 1;
    private bool _rebuildQueued;
    private readonly List<CollisionObject3D> _exceptions = new();
    private Transform3D _lastSynced;
    private bool _hasSynced;

    /// <summary>Layers this object is in, one bit per layer (layer 1 is bit 0).</summary>
    public uint CollisionLayer { get => _layer; set { _layer = value; PushFilter(); } }

    /// <summary>Layers this object collides with or detects.</summary>
    public uint CollisionMask { get => _mask; set { _mask = value; PushFilter(); } }

    public bool InputRayPickable { get; set; } = true;

    public bool GetCollisionLayerValue(int layerNumber) => (CollisionLayer & (1u << (layerNumber - 1))) != 0;
    public void SetCollisionLayerValue(int layerNumber, bool value) =>
        CollisionLayer = value ? CollisionLayer | (1u << (layerNumber - 1)) : CollisionLayer & ~(1u << (layerNumber - 1));
    public bool GetCollisionMaskValue(int layerNumber) => (CollisionMask & (1u << (layerNumber - 1))) != 0;
    public void SetCollisionMaskValue(int layerNumber, bool value) =>
        CollisionMask = value ? CollisionMask | (1u << (layerNumber - 1)) : CollisionMask & ~(1u << (layerNumber - 1));

    /// <summary>The object's id as a physics resource id (what query exclude lists and the physics server take).</summary>
    public Rid GetRid() => new(_body.IsValid ? (ulong)_body.Handle : 0UL);

    /// <summary>Stops this object colliding with another one (Godot's add_collision_exception_with).</summary>
    public void AddCollisionExceptionWith(Node body)
    {
        if (body is not CollisionObject3D other || _exceptions.Contains(other)) return;
        _exceptions.Add(other);
        if (_body.IsValid && other._body.IsValid) _body.AddCollisionException(other._body);
    }

    public void RemoveCollisionExceptionWith(Node body)
    {
        if (body is not CollisionObject3D other || !_exceptions.Remove(other)) return;
        if (_body.IsValid && other._body.IsValid) _body.RemoveCollisionException(other._body);
    }

    /// <summary>The Aver physics body behind this node, once it is ready in the engine.</summary>
    internal PhysBody Body => _body;

    internal override bool UsesInternalPhysicsProcess => true;

    internal virtual Aver.Physics.MotionType Motion => Aver.Physics.MotionType.Static;
    internal virtual float BodyMassKg => 0f;
    internal virtual bool IsSensor => false;

    internal override void OnReadying()
    {
        base.OnReadying();
        if (Self.IsValid && NodeHost.Current.IsEngine) BuildBody();
    }

    internal override void OnExitingTree()
    {
        base.OnExitingTree();
        DestroyBody();
    }

    private void BuildBody()
    {
        try
        {
            _body = FabricPhysics.CreateBody(this, Motion, BodyMassKg, IsSensor);
            if (!_body.IsValid) return;
            _body.SetEntity(Self.Handle);
            _body.SetCollisionFilter(_layer, _mask);
            FabricPhysics.Register(_body.Handle, this);
            foreach (CollisionObject3D other in _exceptions) if (other._body.IsValid) _body.AddCollisionException(other._body);
            _lastSynced = GlobalTransform;
            _hasSynced = true;
            OnBodyBuilt(_body);
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException)
        {
            Log.Warn($"[Fabric] {GetClass()} '{Name}' has no physics body: the physics library is not loaded");
        }
    }

    internal virtual void OnBodyBuilt(PhysBody body) { }

    private void DestroyBody()
    {
        if (!_body.IsValid) return;
        FabricPhysics.Unregister(_body.Handle);
        OnBodyDestroying(_body);
        try { _body.Destroy(); }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { }
        _body = default;
    }

    internal virtual void OnBodyDestroying(PhysBody body) { }

    /// <summary>Makes the body again from the node's current shapes, at the end of the pass (a shape was added, changed or disabled).</summary>
    internal void QueueRebuild()
    {
        if (_rebuildQueued || !_body.IsValid) return;
        _rebuildQueued = true;
        Aver.Framework.Deferred.Call(() =>
        {
            _rebuildQueued = false;
            if (!IsAlive || !_body.IsValid) return;
            DestroyBody();
            if (IsInsideTree()) BuildBody();
        });
    }

    private void PushFilter()
    {
        if (!_body.IsValid) return;
        try { _body.SetCollisionFilter(_layer, _mask); OnFilterChanged(); }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { }
    }

    internal virtual void OnFilterChanged() { }

    internal override void InternalPhysicsProcess(double delta)
    {
        if (!_body.IsValid) return;
        FabricPhysics.Pump();
        Transform3D now = GlobalTransform;
        if (!_hasSynced || !now.Equals(_lastSynced))
        {
            _lastSynced = now;
            _hasSynced = true;
            OnNodeMoved(now, (float)delta);
        }
        OnPhysicsStep((float)delta);
    }

    // The node was moved by script or animation since the last step: bodies that do not simulate themselves follow it.
    internal virtual void OnNodeMoved(Transform3D global, float dt) { }

    // Once per physics step, before the step runs.
    internal virtual void OnPhysicsStep(float dt) { }

    // Moves the body so its shape origin is at the node's pose (a body's position is its centre of mass, which an offset shape differs from).
    internal static void PlaceBody(PhysBody body, Transform3D global, bool carry, float dt)
    {
        PhysFloat3 origin = FabricPhysics.ToPhysPosition(global.Origin);
        Aver.Physics.Quaternion rot = FabricPhysics.ToPhysRotation(global.Basis.Orthonormalized().GetRotationQuaternion());
        PhysFloat3 delta = origin - body.Origin;
        PhysFloat3 com = body.Position + delta;
        if (carry && dt > 0f) body.MoveKinematic(com, rot, dt);
        else { body.SetPosition(com); body.SetRotation(rot); }
    }

    internal static PhysFloat3 ToPhysDirection(Vector3 v, float scale = 1f)
    {
        Aver.Scene.Vec3 a = FabricSpace.ToAverDirection(v);
        return new PhysFloat3(a.X * scale, a.Y * scale, a.Z * scale);
    }

    internal static Vector3 FromPhys(PhysFloat3 v, float scale = 1f) =>
        FabricSpace.FromAverDirection(new Aver.Scene.Vec3(v.X, v.Y, v.Z)) * scale;

    // Angular velocity, torque and angular impulse are pseudovectors: they take the rotation map's sign (see FabricSpace.ToAverAngular).
    internal static PhysFloat3 ToPhysAngular(Vector3 w, float scale = 1f)
    {
        Aver.Scene.Vec3 a = FabricSpace.ToAverAngular(w);
        return new PhysFloat3(a.X * scale, a.Y * scale, a.Z * scale);
    }

    internal static Vector3 FromPhysAngular(PhysFloat3 w, float scale = 1f) =>
        FabricSpace.FromAverAngular(new Aver.Scene.Vec3(w.X, w.Y, w.Z)) * scale;

    /// <summary>The node of a body handle, or null.</summary>
    internal static Node? NodeOfBody(PhysBody b)
    {
        if (!b.IsValid) return null;
        return FabricPhysics.NodeOf(b.Handle);
    }
}

public class PhysicsBody3D : CollisionObject3D
{
    private PhysicsMaterial? _material;

    /// <summary>The gravity acting on this body in metres per second squared: the world's plus the gravity areas it is in, before its gravity scale
    /// (Godot 4.3's get_gravity). Before the body exists in the engine, the project's default gravity.</summary>
    public Vector3 GetGravity()
    {
        if (Body.IsValid) return FromPhys(Body.AreaGravity, 1f / FabricSpace.CmPerMetre);
        double g = ProjectSettings.GetSetting("physics/3d/default_gravity", 9.8).AsDouble();
        return ProjectSettings.GetSetting("physics/3d/default_gravity_vector", Vector3.Down).AsVector3() * (float)g;
    }

    /// <summary>The surface the body has instead of Godot's defaults (friction 1, bounce 0).</summary>
    public PhysicsMaterial? PhysicsMaterialOverride
    {
        get => _material;
        set { _material = value; if (Body.IsValid) ApplySurface(Body); }
    }

    public bool AxisLockLinearX { get; set; }
    public bool AxisLockLinearY { get; set; }
    public bool AxisLockLinearZ { get; set; }
    public bool AxisLockAngularX { get; set; }
    public bool AxisLockAngularY { get; set; }
    public bool AxisLockAngularZ { get; set; }

    internal void ApplySurface(PhysBody body)
    {
        // Godot's own defaults apply with the Godot combine rule; with Jolt's, the body keeps Aver's.
        if (_material == null && Aver.Physics.Physics.CombineRule != Aver.Physics.CombineMode.Godot) return;
        float friction = _material?.Friction ?? 1f, bounce = _material?.Bounce ?? 0f;
        body.SetFriction(friction);
        body.SetRestitution(bounce);
        body.SetCombineFlags((_material is { Rough: true } ? Aver.Physics.CombineFlags.Rough : 0) | (_material is { Absorbent: true } ? Aver.Physics.CombineFlags.Absorbent : 0));
    }

    internal override void OnBodyBuilt(PhysBody body)
    {
        base.OnBodyBuilt(body);
        ApplySurface(body);
    }

    /// <summary>Stops this body from moving along a linear axis or turning about an angular one (the axis lock flags, as a body-level rule).</summary>
    internal bool AnyAxisLock => AxisLockLinearX || AxisLockLinearY || AxisLockLinearZ || AxisLockAngularX || AxisLockAngularY || AxisLockAngularZ;

    /// <summary>Zeroes the locked components of a body's velocity (Godot space), so the body cannot gain motion along or about them.</summary>
    internal void EnforceAxisLocks(PhysBody body)
    {
        if (!AnyAxisLock) return;
        Vector3 v = FromPhys(body.Velocity, 1f / FabricSpace.CmPerMetre), w = FromPhysAngular(body.AngularVelocity);
        if (AxisLockLinearX) v.X = 0; if (AxisLockLinearY) v.Y = 0; if (AxisLockLinearZ) v.Z = 0;
        if (AxisLockAngularX) w.X = 0; if (AxisLockAngularY) w.Y = 0; if (AxisLockAngularZ) w.Z = 0;
        body.SetVelocity(ToPhysDirection(v, FabricSpace.CmPerMetre));
        body.SetAngularVelocity(ToPhysAngular(w));
    }
}

/// <summary>A body that does not move by itself (Godot's StaticBody3D).</summary>
public class StaticBody3D : PhysicsBody3D
{
    private Vector3 _constLinear, _constAngular;

    /// <summary>A surface speed in metres per second: bodies touching this one are carried along it (a conveyor).</summary>
    public Vector3 ConstantLinearVelocity { get => _constLinear; set { _constLinear = value; PushConstants(); } }

    public Vector3 ConstantAngularVelocity { get => _constAngular; set { _constAngular = value; PushConstants(); } }

    private void PushConstants()
    {
        if (!Body.IsValid) return;
        Body.SetConstantVelocity(ToPhysDirection(_constLinear, FabricSpace.CmPerMetre), ToPhysAngular(_constAngular));
    }

    internal override void OnBodyBuilt(PhysBody body)
    {
        base.OnBodyBuilt(body);
        if (_constLinear != Vector3.Zero || _constAngular != Vector3.Zero) PushConstants();
    }

    internal override void OnNodeMoved(Transform3D global, float dt) => PlaceBody(Body, global, carry: false, dt);
}

/// <summary>A body moved by script or animation that pushes others (Godot's AnimatableBody3D).</summary>
public class AnimatableBody3D : StaticBody3D
{
    /// <summary>When true the body follows its node through the physics step, so what rests on it is carried.</summary>
    public bool SyncToPhysics { get; set; }
    internal override Aver.Physics.MotionType Motion => Aver.Physics.MotionType.Kinematic;

    internal override void OnNodeMoved(Transform3D global, float dt) => PlaceBody(Body, global, carry: SyncToPhysics, dt);
}

/// <summary>What a rigid body's _IntegrateForces sees and can change (Godot's PhysicsDirectBodyState3D). Metres and kilograms.</summary>
public class PhysicsDirectBodyState3D : GodotObject
{
    private float _dt, _invMass;
    private Vector3 _linear, _angular, _gravity;
    private Transform3D _transform;
    private int _contacts;
    private readonly Aver.Physics.Float3[] _invInertia = new Aver.Physics.Float3[3];

    internal bool VelocityChanged;

    /// <summary>The length of this step in seconds.</summary>
    public float Step => _dt;

    public float InverseMass => _invMass;
    public Vector3 TotalGravity => _gravity;

    public Vector3 LinearVelocity { get => _linear; set { _linear = value; VelocityChanged = true; } }
    public Vector3 AngularVelocity { get => _angular; set { _angular = value; VelocityChanged = true; } }

    /// <summary>The body's pose this step. Read only: the engine takes velocities back from the callback, not a pose.</summary>
    public Transform3D Transform => _transform;

    public int GetContactCount() => _contacts;

    // ---- per-contact reads: the body's tracked contacts (ContactMonitor on, MaxContactsReported above 0), in ascending partner order ----
    // The engine tracks the partner, the point where the touch began and the normal, nothing per shape and no impulse, so the shape indices and
    // GetContactImpulse are not offered. Both positions are that one point.

    internal RigidBody3D? Owner;

    private bool Contact(int index, out Aver.Physics.Body other, out Vector3 point, out Vector3 normal)
    {
        other = default; point = default; normal = default;
        if (Owner == null || !Owner.Body.IsValid || index < 0 || index >= _contacts) return false;
        if (!Owner.Body.TryGetContactAt(index, out other, out Aver.Physics.Float3 p, out Aver.Physics.Float3 n)) return false;
        point = FabricSpace.FromAverPosition(new Aver.Scene.Vec3(p.X, p.Y, p.Z));
        // The engine's normal runs from this body to the partner; Godot's local normal points from the partner toward this body.
        normal = -CollisionObject3D.FromPhys(n);
        return true;
    }

    /// <summary>The node this body touches in contact <paramref name="contactIdx"/>, or null.</summary>
    public GodotObject? GetContactColliderObject(int contactIdx) => Contact(contactIdx, out var o, out _, out _) ? CollisionObject3D.NodeOfBody(o) : null;

    public ulong GetContactColliderId(int contactIdx) => GetContactColliderObject(contactIdx)?.GetInstanceId() ?? 0UL;

    public Rid GetContactColliderRid(int contactIdx) => Contact(contactIdx, out var o, out _, out _) ? new Rid((ulong)o.Handle) : default;

    /// <summary>Where the touch began, in global coordinates (metres).</summary>
    public Vector3 GetContactLocalPosition(int contactIdx) => Contact(contactIdx, out _, out var p, out _) ? p : Vector3.Zero;

    /// <summary>The same point: the engine does not give the two bodies' surface points apart.</summary>
    public Vector3 GetContactColliderPosition(int contactIdx) => GetContactLocalPosition(contactIdx);

    /// <summary>The contact normal in global axes, pointing from the other body toward this one.</summary>
    public Vector3 GetContactLocalNormal(int contactIdx) => Contact(contactIdx, out _, out _, out var n) ? n : Vector3.Zero;

    /// <summary>This body's velocity at the contact point (metres per second).</summary>
    public Vector3 GetContactLocalVelocityAtPosition(int contactIdx) =>
        Contact(contactIdx, out _, out var p, out _) ? _linear + _angular.Cross(p - _transform.Origin) : Vector3.Zero;

    /// <summary>The other body's velocity at the contact point (metres per second).</summary>
    public Vector3 GetContactColliderVelocityAtPosition(int contactIdx)
    {
        if (!Contact(contactIdx, out var o, out var p, out _) || !o.IsValid) return Vector3.Zero;
        Vector3 centre = FabricSpace.FromAverPosition(new Aver.Scene.Vec3(o.Position.X, o.Position.Y, o.Position.Z));
        return CollisionObject3D.FromPhys(o.Velocity, 1f / FabricSpace.CmPerMetre) + CollisionObject3D.FromPhysAngular(o.AngularVelocity).Cross(p - centre);
    }

    public void ApplyCentralImpulse(Vector3 impulse) => LinearVelocity = _linear + impulse * _invMass;
    public void ApplyCentralForce(Vector3 force) => LinearVelocity = _linear + force * (_invMass * _dt);
    public void ApplyImpulse(Vector3 impulse, Vector3 position = default)
    {
        ApplyCentralImpulse(impulse);
        ApplyTorqueImpulse(position.Cross(impulse));
    }
    public void ApplyForce(Vector3 force, Vector3 position = default)
    {
        ApplyCentralForce(force);
        ApplyTorque(position.Cross(force));
    }
    public void ApplyTorqueImpulse(Vector3 impulse)
    {
        // The inverse inertia is held in Aver's axes (kg^-1 cm^-2): take the impulse there, apply it, bring the change back.
        Aver.Scene.Vec3 t = FabricSpace.ToAverAngular(impulse * (FabricSpace.CmPerMetre * FabricSpace.CmPerMetre));
        var dw = new Aver.Physics.Float3(
            _invInertia[0].X * t.X + _invInertia[0].Y * t.Y + _invInertia[0].Z * t.Z,
            _invInertia[1].X * t.X + _invInertia[1].Y * t.Y + _invInertia[1].Z * t.Z,
            _invInertia[2].X * t.X + _invInertia[2].Y * t.Y + _invInertia[2].Z * t.Z);
        AngularVelocity = _angular + CollisionObject3D.FromPhysAngular(dw);
    }
    public void ApplyTorque(Vector3 torque) => ApplyTorqueImpulse(torque * _dt);
    public void AddConstantCentralForce(Vector3 force) => ApplyCentralForce(force);

    internal void Load(ref Aver.Physics.DirectBodyState s)
    {
        _dt = s.Dt;
        _invMass = s.InverseMass;
        _linear = CollisionObject3D.FromPhys(s.LinearVelocity, 1f / FabricSpace.CmPerMetre);
        _angular = CollisionObject3D.FromPhysAngular(s.AngularVelocity);
        _gravity = CollisionObject3D.FromPhys(s.TotalGravity, 1f / FabricSpace.CmPerMetre);
        _contacts = s.ContactCount;
        _invInertia[0] = s.InverseInertiaRow0; _invInertia[1] = s.InverseInertiaRow1; _invInertia[2] = s.InverseInertiaRow2;
        _transform = new Transform3D(Basis.FromQuaternionScale(FabricSpace.FromAverRotation(new Aver.Scene.Quat(s.Rotation.X, s.Rotation.Y, s.Rotation.Z, s.Rotation.W)), Vector3.One),
                                     FabricSpace.FromAverPosition(new Aver.Scene.Vec3(s.Position.X, s.Position.Y, s.Position.Z)));
        VelocityChanged = false;
    }

    internal void Store(ref Aver.Physics.DirectBodyState s)
    {
        if (!VelocityChanged) return;
        s.LinearVelocity = CollisionObject3D.ToPhysDirection(_linear, FabricSpace.CmPerMetre);
        s.AngularVelocity = CollisionObject3D.ToPhysAngular(_angular);
    }
}

/// <summary>A body the physics engine moves (Godot's RigidBody3D).</summary>
public class RigidBody3D : PhysicsBody3D
{
    public new class SignalName : CollisionObject3D.SignalName
    {
        public static readonly StringName BodyEntered = "body_entered";
        public static readonly StringName BodyExited = "body_exited";
        public static readonly StringName BodyShapeEntered = "body_shape_entered";
        public static readonly StringName BodyShapeExited = "body_shape_exited";
        public static readonly StringName SleepingStateChanged = "sleeping_state_changed";
    }

    public enum FreezeModeEnum { Static = 0, Kinematic = 1 }

    public event Action<Node> BodyEntered { add => Connect(SignalName.BodyEntered, Callable.From(value)); remove => Disconnect(SignalName.BodyEntered, Callable.From(value)); }
    public event Action<Node> BodyExited { add => Connect(SignalName.BodyExited, Callable.From(value)); remove => Disconnect(SignalName.BodyExited, Callable.From(value)); }
    public event Action SleepingStateChanged { add => Connect(SignalName.SleepingStateChanged, Callable.From(value)); remove => Disconnect(SignalName.SleepingStateChanged, Callable.From(value)); }

    private float _mass = 1f, _gravityScale = 1f, _linearDamp, _angularDamp;
    private bool _freeze, _contactMonitor, _lastActive = true, _customIntegrator;
    private FreezeModeEnum _freezeMode = FreezeModeEnum.Static;
    private int _maxContacts;
    private Vector3 _linearVelocity, _angularVelocity, _constantForce, _constantTorque;
    private readonly PhysicsDirectBodyState3D _state = new();
    private readonly bool _integrates;

    public RigidBody3D()
    {
        _integrates = System.Array.Exists(GetType().GetMethods(System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.Public),
            m => m.Name == nameof(_IntegrateForces) && m.GetBaseDefinition().DeclaringType == typeof(RigidBody3D) && m.DeclaringType != typeof(RigidBody3D));
        _state.Owner = this;
    }

    /// <summary>The mass in kilograms.</summary>
    public float Mass { get => _mass; set { _mass = value; if (Body.IsValid && value > 0f) Body.SetMass(value); } }

    public float GravityScale { get => _gravityScale; set { _gravityScale = value; if (Body.IsValid) Body.SetGravityFactor(value); } }
    public float LinearDamp { get => _linearDamp; set { _linearDamp = value; PushDamping(); } }
    public float AngularDamp { get => _angularDamp; set { _angularDamp = value; PushDamping(); } }
    public bool CanSleep { get; set; } = true;

    /// <summary>Holds the body in place (Static, or Kinematic to be moved by script and carry others) until unfrozen.</summary>
    public bool Freeze
    {
        get => _freeze;
        set { _freeze = value; PushMotion(); }
    }

    public FreezeModeEnum FreezeMode
    {
        get => _freezeMode;
        set { _freezeMode = value; PushMotion(); }
    }

    public bool ContactMonitor
    {
        get => _contactMonitor;
        set { _contactMonitor = value; PushContactMonitor(); }
    }

    public int MaxContactsReported { get => _maxContacts; set { _maxContacts = value; PushContactMonitor(); } }

    /// <summary>When true the engine's own integration of forces is off and _IntegrateForces moves the body.</summary>
    public bool CustomIntegrator { get => _customIntegrator; set { _customIntegrator = value; if (Body.IsValid) Body.SetCustomIntegrator(value); } }

    /// <summary>A force applied to the centre of the body every step, in newtons.</summary>
    public Vector3 ConstantForce { get => _constantForce; set => _constantForce = value; }

    /// <summary>A torque applied every step, in newton metres.</summary>
    public Vector3 ConstantTorque { get => _constantTorque; set => _constantTorque = value; }

    internal override Aver.Physics.MotionType Motion =>
        _freeze ? (_freezeMode == FreezeModeEnum.Kinematic ? Aver.Physics.MotionType.Kinematic : Aver.Physics.MotionType.Static) : Aver.Physics.MotionType.Dynamic;
    internal override float BodyMassKg => _mass;

    /// <summary>Velocity in metres per second.</summary>
    public Vector3 LinearVelocity
    {
        get => Body.IsValid ? FromPhys(Body.Velocity, 1f / FabricSpace.CmPerMetre) : _linearVelocity;
        set { _linearVelocity = value; if (Body.IsValid) Body.SetVelocity(ToPhysDirection(value, FabricSpace.CmPerMetre)); }
    }

    /// <summary>Spin in radians per second about each axis.</summary>
    public Vector3 AngularVelocity
    {
        get => Body.IsValid ? FromPhysAngular(Body.AngularVelocity) : _angularVelocity;
        set { _angularVelocity = value; if (Body.IsValid) Body.SetAngularVelocity(ToPhysAngular(value)); }
    }

    public void ApplyCentralImpulse(Vector3 impulse) { if (Body.IsValid) Body.AddImpulse(ToPhysDirection(impulse, FabricSpace.CmPerMetre)); }

    /// <summary>An impulse (newton seconds) at a point given as an offset from the body's origin in global axes.</summary>
    public void ApplyImpulse(Vector3 impulse, Vector3 position = default)
    {
        if (!Body.IsValid) return;
        Aver.Scene.Vec3 p = FabricSpace.ToAverPosition(GlobalPosition + position);
        Body.AddImpulseAt(ToPhysDirection(impulse, FabricSpace.CmPerMetre), new PhysFloat3(p.X, p.Y, p.Z));
    }

    public void ApplyTorqueImpulse(Vector3 impulse) { if (Body.IsValid) Body.AddAngularImpulse(ToPhysAngular(impulse, FabricSpace.CmPerMetre * FabricSpace.CmPerMetre)); }
    public void ApplyCentralForce(Vector3 force) { if (Body.IsValid) Body.AddForce(ToPhysDirection(force, FabricSpace.CmPerMetre)); }
    public void ApplyTorque(Vector3 torque) { if (Body.IsValid) Body.AddTorque(ToPhysAngular(torque, FabricSpace.CmPerMetre * FabricSpace.CmPerMetre)); }

    public void ApplyForce(Vector3 force, Vector3 position = default)
    {
        if (!Body.IsValid) return;
        Aver.Scene.Vec3 p = FabricSpace.ToAverPosition(GlobalPosition + position);
        Body.AddForceAt(ToPhysDirection(force, FabricSpace.CmPerMetre), new PhysFloat3(p.X, p.Y, p.Z));
    }

    /// <summary>Adds to the force applied every step (Godot's add_constant_force); the position is an offset from the origin.</summary>
    public void AddConstantCentralForce(Vector3 force) => _constantForce += force;
    public void AddConstantForce(Vector3 force, Vector3 position = default) { _constantForce += force; _constantTorque += position.Cross(force); }
    public void AddConstantTorque(Vector3 torque) => _constantTorque += torque;

    public bool Sleeping
    {
        get => Body.IsValid && !Body.IsActive;
        set { if (Body.IsValid) { if (value) Body.Deactivate(); else Body.Activate(); } }
    }

    public int GetContactCount() => Body.IsValid && _contactMonitor ? Body.Contacts.Count : 0;

    /// <summary>The bodies this one touches now (needs ContactMonitor).</summary>
    public Collections.Array<Node3D> GetCollidingBodies()
    {
        var list = new Collections.Array<Node3D>();
        if (!Body.IsValid || !_contactMonitor) return list;
        foreach (PhysBody b in Body.Contacts)
            if (NodeOfBody(b) is Node3D n) list.Add(n);
        return list;
    }

    /// <summary>Override to move the body yourself: called each physics step with the body's state, before the engine integrates it.</summary>
    public virtual void _IntegrateForces(PhysicsDirectBodyState3D state) { }

    private void PushDamping() { if (Body.IsValid) Body.SetDamping(_linearDamp, _angularDamp); }

    private void PushMotion()
    {
        if (!Body.IsValid) return;
        Body.SetMotionType(Motion);
    }

    private void PushContactMonitor()
    {
        if (!Body.IsValid) return;
        Body.SetContactMonitor(_contactMonitor, Math.Max(_maxContacts, 1));
    }

    internal override void OnBodyBuilt(PhysBody body)
    {
        base.OnBodyBuilt(body);
        body.SetGravityFactor(_gravityScale);
        body.SetDamping(_linearDamp, _angularDamp);
        if (_linearVelocity != Vector3.Zero) body.SetVelocity(ToPhysDirection(_linearVelocity, FabricSpace.CmPerMetre));
        if (_angularVelocity != Vector3.Zero) body.SetAngularVelocity(ToPhysAngular(_angularVelocity));
        if (_contactMonitor) body.SetContactMonitor(true, Math.Max(_maxContacts, 1));
        if (_customIntegrator) body.SetCustomIntegrator(true);
        if (_integrates) body.SetIntegrateForces(IntegrateForcesCallback);
        _lastActive = body.IsActive;
    }

    internal override void OnBodyDestroying(PhysBody body)
    {
        // The state it had goes into the node, so a body made again (a shape changed) carries on.
        _linearVelocity = FromPhys(body.Velocity, 1f / FabricSpace.CmPerMetre);
        _angularVelocity = FromPhysAngular(body.AngularVelocity);
        base.OnBodyDestroying(body);
    }

    private void IntegrateForcesCallback(ref Aver.Physics.DirectBodyState s)
    {
        _state.Load(ref s);
        try { _IntegrateForces(_state); }
        catch (Exception ex) { Log.Error($"[Fabric] {GetClass()} '{Name}': _IntegrateForces threw: {ex.GetType().Name}: {ex.Message}"); }
        _state.Store(ref s);
    }

    internal override void OnPhysicsStep(float dt)
    {
        base.OnPhysicsStep(dt);
        PhysBody b = Body;
        if (_freeze) return;
        if (_constantForce != Vector3.Zero) b.AddForce(ToPhysDirection(_constantForce, FabricSpace.CmPerMetre));
        if (_constantTorque != Vector3.Zero) b.AddTorque(ToPhysAngular(_constantTorque, FabricSpace.CmPerMetre * FabricSpace.CmPerMetre));
        EnforceAxisLocks(b);
        bool active = b.IsActive;
        if (active != _lastActive)
        {
            _lastActive = active;
            EmitSignal(SignalName.SleepingStateChanged);
        }
    }

    // A frozen body follows its node (a kinematic one carries others); an unfrozen one is the engine's.
    internal override void OnNodeMoved(Transform3D global, float dt)
    {
        if (_freeze) PlaceBody(Body, global, carry: _freezeMode == FreezeModeEnum.Kinematic, dt);
    }

    // Called by FabricPhysics for a contact-monitor record on this body.
    internal void RaiseContact(CollisionObject3D other, bool began)
    {
        if (!ContactMonitor) return;
        EmitSignal(began ? SignalName.BodyEntered : SignalName.BodyExited, other);
    }
}

/// <summary>What a move or slide ran into (Godot's KinematicCollision3D). Metres.</summary>
public class KinematicCollision3D : RefCounted
{
    internal sealed record Hit(Vector3 Position, Vector3 Normal, GodotObject? Collider, ulong ColliderId, Vector3 ColliderVelocity, int ShapeIndex, float Depth);

    internal readonly List<Hit> Hits = new();
    internal Vector3 TravelVector, RemainderVector;

    public Vector3 GetTravel() => TravelVector;
    public Vector3 GetRemainder() => RemainderVector;
    public int GetCollisionCount() => Hits.Count;
    public Vector3 GetPosition(int collisionIndex = 0) => At(collisionIndex)?.Position ?? Vector3.Zero;
    public Vector3 GetNormal(int collisionIndex = 0) => At(collisionIndex)?.Normal ?? Vector3.Zero;
    public GodotObject? GetCollider(int collisionIndex = 0) => At(collisionIndex)?.Collider;
    public ulong GetColliderId(int collisionIndex = 0) => At(collisionIndex)?.ColliderId ?? 0;
    public Vector3 GetColliderVelocity(int collisionIndex = 0) => At(collisionIndex)?.ColliderVelocity ?? Vector3.Zero;
    public int GetColliderShapeIndex(int collisionIndex = 0) => At(collisionIndex)?.ShapeIndex ?? 0;
    public float GetDepth() => Hits.Count > 0 ? Hits[0].Depth : 0f;

    /// <summary>The angle between the surface and <paramref name="upDirection"/> (up by default), in radians.</summary>
    public float GetAngle(Vector3? upDirection = null, int collisionIndex = 0)
    {
        Vector3 up = upDirection ?? Vector3.Up;
        Hit? h = At(collisionIndex);
        return h == null ? 0f : MathF.Acos(Math.Clamp(h.Normal.Dot(up), -1f, 1f));
    }

    private Hit? At(int i) => i >= 0 && i < Hits.Count ? Hits[i] : null;
}

/// <summary>A body moved by script with MoveAndSlide (Godot's CharacterBody3D), on the G2 kinematic controller.</summary>
public class CharacterBody3D : PhysicsBody3D
{
    public enum MotionModeEnum { Grounded = 0, Floating = 1 }
    public enum PlatformOnLeaveEnum { AddVelocity = 0, AddUpwardVelocity = 1, DoNothing = 2 }

    private Aver.Physics.KinematicCharacter _kc;

    /// <summary>The velocity in metres per second; MoveAndSlide moves by it and leaves what remains after sliding.</summary>
    public Vector3 Velocity { get; set; }
    public Vector3 UpDirection { get; set; } = Vector3.Up;
    public MotionModeEnum MotionMode { get; set; } = MotionModeEnum.Grounded;
    public bool SlideOnCeiling { get; set; } = true;
    public int MaxSlides { get; set; } = 6;
    public float WallMinSlideAngle { get; set; } = 0.261799f;
    public bool FloorStopOnSlope { get; set; } = true;
    public bool FloorConstantSpeed { get; set; }
    public bool FloorBlockOnWall { get; set; } = true;
    public float FloorMaxAngle { get; set; } = 0.785398f;
    public float FloorSnapLength { get; set; } = 0.1f;
    public float SafeMargin { get; set; } = 0.001f;
    public PlatformOnLeaveEnum PlatformOnLeave { get; set; } = PlatformOnLeaveEnum.AddVelocity;
    public uint PlatformFloorLayers { get; set; } = uint.MaxValue;
    public uint PlatformWallLayers { get; set; }

    internal override Aver.Physics.MotionType Motion => Aver.Physics.MotionType.Kinematic;

    internal override void OnBodyBuilt(PhysBody body)
    {
        base.OnBodyBuilt(body);
        _kc = Aver.Physics.KinematicCharacter.Create(body, Options());
    }

    internal override void OnBodyDestroying(PhysBody body)
    {
        if (_kc.Handle != 0) { _kc.Destroy(); _kc = default; }
        base.OnBodyDestroying(body);
    }

    // A character is moved by MoveAndSlide, not by the step: a node moved by script is brought to the body in MoveAndSlide.
    internal override void OnNodeMoved(Transform3D global, float dt) { }

    private Aver.Physics.KinematicCharacterOptions Options()
    {
        Aver.Scene.Vec3 up = FabricSpace.ToAverDirection(UpDirection);
        return new Aver.Physics.KinematicCharacterOptions
        {
            MotionMode = MotionMode == MotionModeEnum.Floating ? Aver.Physics.KinematicMotionMode.Floating : Aver.Physics.KinematicMotionMode.Grounded,
            MaxSlides = MaxSlides,
            FloorMaxAngle = FloorMaxAngle,
            WallMinSlideAngle = WallMinSlideAngle,
            FloorSnapLengthCm = FabricSpace.ToAverLength(FloorSnapLength),
            SafeMarginCm = FabricSpace.ToAverLength(SafeMargin),
            FloorStopOnSlope = FloorStopOnSlope,
            FloorConstantSpeed = FloorConstantSpeed,
            FloorBlockOnWall = FloorBlockOnWall,
            SlideOnCeiling = SlideOnCeiling,
            PlatformOnLeave = (Aver.Physics.PlatformOnLeave)(int)PlatformOnLeave,
            PlatformFloorLayers = PlatformFloorLayers,
            PlatformWallLayers = PlatformWallLayers,
            UpDirection = new PhysFloat3(up.X, up.Y, up.Z),
        };
    }

    // The body goes to the node's pose (a script may have set GlobalPosition since the last move).
    private void BringBodyToNode()
    {
        Transform3D g = GlobalTransform;
        PhysFloat3 want = FabricPhysics.ToPhysPosition(g.Origin);
        PhysFloat3 have = _kc.Body.Origin;
        if (MathF.Abs(want.X - have.X) + MathF.Abs(want.Y - have.Y) + MathF.Abs(want.Z - have.Z) > 0.001f)
            PlaceBody(_kc.Body, g, carry: false, 0f);
    }

    private void TakeNodeFromBody()
    {
        PhysFloat3 o = _kc.Body.Origin;
        GlobalPosition = FabricPhysics.FromPhysPosition(o);
    }

    /// <summary>Moves by <see cref="Velocity"/> for one physics step, sliding along what it hits (Godot's move_and_slide). Returns true if it collided.</summary>
    public bool MoveAndSlide()
    {
        if (_kc.Handle == 0) return false;
        BringBodyToNode();
        _kc.Apply(Options());
        _kc.Velocity = ToPhysDirection(Velocity, FabricSpace.CmPerMetre);
        bool collided = _kc.MoveAndSlide(PhysicsDelta);
        Velocity = FromPhys(_kc.Velocity, 1f / FabricSpace.CmPerMetre);
        TakeNodeFromBody();
        return collided;
    }

    /// <summary>Moves by a vector without sliding and reports what stopped it (Godot's move_and_collide). With <paramref name="testOnly"/>
    /// the body stays where it is. Null when nothing was in the way.</summary>
    public KinematicCollision3D? MoveAndCollide(Vector3 motion, bool testOnly = false, float safeMargin = 0.001f, bool recoveryAsCollision = false, int maxCollisions = 1)
    {
        if (_kc.Handle == 0) return null;
        BringBodyToNode();
        var flags = Aver.Physics.SpaceQueryFlags.Bodies | (recoveryAsCollision ? Aver.Physics.SpaceQueryFlags.RecoveryAsCollision : 0);
        PhysFloat3 cm = ToPhysDirection(motion, FabricSpace.CmPerMetre);
        Aver.Physics.MotionTestResult? r = _kc.Body.TestMotion(cm, FabricSpace.ToAverLength(safeMargin), flags, null, Math.Max(maxCollisions, 1));
        if (r is not { } result) return null;
        if (!testOnly)
        {
            PhysFloat3 travel = result.Travel + result.Recovery;
            _kc.Body.SetPosition(_kc.Body.Position + travel);
            TakeNodeFromBody();
        }
        if (!result.Collided) return null;
        var hit = new KinematicCollision3D { TravelVector = FromPhys(result.Travel, 1f / FabricSpace.CmPerMetre), RemainderVector = FromPhys(result.Remainder, 1f / FabricSpace.CmPerMetre) };
        foreach (Aver.Physics.MotionContact c in result.Contacts)
        {
            GodotObject? collider = NodeOfBody(c.Collider);
            hit.Hits.Add(new KinematicCollision3D.Hit(FabricSpace.FromAverPosition(new Aver.Scene.Vec3(c.Point.X, c.Point.Y, c.Point.Z)), FromPhys(c.Normal), collider,
                                                      collider?.GetInstanceId() ?? 0, FromPhys(c.ColliderVelocity, 1f / FabricSpace.CmPerMetre), c.SubShape, c.Depth / FabricSpace.CmPerMetre));
        }
        return hit;
    }

    public bool IsOnFloor() => _kc.Handle != 0 && _kc.IsOnFloor;
    public bool IsOnFloorOnly() => _kc.Handle != 0 && _kc.IsOnFloorOnly;
    public bool IsOnWall() => _kc.Handle != 0 && _kc.IsOnWall;
    public bool IsOnWallOnly() => _kc.Handle != 0 && _kc.IsOnWallOnly;
    public bool IsOnCeiling() => _kc.Handle != 0 && _kc.IsOnCeiling;
    public bool IsOnCeilingOnly() => _kc.Handle != 0 && _kc.IsOnCeilingOnly;
    public Vector3 GetFloorNormal() => _kc.Handle != 0 ? FromPhys(_kc.GetFloorNormal()) : Vector3.Zero;
    public Vector3 GetWallNormal() => _kc.Handle != 0 ? FromPhys(_kc.GetWallNormal()) : Vector3.Zero;
    public Vector3 GetPlatformVelocity() => _kc.Handle != 0 ? FromPhys(_kc.GetPlatformVelocity(), 1f / FabricSpace.CmPerMetre) : Vector3.Zero;
    public Vector3 GetRealVelocity() => _kc.Handle != 0 ? FromPhys(_kc.GetRealVelocity(), 1f / FabricSpace.CmPerMetre) : Vector3.Zero;
    public Vector3 GetLastMotion() => _kc.Handle != 0 ? FromPhys(_kc.GetLastMotion(), 1f / FabricSpace.CmPerMetre) : Vector3.Zero;
    public Vector3 GetPositionDelta() => _kc.Handle != 0 ? FromPhys(_kc.GetPositionDelta(), 1f / FabricSpace.CmPerMetre) : Vector3.Zero;
    public float GetFloorAngle(Vector3? upDirection = null) => _kc.Handle != 0 ? _kc.GetFloorAngle() : 0f;
    public int GetSlideCollisionCount() => _kc.Handle != 0 ? _kc.GetSlideCollisionCount() : 0;

    /// <summary>One collision of the last MoveAndSlide, in the order they happened.</summary>
    public KinematicCollision3D? GetSlideCollision(int slideIdx)
    {
        if (_kc.Handle == 0 || _kc.GetSlideCollision(slideIdx) is not { } c) return null;
        return OneHit(c);
    }

    public KinematicCollision3D? GetLastSlideCollision()
    {
        if (_kc.Handle == 0 || _kc.GetLastSlideCollision() is not { } c) return null;
        return OneHit(c);
    }

    private static KinematicCollision3D OneHit(Aver.Physics.SlideCollision c)
    {
        GodotObject? collider = NodeOfBody(c.Collider);
        var k = new KinematicCollision3D
        {
            TravelVector = FromPhys(c.Travel, 1f / FabricSpace.CmPerMetre),
            RemainderVector = FromPhys(c.Remainder, 1f / FabricSpace.CmPerMetre),
        };
        k.Hits.Add(new KinematicCollision3D.Hit(FabricSpace.FromAverPosition(new Aver.Scene.Vec3(c.Position.X, c.Position.Y, c.Position.Z)), FromPhys(c.Normal), collider,
                                                collider?.GetInstanceId() ?? 0, FromPhys(c.ColliderVelocity, 1f / FabricSpace.CmPerMetre), c.SubShape, c.Depth / FabricSpace.CmPerMetre));
        return k;
    }
}

/// <summary>A region that detects bodies and areas entering it (Godot's Area3D), on the G2 area layer.</summary>
public class Area3D : CollisionObject3D
{
    public new class SignalName : CollisionObject3D.SignalName
    {
        public static readonly StringName BodyEntered = "body_entered";
        public static readonly StringName BodyExited = "body_exited";
        public static readonly StringName BodyShapeEntered = "body_shape_entered";
        public static readonly StringName BodyShapeExited = "body_shape_exited";
        public static readonly StringName AreaEntered = "area_entered";
        public static readonly StringName AreaExited = "area_exited";
        public static readonly StringName AreaShapeEntered = "area_shape_entered";
        public static readonly StringName AreaShapeExited = "area_shape_exited";
    }

    /// <summary>How an area's gravity or damping combines with what other areas and the world give (Godot's SpaceOverride).</summary>
    public enum SpaceOverride { Disabled = 0, Combine = 1, CombineReplace = 2, Replace = 3, ReplaceCombine = 4 }

    public event Action<Node3D> BodyEntered { add => Connect(SignalName.BodyEntered, Callable.From(value)); remove => Disconnect(SignalName.BodyEntered, Callable.From(value)); }
    public event Action<Node3D> BodyExited { add => Connect(SignalName.BodyExited, Callable.From(value)); remove => Disconnect(SignalName.BodyExited, Callable.From(value)); }
    public event Action<Area3D> AreaEntered { add => Connect(SignalName.AreaEntered, Callable.From(value)); remove => Disconnect(SignalName.AreaEntered, Callable.From(value)); }
    public event Action<Area3D> AreaExited { add => Connect(SignalName.AreaExited, Callable.From(value)); remove => Disconnect(SignalName.AreaExited, Callable.From(value)); }

    private readonly List<Node3D> _bodies = new();
    private readonly List<Area3D> _areas = new();
    private bool _monitoring = true, _monitorable = true;
    private int _priority;
    private Aver.Physics.Area _area;
    private SpaceOverride _gravityOverride, _linearDampOverride, _angularDampOverride;
    private float _gravity = 9.8f, _linearDamp = 0.1f, _angularDamp = 0.1f, _pointUnitDistance;
    private Vector3 _gravityDirection = new(0, -1, 0), _pointCenter = new(0, -1, 0);
    private bool _gravityPoint;

    public bool Monitoring
    {
        get => _monitoring;
        set { _monitoring = value; PushFlags(); }
    }

    public bool Monitorable
    {
        get => _monitorable;
        set { _monitorable = value; PushFlags(); }
    }

    public int Priority { get => _priority; set { _priority = value; if (_area.Handle != 0) _area.SetPriority(value); } }

    public SpaceOverride GravitySpaceOverride { get => _gravityOverride; set { _gravityOverride = value; PushOverrides(); } }
    /// <summary>The strength of gravity inside, in metres per second squared.</summary>
    public float Gravity { get => _gravity; set { _gravity = value; PushOverrides(); } }
    public Vector3 GravityDirection { get => _gravityDirection; set { _gravityDirection = value; PushOverrides(); } }
    /// <summary>True: gravity pulls toward <see cref="GravityPointCenter"/> instead of along a direction.</summary>
    public bool GravityPoint { get => _gravityPoint; set { _gravityPoint = value; PushOverrides(); } }
    public Vector3 GravityPointCenter { get => _pointCenter; set { _pointCenter = value; PushOverrides(); } }
    /// <summary>The distance at which a point's pull is full strength; beyond it the pull falls off with the square of the distance (0: it does not).</summary>
    public float GravityPointUnitDistance { get => _pointUnitDistance; set { _pointUnitDistance = value; PushOverrides(); } }
    public SpaceOverride LinearDampSpaceOverride { get => _linearDampOverride; set { _linearDampOverride = value; PushOverrides(); } }
    public SpaceOverride AngularDampSpaceOverride { get => _angularDampOverride; set { _angularDampOverride = value; PushOverrides(); } }
    public float LinearDamp { get => _linearDamp; set { _linearDamp = value; PushOverrides(); } }
    public float AngularDamp { get => _angularDamp; set { _angularDamp = value; PushOverrides(); } }

    internal override bool IsSensor => true;

    private void PushFlags()
    {
        if (_area.Handle != 0) _area.SetFlags(_monitoring, _monitorable);
    }

    private void PushOverrides()
    {
        if (_area.Handle == 0) return;
        _area.SetGravity((Aver.Physics.AreaOverrideMode)(int)_gravityOverride, _gravity * FabricSpace.CmPerMetre, ToPhysDirection(_gravityDirection));
        Aver.Scene.Vec3 c = FabricSpace.ToAverPosition(_pointCenter);
        _area.SetGravityPoint(_gravityPoint, new PhysFloat3(c.X, c.Y, c.Z), _pointUnitDistance * FabricSpace.CmPerMetre);
        _area.SetDamping((Aver.Physics.AreaOverrideMode)(int)_linearDampOverride, _linearDamp, (Aver.Physics.AreaOverrideMode)(int)_angularDampOverride, _angularDamp);
    }

    internal override void OnFilterChanged()
    {
        if (_area.Handle != 0) _area.SetCollision(CollisionLayer, CollisionMask);
    }

    internal override void OnBodyBuilt(PhysBody body)
    {
        _area = Aver.Physics.Area.Create(body);
        if (_area.Handle == 0) return;
        _area.SetFlags(_monitoring, _monitorable);
        _area.SetCollision(CollisionLayer, CollisionMask);
        _area.SetPriority(_priority);
        PushOverrides();
    }

    internal override void OnBodyDestroying(PhysBody body)
    {
        if (_area.Handle != 0) { _area.Destroy(); _area = default; }
    }

    public bool HasOverlappingBodies() => _bodies.Count > 0;
    public bool HasOverlappingAreas() => _areas.Count > 0;
    public Collections.Array<Node3D> GetOverlappingBodies() => new(_bodies);
    public Collections.Array<Area3D> GetOverlappingAreas() => new(_areas);
    public bool OverlapsBody(Node body) => body is Node3D n && _bodies.Contains(n);
    public bool OverlapsArea(Node area) => area is Area3D a && _areas.Contains(a);

    // Called by FabricPhysics (and tests): something entered or left, so the lists change and the signal goes out.
    internal void RaiseBodyOverlap(Node3D body, bool entered)
    {
        if (!_monitoring) return;
        if (entered) { if (_bodies.Contains(body)) return; _bodies.Add(body); }
        else if (!_bodies.Remove(body)) return;
        EmitSignal(entered ? SignalName.BodyEntered : SignalName.BodyExited, body);
    }

    internal void RaiseAreaOverlap(Area3D area, bool entered)
    {
        if (!_monitoring) return;
        if (entered) { if (_areas.Contains(area)) return; _areas.Add(area); }
        else if (!_areas.Remove(area)) return;
        EmitSignal(entered ? SignalName.AreaEntered : SignalName.AreaExited, area);
    }

    internal override void OnNodeMoved(Transform3D global, float dt) => PlaceBody(Body, global, carry: false, dt);
}
