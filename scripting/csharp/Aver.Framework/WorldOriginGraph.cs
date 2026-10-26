// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The graph-node surface for the floating origin: thin wrappers over WorldOrigin and Entity with the primitive-only signatures GraphCompiler's
// emitted IL needs. A graph float is a single, so a node never carries a bare absolute: it carries a REGION (whole regions along X, Y and Z, each
// 16.384 km) and a position relative to that region's centre, which a float holds to 0.06 cm everywhere. A failure returns 0 or false, is logged
// once, and never throws. Rows: scripting/csharp/Aver.Graph/GraphWorldOriginNodeTable.cs.

using Aver.Scripting;

namespace Aver.Framework;

internal static class WorldOriginGraph
{
    private static readonly HashSet<string> s_logged = new();

    private static void LogOnce(string key, string message)
    {
        if (s_logged.Add(key)) Log.Warn(message);
    }

    private static void Failed(string what, Exception e) => LogOnce(what + ":threw", $"[WorldOrigin] {what} failed: {e.GetType().Name}: {e.Message}");

    // The region an absolute point is in, and its offset from that region's centre. Region r is centred on r * StepCm.
    internal static void RegionOf(Double3 abs, out int rx, out int ry, out int rz, out float x, out float y, out float z)
    {
        (rx, ry, rz) = WorldPos.FromAbsolute(abs).Region;
        x = (float)(abs.X - (double)rx * WorldOrigin.StepCm);
        y = (float)(abs.Y - (double)ry * WorldOrigin.StepCm);
        z = (float)(abs.Z - (double)rz * WorldOrigin.StepCm);
    }

    internal static Double3 AbsoluteOf(int rx, int ry, int rz, float x, float y, float z) =>
        new((double)rx * WorldOrigin.StepCm + x, (double)ry * WorldOrigin.StepCm + y, (double)rz * WorldOrigin.StepCm + z);

    /// <summary>AN_GetWorldOrigin: the region the floating origin is in (whole regions along X and Y), the shift generation, and whether the origin is on.</summary>
    internal static int GetOriginForGraph(out int regionY, out int generation, out bool enabled)
    {
        regionY = 0; generation = 0; enabled = false;
        try
        {
            (int rx, int ry) = WorldOrigin.Region;
            regionY = ry;
            generation = unchecked((int)WorldOrigin.Generation);
            enabled = WorldOrigin.Enabled;
            return rx;
        }
        catch (Exception e) { Failed("GetWorldOrigin", e); return 0; }
    }

    /// <summary>AN_GetWorldRegion: an entity's region (X, Y, Z) and its position relative to that region's centre in centimetres. Zeros for no entity.</summary>
    internal static int GetRegionForGraph(int entity, out int regionY, out int regionZ, out float x, out float y, out float z)
    {
        regionY = regionZ = 0; x = y = z = 0.0f;
        if (entity == 0) return 0;
        try
        {
            RegionOf(new Entity(entity).AbsolutePositionCm, out int rx, out regionY, out regionZ, out x, out y, out z);
            return rx;
        }
        catch (Exception e) { Failed("GetWorldRegion", e); return 0; }
    }

    /// <summary>AN_SetWorldRegion: moves an entity to a region and an offset from its centre. False for no entity or a failure.</summary>
    internal static bool SetRegionForGraph(int entity, int regionX, int regionY, int regionZ, float x, float y, float z)
    {
        if (entity == 0 || !float.IsFinite(x) || !float.IsFinite(y) || !float.IsFinite(z)) return false;
        try
        {
            var e = new Entity(entity);
            if (!e.IsAlive) return false;
            e.SetAbsolutePositionCm(AbsoluteOf(regionX, regionY, regionZ, x, y, z));
            return true;
        }
        catch (Exception ex) { Failed("SetWorldRegion", ex); return false; }
    }
}
