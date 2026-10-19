// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// Shape casts, overlaps, every-hit rays and the point query. See physics_queries_abi.h's file comment
// for the shape conventions (a capsule's height is TOTAL and it stands along +Z), what a cast hit
// means, and why sensors are skipped unless QueryFlags.IncludeSensors is passed.

using System;

namespace Aver.Physics;

/// <summary>Options for the queries in this file.</summary>
[Flags]
public enum QueryFlags
{
    /// <summary>Sensors are ignored: a trigger volume is not a surface.</summary>
    None = 0,
    /// <summary>Sensors take part like any other body.</summary>
    IncludeSensors = 1,
}

/// <summary>What a shape cast, or one hit of <see cref="Physics.RaycastAll"/>, found.</summary>
public readonly struct ShapeHit
{
    /// <summary><see cref="Body.None"/> when nothing was hit.</summary>
    public Body Body { get; }
    /// <summary>How far the shape's centre (or the ray) travelled before touching, centimetres.</summary>
    public float Distance { get; }
    /// <summary>The contact point on the hit body's surface, centimetres.</summary>
    public Float3 Point { get; }
    /// <summary>Unit surface normal at the contact, pointing back toward the cast shape.</summary>
    public Float3 Normal { get; }
    /// <summary>True when something was hit. Check this before reading the rest.</summary>
    public bool Hit => Body.IsValid;

    internal ShapeHit(int body, float distance, Float3 point, Float3 normal)
    { Body = new Body(body); Distance = distance; Point = point; Normal = normal; }
}

public static partial class Physics
{
    private static ShapeHit MakeShapeHit(int body, float distance, float[] p, float[] n) =>
        body == 0 ? default : new ShapeHit(body, distance, new Float3(p[0], p[1], p[2]), new Float3(n[0], n[1], n[2]));

    private static Body[] ToBodies(int[] raw, int n)
    {
        var outv = new Body[n];
        for (int i = 0; i < n; i++) outv[i] = new Body(raw[i]);
        return outv;
    }

    // ---- Shape casts ---------------------------------------------------------------------------------

    /// <summary>Sweeps a box by <paramref name="displacement"/> (direction and distance in one vector,
    /// centimetres) and returns the first body it touches. <paramref name="halfExtents"/> are along the
    /// engine's X, Y, Z in the box's own frame, then rotated by <paramref name="rotation"/>.</summary>
    public static ShapeHit BoxCast(Float3 centre, Float3 halfExtents, Quaternion rotation, Float3 displacement,
                                   uint layerMask = LayerMaskAll, Body ignoreBody = default, QueryFlags flags = QueryFlags.None)
    {
        var p = new float[3]; var n = new float[3];
        int body = Native.aver_phys_box_cast(centre.X, centre.Y, centre.Z, halfExtents.X, halfExtents.Y, halfExtents.Z,
                                              rotation.X, rotation.Y, rotation.Z, rotation.W,
                                              displacement.X, displacement.Y, displacement.Z,
                                              layerMask, ignoreBody.Handle, (int)flags, out float dist, p, n);
        return MakeShapeHit(body, dist, p, n);
    }

    /// <summary>Sweeps a capsule by <paramref name="displacement"/>. <paramref name="height"/> is TOTAL,
    /// both caps included, and the capsule stands along +Z before <paramref name="rotation"/> -- the
    /// same description as <see cref="Shapes.AddStaticCapsule"/>.</summary>
    public static ShapeHit CapsuleCast(Float3 centre, float radius, float height, Quaternion rotation, Float3 displacement,
                                       uint layerMask = LayerMaskAll, Body ignoreBody = default, QueryFlags flags = QueryFlags.None)
    {
        var p = new float[3]; var n = new float[3];
        int body = Native.aver_phys_capsule_cast(centre.X, centre.Y, centre.Z, radius, height,
                                                  rotation.X, rotation.Y, rotation.Z, rotation.W,
                                                  displacement.X, displacement.Y, displacement.Z,
                                                  layerMask, ignoreBody.Handle, (int)flags, out float dist, p, n);
        return MakeShapeHit(body, dist, p, n);
    }

    /// <summary>Sweeps a sphere by <paramref name="displacement"/> (centimetres). Two bodies can be skipped (a camera skips
    /// the target's character and a vehicle chassis); sensors are skipped unless <see cref="QueryFlags.IncludeSensors"/>.</summary>
    public static ShapeHit SphereCastQ(Float3 centre, float radius, Float3 displacement,
                                       uint layerMask = LayerMaskAll, Body ignoreA = default, Body ignoreB = default, QueryFlags flags = QueryFlags.None)
    {
        var p = new float[3]; var n = new float[3];
        int body = Native.aver_phys_sphere_cast_q(centre.X, centre.Y, centre.Z, displacement.X, displacement.Y, displacement.Z, radius,
                                                   layerMask, ignoreA.Handle, ignoreB.Handle, (int)flags, out float dist, p, n);
        return MakeShapeHit(body, dist, p, n);
    }

    /// <summary>Sweeps an existing body's own shape from its current pose by <paramref name="displacement"/>,
    /// ignoring that body. <paramref name="ignoreBody"/> is a second body to skip. Misses for a dead
    /// handle, a character, a soft body or a mesh/heightfield body.</summary>
    public static ShapeHit BodyCast(Body body, Float3 displacement,
                                    uint layerMask = LayerMaskAll, Body ignoreBody = default, QueryFlags flags = QueryFlags.None)
    {
        var p = new float[3]; var n = new float[3];
        int hit = Native.aver_phys_body_cast(body.Handle, displacement.X, displacement.Y, displacement.Z,
                                              layerMask, ignoreBody.Handle, (int)flags, out float dist, p, n);
        return MakeShapeHit(hit, dist, p, n);
    }

