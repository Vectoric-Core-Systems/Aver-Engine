// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Space queries and the physics server in Godot's shapes: World3D.DirectSpaceState.IntersectRay(PhysicsRayQueryParameters3D) returns
// the dictionary Godot returns, RayCast3D and ShapeCast3D nodes, and a lite PhysicsServer3D keyed by the Rid of a collision object.
// All on the G2 space-state ABI (docs/physics/GODOT_QUERIES.md). Godot units at the boundary: metres, with Godot's axes.

using Aver.Scripting;
using PhysBody = Aver.Physics.Body;
using PhysFloat3 = Aver.Physics.Float3;
using PhysShape = Aver.Physics.CollisionShape;

namespace Aver.Fabric;

/// <summary>What the 3D scene lives in (Godot's World3D): here, the physics space the queries run against.</summary>
public class World3D : Resource
{
    private readonly PhysicsDirectSpaceState3D _space = new();

    /// <summary>The physics space for ray and shape queries. Valid during the physics step and outside it.</summary>
    public PhysicsDirectSpaceState3D DirectSpaceState => _space;

    public Rid Space => new(1);
}

/// <summary>A ray query (Godot's PhysicsRayQueryParameters3D). Defaults are Godot's: bodies yes, areas no, back faces yes, from inside no.</summary>
public class PhysicsRayQueryParameters3D : RefCounted
{
    public Vector3 From { get; set; }
    public Vector3 To { get; set; }
    public uint CollisionMask { get; set; } = 0xFFFFFFFF;
    public Collections.Array<Rid> Exclude { get; set; } = new();
    public bool CollideWithBodies { get; set; } = true;
    public bool CollideWithAreas { get; set; }
    public bool HitFromInside { get; set; }
    public bool HitBackFaces { get; set; } = true;

    public static PhysicsRayQueryParameters3D Create(Vector3 from, Vector3 to, uint collisionMask = 0xFFFFFFFF, Collections.Array<Rid>? exclude = null) =>
        new() { From = from, To = to, CollisionMask = collisionMask, Exclude = exclude ?? new Collections.Array<Rid>() };
}

/// <summary>A shape query (Godot's PhysicsShapeQueryParameters3D).</summary>
public class PhysicsShapeQueryParameters3D : RefCounted
{
    public Shape3D? Shape { get; set; }
    public Transform3D Transform { get; set; } = Transform3D.Identity;
    /// <summary>How far past the surface counts as touching, in metres.</summary>
    public float Margin { get; set; }
    /// <summary>For CastMotion: where the shape is moving to, as a displacement.</summary>
    public Vector3 Motion { get; set; }
    public uint CollisionMask { get; set; } = 0xFFFFFFFF;
    public Collections.Array<Rid> Exclude { get; set; } = new();
    public bool CollideWithBodies { get; set; } = true;
    public bool CollideWithAreas { get; set; }
}

/// <summary>Asks the physics world what is where (Godot's PhysicsDirectSpaceState3D). Results are Godot's dictionaries.</summary>
public class PhysicsDirectSpaceState3D : GodotObject
{
    private static List<PhysBody> Excluded(Collections.Array<Rid> exclude)
    {
        var list = new List<PhysBody>();
        foreach (Rid r in exclude) if (r.Id != 0) list.Add(new PhysBody((int)r.Id));
        return list;
    }

    private static Aver.Physics.SpaceQueryFlags Flags(bool bodies, bool areas) =>
        (bodies ? Aver.Physics.SpaceQueryFlags.Bodies : 0) | (areas ? Aver.Physics.SpaceQueryFlags.Areas : 0);

    // The node, the instance id and the rid of a body in the dictionaries.
    private static void AddCollider(Collections.Dictionary d, PhysBody b)
    {
        GodotObject? node = CollisionObject3D.NodeOfBody(b);
        d["collider"] = node;
        d["collider_id"] = node?.GetInstanceId() ?? 0UL;
        d["rid"] = new Rid((ulong)b.Handle);
    }

