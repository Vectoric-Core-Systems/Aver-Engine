// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Script-facing landscape queries: which paint layer is under a point and what it is made of.

using System.Runtime.InteropServices;
using System.Text;
using Aver.Scene;

namespace Aver.Framework;

/// <summary>What the landscape is at one point: height, slope, the dominant paint layer and its physical surface.
/// <see cref="Friction"/> and <see cref="Restitution"/> are the paint-weighted average of the layers under the point.
/// <see cref="Found"/> is false off the landscape section (or with no landscape), and every other field is then its default
/// (an empty <see cref="Surface"/>, friction 0.6, a straight-up normal) when it comes from <see cref="Landscape.Sample(float, float)"/>.</summary>
/// <param name="Found">True when the point is inside the landscape section.</param>
/// <param name="Z">World height of the ground in centimetres.</param>
/// <param name="Normal">Unit ground normal.</param>
/// <param name="DominantLayer">Index of the layer with the most paint here, or 0 when the terrain material has no layers.</param>
/// <param name="Surface">The dominant layer's physical surface name, or an empty string.</param>
/// <param name="SurfaceId"><see cref="Surface.IdOf"/> of <paramref name="Surface"/>; 0 when there is none.</param>
/// <param name="Friction">Weight-averaged layer friction (0.6 when the material has no layers).</param>
/// <param name="Restitution">Weight-averaged layer restitution.</param>
public readonly record struct GroundSample(bool Found, float Z, Vec3 Normal, int DominantLayer, string Surface,
                                           int SurfaceId, float Friction, float Restitution);

/// <summary>The landscape's paint layers, as the level's terrain material defines them. All calls are main-thread and never
/// throw: a missing landscape or a point off the section just answers the defaults.</summary>
public static class Landscape
{
    private const int MaxLayers = 8;
    private const int NameCap = 256, SurfaceCap = 64;
    private const float DefaultFriction = 0.6f;

    // What a query off the landscape answers: not found, empty surface, the default friction.
    private static readonly GroundSample NotFound = new(false, 0.0f, new Vec3(0.0f, 0.0f, 1.0f), 0, string.Empty, 0, DefaultFriction, 0.0f);

    /// <summary>How many paint layers the terrain material has (0 with no landscape or a plain material).</summary>
    public static int LayerCount => Math.Clamp(FwTerrain.aver_fw_terrain_layer_count(), 0, MaxLayers);

    /// <summary>The name of layer <paramref name="layer"/>, or an empty string for an unknown index.</summary>
    public static string LayerName(int layer)
    {
        Span<byte> name = stackalloc byte[NameCap];
        Span<byte> unused = stackalloc byte[1];
        if (FwTerrain.aver_fw_terrain_layer(layer, ref MemoryMarshal.GetReference(name), NameCap,
                ref MemoryMarshal.GetReference(unused), 0, out _, out _) == 0) return string.Empty;
        return Utf8(name);
    }

    /// <summary>The index of the layer called <paramref name="name"/> (case-sensitive), or -1.</summary>
    public static int LayerIndex(string name)
    {
        if (string.IsNullOrEmpty(name)) return -1;
        int n = LayerCount;
        for (int i = 0; i < n; ++i)
            if (LayerName(i) == name) return i;
        return -1;
    }

    /// <summary>Samples the landscape at a world XY (centimetres).</summary>
    public static GroundSample Sample(float xCm, float yCm)
    {
        Span<float> normal = stackalloc float[3];
        Span<float> weights = stackalloc float[MaxLayers];
        if (FwTerrain.aver_fw_terrain_sample(xCm, yCm, out float z, ref MemoryMarshal.GetReference(normal), out int dominant,
                ref MemoryMarshal.GetReference(weights)) == 0)
            return NotFound;

        Span<byte> surfaceBuf = stackalloc byte[SurfaceCap];
        Span<byte> unused = stackalloc byte[1];
        float friction = DefaultFriction, restitution = 0.0f;
        string surface = string.Empty;
        int n = LayerCount;
        if (n > 0)
        {
            friction = 0.0f;
            float total = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                float w = weights[i];
                bool dom = i == dominant;
                if (w <= 0.0f && !dom) continue;
                if (FwTerrain.aver_fw_terrain_layer(i, ref MemoryMarshal.GetReference(unused), 0,
                        ref MemoryMarshal.GetReference(surfaceBuf), dom ? SurfaceCap : 0, out float f, out float r) == 0) continue;
                friction += f * w;
                restitution += r * w;
                total += w;
                if (dom) surface = Utf8(surfaceBuf);
            }
            if (total > 1e-6f) { friction /= total; restitution /= total; }
            else { friction = DefaultFriction; restitution = 0.0f; }
        }
        return new GroundSample(true, z, new Vec3(normal[0], normal[1], normal[2]), dominant, surface,
                                Aver.Framework.Surface.IdOf(surface), friction, restitution);
    }

    /// <summary>Samples the landscape under a world position.</summary>
    public static GroundSample Sample(Vec3 p) => Sample(p.X, p.Y);

    /// <summary>The paint weight (0..1) of layer <paramref name="layer"/> at a world XY; 0 off the section or for an unknown layer.</summary>
    public static float LayerWeight(float xCm, float yCm, int layer)
    {
        if (layer < 0 || layer >= MaxLayers) return 0.0f;
        Span<float> weights = stackalloc float[MaxLayers];
        if (!Weights(xCm, yCm, weights)) return 0.0f;
        return weights[layer];
    }

    /// <summary>The paint weight of the layer called <paramref name="layer"/> at a world XY; 0 for an unknown name.</summary>
    public static float LayerWeight(float xCm, float yCm, string layer) => LayerWeight(xCm, yCm, LayerIndex(layer));

    /// <summary>Writes the weights of every layer (material order, summing to 1) into <paramref name="dst"/>, which must hold
    /// at least 8 floats. False off the section, with <paramref name="dst"/> zeroed.</summary>
    public static bool Weights(float xCm, float yCm, Span<float> dst)
    {
        dst.Clear();
        if (dst.Length < MaxLayers) return false;
        Span<float> normal = stackalloc float[3];
        return FwTerrain.aver_fw_terrain_sample(xCm, yCm, out _, ref MemoryMarshal.GetReference(normal), out _,
                   ref MemoryMarshal.GetReference(dst)) != 0;
    }

    private static string Utf8(ReadOnlySpan<byte> buf)
    {
        int len = buf.IndexOf((byte)0);
        return Encoding.UTF8.GetString(len < 0 ? buf : buf[..len]);
    }
}

/// <summary>Physical-surface ids: graphs and scripts compare integers instead of names. The id is the one the physics
/// registry, a raycast hit and a contact report (<c>Aver.Physics.Surfaces</c>).</summary>
public static partial class Surface
{
    /// <summary>The 32-bit FNV-1a hash of the UTF-8 name, never 0 (a zero hash becomes 1); 0 for a null or empty name.
    /// One definition, shared with <c>Aver.Physics.Surfaces.IdOf</c>.</summary>
    public static int IdOf(string? name) => global::Aver.Physics.Surfaces.IdOf(name);

    /// <summary>The name registered for an id (a <c>.ocsurf</c> table entry, a landscape layer or <c>Surfaces.Register</c>);
    /// an empty string when none is, or with no physics library loaded.</summary>
    public static string NameOf(int surface)
    {
        if (surface == 0) return string.Empty;
        try { return global::Aver.Physics.Surfaces.NameOf(surface); }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return string.Empty; }
    }
}
