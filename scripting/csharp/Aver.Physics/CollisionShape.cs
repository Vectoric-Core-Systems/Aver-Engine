// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.

// Generic shape handles (physics_shape_builder_abi.h). Units: cm; +X forward, +Y right, +Z up.

using System;

namespace Aver.Physics;

/// <summary>Optional build parameters of a shape. A negative <see cref="ConvexRadiusCm"/> is the shape's own default.</summary>
/// <param name="EdgeAngleDeg">Negative makes every edge active, so bodies touching it want enhanced edges.</param>
public readonly record struct ShapeParams(float ConvexRadiusCm = -1f, float DensityKgM3 = 1000f, float HullToleranceCm = 0.1f,
                                          float EdgeAngleDeg = 5f, bool FavorBuildSpeed = false, int MaxTrianglesPerLeaf = 8)
{
    internal float[] ToBlock() => new[]
    {
        ConvexRadiusCm, DensityKgM3, HullToleranceCm, EdgeAngleDeg, FavorBuildSpeed ? 1f : 0f, (float)MaxTrianglesPerLeaf,
    };
}

/// <summary>One child of a compound: a shape at a position and rotation in the compound's frame, with a user value
/// a raycast detail reports back.</summary>
public readonly record struct ShapeChild(CollisionShape Shape, Float3 Position, Quaternion Rotation, int UserData = 0);

/// <summary>What a shape is made of (aver_phys_shape_info). Lengths in cm, volume in cm^3, inertia in kg cm^2.</summary>
public readonly record struct ShapeInfo(ShapeType Type, float VolumeCm3, float MassKg, Float3 CenterOfMass, Float3 BoundsMin, Float3 BoundsMax,
                                        float InnerRadius, Float3 InertiaPrincipal, Quaternion InertiaFrame,
                                        int Children, int Leaves, int Triangles, long Bytes, bool MustBeStatic, bool HasMass);

