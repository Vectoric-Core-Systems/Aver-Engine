// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// Godot's PhysicsDirectSpaceState3D, body_test_motion, RayCast3D and ShapeCast3D on Aver.Physics (G2). The native side is the
// "Godot-style space-state queries" section of physics_queries_abi.h; docs/physics/GODOT_QUERIES.md has the semantics.
//
// NONE OF GODOT'S DEFAULTS IS BUILT IN. Every option is a parameter; the converter or Aver.Fabric passes Godot's values (hit back faces
// true, hit from inside false, bodies true, areas false, margin 0.001 m = 0.1 cm).

using System;
using System.Collections.Generic;

namespace Aver.Physics;

/// <summary>Options of the Godot-style queries. Values are the header's AVER_PHYS_SQ_*. <see cref="None"/> collides with nothing.</summary>
[Flags]
public enum SpaceQueryFlags
{
    None = 0,
    /// <summary>Bodies and characters count (Godot collide_with_bodies).</summary>
    Bodies = 1,
    /// <summary>Sensors count (Godot collide_with_areas).</summary>
    Areas = 2,
    /// <summary>Ray: the back of a triangle and the exit face of a convex body are hit (Godot hit_back_faces).</summary>
    BackFaces = 4,
    /// <summary>Ray: starting inside a convex body hits it at distance 0 with a zero normal (Godot hit_from_inside).</summary>
    FromInside = 8,
    /// <summary>Test motion: depenetration counts as a collision (Godot recovery_as_collision).</summary>
    RecoveryAsCollision = 16,
}

// The blocks of aver_phys_body_test_motion, as enums so the parity test pins every number. A vector member is the index of its X.
internal enum MotionFloat
{
    Travel = 0, Remainder = 3, Point = 6, Normal = 9, Velocity = 12, AngularVelocity = 15, Depth = 18, Safe = 19, Unsafe = 20, Recovery = 21, Count = 24,
}
internal enum MotionInt { Collider = 0, Entity = 1, Subshape = 2, Contacts = 3, Count = 4 }
internal enum MotionContactFloat { Point = 0, Normal = 3, Velocity = 6, AngularVelocity = 9, Depth = 12, Count = 13 }
internal enum MotionContactInt { Body = 0, Entity = 1, Subshape = 2, Count = 3 }

/// <summary>What a Godot-style ray found.</summary>
public readonly struct SpaceRayHit
{
    public Body Collider { get; }
    /// <summary>The collider's entity stamp (0 when none).</summary>
    public int Entity { get; }
    public int SubShape { get; }
    /// <summary>Centimetres along the ray.</summary>
    public float Distance { get; }
    public Float3 Position { get; }
    /// <summary>Outward surface normal; zero for a hit from inside.</summary>
    public Float3 Normal { get; }
    public bool Hit => Collider.IsValid;
    internal SpaceRayHit(Body c, int entity, int sub, float d, Float3 p, Float3 n) { Collider = c; Entity = entity; SubShape = sub; Distance = d; Position = p; Normal = n; }
}

/// <summary>One (body, sub-shape) a shape overlaps.</summary>
public readonly record struct ShapeOverlap(Body Collider, int SubShape);

/// <summary>One contact of a shape against a collider (collide_shape). <see cref="Normal"/> points from the collider toward the shape;
/// <see cref="Depth"/> is signed, negative when the two are apart by less than the margin.</summary>
public readonly record struct ShapeContact(Body Collider, Float3 PointOnShape, Float3 PointOnCollider, Float3 Normal, float Depth);

/// <summary>get_rest_info: the deepest contact, with the collider's velocity at that point.</summary>
public readonly record struct RestInfo(Body Collider, int Entity, int SubShape, Float3 Point, Float3 Normal, Float3 LinearVelocity, float Depth);

/// <summary>cast_motion: the fractions (0..1) of the motion that are safe and where the shape first touches. Both 1 when nothing is in the way.</summary>
public readonly record struct MotionCast(float Safe, float Unsafe, Body Collider, Float3 Point, Float3 Normal)
{
    public bool Hit => Collider.IsValid;
}

/// <summary>A contact found by <see cref="Body.TestMotion"/>.</summary>
public readonly record struct MotionContact(Body Collider, int Entity, int SubShape, Float3 Point, Float3 Normal,
                                            Float3 ColliderVelocity, Float3 ColliderAngularVelocity, float Depth);

