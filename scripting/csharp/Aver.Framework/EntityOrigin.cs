// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Entity members for the floating origin (docs/plans/wave11/LARGE_WORLD.md 2.5). Entity.WorldPosition keeps its meaning: origin-relative.

using Aver.Scene;

namespace Aver.Framework;

public readonly partial struct Entity
{
    /// <summary>Where this entity is in the whole world, exact at any distance. <see cref="WorldPosition"/> is relative to the floating
    /// origin and changes by a region's width whenever the host rebases; store this in any field that outlives a frame.</summary>
    public WorldPos AbsolutePosition => WorldPos.FromAbsolute(AbsolutePositionCm);

    /// <summary>The same place as absolute centimetres in double precision (exact to 2^53 cm).</summary>
    public Double3 AbsolutePositionCm => WorldOrigin.ToAbsolute(WorldPosition);

    /// <summary>Moves the entity so its world position is <paramref name="position"/>: converted through the current origin in double, then
    /// written like <see cref="SetLocalPosition"/> (through the parent's transform for a child). A point beyond the resident window is
    /// quantised by the single-precision scene, as any position there is.</summary>
    public void SetAbsolutePosition(WorldPos position) => SetAbsolutePositionCm(position.ToAbsolute());

    /// <summary>Moves the entity so its world position is the absolute point <paramref name="cm"/> (centimetres from the world's zero).</summary>
    public void SetAbsolutePositionCm(Double3 cm)
    {
        Vec3 world = WorldOrigin.ToLocal(cm);
        Entity parent = Parent;
        if (!parent.IsValid) { SetLocalPosition(world); return; }
        SetLocalPosition(ParentLocalOf(parent, world));
    }

    // The local position under `parent` whose world position is `world` (row-vector convention: world = local * M + t).
    private static Vec3 ParentLocalOf(Entity parent, Vec3 world)
    {
        float[] m = new float[16];
        if (SceneNative.aver_scene_world_matrix(parent.Handle, m) == 0) return world;
        double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
        double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
        if (Math.Abs(det) < 1e-12) return world;
        double inv = 1.0 / det;
        double i00 = (e * i - f * h) * inv, i01 = (c * h - b * i) * inv, i02 = (b * f - c * e) * inv;
        double i10 = (f * g - d * i) * inv, i11 = (a * i - c * g) * inv, i12 = (c * d - a * f) * inv;
        double i20 = (d * h - e * g) * inv, i21 = (b * g - a * h) * inv, i22 = (a * e - b * d) * inv;
        double x = world.X - m[12], y = world.Y - m[13], z = world.Z - m[14];
        return new Vec3((float)(x * i00 + y * i10 + z * i20), (float)(x * i01 + y * i11 + z * i21), (float)(x * i02 + y * i12 + z * i22));
    }
}
