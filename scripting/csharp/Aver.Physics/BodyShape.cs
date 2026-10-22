// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// What a body is made of, and changing it while it runs (physics_shape_builder_abi.h): the shape handle behind a
// body, a new shape or scale, mutable-compound children, and the collision triangles for an overlay.

using System;

namespace Aver.Physics;

public readonly partial struct Body
{
    /// <summary>A new handle on this body's current shape (release it when done). <see cref="CollisionShape.None"/>
    /// for a character or a soft body.</summary>
    public CollisionShape Shape() => new(Native.aver_phys_shape_of_body(Handle));

    /// <summary>Swaps the shape; the body's origin stays where it was. <paramref name="keepMass"/> keeps the old mass,
    /// otherwise the new shape's volume times its density. Refused for a character, a soft body, a vehicle chassis and
    /// a ragdoll part.</summary>
    public bool SetShape(CollisionShape shape, bool keepMass = true) =>
        Native.aver_phys_body_set_shape(Handle, shape.Handle, keepMass ? 1 : 0) != 0;

    /// <summary>Scale relative to the unscaled shape (absolute, not cumulative). Mass follows volume unless
    /// <paramref name="keepMass"/>.</summary>
    public bool SetScale(Float3 scale, bool keepMass = false) =>
        Native.aver_phys_body_set_scale(Handle, scale.ToArray(), keepMass ? 1 : 0) != 0;

    /// <summary>The scale <see cref="SetScale"/> or a scaled placement gave the body; (1, 1, 1) when unscaled.</summary>
    public Float3 Scale
    {
        get
        {
            var v = new float[3];
            return Native.aver_phys_body_scale(Handle, v) != 0 ? new Float3(v[0], v[1], v[2]) : new Float3(1, 1, 1);
        }
    }

    /// <summary>The shape's origin in the world. <see cref="Position"/> reports the centre of mass, which differs for a
    /// compound or an offset shape: use this one to place a model.</summary>
    public Float3 Origin
    {
        get
        {
            var v = new float[3];
            return Native.aver_phys_body_origin(Handle, v) != 0 ? new Float3(v[0], v[1], v[2]) : Float3.Zero;
        }
    }

    /// <summary>Jolt's enhanced internal edge removal: smoother motion over a mesh with convex edges, a little dearer.</summary>
    public bool SetEnhancedEdges(bool on) => Native.aver_phys_body_set_enhanced_edges(Handle, on ? 1 : 0) != 0;

    /// <summary>Adds a child to a mutable compound this body owns; the new child's index, or -1. Raw sub-shape ids taken
    /// before are no longer valid.</summary>
    public int AddChild(CollisionShape child, Float3 position, Quaternion rotation, int userData = 0) =>
        Native.aver_phys_body_compound_add(Handle, child.Handle, position.ToArray(), CollisionShape.Quat(rotation), userData);

    /// <summary>Removes a child (never the last one); later children shift down by one.</summary>
    public bool RemoveChild(int index) => Native.aver_phys_body_compound_remove(Handle, index) != 0;

    /// <summary>Moves a child, in the compound's own frame.</summary>
    public bool SetChildTransform(int index, Float3 position, Quaternion rotation) =>
        Native.aver_phys_body_compound_set_child(Handle, index, position.ToArray(), CollisionShape.Quat(rotation)) != 0;

    /// <summary>The children of a compound shape; -1 when the body is not a compound.</summary>
    public int ChildCount => Native.aver_phys_body_compound_count(Handle);

    /// <summary>The collision triangles in a world box (nine floats each, engine cm) with a surface id per triangle.
    /// Fills as many as fit in <paramref name="xyz"/> (9 floats each) and <paramref name="surfaces"/> (when non-empty);
    /// returns the count.</summary>
    public int Triangles(Float3 min, Float3 max, Span<float> xyz, Span<int> surfaces)
    {
        int max3 = xyz.Length / 9;
        if (surfaces.Length > 0) max3 = Math.Min(max3, surfaces.Length);
        if (max3 <= 0) return 0;
        var f = new float[max3 * 9];
        var s = surfaces.Length > 0 ? new int[max3] : null;
        int n = Native.aver_phys_body_triangles(Handle, min.ToArray(), max.ToArray(), f, s, max3);
        if (n <= 0) return 0;
        f.AsSpan(0, n * 9).CopyTo(xyz);
        if (s is not null) s.AsSpan(0, n).CopyTo(surfaces);
        return n;
    }
}