    /// <summary>The nearest hit along the ray: position, normal, collider, collider_id, rid, shape and face_index; empty when nothing was hit.</summary>
    public Collections.Dictionary IntersectRay(PhysicsRayQueryParameters3D parameters)
    {
        var d = new Collections.Dictionary();
        try
        {
            Aver.Physics.SpaceRayHit hit = Aver.Physics.SpaceState.Raycast(FabricPhysics.ToPhysPosition(parameters.From), FabricPhysics.ToPhysPosition(parameters.To),
                parameters.CollisionMask,
                Flags(parameters.CollideWithBodies, parameters.CollideWithAreas) | (parameters.HitFromInside ? Aver.Physics.SpaceQueryFlags.FromInside : 0) | (parameters.HitBackFaces ? Aver.Physics.SpaceQueryFlags.BackFaces : 0),
                Excluded(parameters.Exclude));
            if (!hit.Hit) return d;
            d["position"] = FabricPhysics.FromPhysPosition(hit.Position);
            d["normal"] = CollisionObject3D.FromPhys(hit.Normal);
            AddCollider(d, hit.Collider);
            d["shape"] = hit.SubShape;
            d["face_index"] = -1;
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { }
        return d;
    }

    // Runs a query against a shape built for the call and released after it.
    private static T WithShape<T>(PhysicsShapeQueryParameters3D q, T none, Func<PhysShape, PhysFloat3, Aver.Physics.Quaternion, T> run)
    {
        if (q.Shape == null) return none;
        try
        {
            PhysShape shape = FabricPhysics.BuildShape(q.Shape, Vector3.One);
            if (!shape.IsValid) return none;
            try
            {
                return run(shape, FabricPhysics.ToPhysPosition(q.Transform.Origin), FabricPhysics.ToPhysRotation(q.Transform.Basis.Orthonormalized().GetRotationQuaternion()));
            }
            finally { shape.Release(); }
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { return none; }
    }

    /// <summary>The objects the shape overlaps: a dictionary each with collider, collider_id, rid and shape.</summary>
    public Collections.Array<Collections.Dictionary> IntersectShape(PhysicsShapeQueryParameters3D parameters, int maxResults = 32)
    {
        var result = new Collections.Array<Collections.Dictionary>();
        Aver.Physics.ShapeOverlap[] overlaps = WithShape(parameters, System.Array.Empty<Aver.Physics.ShapeOverlap>(), (shape, pos, rot) =>
            Aver.Physics.SpaceState.IntersectShape(shape, pos, rot, FabricSpace.ToAverLength(parameters.Margin), parameters.CollisionMask,
                Flags(parameters.CollideWithBodies, parameters.CollideWithAreas), Excluded(parameters.Exclude), maxResults));
        foreach (Aver.Physics.ShapeOverlap o in overlaps)
        {
            var d = new Collections.Dictionary();
            AddCollider(d, o.Collider);
            d["shape"] = o.SubShape;
            result.Add(d);
        }
        return result;
    }

    /// <summary>How far the shape can move along its Motion before it touches: [safe, unsafe] fractions of the motion, both 1 when nothing is in the way.</summary>
    public float[] CastMotion(PhysicsShapeQueryParameters3D parameters)
    {
        Aver.Physics.MotionCast cast = WithShape(parameters, new Aver.Physics.MotionCast(1f, 1f, default, default, default), (shape, pos, rot) =>
            Aver.Physics.SpaceState.CastMotion(shape, pos, rot, CollisionObject3D.ToPhysDirection(parameters.Motion, FabricSpace.CmPerMetre),
                FabricSpace.ToAverLength(parameters.Margin), parameters.CollisionMask, Flags(parameters.CollideWithBodies, parameters.CollideWithAreas), Excluded(parameters.Exclude)));
        return new[] { cast.Safe, cast.Unsafe };
    }

    /// <summary>The points where the shape touches things: pairs (on the shape, on the other object), as Godot returns them.</summary>
    public Collections.Array<Vector3> CollideShape(PhysicsShapeQueryParameters3D parameters, int maxResults = 32)
    {
        var points = new Collections.Array<Vector3>();
        Aver.Physics.ShapeContact[] contacts = WithShape(parameters, System.Array.Empty<Aver.Physics.ShapeContact>(), (shape, pos, rot) =>
            Aver.Physics.SpaceState.CollideShape(shape, pos, rot, FabricSpace.ToAverLength(parameters.Margin), parameters.CollisionMask,
                Flags(parameters.CollideWithBodies, parameters.CollideWithAreas), Excluded(parameters.Exclude), maxResults));
        foreach (Aver.Physics.ShapeContact c in contacts)
        {
            points.Add(FabricPhysics.FromPhysPosition(c.PointOnShape));
            points.Add(FabricPhysics.FromPhysPosition(c.PointOnCollider));
        }
        return points;
    }

    /// <summary>The deepest contact of the shape: point, normal, rid, collider_id, shape and linear_velocity; empty when it touches nothing.</summary>
    public Collections.Dictionary GetRestInfo(PhysicsShapeQueryParameters3D parameters)
    {
        var d = new Collections.Dictionary();
        Aver.Physics.RestInfo? info = WithShape(parameters, (Aver.Physics.RestInfo?)null, (shape, pos, rot) =>
            Aver.Physics.SpaceState.GetRestInfo(shape, pos, rot, FabricSpace.ToAverLength(parameters.Margin), parameters.CollisionMask,
                Flags(parameters.CollideWithBodies, parameters.CollideWithAreas), Excluded(parameters.Exclude)));
        if (info is not { } r) return d;
        d["point"] = FabricPhysics.FromPhysPosition(r.Point);
        d["normal"] = CollisionObject3D.FromPhys(r.Normal);
        d["rid"] = new Rid((ulong)r.Collider.Handle);
        d["collider_id"] = CollisionObject3D.NodeOfBody(r.Collider)?.GetInstanceId() ?? 0UL;
        d["shape"] = r.SubShape;
        d["linear_velocity"] = CollisionObject3D.FromPhys(r.LinearVelocity, 1f / FabricSpace.CmPerMetre);
        return d;
    }
}

/// <summary>A few of Godot's PhysicsServer3D calls, by the Rid of a collision object (<see cref="CollisionObject3D.GetRid"/>).</summary>
public static class PhysicsServer3D
{
    public enum BodyState { Transform = 0, LinearVelocity = 1, AngularVelocity = 2, Sleeping = 3, CanSleep = 4 }

    private static PhysBody Of(Rid rid) => rid.Id == 0 ? default : new PhysBody((int)rid.Id);

    /// <summary>The query interface of the 3D world's physics space (there is one space).</summary>
    public static PhysicsDirectSpaceState3D SpaceGetDirectState(Rid space) => SceneTree.Instance.Root.World3D.DirectSpaceState;

    public static void BodySetState(Rid body, BodyState state, Variant value)
    {
        PhysBody b = Of(body);
        if (!b.IsValid) return;
        switch (state)
        {
            case BodyState.Transform: CollisionObject3D.PlaceBody(b, value.AsTransform3D(), carry: false, 0f); break;
            case BodyState.LinearVelocity: b.SetVelocity(CollisionObject3D.ToPhysDirection(value.AsVector3(), FabricSpace.CmPerMetre)); break;
            case BodyState.AngularVelocity: b.SetAngularVelocity(CollisionObject3D.ToPhysAngular(value.AsVector3())); break;
            case BodyState.Sleeping: if (value.AsBool()) b.Deactivate(); else b.Activate(); break;
        }
    }

    public static Variant BodyGetState(Rid body, BodyState state)
    {
        PhysBody b = Of(body);
        if (!b.IsValid) return default;
        switch (state)
        {
            case BodyState.Transform:
            {
                Aver.Physics.Quaternion q = b.Rotation;
                Quaternion g = FabricSpace.FromAverRotation(new Aver.Scene.Quat(q.X, q.Y, q.Z, q.W));
                return new Transform3D(new Basis(g), FabricPhysics.FromPhysPosition(b.Origin));
            }
            case BodyState.LinearVelocity: return CollisionObject3D.FromPhys(b.Velocity, 1f / FabricSpace.CmPerMetre);
            case BodyState.AngularVelocity: return CollisionObject3D.FromPhysAngular(b.AngularVelocity);
            case BodyState.Sleeping: return !b.IsActive;
            default: return default;
        }
    }

    public static void BodyApplyCentralImpulse(Rid body, Vector3 impulse) => Of(body).AddImpulse(CollisionObject3D.ToPhysDirection(impulse, FabricSpace.CmPerMetre));
    public static void BodyApplyCentralForce(Rid body, Vector3 force) => Of(body).AddForce(CollisionObject3D.ToPhysDirection(force, FabricSpace.CmPerMetre));
    public static void BodyApplyTorqueImpulse(Rid body, Vector3 impulse) =>
        Of(body).AddAngularImpulse(CollisionObject3D.ToPhysAngular(impulse, FabricSpace.CmPerMetre * FabricSpace.CmPerMetre));

    public static void BodySetCollisionLayer(Rid body, uint layer)
    {
        PhysBody b = Of(body);
        if (b.IsValid && b.CollisionFilter is { } f) b.SetCollisionFilter(layer, f.Mask);
    }

    public static void BodySetCollisionMask(Rid body, uint mask)
    {
        PhysBody b = Of(body);
        if (b.IsValid && b.CollisionFilter is { } f) b.SetCollisionFilter(f.Layer, mask);
    }

    public static uint BodyGetCollisionLayer(Rid body) => Of(body) is { IsValid: true } b ? b.CollisionFilter?.Layer ?? 0u : 0u;
    public static uint BodyGetCollisionMask(Rid body) => Of(body) is { IsValid: true } b ? b.CollisionFilter?.Mask ?? 0u : 0u;

    public static void BodyAddCollisionException(Rid body, Rid exceptedBody) => Of(body).AddCollisionException(Of(exceptedBody));
    public static void BodyRemoveCollisionException(Rid body, Rid exceptedBody) => Of(body).RemoveCollisionException(Of(exceptedBody));
}

/// <summary>Casts a ray every physics step and keeps the nearest hit (Godot's RayCast3D).</summary>
public class RayCast3D : Node3D
{
    private readonly Aver.Physics.RayCast3D _ray = new();
    private readonly List<CollisionObject3D> _exceptions = new();
    private Vector3 _target = new(0, -1, 0);

    public bool Enabled { get; set; } = true;
    public bool ExcludeParent { get; set; } = true;
    /// <summary>Where the ray ends, in this node's own space (metres).</summary>
    public Vector3 TargetPosition { get => _target; set => _target = value; }
    public uint CollisionMask { get; set; } = 1;
    public bool CollideWithAreas { get; set; }
    public bool CollideWithBodies { get; set; } = true;
    public bool HitFromInside { get; set; }
    public bool HitBackFaces { get; set; } = true;

    public bool GetCollisionMaskValue(int layerNumber) => (CollisionMask & (1u << (layerNumber - 1))) != 0;
    public void SetCollisionMaskValue(int layerNumber, bool value) =>
        CollisionMask = value ? CollisionMask | (1u << (layerNumber - 1)) : CollisionMask & ~(1u << (layerNumber - 1));

    internal override bool UsesInternalPhysicsProcess => true;

    internal override void InternalPhysicsProcess(double delta)
    {
        if (Enabled && IsInsideTree()) ForceRaycastUpdate();
    }

    public void AddException(CollisionObject3D node) { if (!_exceptions.Contains(node)) _exceptions.Add(node); }
    public void RemoveException(CollisionObject3D node) => _exceptions.Remove(node);
    public void AddExceptionRid(Rid rid) { if (rid.Id != 0 && !_ridExceptions.Contains(rid)) _ridExceptions.Add(rid); }
    public void ClearExceptions() { _exceptions.Clear(); _ridExceptions.Clear(); }

    private readonly List<Rid> _ridExceptions = new();

    /// <summary>Casts now instead of waiting for the next step.</summary>
    public void ForceRaycastUpdate()
    {
        if (!NodeHost.Current.IsEngine) return;
        try
        {
            Transform3D g = GlobalTransform;
            _ray.Origin = FabricPhysics.ToPhysPosition(g.Origin);
            _ray.Target = FabricPhysics.ToPhysPosition(g * _target);
            _ray.Enabled = Enabled;
            _ray.CollisionMask = CollisionMask;
            _ray.CollideWithBodies = CollideWithBodies;
            _ray.CollideWithAreas = CollideWithAreas;
            _ray.HitFromInside = HitFromInside;
            _ray.HitBackFaces = HitBackFaces;
            _ray.ClearExceptions();
            foreach (Rid r in _ridExceptions) _ray.AddException(new PhysBody((int)r.Id));
            foreach (CollisionObject3D e in _exceptions) if (e.Body.IsValid) _ray.AddException(e.Body);
            if (ExcludeParent && GetParent() is CollisionObject3D p && p.Body.IsValid) _ray.AddException(p.Body);
            _ray.Update();
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { }
    }

    public bool IsColliding() => _ray.IsColliding;
    public GodotObject? GetCollider() => _ray.IsColliding ? CollisionObject3D.NodeOfBody(_ray.Collider) : null;
    public Rid GetColliderRid() => _ray.IsColliding ? new Rid((ulong)_ray.Collider.Handle) : default;
    public int GetColliderShape() => _ray.IsColliding ? _ray.Result.SubShape : 0;
    public Vector3 GetCollisionPoint() => _ray.IsColliding ? FabricPhysics.FromPhysPosition(_ray.CollisionPoint) : Vector3.Zero;
    public Vector3 GetCollisionNormal() => _ray.IsColliding ? CollisionObject3D.FromPhys(_ray.CollisionNormal) : Vector3.Zero;
    public int GetCollisionFaceIndex() => -1;
}

/// <summary>Sweeps a shape along a line every physics step and lists what it touches (Godot's ShapeCast3D).</summary>
public class ShapeCast3D : Node3D
{
    private readonly Aver.Physics.ShapeCast3D _cast = new();
    private readonly List<CollisionObject3D> _exceptions = new();
    private Shape3D? _shape;
    private PhysShape _built;

    public Shape3D? Shape { get => _shape; set { _shape = value; ReleaseShape(); } }
    public bool Enabled { get; set; } = true;
    public bool ExcludeParent { get; set; } = true;
    /// <summary>The displacement to sweep by, in this node's own space (metres).</summary>
    public Vector3 TargetPosition { get; set; } = new(0, -1, 0);
    public float Margin { get; set; }
    public int MaxResults { get; set; } = 32;
    public uint CollisionMask { get; set; } = 1;
    public bool CollideWithAreas { get; set; }
    public bool CollideWithBodies { get; set; } = true;

    internal override bool UsesInternalPhysicsProcess => true;

    internal override void InternalPhysicsProcess(double delta)
    {
        if (Enabled && IsInsideTree()) ForceShapecastUpdate();
    }

    internal override void OnExitingTree()
    {
        base.OnExitingTree();
        ReleaseShape();
    }

    private void ReleaseShape()
    {
        if (!_built.IsValid) return;
        try { _built.Release(); } catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { }
        _built = default;
    }

    public void AddException(CollisionObject3D node) { if (!_exceptions.Contains(node)) _exceptions.Add(node); }
    public void RemoveException(CollisionObject3D node) => _exceptions.Remove(node);
    public void ClearExceptions() => _exceptions.Clear();

    public void ForceShapecastUpdate()
    {
        if (!NodeHost.Current.IsEngine || _shape == null) return;
        try
        {
            if (!_built.IsValid) _built = FabricPhysics.BuildShape(_shape, Vector3.One);
            Transform3D g = GlobalTransform;
            _cast.Shape = _built;
            _cast.Position = FabricPhysics.ToPhysPosition(g.Origin);
            _cast.Rotation = FabricPhysics.ToPhysRotation(g.Basis.Orthonormalized().GetRotationQuaternion());
            _cast.Motion = CollisionObject3D.ToPhysDirection(g.Basis * TargetPosition, FabricSpace.CmPerMetre);
            _cast.MarginCm = FabricSpace.ToAverLength(Margin);
            _cast.MaxResults = MaxResults;
            _cast.Enabled = Enabled;
            _cast.CollisionMask = CollisionMask;
            _cast.CollideWithBodies = CollideWithBodies;
            _cast.CollideWithAreas = CollideWithAreas;
            _cast.Exceptions.Clear();
            foreach (CollisionObject3D e in _exceptions) if (e.Body.IsValid) _cast.AddException(e.Body);
            if (ExcludeParent && GetParent() is CollisionObject3D p && p.Body.IsValid) _cast.AddException(p.Body);
            _cast.Update();
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { }
    }

    public bool IsColliding() => _cast.IsColliding;
    public int GetCollisionCount() => _cast.Contacts.Length;
    public GodotObject? GetCollider(int index) => index >= 0 && index < _cast.Contacts.Length ? CollisionObject3D.NodeOfBody(_cast.Contacts[index].Collider) : null;
    public Rid GetColliderRid(int index) => index >= 0 && index < _cast.Contacts.Length ? new Rid((ulong)_cast.Contacts[index].Collider.Handle) : default;
    public Vector3 GetCollisionPoint(int index) => index >= 0 && index < _cast.Contacts.Length ? FabricPhysics.FromPhysPosition(_cast.Contacts[index].PointOnCollider) : Vector3.Zero;
    public Vector3 GetCollisionNormal(int index) => index >= 0 && index < _cast.Contacts.Length ? CollisionObject3D.FromPhys(_cast.Contacts[index].Normal) : Vector3.Zero;
    public float GetClosestCollisionSafeFraction() => _cast.ClosestSafeFraction;
    public float GetClosestCollisionUnsafeFraction() => _cast.ClosestUnsafeFraction;
}
