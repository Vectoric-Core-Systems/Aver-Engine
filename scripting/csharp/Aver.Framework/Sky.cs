// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
using Aver.Scene;

namespace Aver.Framework;

/// <summary>
/// The sky as a script sees it: the cloud layer, and overrides for the sun, the fog and the sky light.
/// </summary>
/// <remarks>
/// <para>
/// A REQUEST, NOT THE TRUTH. Until something calls <see cref="SetClouds"/> the host renders the sky
/// the level authored, so a project that never touches this behaves exactly as it did before this
/// type existed. <see cref="Clear"/> hands the sky back.
/// </para>
/// <para>
/// This is the only route from a script to the sky. Nothing under the scripting or framework
/// modules mentioned PCG at all before it, so a project's sky could only ever be numbers typed into
/// its <c>.ocworld</c> — which is fine for a fixed sky and useless for one that is supposed to be
/// generated.
/// </para>
/// <para>
/// Lengths are CENTIMETRES and wind is centimetres per second, like everything else in this engine.
/// </para>
/// </remarks>
public static class Sky
{
    /// <summary>Publishes a cloud layer. Any later call replaces the whole request.</summary>
    /// <param name="seed">Which sky this is. Two seeds give different cloud fields from one set of
    /// settings; 0 is the unseeded field and reproduces the engine's original output exactly.</param>
    /// <param name="coverage">0 clear, 1 overcast. Clamped by the host.</param>
    /// <param name="density">Optical thickness of what is there. Negative is clamped to 0.</param>
    /// <param name="bottomCm">World Z of the layer's base.</param>
    /// <param name="topCm">World Z of its top.</param>
    /// <param name="featureScale">1 / the width of one noise feature, in world units.</param>
    /// <param name="windXCmPerSec">Drift along +X.</param>
    /// <param name="windYCmPerSec">Drift along +Y.</param>
    public static void SetClouds(int seed, float coverage, float density = 1.0f,
                                 float bottomCm = 150000.0f, float topCm = 280000.0f,
                                 float featureScale = 0.00002f,
                                 float windXCmPerSec = 900.0f, float windYCmPerSec = 260.0f)
        => Fw.aver_fw_set_sky_clouds(seed, coverage, density, bottomCm, topCm,
                                     featureScale, windXCmPerSec, windYCmPerSec);

    /// <summary>Drops the request, so the level's own sky takes over again.</summary>
    public static void Clear() => Fw.aver_fw_clear_sky_clouds();

    // ---- sun, fog and sky light ------------------------------------------------------------------
    //
    // The same request-not-truth rule as the clouds: each setter overrides one setting until it is
    // released, and releasing hands the level's authored value back. Values are sanitised by the
    // framework (negatives clamp to 0, the sun direction is normalised); a non-finite value or a
    // zero direction is refused and the setter returns false. In the editor these apply during Play only.

    /// <summary>Points the sun: the world direction TOWARD the light. A zero vector is refused.</summary>
    public static bool SetSunDirection(float x, float y, float z) => Env(WorldEnvKey.SunDir, x, y, z);

    /// <summary>Points the sun: the world direction TOWARD the light. A zero vector is refused.</summary>
    public static bool SetSunDirection(Vec3 towardLight) => SetSunDirection(towardLight.X, towardLight.Y, towardLight.Z);

    /// <summary>Sun illuminance in lux (about 100000 on a clear day). Negative clamps to 0.</summary>
    public static bool SetSunIntensity(float lux) => Env(WorldEnvKey.SunLux, lux, 0f, 0f);

    /// <summary>Sun colour as linear rgb, or a colour temperature in kelvin when <paramref name="temperatureK"/>
    /// is above 0 (it replaces the colour; 0 uses the colour).</summary>
    public static bool SetSunColor(float r, float g, float b, float temperatureK = 0f)
    {
        bool ok = Env(WorldEnvKey.SunColor, r, g, b);
        return Env(WorldEnvKey.SunTemp, temperatureK, 0f, 0f) && ok;
    }

    /// <summary>Fog density per centimetre (the level's default is about 0.000004).</summary>
    public static bool SetFogDensity(float perCm) => Env(WorldEnvKey.FogDensity, perCm, 0f, 0f);

    /// <summary>Fog tint as linear rgb; white is clear air.</summary>
    public static bool SetFogColor(float r, float g, float b) => Env(WorldEnvKey.FogColor, r, g, b);

    /// <summary>The shape of the fog: how fast it thins going up (0 is uniform), the world Z at which the density is
    /// exactly the fog density, the distance in front of the camera before it starts (cm), and the most opaque it gets.</summary>
    public static bool SetFogShape(float falloff, float heightZ, float startCm, float maxOpacity = 1f)
    {
        bool ok = Env(WorldEnvKey.FogFalloff, falloff, 0f, 0f);
        ok &= Env(WorldEnvKey.FogHeight, heightZ, 0f, 0f);
        ok &= Env(WorldEnvKey.FogStart, startCm, 0f, 0f);
        return Env(WorldEnvKey.FogMaxOpac, maxOpacity, 0f, 0f) && ok;
    }

    /// <summary>Multiplier on the light the sky hemisphere gives the scene (1 is the level's own).</summary>
    public static bool SetSkyLight(float multiplier) => Env(WorldEnvKey.SkyLight, multiplier, 0f, 0f);

    /// <summary>Releases every sun, fog and sky-light override, so the level's authored values return.</summary>
    public static void ClearOverrides() => FwWorldFx.aver_fw_env_clear(0);

    /// <summary>Releases one override.</summary>
    public static bool ClearOverride(WorldEnvKey key) => FwWorldFx.aver_fw_env_clear((int)key) != 0;

    /// <summary>Releases one override by its dotted name (<c>sun.dir</c>, <c>sun.lux</c>, <c>sun.color</c>, <c>sun.temp</c>,
    /// <c>fog.density</c>, <c>fog.color</c>, <c>fog.falloff</c>, <c>fog.height</c>, <c>fog.start</c>, <c>fog.maxopacity</c>,
    /// <c>sky.light</c>); null or empty releases all of them. False for a name that is not one of those.</summary>
    public static bool ClearOverride(string? name)
    {
        if (string.IsNullOrWhiteSpace(name)) { ClearOverrides(); return true; }
        return EnvKeyNames.TryParse(name, out WorldEnvKey key) && ClearOverride(key);
    }

    /// <summary>True while a script holds this setting.</summary>
    public static bool IsOverridden(WorldEnvKey key) => FwWorldFx.aver_fw_env_get((int)key, null) != 0;

    private static bool Env(WorldEnvKey key, float v0, float v1, float v2) => FwWorldFx.aver_fw_env_set((int)key, v0, v1, v2) != 0;
}
