// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Render settings and statistics a script can ask for while the game runs (P17, P18). The C ABI is framework_render_abi.h; the host reads it once per frame.

using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>Which render setting a script has taken over. The numbers are the C ABI's <c>AVER_FW_RENDER_*</c> constants (checked by AbiEnumTest).</summary>
public enum RenderKey
{
    /// <summary>3D render scale, 0.25 to 2.</summary>
    Scale = 1,
    /// <summary>Upscaler sharpness, 0 to 1. Stored: the upscaler is chosen when the project starts.</summary>
    Sharpness = 2,
    /// <summary>Temporal anti-aliasing, 0 or 1. Stored: it is chosen when the project starts.</summary>
    Taa = 3,
    /// <summary>MSAA samples: 1, 2, 4 or 8.</summary>
    Msaa = 4,
    /// <summary>Mesh LOD pixel-error threshold, 0 to 64. Applied by the editor; the shipped runtime has no LOD selection.</summary>
    LodThreshold = 5,
    /// <summary>Occlusion culling, 0 or 1. Applied by the editor; the shipped runtime has no occlusion culling.</summary>
    Occlusion = 6,
    /// <summary>Sun shadow cascades, 1 to 4.</summary>
    ShadowCascades = 7,
    /// <summary>Cascade split scheme, 0 (even) to 1 (logarithmic).</summary>
    ShadowSplit = 8,
    /// <summary>Farthest shadowed distance in centimetres; 0 is automatic.</summary>
    ShadowDistance = 9,
    /// <summary>Sun shadow filter width in shadow texels, 0 (hard) to 4.</summary>
    ShadowFilter = 10,
    /// <summary>Light-shaft fog scattering anisotropy, -0.95 to 0.95.</summary>
    VolFogAniso = 11,
    /// <summary>Light-shaft fog reach in centimetres, 100 to 500000.</summary>
    VolFogLength = 12,
    /// <summary>Light-shaft fog history weight, 0 to 0.98.</summary>
    VolFogTemporal = 13,
    /// <summary>Multiplier on the sun's scattered light in the fog, 0 to 16.</summary>
    VolFogIntensity = 14,
}

/// <summary>What the last frame submitted (<see cref="RenderSettings.GetStats"/>).</summary>
/// <param name="Objects">Meshes in the scene's draw list.</param>
/// <param name="Triangles">Triangles of those meshes.</param>
/// <param name="DrawCalls">Raster scene draws; 0 while a ray-driven frame draws the scene itself.</param>
/// <param name="Translucent">Blended draws among the objects.</param>
/// <param name="ShadowDraws">Draws the sun shadow pass submitted, summed over cascades (0 while ray-traced shadows are on).</param>
/// <param name="Frame">The host's frame number when these were published.</param>
public readonly record struct RenderStats(int Objects, int Triangles, int DrawCalls, int Translucent, int ShadowDraws, int Frame);

internal static class FwRender
{
    private const string Lib = "Aver.Framework";

    [DllImport(Lib)] internal static extern int aver_fw_render_abi_version();
    [DllImport(Lib)] internal static extern int aver_fw_render_set(int key, float value);
    [DllImport(Lib)] internal static extern int aver_fw_render_clear(int key);
    [DllImport(Lib)] internal static extern int aver_fw_render_get(int key, out float value);
    [DllImport(Lib)] internal static extern int aver_fw_render_stats(int[] out6);
}

/// <summary>Render settings a script can change at run time. A request, not the truth: each setter overrides the project's value until it is released, and
/// releasing it puts the project's value back (the host applies the change at the next frame boundary). Values outside a key's range are clamped; a non-finite
/// value is refused and the setter returns false. In the editor these apply during Play only. See <see cref="RenderKey"/> for what each host applies.
/// Post-processing (glow, debanding, exposure) is not here: those are post-volume keys (<c>Post.SetValue</c>).</summary>
public static class RenderSettings
{
    /// <summary>Overrides one setting. False for a non-finite value.</summary>
    public static bool Set(RenderKey key, float value) => FwRender.aver_fw_render_set((int)key, value) != 0;

    /// <summary>The value a script has set for the key, or null when it holds none.</summary>
    public static float? Get(RenderKey key) => FwRender.aver_fw_render_get((int)key, out float v) != 0 ? v : null;

    /// <summary>Releases one setting, so the project's value returns.</summary>
    public static bool Clear(RenderKey key) => FwRender.aver_fw_render_clear((int)key) != 0;

    /// <summary>Releases every setting.</summary>
    public static void ClearAll() => FwRender.aver_fw_render_clear(0);

    /// <summary>3D render scale (Godot's <c>scaling_3d_scale</c>), 0.25 to 2.</summary>
    public static bool SetScale(float scale) => Set(RenderKey.Scale, scale);

    /// <summary>MSAA samples: 1, 2, 4 or 8 (anything between snaps down to a power of two).</summary>
    public static bool SetMsaa(int samples) => Set(RenderKey.Msaa, samples);

    /// <summary>The sun's shadow cascades: how many (1 to 4), the split scheme (0 even to 1 logarithmic), the farthest shadowed distance in centimetres
    /// (0 is automatic) and the filter width in shadow texels (1 is the engine's own 3x3 kernel, 0 a hard edge).</summary>
    public static bool SetSunShadows(int cascades, float split, float maxDistanceCm, float filterWidth)
    {
        bool ok = Set(RenderKey.ShadowCascades, cascades);
        ok &= Set(RenderKey.ShadowSplit, split);
        ok &= Set(RenderKey.ShadowDistance, maxDistanceCm);
        return Set(RenderKey.ShadowFilter, filterWidth) && ok;
    }

    /// <summary>What the last frame submitted. Asking turns the counting on: nothing is counted until a script has asked, so the first call returns null; the
    /// next frame's numbers follow. Correct in ray-driven mode, where <see cref="RenderStats.DrawCalls"/> is 0.</summary>
    public static RenderStats? GetStats()
    {
        var s = new int[6];
        return FwRender.aver_fw_render_stats(s) != 0 ? new RenderStats(s[0], s[1], s[2], s[3], s[4], s[5]) : null;
    }
}
