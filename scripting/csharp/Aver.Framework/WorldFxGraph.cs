// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The graph-node surface for the sun, fog, sky-light and post-layer nodes: thin wrappers over Sky and PostFx with the
// primitive-only signatures GraphCompiler's emitted IL needs. A failure (a refused value, no host) returns false and
// is logged once; nothing throws. Rows: scripting/csharp/Aver.Graph/GraphWorldFxNodeTable.cs.

using Aver.Scripting;

namespace Aver.Framework;

internal static class WorldFxGraph
{
    private static readonly HashSet<string> s_logged = new();

    private static void LogOnce(string key, string message)
    {
        if (s_logged.Add(key)) Log.Warn(message);
    }

    private static bool Note(bool ok, string what)
    {
        if (!ok) LogOnce(what, $"[WorldFx] {what} was refused (a non-finite value, an unknown key or name, a full table, or no host)");
        return ok;
    }

    private static bool Failed(string what, Exception e)
    {
        LogOnce(what + ":threw", $"[WorldFx] {what} failed: {e.GetType().Name}: {e.Message}");
        return false;
    }

    internal static bool SetSunDirectionForGraph(float x, float y, float z)
    {
        try { return Note(Sky.SetSunDirection(x, y, z), "SetSunDirection"); }
        catch (Exception e) { return Failed("SetSunDirection", e); }
    }

    internal static bool SetSunIntensityForGraph(float lux)
    {
        try { return Note(Sky.SetSunIntensity(lux), "SetSunIntensity"); }
        catch (Exception e) { return Failed("SetSunIntensity", e); }
    }

    internal static bool SetSunColorForGraph(float r, float g, float b, float temperatureK)
    {
        try { return Note(Sky.SetSunColor(r, g, b, temperatureK), "SetSunColor"); }
        catch (Exception e) { return Failed("SetSunColor", e); }
    }

    internal static bool SetFogDensityForGraph(float density)
    {
        try { return Note(Sky.SetFogDensity(density), "SetFogDensity"); }
        catch (Exception e) { return Failed("SetFogDensity", e); }
    }

    internal static bool SetFogColorForGraph(float r, float g, float b)
    {
        try { return Note(Sky.SetFogColor(r, g, b), "SetFogColor"); }
        catch (Exception e) { return Failed("SetFogColor", e); }
    }

    internal static bool SetFogShapeForGraph(float falloff, float height, float start, float maxOpacity)
    {
        try { return Note(Sky.SetFogShape(falloff, height, start, maxOpacity), "SetFogShape"); }
        catch (Exception e) { return Failed("SetFogShape", e); }
    }

    internal static bool SetSkyLightForGraph(float multiplier)
    {
        try { return Note(Sky.SetSkyLight(multiplier), "SetSkyLight"); }
        catch (Exception e) { return Failed("SetSkyLight", e); }
    }

    /// <summary>An empty key releases every override.</summary>
    internal static bool ClearEnvironmentForGraph(string key)
    {
        try { return Note(Sky.ClearOverride(key), "ClearEnvironment"); }
        catch (Exception e) { return Failed("ClearEnvironment", e); }
    }

    /// <summary>An empty layer is the layer named <c>script</c>.</summary>
    internal static bool SetPostValueForGraph(string layer, string key, float v0, float v1, float v2)
    {
        try { return Note(PostFx.SetValue(layer, key, v0, v1, v2), "SetPostValue"); }
        catch (Exception e) { return Failed("SetPostValue", e); }
    }

    internal static bool SetPostLayerWeightForGraph(string layer, float weight, float blendSeconds)
    {
        try { return Note(PostFx.SetLayerWeight(layer, weight, blendSeconds), "SetPostLayerWeight"); }
        catch (Exception e) { return Failed("SetPostLayerWeight", e); }
    }

    internal static bool ClearPostLayerForGraph(string layer)
    {
        try { return Note(PostFx.ClearLayer(layer), "ClearPostLayer"); }
        catch (Exception e) { return Failed("ClearPostLayer", e); }
    }

    internal static bool SetPostVolumeWeightForGraph(string volume, float weight, float blendSeconds)
    {
        try { return Note(PostFx.SetVolumeWeight(volume, weight, blendSeconds), "SetPostVolumeWeight"); }
        catch (Exception e) { return Failed("SetPostVolumeWeight", e); }
    }
}
