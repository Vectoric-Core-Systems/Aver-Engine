// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// What a hit, a contact, the ground under a character and a wheel's tyre report about what they touched
// (physics_surface_abi.h): the body, the surface and its numbers, the compound child, the mesh triangle's user data and the
// heightfield cell.

using System.Collections.Generic;

namespace Aver.Physics;

/// <summary>What a ray, a character's ground or a wheel touched. <see cref="Friction"/> and <see cref="Restitution"/> are the
/// numbers the surface carries (-1 when it defers to the body), not necessarily what a contact applied. <see cref="SubShape"/>
/// is a raw Jolt id, valid only while the shape is unchanged. <see cref="Child"/>, <see cref="Triangle"/> and
/// <see cref="CellX"/>/<see cref="CellY"/> are -1 when they do not apply; <see cref="Triangle"/> needs the mesh to have been
/// built with per-triangle user data.</summary>
public readonly record struct SurfaceHit(bool Hit, Body Body, int Entity, Float3 Point, Float3 Normal, float DistanceCm, int SurfaceId,
                                         float Friction, float Restitution, int Tag, int SubShape, int Child, int ChildUserData,
                                         int Triangle, int CellX, int CellY);

/// <summary>One contact of the last step, index-aligned with <see cref="Physics.Contacts"/>. <c>ClosingSpeedCmS</c> is the
/// relative speed along the contact normal when the contact began (positive = approaching); <c>Friction</c> and
/// <c>Restitution</c> are what the contact applies, leaf materials and any surface sampler included.</summary>
public readonly record struct ContactDetail(int SubShapeA, int SubShapeB, int SurfaceA, int SurfaceB, float ClosingSpeedCmS,
                                            float Friction, float Restitution);

internal static class SurfaceBlocks
{
    internal static SurfaceHit Read(float[] f, int[] i) => new(
        true, new Body(i[(int)HitInt.Body]), i[(int)HitInt.Entity],
        new Float3(f[(int)HitFloat.PointX], f[(int)HitFloat.PointY], f[(int)HitFloat.PointZ]),
        new Float3(f[(int)HitFloat.NormalX], f[(int)HitFloat.NormalY], f[(int)HitFloat.NormalZ]),
        f[(int)HitFloat.DistanceCm], i[(int)HitInt.Surface], f[(int)HitFloat.Friction], f[(int)HitFloat.Restitution],
        i[(int)HitInt.Tag], i[(int)HitInt.SubShape], i[(int)HitInt.Child], i[(int)HitInt.ChildUserData],
        i[(int)HitInt.Triangle], i[(int)HitInt.CellX], i[(int)HitInt.CellY]);

    internal static float[] NewF() => new float[(int)HitFloat.Count];
    internal static int[] NewI() => new int[(int)HitInt.Count];
}

public static partial class Physics
{
    /// <summary>A ray that reports what it hit: the body, the surface and its numbers, the compound child, the mesh triangle's
    /// user data and the heightfield cell. <c>Hit</c> is false on a miss. Sensors are skipped unless asked.</summary>
    public static SurfaceHit RaycastDetail(Float3 origin, Float3 direction, float maxDistanceCm, uint layerMask = LayerMaskAll,
                                           Body ignoreBody = default, QueryFlags flags = QueryFlags.None)
    {
        var f = SurfaceBlocks.NewF(); var i = SurfaceBlocks.NewI();
        int body = Native.aver_phys_raycast_detail(origin.X, origin.Y, origin.Z, direction.X, direction.Y, direction.Z, maxDistanceCm,
                                                   layerMask, ignoreBody.Handle, (int)flags, f, f.Length, i, i.Length);
        return body == 0 ? default : SurfaceBlocks.Read(f, i);
    }

    /// <summary>The same detail for a (body, sub-shape) pair taken from a hit, a contact or the ground; point, normal and
    /// distance are zero. <c>Hit</c> is false for a dead body.</summary>
    public static SurfaceHit SubShapeInfo(Body body, int subShape)
    {
        var f = SurfaceBlocks.NewF(); var i = SurfaceBlocks.NewI();
        return Native.aver_phys_subshape_info(body.Handle, subShape, f, f.Length, i, i.Length) == 0 ? default : SurfaceBlocks.Read(f, i);
    }

    /// <summary>The contacts that began in the last step with their sub-shapes, surfaces, closing speed and the friction
    /// applied. Entry <c>n</c> belongs to <see cref="Contacts"/>[<c>n</c>].</summary>
    public static IReadOnlyList<ContactDetail> ContactDetails
    {
        get
        {
            int n = Native.aver_phys_contact_count();
            var list = new List<ContactDetail>(n);
            var f = new float[(int)ContactFloat.Count]; var iv = new int[(int)ContactInt.Count];
            for (int k = 0; k < n; k++)
            {
                if (Native.aver_phys_contact_detail(k, f, f.Length, iv, iv.Length) == 0) { list.Add(default); continue; }
                list.Add(new ContactDetail(iv[(int)ContactInt.SubShapeA], iv[(int)ContactInt.SubShapeB],
                                           iv[(int)ContactInt.SurfaceA], iv[(int)ContactInt.SurfaceB],
                                           f[(int)ContactFloat.ClosingSpeedCms], f[(int)ContactFloat.Friction],
                                           f[(int)ContactFloat.Restitution]));
            }
            return list;
        }
    }
}

/// <summary>Ground and wheel surface queries on the types that own them.</summary>
public static class SurfaceHitExtensions
{
    /// <summary>What the character stands on (or touches, when it is not supported). <c>Hit</c> is false in the air.</summary>
    public static SurfaceHit GroundDetail(this CharacterBody character)
    {
        var f = SurfaceBlocks.NewF(); var i = SurfaceBlocks.NewI();
        return Native.aver_phys_character_ground_detail(character.Handle, f, f.Length, i, i.Length) == 0 ? default : SurfaceBlocks.Read(f, i);
    }
}

public readonly partial struct Vehicle
{
    /// <summary>What wheel <paramref name="wheel"/> of a finished vehicle is touching. <c>Hit</c> is false in the air.</summary>
    public SurfaceHit WheelSurface(int wheel)
    {
        var f = SurfaceBlocks.NewF(); var i = SurfaceBlocks.NewI();
        return Native.aver_phys_vehicle_wheel_surface(Handle, wheel, f, f.Length, i, i.Length) == 0 ? default : SurfaceBlocks.Read(f, i);
    }
}
