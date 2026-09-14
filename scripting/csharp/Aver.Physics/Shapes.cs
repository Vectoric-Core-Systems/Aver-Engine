// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
//
// Rigid-body shapes beyond box, sphere, convex hull, mesh and heightfield (those live on Physics).
// See physics_shapes_abi.h's own file comment for why capsule/tapered-capsule height is TOTAL (both
// caps included) while a cylinder's is the full end-to-end height with no cap radius to add.

using System;

namespace Aver.Physics;

/// <summary>Capsule, cylinder, tapered capsule and box-compound creators.</summary>
/// <remarks>Engine units and axes: centimetres, upright along +Z.</remarks>
public static class Shapes
{
    // ---- Capsule ------------------------------------------------------------------------------------

    /// <summary>A capsule that never moves. <paramref name="height"/> is TOTAL, both hemispherical
    /// caps included.</summary>
    public static Body AddStaticCapsule(Float3 centre, float radius, float height) =>
        new(Native.aver_phys_add_static_capsule(centre.X, centre.Y, centre.Z, radius, height));

    /// <summary>A capsule that falls and collides. <paramref name="massKg"/> &lt;= 0 derives mass from the volume.</summary>
    public static Body AddDynamicCapsule(Float3 centre, float radius, float height, float massKg = 0f) =>
        new(Native.aver_phys_add_dynamic_capsule(centre.X, centre.Y, centre.Z, radius, height, massKg));

    // ---- Cylinder -------------------------------------------------------------------------------------
    // Flat-ended: a drum, a wheel on its side, a pipe segment. Height is the FULL end-to-end length --
    // a cylinder has no cap radius of its own to add, unlike the capsule above.

    public static Body AddStaticCylinder(Float3 centre, float radius, float height) =>
        new(Native.aver_phys_add_static_cylinder(centre.X, centre.Y, centre.Z, radius, height));

    public static Body AddDynamicCylinder(Float3 centre, float radius, float height, float massKg = 0f) =>
        new(Native.aver_phys_add_dynamic_cylinder(centre.X, centre.Y, centre.Z, radius, height, massKg));

    // ---- Tapered capsule -----------------------------------------------------------------------------
    // A capsule whose two caps have different radii: a forearm thicker at the elbow than the wrist.
    // topRadius is the cap at +Z, bottomRadius the cap at -Z. Height is TOTAL, both caps included.

    public static Body AddStaticTaperedCapsule(Float3 centre, float topRadius, float bottomRadius, float height) =>
        new(Native.aver_phys_add_static_tapered_capsule(centre.X, centre.Y, centre.Z, topRadius, bottomRadius, height));

    public static Body AddDynamicTaperedCapsule(Float3 centre, float topRadius, float bottomRadius, float height, float massKg = 0f) =>
        new(Native.aver_phys_add_dynamic_tapered_capsule(centre.X, centre.Y, centre.Z, topRadius, bottomRadius, height, massKg));

    // ---- Compound of boxes ---------------------------------------------------------------------------
    // N boxes welded into ONE rigid body -- a chair, a table -- rather than N separate bodies held
    // together by joints. offsets/halfExtents are in the BODY'S OWN LOCAL FRAME: offsets[i] is that
    // box's centre relative to `centre`, not a world position. Every box shares the compound's one
    // rotation and motion type.

    public static Body AddStaticCompoundBoxes(Float3 centre, Float3[] offsets, Float3[] halfExtents)
    {
        int count = RequireMatchingLength(offsets, halfExtents);
        return new(Native.aver_phys_add_static_compound_boxes(centre.X, centre.Y, centre.Z, Flatten(offsets), Flatten(halfExtents), count));
    }

    public static Body AddDynamicCompoundBoxes(Float3 centre, Float3[] offsets, Float3[] halfExtents, float massKg = 0f)
    {
        int count = RequireMatchingLength(offsets, halfExtents);
        return new(Native.aver_phys_add_dynamic_compound_boxes(centre.X, centre.Y, centre.Z, Flatten(offsets), Flatten(halfExtents), count, massKg));
    }

    private static int RequireMatchingLength(Float3[] offsets, Float3[] halfExtents)
    {
        if (offsets.Length != halfExtents.Length)
            throw new ArgumentException($"{nameof(offsets)} and {nameof(halfExtents)} must have the same length (one box each), got {offsets.Length} and {halfExtents.Length}.");
        return offsets.Length;
    }

    private static float[] Flatten(Float3[] points)
    {
        var flat = new float[points.Length * 3];
        for (int i = 0; i < points.Length; i++) { flat[i * 3] = points[i].X; flat[i * 3 + 1] = points[i].Y; flat[i * 3 + 2] = points[i].Z; }
        return flat;
    }
}
