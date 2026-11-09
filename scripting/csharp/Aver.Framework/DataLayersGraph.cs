// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Data layers: the entry points the Aver Node data layer nodes call (docs/LEVEL_STREAMING.md).

using Aver.Scripting;

namespace Aver.Framework;

/// <summary>What the data layer graph nodes call. Reached by reflection from Aver.Graph, like <c>TerrainGraph</c>. The layer name is a node
/// attribute, never a pin. Nothing here throws: no level, no layers or an unknown name answer the defaults (state -1 for a read, false
/// for a set), and a node with no layer= attribute logs once.</summary>
internal static class DataLayersGraph
{
    private static readonly HashSet<string> Logged = new();

    private static void LogOnce(string key, string message)
    {
        if (Logged.Add(key)) Log.Warn(message);
    }

    /// <summary>AN_GetDataLayerState: the state in force for the layer (0 unloaded, 1 loaded, 2 activated), or -1 for an unknown layer.</summary>
    internal static int GetStateForGraph(string layer)
    {
        if (string.IsNullOrEmpty(layer))
        {
            LogOnce("datalayer:noget", "[DataLayers] an AN_GetDataLayerState node has no layer= attribute");
            return -1;
        }
        return DataLayers.TryGetState(layer, out DataLayerState s) ? (int)s : -1;
    }

    /// <summary>AN_SetDataLayerState: asks the layer to take the state (0 unloaded, 1 loaded, 2 activated). False for an unknown layer, a
    /// layer that is not a runtime layer, or a state out of range.</summary>
    internal static bool SetStateForGraph(string layer, int state)
    {
        if (string.IsNullOrEmpty(layer))
        {
            LogOnce("datalayer:noset", "[DataLayers] an AN_SetDataLayerState node has no layer= attribute");
            return false;
        }
        if (state < 0 || state > 2) return false;
        return DataLayers.SetState(layer, (DataLayerState)state) == DataLayerResult.Ok;
    }
}
