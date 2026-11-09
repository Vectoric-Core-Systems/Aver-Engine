// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Data layers: the named layers a level declares (DATALAYER records) and the state a script or graph sets on them.

using System.Runtime.InteropServices;
using System.Text;

namespace Aver.Framework;

/// <summary>The runtime state of a data layer. Ordered: a placement is resident only while every layer it is in is at least
/// <see cref="Loaded"/>; <see cref="Activated"/> additionally lets it render and simulate (Loaded = resident but hidden).</summary>
public enum DataLayerState
{
    /// <summary>The layer's placements are not loaded.</summary>
    Unloaded = 0,
    /// <summary>Loaded into memory but neither shown nor simulated.</summary>
    Loaded = 1,
    /// <summary>Loaded, shown and simulated.</summary>
    Activated = 2,
}

/// <summary>What <see cref="DataLayers.SetState(string, DataLayerState)"/> did.</summary>
public enum DataLayerResult
{
    /// <summary>The state was applied; the streamer acts on it over the next updates.</summary>
    Ok = 1,
    /// <summary>No layer has that name (or no level with layers is loaded).</summary>
    Unknown = 0,
    /// <summary>The layer is not a runtime layer (<c>runtime=0</c>): only the editor changes it.</summary>
    NotRuntime = -1,
    /// <summary>The state value is out of range.</summary>
    BadState = -2,
}

/// <summary>The level's data layers. Main-thread, never throws: with no level or no layers every query answers the defaults
/// (no layers, <see cref="DataLayerResult.Unknown"/>).</summary>
public static class DataLayers
{
    private const int NameCap = 256;

    /// <summary>How many layers the loaded level declares.</summary>
    public static int Count
    {
        get
        {
            try { return Math.Max(0, FwDataLayer.aver_fw_datalayer_count()); }
            catch (Exception e) when (NoNative(e)) { return 0; }
        }
    }

    /// <summary>The name of layer <paramref name="index"/>, or an empty string for an unknown index.</summary>
    public static string Name(int index)
    {
        Span<byte> buf = stackalloc byte[NameCap];
        try { return FwDataLayer.aver_fw_datalayer_name(index, ref MemoryMarshal.GetReference(buf), NameCap) == 0 ? string.Empty : Utf8(buf); }
        catch (Exception e) when (NoNative(e)) { return string.Empty; }
    }

    /// <summary>The names of every layer, in file order.</summary>
    public static IReadOnlyList<string> Names()
    {
        int n = Count;
        var list = new List<string>(n);
        for (int i = 0; i < n; ++i) list.Add(Name(i));
        return list;
    }

    /// <summary>The state in force for a layer (never above its parent's), or false for an unknown name.</summary>
    public static bool TryGetState(string name, out DataLayerState state)
    {
        state = DataLayerState.Unloaded;
        if (string.IsNullOrEmpty(name)) return false;
        try
        {
            if (FwDataLayer.aver_fw_datalayer_get_state(name, out int s) == 0) return false;
            state = (DataLayerState)Math.Clamp(s, 0, 2);
            return true;
        }
        catch (Exception e) when (NoNative(e)) { return false; }
    }

    /// <summary>The state in force for a layer; <see cref="DataLayerState.Unloaded"/> for an unknown name (use <see cref="TryGetState"/> to tell the two apart).</summary>
    public static DataLayerState GetState(string name) => TryGetState(name, out DataLayerState s) ? s : DataLayerState.Unloaded;

    /// <summary>Asks for a layer to take a state. Only runtime layers (<c>runtime=1</c>) may change in a running game. The change takes
    /// effect as the streamer next updates; <see cref="TryPollCompleted"/> reports when it has.</summary>
    public static DataLayerResult SetState(string name, DataLayerState state)
    {
        if (string.IsNullOrEmpty(name)) return DataLayerResult.Unknown;
        try
        {
            int r = FwDataLayer.aver_fw_datalayer_set_state(name, (int)state);
            return r is 1 or -1 or -2 ? (DataLayerResult)r : DataLayerResult.Unknown;
        }
        catch (Exception e) when (NoNative(e)) { return DataLayerResult.Unknown; }
    }

    /// <summary>Takes the oldest finished change: the streamer has loaded what an activated layer needs in range, or evicted everything an
    /// unloaded one held. False when there is none.</summary>
    public static bool TryPollCompleted(out string name, out DataLayerState state)
    {
        Span<byte> buf = stackalloc byte[NameCap];
        name = string.Empty; state = DataLayerState.Unloaded;
        try
        {
            if (FwDataLayer.aver_fw_datalayer_poll_completed(ref MemoryMarshal.GetReference(buf), NameCap, out int s) == 0) return false;
            name = Utf8(buf);
            state = (DataLayerState)Math.Clamp(s, 0, 2);
            return true;
        }
        catch (Exception e) when (NoNative(e)) { return false; }
    }

    // No native framework library in this process (a bare managed test host): the same answer as no provider.
    private static bool NoNative(Exception e) => e is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException;

    private static string Utf8(ReadOnlySpan<byte> buf)
    {
        int len = buf.IndexOf((byte)0);
        return Encoding.UTF8.GetString(len < 0 ? buf : buf[..len]);
    }
}
