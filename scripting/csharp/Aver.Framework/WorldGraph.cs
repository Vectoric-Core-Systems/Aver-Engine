// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The World rows of the node table: the sky's cloud layer and simulated fluid volumes
// (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md 2.2). Wrappers over Sky and Game; sun, fog and post layers are
// the WorldFx family, not here. A failure returns false and logs once, never throws.

using Aver.Scene;

namespace Aver.Framework;

/// <summary>The per-node wrappers behind the World rows.</summary>
internal static class WorldGraph
{
    /// <summary>Publishes a cloud layer (<see cref="Sky.SetClouds"/>); a later call replaces the whole request.
    /// False for a NaN argument or when the native service is missing.</summary>
    internal static bool SetSkyCloudsForGraph(int seed, float coverage, float density, float bottomCm, float topCm,
                                              float featureScale, float windX, float windY)
    {
        if (float.IsNaN(coverage) || float.IsNaN(density) || float.IsNaN(bottomCm) || float.IsNaN(topCm) ||
            float.IsNaN(featureScale) || float.IsNaN(windX) || float.IsNaN(windY))
        {
            FlowGuard.Refused("SetSkyClouds", "an argument is NaN");
            return false;
        }
        try { Sky.SetClouds(seed, coverage, density, bottomCm, topCm, featureScale, windX, windY); return true; }
        catch (Exception e) { FlowGuard.Failed("SetSkyClouds", e); return false; }
    }

    /// <summary>Drops the cloud request so the level's own sky takes over again.</summary>
    internal static bool ClearSkyCloudsForGraph()
    {
        try { Sky.Clear(); return true; }
        catch (Exception e) { FlowGuard.Failed("ClearSkyClouds", e); return false; }
    }

    /// <summary>Requests a fluid volume centred on (cx, cy, cz) with the given half extents in centimetres, using the
    /// solver's default knobs (<see cref="Game.SpawnFluidVolume"/>). The name is optional. The result says a provider
    /// accepted the request, not that the volume exists yet. False for a non-positive or NaN half extent.</summary>
    internal static bool SpawnFluidVolumeForGraph(float cx, float cy, float cz, float hx, float hy, float hz, string name)
    {
        if (!(hx > 0f) || !(hy > 0f) || !(hz > 0f))
        {
            FlowGuard.Refused("SpawnFluidVolume", "half extents must be positive");
            return false;
        }
        try
        {
            return Game.SpawnFluidVolume(new Vec3(cx, cy, cz), new Vec3(hx, hy, hz),
                                         string.IsNullOrEmpty(name) ? "unnamed" : name);
        }
        catch (Exception e) { FlowGuard.Failed("SpawnFluidVolume", e); return false; }
    }
}
