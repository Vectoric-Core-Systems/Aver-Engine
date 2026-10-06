// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The pooled gameplay-decal spawn API (decal_abi.h). Authored decals are ordinary CDecal components
// (SceneIds.Field("CDecal.tint") etc.); this class is only the runtime spawn side.

using System.Runtime.InteropServices;

namespace Aver.Scene;

/// <summary>What a gameplay decal looks like. Every zero means "default", as on CDecal.</summary>
public struct DecalDesc
{
    /// <summary>World position of the decal's centre, centimetres.</summary>
    public Vec3 Position;
    /// <summary>The surface normal the decal sits on. When non-zero the decal faces into it and
    /// <see cref="Rotation"/> is ignored.</summary>
    public Vec3 Normal;
    /// <summary>Up hint for a surface-aligned decal; zero means +Z.</summary>
    public Vec3 Up;
    /// <summary>Roll about the projection axis in radians (surface-aligned decals).</summary>
    public float RollRadians;
    /// <summary>Pose used when <see cref="Normal"/> is zero. The decal projects along its local +X.</summary>
    public Quat Rotation;
    /// <summary>Full box size: X depth, Y width, Z height, centimetres. Zero axes read as 100.</summary>
    public Vec3 SizeCm;
    /// <summary>Linear colour multiplying the base texture; all zero reads as white.</summary>
    public Vec3 Tint;
    /// <summary>0 opaque .. 1 invisible.</summary>
    public float Transparency;
    public float NormalStrength;
    public float Roughness;
    public float Metallic;
    public float EdgeFade;
    public float AngleFadeStartDegrees;
    public float AngleFadeEndDegrees;
    public float FadeDistanceCm;
    public float UvScaleU, UvScaleV, UvOffsetU, UvOffsetV;
    /// <summary>Seconds until the decal is gone; 0 keeps it until the pool recycles it.</summary>
    public float LifetimeSeconds;
    /// <summary>Fade-out at the end of the lifetime.</summary>
    public float FadeOutSeconds;
    public int SortOrder;
    public DecalChannels Disable;
    /// <summary>Texture ObjectIds: <see cref="Assets.ObjectIdOf"/> of the content-relative path. 0 = none.</summary>
    public long BaseTexture, NormalTexture, OrmTexture;

    /// <summary>A decal at <paramref name="position"/> on a surface with normal <paramref name="normal"/>.</summary>
    public static DecalDesc OnSurface(Vec3 position, Vec3 normal, Vec3 sizeCm) =>
        new() { Position = position, Normal = normal, SizeCm = sizeCm, Rotation = Quat.Identity };
}

/// <summary>Channels a decal can opt out of (CDecal flag bits).</summary>
[System.Flags]
public enum DecalChannels
{
    None = 0,
    Colour = 0x2,
    Normal = 0x4,
    Roughness = 0x8,
}

/// <summary>The pooled gameplay-decal API: impacts, footprints, scorch marks. The pool recycles its
/// oldest decal when full, so spawning never allocates once it has warmed up.</summary>
public static class Decals
{
    /// <summary>The fixed component id of CDecal (Components.hpp).</summary>
    public const int ComponentId = 16;

    /// <summary>The ABI version the DLL was built with.</summary>
    public static int AbiVersion => DecalNative.aver_decal_abi_version();

    /// <summary>Sets how many pooled decals may be alive at once (default 256), dropping the current ones.
    /// Returns the capacity in force; 0 disables spawning.</summary>
    public static int SetCapacity(int capacity) => DecalNative.aver_decal_pool_set_capacity(capacity);

    /// <summary>Spawns a decal and returns its entity, or 0 when the pool is disabled.</summary>
    public static int Spawn(in DecalDesc d)
    {
        var n = new DecalNative.Desc
        {
            baseTexture = d.BaseTexture, normalTexture = d.NormalTexture, ormTexture = d.OrmTexture,
            position0 = d.Position.X, position1 = d.Position.Y, position2 = d.Position.Z,
            normal0 = d.Normal.X, normal1 = d.Normal.Y, normal2 = d.Normal.Z,
            up0 = d.Up.X, up1 = d.Up.Y, up2 = d.Up.Z,
            rotation0 = d.Rotation.X, rotation1 = d.Rotation.Y, rotation2 = d.Rotation.Z, rotation3 = d.Rotation.W,
            rollRad = d.RollRadians,
            size0 = d.SizeCm.X, size1 = d.SizeCm.Y, size2 = d.SizeCm.Z,
            tint0 = d.Tint.X, tint1 = d.Tint.Y, tint2 = d.Tint.Z,
            transparency = d.Transparency, normalStrength = d.NormalStrength,
            roughness = d.Roughness, metallic = d.Metallic, edgeFade = d.EdgeFade,
            angleFadeStartDeg = d.AngleFadeStartDegrees, angleFadeEndDeg = d.AngleFadeEndDegrees,
            fadeDistanceCm = d.FadeDistanceCm,
            uvScale0 = d.UvScaleU, uvScale1 = d.UvScaleV, uvOffset0 = d.UvOffsetU, uvOffset1 = d.UvOffsetV,
            lifetimeSec = d.LifetimeSeconds, fadeOutSec = d.FadeOutSeconds,
            sortOrder = d.SortOrder, flags = (uint)d.Disable,
        };
        return DecalNative.aver_decal_spawn(ref n);
    }