/// <summary>The outcome of <see cref="Body.TestMotion"/>. The body has not moved: apply <see cref="Travel"/>.</summary>
public readonly record struct MotionTestResult(bool Collided, Float3 Travel, Float3 Remainder, Float3 Recovery,
                                               float SafeFraction, float UnsafeFraction, MotionContact[] Contacts)
{
    /// <summary>The contact that stopped the body (or the deepest recovery contact); default when there is none.</summary>
    public MotionContact Collision => Contacts.Length > 0 ? Contacts[0] : default;
}

/// <summary>Godot's PhysicsDirectSpaceState3D. Poses are the shape's ORIGIN (as <see cref="CollisionShape.CreateBody"/> places it).
/// The exclude list is Godot's exclude array. Allocates per call: this is the scripting surface.</summary>
public static class SpaceState
{
    internal static float[] Pose(Float3 position, Quaternion rotation) =>
        new[] { position.X, position.Y, position.Z, rotation.X, rotation.Y, rotation.Z, rotation.W };

    internal static int[]? Ids(IReadOnlyList<Body>? exclude, out int count)
    {
        count = 0;
        if (exclude == null || exclude.Count == 0) return null;
        var ids = new int[exclude.Count];
        for (int i = 0; i < ids.Length; i++) ids[i] = exclude[i].Handle;
        count = ids.Length;
        return ids;
    }

    /// <summary>Godot intersect_ray: the nearest hit along <paramref name="from"/> to <paramref name="to"/>.</summary>
    public static SpaceRayHit Raycast(Float3 from, Float3 to, uint collisionMask, SpaceQueryFlags flags, IReadOnlyList<Body>? exclude = null)
    {
        var dir = to - from;
        float len = MathF.Sqrt(dir.X * dir.X + dir.Y * dir.Y + dir.Z * dir.Z);
        if (len <= 0f) return default;
        var ex = Ids(exclude, out int exN);
        var pt = new float[3]; var n = new float[3];
        int body = Native.aver_phys_space_raycast(from.ToArray(), dir.ToArray(), len, collisionMask, (int)flags, ex, exN,
                                                   out float dist, pt, n, out int entity, out int sub);
        return body == 0 ? default : new SpaceRayHit(new Body(body), entity, sub, dist, new Float3(pt[0], pt[1], pt[2]), new Float3(n[0], n[1], n[2]));
    }

    /// <summary>Godot intersect_shape: what the shape overlaps (within <paramref name="marginCm"/>), deepest first.</summary>
    public static ShapeOverlap[] IntersectShape(CollisionShape shape, Float3 position, Quaternion rotation, float marginCm, uint collisionMask,
                                                SpaceQueryFlags flags, IReadOnlyList<Body>? exclude = null, int maxResults = 32)
    {
        if (maxResults <= 0) return Array.Empty<ShapeOverlap>();
        var ex = Ids(exclude, out int exN);
        var bodies = new int[maxResults]; var subs = new int[maxResults];
        int n = Native.aver_phys_space_intersect_shape(shape.Handle, Pose(position, rotation), marginCm, collisionMask, (int)flags, ex, exN, maxResults, bodies, subs);
        var res = new ShapeOverlap[n];
        for (int i = 0; i < n; i++) res[i] = new ShapeOverlap(new Body(bodies[i]), subs[i]);
        return res;
    }

    /// <summary>Godot collide_shape: the contacts of the shape, deepest first.</summary>
    public static ShapeContact[] CollideShape(CollisionShape shape, Float3 position, Quaternion rotation, float marginCm, uint collisionMask,
                                              SpaceQueryFlags flags, IReadOnlyList<Body>? exclude = null, int maxContacts = 32)
    {
        if (maxContacts <= 0) return Array.Empty<ShapeContact>();
        var ex = Ids(exclude, out int exN);
        var bodies = new int[maxContacts];
        var onShape = new float[maxContacts * 3]; var onOther = new float[maxContacts * 3];
        var normals = new float[maxContacts * 3]; var depths = new float[maxContacts];
        int n = Native.aver_phys_space_collide_shape(shape.Handle, Pose(position, rotation), marginCm, collisionMask, (int)flags, ex, exN,
                                                      maxContacts, bodies, onShape, onOther, normals, depths);
        var res = new ShapeContact[n];
        for (int i = 0; i < n; i++)
            res[i] = new ShapeContact(new Body(bodies[i]), V(onShape, i), V(onOther, i), V(normals, i), depths[i]);
        return res;
    }