    // ---- Overlaps ------------------------------------------------------------------------------------
    // Each body appears once. A result of maxResults entries may mean truncated.

    /// <summary>Every body overlapping a box.</summary>
    public static Body[] OverlapBox(Float3 centre, Float3 halfExtents, Quaternion rotation,
                                    uint layerMask = LayerMaskAll, Body ignoreBody = default, QueryFlags flags = QueryFlags.None, int maxResults = 64)
    {
        if (maxResults <= 0) return Array.Empty<Body>();
        var raw = new int[maxResults];
        int n = Native.aver_phys_overlap_box(centre.X, centre.Y, centre.Z, halfExtents.X, halfExtents.Y, halfExtents.Z,
                                              rotation.X, rotation.Y, rotation.Z, rotation.W,
                                              layerMask, ignoreBody.Handle, (int)flags, raw, maxResults);
        return ToBodies(raw, n);
    }

    /// <summary>Every body overlapping a capsule (<paramref name="height"/> TOTAL, standing along +Z
    /// before <paramref name="rotation"/>).</summary>
    public static Body[] OverlapCapsule(Float3 centre, float radius, float height, Quaternion rotation,
                                        uint layerMask = LayerMaskAll, Body ignoreBody = default, QueryFlags flags = QueryFlags.None, int maxResults = 64)
    {
        if (maxResults <= 0) return Array.Empty<Body>();
        var raw = new int[maxResults];
        int n = Native.aver_phys_overlap_capsule(centre.X, centre.Y, centre.Z, radius, height,
                                                  rotation.X, rotation.Y, rotation.Z, rotation.W,
                                                  layerMask, ignoreBody.Handle, (int)flags, raw, maxResults);
        return ToBodies(raw, n);
    }

    /// <summary>Every body an existing body's shape would overlap if it stood at
    /// <paramref name="position"/>/<paramref name="rotation"/> (its centre of mass, as
    /// <see cref="Body.Position"/> reports it). The body is not moved and never reported.</summary>
    public static Body[] OverlapBody(Body body, Float3 position, Quaternion rotation,
                                     uint layerMask = LayerMaskAll, Body ignoreBody = default, QueryFlags flags = QueryFlags.None, int maxResults = 64)
    {
        if (maxResults <= 0) return Array.Empty<Body>();
        var raw = new int[maxResults];
        int n = Native.aver_phys_overlap_body(body.Handle, position.X, position.Y, position.Z,
                                               rotation.X, rotation.Y, rotation.Z, rotation.W,
                                               layerMask, ignoreBody.Handle, (int)flags, raw, maxResults);
        return ToBodies(raw, n);
    }

    // ---- Every hit along a ray ---------------------------------------------------------------------------

    /// <summary>Every hit along a ray up to <paramref name="maxDistanceCm"/>, nearest first. One entry per
    /// hit, not per body. When more than <paramref name="maxResults"/> are hit the nearest are kept.</summary>
    public static ShapeHit[] RaycastAll(Float3 origin, Float3 direction, float maxDistanceCm,
                                        uint layerMask = LayerMaskAll, Body ignoreBody = default, QueryFlags flags = QueryFlags.None, int maxResults = 64)
    {
        if (maxResults <= 0) return Array.Empty<ShapeHit>();
        var bodies = new int[maxResults];
        var dist = new float[maxResults];
        var pts = new float[maxResults * 3];
        var nrm = new float[maxResults * 3];
        int n = Native.aver_phys_raycast_all(origin.X, origin.Y, origin.Z, direction.X, direction.Y, direction.Z,
                                              maxDistanceCm, layerMask, ignoreBody.Handle, (int)flags, maxResults,
                                              bodies, dist, pts, nrm);
        var hits = new ShapeHit[n];
        for (int i = 0; i < n; i++)
            hits[i] = new ShapeHit(bodies[i], dist[i],
                                   new Float3(pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2]),
                                   new Float3(nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]));
        return hits;
    }

    // ---- Point query ------------------------------------------------------------------------------------

    /// <summary>Bodies whose shape contains <paramref name="point"/>. Triangle meshes and heightfields
    /// have no inside and never match.</summary>
    public static Body[] BodiesAtPoint(Float3 point,
                                       uint layerMask = LayerMaskAll, Body ignoreBody = default, QueryFlags flags = QueryFlags.None, int maxResults = 64)
    {
        if (maxResults <= 0) return Array.Empty<Body>();
        var raw = new int[maxResults];
        int n = Native.aver_phys_point_query(point.X, point.Y, point.Z, layerMask, ignoreBody.Handle, (int)flags, raw, maxResults);
        return ToBodies(raw, n);
    }
}

public readonly partial struct Body
{
    /// <summary>Sweeps this body's own shape by <paramref name="displacement"/> and returns the first
    /// thing it would touch. See <see cref="Physics.BodyCast"/>.</summary>
    public ShapeHit Cast(Float3 displacement, uint layerMask = Physics.LayerMaskAll, Body ignoreBody = default, QueryFlags flags = QueryFlags.None) =>
        Physics.BodyCast(this, displacement, layerMask, ignoreBody, flags);

    /// <summary>What this body's shape would overlap at another pose. See <see cref="Physics.OverlapBody"/>.</summary>
    public Body[] OverlapAt(Float3 position, Quaternion rotation, uint layerMask = Physics.LayerMaskAll, Body ignoreBody = default, QueryFlags flags = QueryFlags.None, int maxResults = 64) =>
        Physics.OverlapBody(this, position, rotation, layerMask, ignoreBody, flags, maxResults);
}