/// <summary>A handle to a collision shape. 0 is invalid. A shape is not a body: put it on one with <see cref="CreateBody"/>.
/// <see cref="Release"/> it when done (a body keeps it alive).</summary>
public readonly struct CollisionShape : IEquatable<CollisionShape>
{
    public int Handle { get; }
    public CollisionShape(int handle) { Handle = handle; }

    public bool IsValid => Handle != 0;
    public static CollisionShape None => new(0);

    /// <summary>A box with these half extents.</summary>
    public static CollisionShape Box(Float3 half, int surface = 0, ShapeParams? p = null) =>
        new(Native.aver_phys_shape_box(half.X, half.Y, half.Z, surface, Block(p), BlockLen(p)));

    public static CollisionShape Sphere(float radius, int surface = 0) =>
        new(Native.aver_phys_shape_sphere(radius, surface, null, 0));

    /// <summary>A capsule; <paramref name="height"/> is TOTAL, both caps included.</summary>
    public static CollisionShape Capsule(float radius, float height, int surface = 0) =>
        new(Native.aver_phys_shape_capsule(radius, height, surface, null, 0));

    /// <summary>A flat-ended cylinder; <paramref name="height"/> is end to end.</summary>
    public static CollisionShape Cylinder(float radius, float height, int surface = 0, ShapeParams? p = null) =>
        new(Native.aver_phys_shape_cylinder(radius, height, surface, Block(p), BlockLen(p)));

    public static CollisionShape TaperedCapsule(float top, float bottom, float height, int surface = 0) =>
        new(Native.aver_phys_shape_tapered_capsule(top, bottom, height, surface, null, 0));

    /// <summary>A cylinder whose two ends differ in radius (top at +Z); a barrel, a cone when one radius is 0.</summary>
    public static CollisionShape TaperedCylinder(float top, float bottom, float height, int surface = 0, ShapeParams? p = null) =>
        new(Native.aver_phys_shape_tapered_cylinder(top, bottom, height, surface, Block(p), BlockLen(p)));

    /// <summary>A thin convex triangle (not a mesh): a one-off blade or ramp.</summary>
    public static CollisionShape Triangle(Float3 a, Float3 b, Float3 c, int surface = 0, ShapeParams? p = null) =>
        new(Native.aver_phys_shape_triangle(new[] { a.X, a.Y, a.Z, b.X, b.Y, b.Z, c.X, c.Y, c.Z }, surface, Block(p), BlockLen(p)));

    /// <summary>The points with <c>normal . x = offsetCm</c>; the side opposite the normal is solid. Static or kinematic bodies only.</summary>
    public static CollisionShape Plane(Float3 normal, float offsetCm, float halfExtentCm, int surface = 0) =>
        new(Native.aver_phys_shape_plane(normal.ToArray(), offsetCm, halfExtentCm, surface));

    /// <summary>No volume, collides with nothing: a placeholder, or a kinematic anchor for a joint.</summary>
    public static CollisionShape Empty(Float3 centerOfMass) => new(Native.aver_phys_shape_empty(centerOfMass.ToArray()));

    /// <summary>The convex hull of at least four non-coplanar points.</summary>
    public static CollisionShape ConvexHull(Float3[] points, int surface = 0, ShapeParams? p = null) =>
        points is null || points.Length < 4 ? None
            : new(Native.aver_phys_shape_convex_hull(Flatten(points), points.Length, surface, Block(p), BlockLen(p)));

    /// <summary>A triangle mesh. <paramref name="indices"/> is 3 per triangle. Mismatched array lengths return <see cref="None"/>.</summary>
    public static CollisionShape Mesh(Float3[] vertices, int[] indices, byte[]? triPalette = null, int[]? palette = null,
                                      int[]? triUserData = null, ShapeParams? p = null)
    {
        if (vertices is null || indices is null || vertices.Length < 3 || indices.Length < 3) return None;
        int triangles = indices.Length / 3;
        if (triPalette is not null && triPalette.Length < triangles) return None;
        if (triUserData is not null && triUserData.Length < triangles) return None;
        if (palette is { Length: > 32 }) return None;
        return new(Native.aver_phys_shape_mesh(Flatten(vertices), vertices.Length, indices, indices.Length,
                                               triPalette, palette, palette?.Length ?? 0, triUserData, Block(p), BlockLen(p)));
    }

    public CollisionShape RotatedTranslated(Float3 position, Quaternion rotation) =>
        new(Native.aver_phys_shape_rotated_translated(Handle, position.ToArray(), Quat(rotation)));

    /// <summary>The centre of mass moves by <paramref name="offset"/>; the geometry does not.</summary>
    public CollisionShape OffsetCenterOfMass(Float3 offset) => new(Native.aver_phys_shape_offset_com(Handle, offset.ToArray()));

    /// <summary>Scales the shape (Jolt's ScaleShape: a scale it cannot wrap is baked where it can, refused where it cannot).</summary>
    public CollisionShape Scaled(Float3 scale) => new(Native.aver_phys_shape_scaled(Handle, scale.ToArray()));

    /// <summary>One shape from several. A mutable compound can gain and lose children at run time (<see cref="Body.AddChild"/>).</summary>
    public static CollisionShape Compound(ShapeChild[] children, bool mutable = false)
    {
        if (children is null || children.Length < 1) return None;
        int n = children.Length;
        var handles = new int[n];
        var pos = new float[n * 3];
        var rot = new float[n * 4];
        var user = new int[n];
        for (int i = 0; i < n; i++)
        {
            handles[i] = children[i].Shape.Handle;
            pos[i * 3] = children[i].Position.X; pos[i * 3 + 1] = children[i].Position.Y; pos[i * 3 + 2] = children[i].Position.Z;
            var q = children[i].Rotation;
            rot[i * 4] = q.X; rot[i * 4 + 1] = q.Y; rot[i * 4 + 2] = q.Z; rot[i * 4 + 3] = q.W;
            user[i] = children[i].UserData;
        }
        return new(Native.aver_phys_shape_compound(mutable ? (int)CompoundKind.Mutable : (int)CompoundKind.Static, handles, pos, rot, user, n));
    }

    /// <summary>Live shape handles (a diagnostic).</summary>
    public static int Count() => Native.aver_phys_shape_count();

    /// <summary>Forgets this handle; bodies built from the shape keep it alive.</summary>
    public bool Release() => Native.aver_phys_shape_release(Handle) != 0;

    /// <summary>What the shape is made of. Default for an invalid handle.</summary>
    public ShapeInfo Info => TryGetInfo(out var info) ? info : default;

    public bool TryGetInfo(out ShapeInfo info)
    {
        var f = new float[(int)SiIndex.Count];
        var i = new int[(int)SiiIndex.Count];
        if (Native.aver_phys_shape_info(Handle, f, f.Length, i, i.Length) == 0) { info = default; return false; }
        info = new ShapeInfo(
            (ShapeType)i[(int)SiiIndex.Type], f[(int)SiIndex.VolumeCm3], f[(int)SiIndex.MassKg],
            new Float3(f[(int)SiIndex.ComX], f[(int)SiIndex.ComY], f[(int)SiIndex.ComZ]),
            new Float3(f[(int)SiIndex.BoundsMinX], f[(int)SiIndex.BoundsMinY], f[(int)SiIndex.BoundsMinZ]),
            new Float3(f[(int)SiIndex.BoundsMaxX], f[(int)SiIndex.BoundsMaxY], f[(int)SiIndex.BoundsMaxZ]),
            f[(int)SiIndex.InnerRadius],
            new Float3(f[(int)SiIndex.InertiaPrincipalX], f[(int)SiIndex.InertiaPrincipalY], f[(int)SiIndex.InertiaPrincipalZ]),
            new Quaternion(f[(int)SiIndex.InertiaFrameX], f[(int)SiIndex.InertiaFrameY], f[(int)SiIndex.InertiaFrameZ], f[(int)SiIndex.InertiaFrameW]),
            i[(int)SiiIndex.Children], i[(int)SiiIndex.Leaves], i[(int)SiiIndex.Triangles], i[(int)SiiIndex.Bytes],
            i[(int)SiiIndex.MustBeStatic] != 0, i[(int)SiiIndex.HasMass] != 0);
        return true;
    }

    /// <summary>A body made from this shape at <paramref name="origin"/> (the shape's origin, which is not its centre of mass for a compound or an offset shape).
    /// A massKg of 0 or less takes volume times density. A plane, heightfield or massless mesh cannot be dynamic.</summary>
    public Body CreateBody(Float3 origin, Quaternion rotation, MotionType motion, float massKg = 0f, int layer = 0,
                           bool sensor = false, bool enhancedEdges = false)
    {
        var p = new float[(int)BpIndex.Count];
        p[(int)BpIndex.Layer] = layer;
        p[(int)BpIndex.Sensor] = sensor ? 1f : 0f;
        p[(int)BpIndex.EnhancedEdges] = enhancedEdges ? 1f : 0f;
        p[(int)BpIndex.Activate] = 1f;
        return new Body(Native.aver_phys_body_from_shape(Handle, origin.ToArray(), Quat(rotation), (int)motion, massKg, p, p.Length));
    }

    public bool Equals(CollisionShape other) => Handle == other.Handle;
    public override bool Equals(object? obj) => obj is CollisionShape s && Equals(s);
    public override int GetHashCode() => Handle;
    public static bool operator ==(CollisionShape a, CollisionShape b) => a.Handle == b.Handle;
    public static bool operator !=(CollisionShape a, CollisionShape b) => a.Handle != b.Handle;
    public override string ToString() => IsValid ? $"CollisionShape#{Handle}" : "CollisionShape(none)";

    internal static float[]? Block(ShapeParams? p) => p?.ToBlock();
    internal static int BlockLen(ShapeParams? p) => p is null ? 0 : (int)SpIndex.Count;
    internal static float[] Quat(Quaternion q) => new[] { q.X, q.Y, q.Z, q.W };

    internal static float[] Flatten(Float3[] points)
    {
        var flat = new float[points.Length * 3];
        for (int i = 0; i < points.Length; i++)
        {
            flat[i * 3] = points[i].X; flat[i * 3 + 1] = points[i].Y; flat[i * 3 + 2] = points[i].Z;
        }
        return flat;
    }
}