    /// <summary>Godot get_rest_info: the single deepest contact, or null when there is none.</summary>
    public static RestInfo? GetRestInfo(CollisionShape shape, Float3 position, Quaternion rotation, float marginCm, uint collisionMask,
                                        SpaceQueryFlags flags, IReadOnlyList<Body>? exclude = null)
    {
        var ex = Ids(exclude, out int exN);
        var pt = new float[3]; var n = new float[3]; var v = new float[3];
        int body = Native.aver_phys_space_rest_info(shape.Handle, Pose(position, rotation), marginCm, collisionMask, (int)flags, ex, exN,
                                                     pt, n, v, out float depth, out int entity, out int sub);
        return body == 0 ? null : new RestInfo(new Body(body), entity, sub, V(pt, 0), V(n, 0), V(v, 0), depth);
    }

    /// <summary>Godot cast_motion: how far the shape can move along <paramref name="motion"/> (centimetres).</summary>
    public static MotionCast CastMotion(CollisionShape shape, Float3 position, Quaternion rotation, Float3 motion, float marginCm, uint collisionMask,
                                        SpaceQueryFlags flags, IReadOnlyList<Body>? exclude = null)
    {
        var ex = Ids(exclude, out int exN);
        var pt = new float[3]; var n = new float[3];
        int ok = Native.aver_phys_space_cast_motion(shape.Handle, Pose(position, rotation), motion.ToArray(), marginCm, collisionMask, (int)flags, ex, exN,
                                                     out float safe, out float unsafeFraction, out int body, pt, n);
        return ok == 0 ? new MotionCast(0f, 0f, default, default, default) : new MotionCast(safe, unsafeFraction, new Body(body), V(pt, 0), V(n, 0));
    }

    internal static Float3 V(float[] a, int i) => new(a[i * 3], a[i * 3 + 1], a[i * 3 + 2]);
}

public readonly partial struct Body
{
    /// <summary>Godot body_test_motion for this body: where would it get to if it moved by <paramref name="motion"/> (centimetres)? The body does
    /// not move. Recovery (depenetration) comes first, then the motion; the pair filter is the body's own layer, mask and exceptions plus
    /// <paramref name="exclude"/>. <paramref name="originPose"/> tests from another pose (the body's origin, not its centre of mass). Returns
    /// null when the body cannot be tested (dead handle, character, soft body, mesh or heightfield).</summary>
    public MotionTestResult? TestMotion(Float3 motion, float marginCm, SpaceQueryFlags flags, IReadOnlyList<Body>? exclude = null,
                                        int maxContacts = 4, (Float3 Position, Quaternion Rotation)? originPose = null)
    {
        var ex = SpaceState.Ids(exclude, out int exN);
        int cap = Math.Max(maxContacts, 1);
        var f = new float[(int)MotionFloat.Count]; var iv = new int[(int)MotionInt.Count];
        var cf = new float[cap * (int)MotionContactFloat.Count]; var ci = new int[cap * (int)MotionContactInt.Count];
        float[]? pose = originPose is { } p ? SpaceState.Pose(p.Position, p.Rotation) : null;
        int r = Native.aver_phys_body_test_motion(Handle, pose, motion.ToArray(), marginCm, (int)flags, ex, exN, cap, f, iv, cf, ci);
        if (r < 0) return null;
        var contacts = new MotionContact[Math.Min(iv[(int)MotionInt.Contacts], maxContacts < 0 ? 0 : maxContacts)];
        for (int i = 0; i < contacts.Length; i++)
        {
            int fo = i * (int)MotionContactFloat.Count, io = i * (int)MotionContactInt.Count;
            contacts[i] = new MotionContact(new Body(ci[io + (int)MotionContactInt.Body]), ci[io + (int)MotionContactInt.Entity],
                                            ci[io + (int)MotionContactInt.Subshape],
                                            At(cf, fo + (int)MotionContactFloat.Point), At(cf, fo + (int)MotionContactFloat.Normal),
                                            At(cf, fo + (int)MotionContactFloat.Velocity), At(cf, fo + (int)MotionContactFloat.AngularVelocity),
                                            cf[fo + (int)MotionContactFloat.Depth]);
        }
        return new MotionTestResult(r == 1, At(f, (int)MotionFloat.Travel), At(f, (int)MotionFloat.Remainder), At(f, (int)MotionFloat.Recovery),
                                    f[(int)MotionFloat.Safe], f[(int)MotionFloat.Unsafe], contacts);
    }

    private static Float3 At(float[] a, int i) => new(a[i], a[i + 1], a[i + 2]);
}

