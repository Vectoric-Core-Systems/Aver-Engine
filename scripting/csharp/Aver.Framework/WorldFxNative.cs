// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke declarations for framework_worldfx_abi.h.

using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>Which sun, fog or sky-light setting a script has taken over. The numbers are the C ABI's
/// <c>AVER_FW_ENV_*</c> constants (checked by AbiEnumTest).</summary>
public enum WorldEnvKey
{
    /// <summary>The world direction TOWARD the light (x, y, z).</summary>
    SunDir = 1,
    /// <summary>Sun illuminance in lux.</summary>
    SunLux = 2,
    /// <summary>Linear rgb.</summary>
    SunColor = 3,
    /// <summary>Kelvin; 0 uses the colour.</summary>
    SunTemp = 4,
    /// <summary>Fog density per centimetre.</summary>
    FogDensity = 5,
    /// <summary>Linear rgb tint of the in-scattered sky.</summary>
    FogColor = 6,
    /// <summary>How fast the fog thins going up; 0 is uniform distance fog.</summary>
    FogFalloff = 7,
    /// <summary>World Z at which the density is exactly the fog density.</summary>
    FogHeight = 8,
    /// <summary>Distance in front of the camera before fog accumulates, in centimetres.</summary>
    FogStart = 9,
    /// <summary>The most opaque the fog may get, 0 to 1.</summary>
    FogMaxOpac = 10,
    /// <summary>Multiplier on the sky-hemisphere irradiance.</summary>
    SkyLight = 11,
}

/// <summary>The world effects C ABI (framework_worldfx_abi.h). Declared here for the same reason as <see cref="Fw"/>:
/// only this assembly has the DllImport resolver. Names are never passed, only fnv1a64 hashes
/// (<c>Assets.ObjectIdOf</c>). The host-side reads (layer count, info, value, volume scale, tick) are not declared:
/// scripts do not call them.</summary>
internal static class FwWorldFx
{
    private const string Lib = "Aver.Framework";

    [DllImport(Lib)] internal static extern int aver_fw_worldfx_abi_version();

    [DllImport(Lib)] internal static extern int aver_fw_env_set(int key, float v0, float v1, float v2);
    [DllImport(Lib)] internal static extern int aver_fw_env_clear(int key);
    [DllImport(Lib)] internal static extern int aver_fw_env_get(int key, float[]? outV3);

    [DllImport(Lib)] internal static extern int aver_fw_post_set(long layer, long keyHash, float v0, float v1, float v2);
    [DllImport(Lib)] internal static extern int aver_fw_post_set_weight(long layer, float weight, float blendSeconds);
    [DllImport(Lib)] internal static extern int aver_fw_post_clear(long layer);
    [DllImport(Lib)] internal static extern int aver_fw_post_volume_weight(long volumeNameHash, float weight, float blendSeconds);
}

/// <summary>The names a graph's <c>key=</c> attribute uses for <see cref="WorldEnvKey"/>.</summary>
internal static class EnvKeyNames
{
    /// <summary>Resolves a dotted name such as <c>sun.dir</c> or <c>fog.density</c> (case-insensitive; a few aliases).</summary>
    internal static bool TryParse(string? name, out WorldEnvKey key)
    {
        switch ((name ?? "").Trim().ToLowerInvariant())
        {
            case "sun.dir": case "sun.direction": key = WorldEnvKey.SunDir; return true;
            case "sun.lux": case "sun.intensity": key = WorldEnvKey.SunLux; return true;
            case "sun.color": case "sun.colour": key = WorldEnvKey.SunColor; return true;
            case "sun.temp": case "sun.temperature": key = WorldEnvKey.SunTemp; return true;
            case "fog.density": key = WorldEnvKey.FogDensity; return true;
            case "fog.color": case "fog.colour": key = WorldEnvKey.FogColor; return true;
            case "fog.falloff": key = WorldEnvKey.FogFalloff; return true;
            case "fog.height": key = WorldEnvKey.FogHeight; return true;
            case "fog.start": key = WorldEnvKey.FogStart; return true;
            case "fog.maxopacity": case "fog.maxopac": key = WorldEnvKey.FogMaxOpac; return true;
            case "sky.light": case "sky.lightintensity": key = WorldEnvKey.SkyLight; return true;
            default: key = 0; return false;
        }
    }
}