    /// <summary>Gives one decal back now. True when it was an active pooled decal.</summary>
    public static bool Release(int entity) => DecalNative.aver_decal_release(entity) != 0;

    /// <summary>Releases every pooled decal.</summary>
    public static void Clear() => DecalNative.aver_decal_clear();

    /// <summary>Advances decal lifetimes; the game loop calls this once per frame.</summary>
    public static void Tick(float deltaSeconds) => DecalNative.aver_decal_tick(deltaSeconds);

    /// <summary>Pooled decals currently visible.</summary>
    public static int ActiveCount => DecalNative.aver_decal_active_count();

    /// <summary>The pool's capacity.</summary>
    public static int Capacity => DecalNative.aver_decal_capacity();
}

internal static class DecalNative
{
    private const string Lib = "Aver.Scene";

    // AverDecalSpawnDesc, field for field. Vector members are spelled as scalars so the managed
    // layout cannot differ from the C struct by array marshalling.
    [StructLayout(LayoutKind.Sequential)]
    internal struct Desc
    {
        public long baseTexture, normalTexture, ormTexture;
        public float position0, position1, position2;
        public float normal0, normal1, normal2;
        public float up0, up1, up2;
        public float rotation0, rotation1, rotation2, rotation3;
        public float rollRad;
        public float size0, size1, size2;
        public float tint0, tint1, tint2;
        public float transparency, normalStrength, roughness, metallic, edgeFade;
        public float angleFadeStartDeg, angleFadeEndDeg, fadeDistanceCm;
        public float uvScale0, uvScale1, uvOffset0, uvOffset1;
        public float lifetimeSec, fadeOutSec;
        public int sortOrder;
        public uint flags;
    }

    [DllImport(Lib)] internal static extern int aver_decal_abi_version();
    [DllImport(Lib)] internal static extern int aver_decal_pool_set_capacity(int capacity);
    [DllImport(Lib)] internal static extern int aver_decal_spawn(ref Desc desc);
    [DllImport(Lib)] internal static extern int aver_decal_release(int entity);
    [DllImport(Lib)] internal static extern void aver_decal_clear();
    [DllImport(Lib)] internal static extern void aver_decal_tick(float dt);
    [DllImport(Lib)] internal static extern int aver_decal_active_count();
    [DllImport(Lib)] internal static extern int aver_decal_capacity();
}

/// <summary>What the AN_SpawnDecal / AN_ClearDecals / AN_SetDecalCapacity nodes call: scalar-only
/// signatures (a graph pin is float, int or bool), with the three texture paths as node attributes.</summary>
internal static class DecalGraph
{
    internal static int SpawnDecalForGraph(float x, float y, float z, float nx, float ny, float nz,
                                           float sx, float sy, float sz, float roll, float lifetime,
                                           float fadeOut, string baseTexture, string normalTexture, string ormTexture)
    {
        var d = DecalDesc.OnSurface(new Vec3(x, y, z), new Vec3(nx, ny, nz), new Vec3(sx, sy, sz));
        d.RollRadians = roll;
        d.LifetimeSeconds = lifetime;
        d.FadeOutSeconds = fadeOut;
        if (!string.IsNullOrEmpty(baseTexture)) d.BaseTexture = Assets.ObjectIdOf(baseTexture);
        if (!string.IsNullOrEmpty(normalTexture)) d.NormalTexture = Assets.ObjectIdOf(normalTexture);
        if (!string.IsNullOrEmpty(ormTexture)) d.OrmTexture = Assets.ObjectIdOf(ormTexture);
        try { return Decals.Spawn(d); }
        catch (DllNotFoundException) { return 0; }
    }

    internal static bool ClearDecalsForGraph()
    {
        try { Decals.Clear(); return true; }
        catch (DllNotFoundException) { return false; }
    }

    internal static bool SetDecalCapacityForGraph(int capacity)
    {
        try { return Decals.SetCapacity(capacity) == capacity; }
        catch (DllNotFoundException) { return false; }
    }
}