/// <summary>Godot's RayCast3D as a plain object: set it up, call <see cref="Update"/> once per physics tick, read the result. World space.</summary>
public sealed class RayCast3D
{
    public Float3 Origin { get; set; }
    /// <summary>The far end of the ray in world space (Godot's target_position, already transformed).</summary>
    public Float3 Target { get; set; }
    public bool Enabled { get; set; } = true;
    public uint CollisionMask { get; set; } = Physics.LayerMaskAll;
    public bool CollideWithBodies { get; set; }
    public bool CollideWithAreas { get; set; }
    public bool HitFromInside { get; set; }
    public bool HitBackFaces { get; set; }
    public List<Body> Exceptions { get; } = new();

    public SpaceRayHit Result { get; private set; }
    public bool IsColliding => Result.Hit;
    public Body Collider => Result.Collider;
    public Float3 CollisionPoint => Result.Position;
    public Float3 CollisionNormal => Result.Normal;

    public void AddException(Body b) { if (!Exceptions.Contains(b)) Exceptions.Add(b); }
    public void RemoveException(Body b) => Exceptions.Remove(b);
    public void ClearExceptions() => Exceptions.Clear();

    /// <summary>Re-casts the ray now (Godot force_raycast_update). A disabled ray reports no collision.</summary>
    public SpaceRayHit Update()
    {
        if (!Enabled) return Result = default;
        var flags = SpaceQueryFlags.None;
        if (CollideWithBodies) flags |= SpaceQueryFlags.Bodies;
        if (CollideWithAreas) flags |= SpaceQueryFlags.Areas;
        if (HitFromInside) flags |= SpaceQueryFlags.FromInside;
        if (HitBackFaces) flags |= SpaceQueryFlags.BackFaces;
        return Result = SpaceState.Raycast(Origin, Target, CollisionMask, flags, Exceptions);
    }
}

/// <summary>Godot's ShapeCast3D as a plain object: sweeps <see cref="Shape"/> along <see cref="Motion"/> and lists what it touches. Call
/// <see cref="Update"/> once per physics tick.</summary>
public sealed class ShapeCast3D
{
    public CollisionShape Shape { get; set; }
    public Float3 Position { get; set; }
    public Quaternion Rotation { get; set; } = Quaternion.Identity;
    /// <summary>The displacement to sweep, centimetres (Godot's target_position).</summary>
    public Float3 Motion { get; set; }
    public float MarginCm { get; set; }
    public int MaxResults { get; set; } = 32;
    public bool Enabled { get; set; } = true;
    public uint CollisionMask { get; set; } = Physics.LayerMaskAll;
    public bool CollideWithBodies { get; set; }
    public bool CollideWithAreas { get; set; }
    public List<Body> Exceptions { get; } = new();

    public MotionCast Cast { get; private set; } = new(1f, 1f, default, default, default);
    /// <summary>What the shape overlaps where it starts, then its contacts where it first touches (Godot get_collision_count and friends).</summary>
    public ShapeContact[] Contacts { get; private set; } = Array.Empty<ShapeContact>();
    public bool IsColliding => Contacts.Length > 0;
    public float ClosestSafeFraction => Cast.Safe;
    public float ClosestUnsafeFraction => Cast.Unsafe;

    public void AddException(Body b) { if (!Exceptions.Contains(b)) Exceptions.Add(b); }
    public void RemoveException(Body b) => Exceptions.Remove(b);
    public void ClearExceptions() => Exceptions.Clear();

    /// <summary>Re-casts now (Godot force_shapecast_update).</summary>
    public void Update()
    {
        Cast = new MotionCast(1f, 1f, default, default, default);
        Contacts = Array.Empty<ShapeContact>();
        if (!Enabled || !Shape.IsValid) return;
        var flags = SpaceQueryFlags.None;
        if (CollideWithBodies) flags |= SpaceQueryFlags.Bodies;
        if (CollideWithAreas) flags |= SpaceQueryFlags.Areas;
        Cast = SpaceState.CastMotion(Shape, Position, Rotation, Motion, MarginCm, CollisionMask, flags, Exceptions);
        var list = new List<ShapeContact>(SpaceState.CollideShape(Shape, Position, Rotation, MarginCm, CollisionMask, flags, Exceptions, MaxResults));
        if (Cast.Hit && list.Count < MaxResults)
        {
            var atPos = Position + Motion * Cast.Unsafe;
            foreach (var c in SpaceState.CollideShape(Shape, atPos, Rotation, MarginCm, CollisionMask, flags, Exceptions, MaxResults - list.Count))
                list.Add(c);
        }
        Contacts = list.ToArray();
    }
}
