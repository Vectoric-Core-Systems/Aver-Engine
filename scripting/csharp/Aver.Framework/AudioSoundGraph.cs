// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Script-facing real-time sound graphs: a .ocsnd evaluated on the audio thread, with named live inputs and a note-off.
//
// Same silent-machine contract as Audio.cs: with no audio DLL or no device every call is a no-op returning
// SoundGraph.None / Voice.None / false. See docs/AUDIO.md section 13.

using System;
using System.Runtime.InteropServices;
using Aver.Scene;

namespace Aver.Framework;

internal static class AxGraph
{
    private const string Lib = "Aver.Audio.Abi";

    [DllImport(Lib)] internal static extern int    aver_audio_soundgraph_load([MarshalAs(UnmanagedType.LPUTF8Str)] string utf8Path);
    [DllImport(Lib)] internal static extern void   aver_audio_soundgraph_unload(int graph);
    [DllImport(Lib)] internal static extern int    aver_audio_soundgraph_param_count(int graph);
    [DllImport(Lib)] internal static extern int    aver_audio_soundgraph_param_name(int graph, int id, byte[] textOut, int cap);
    [DllImport(Lib)] internal static extern int    aver_audio_soundgraph_param_find(int graph, [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8Name);
    [DllImport(Lib)] internal static extern float  aver_audio_soundgraph_param_default(int graph, int id);
    [DllImport(Lib)] internal static extern double aver_audio_soundgraph_duration(int graph);
    // names and values may be null with count 0 (the managed callers pass null, hence the ! at the call sites).
    [DllImport(Lib)] internal static extern int    aver_audio_soundgraph_play(int graph, float volume, int looping, int bus, uint seed,
        [MarshalAs(UnmanagedType.LPArray, ArraySubType = UnmanagedType.LPUTF8Str)] string[] paramNames, float[] paramValues, int paramCount);
    [DllImport(Lib)] internal static extern int    aver_audio_soundgraph_play_at(int graph, float x, float y, float z, float volume, int looping, int bus,
        float innerCm, float outerCm, uint seed,
        [MarshalAs(UnmanagedType.LPArray, ArraySubType = UnmanagedType.LPUTF8Str)] string[] paramNames, float[] paramValues, int paramCount);
    [DllImport(Lib)] internal static extern int    aver_audio_soundgraph_set_param(int voice, [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8Name, float value);
    [DllImport(Lib)] internal static extern int    aver_audio_soundgraph_set_param_id(int voice, int id, float value);
    [DllImport(Lib)] internal static extern void   aver_audio_soundgraph_note_off(int voice);
    [DllImport(Lib)] internal static extern int    aver_audio_soundgraph_is_voice(int voice);
}

/// <summary>A loaded, compiled <c>.ocsnd</c> that can be played as a real-time voice. 0 is "nothing", which every call tolerates.</summary>
/// <remarks>A HANDLE, NOT A FILE: <see cref="Audio.LoadGraph"/> reads and compiles once and gives the same handle for the same path.
/// The sounds its WavePlayer nodes name are loaded with it (path-cached, like <see cref="Audio.Load"/>). Voices already playing keep
/// going if the graph is unloaded.</remarks>
public readonly struct SoundGraph : IEquatable<SoundGraph>
{
    /// <summary>The raw handle. 0 is invalid.</summary>
    public readonly int Handle;

    /// <summary>Wraps a raw handle.</summary>
    public SoundGraph(int handle) { Handle = handle; }

    /// <summary>The absence of a graph, and what a failed load gives back.</summary>
    public static SoundGraph None => new(0);

    /// <summary>True when this refers to a loaded graph.</summary>
    public bool IsValid => Handle != 0;

    /// <summary>The authored length in seconds: how long a non-looping play lasts. 0 for an invalid graph.</summary>
    public double Duration { get { try { return Handle != 0 ? AxGraph.aver_audio_soundgraph_duration(Handle) : 0.0; } catch (DllNotFoundException) { return 0.0; } } }

    /// <summary>How many named live inputs (Parameter nodes) the graph has.</summary>
    public int ParamCount { get { try { return Handle != 0 ? AxGraph.aver_audio_soundgraph_param_count(Handle) : 0; } catch (DllNotFoundException) { return 0; } } }

    /// <summary>The name of live input <paramref name="id"/>, or an empty string.</summary>
    public string ParamName(int id)
    {
        if (Handle == 0 || id < 0) return string.Empty;
        try
        {
            byte[] buf = new byte[128];
            int len = AxGraph.aver_audio_soundgraph_param_name(Handle, id, buf, buf.Length);
            if (len >= buf.Length) { buf = new byte[len + 1]; len = AxGraph.aver_audio_soundgraph_param_name(Handle, id, buf, buf.Length); }
            return System.Text.Encoding.UTF8.GetString(buf, 0, Math.Min(len, buf.Length - 1));
        }
        catch (DllNotFoundException) { return string.Empty; }
    }

    /// <summary>The id of a named live input, or -1.</summary>
    public int FindParam(string name)
    {
        if (Handle == 0 || string.IsNullOrEmpty(name)) return -1;
        try { return AxGraph.aver_audio_soundgraph_param_find(Handle, name); } catch (DllNotFoundException) { return -1; }
    }

    /// <summary>The value live input <paramref name="id"/> was authored with.</summary>
    public float ParamDefault(int id)
    {
        if (Handle == 0 || id < 0) return 0f;
        try { return AxGraph.aver_audio_soundgraph_param_default(Handle, id); } catch (DllNotFoundException) { return 0f; }
    }

    /// <summary>Forgets the graph. Voices already playing it keep going.</summary>
    public void Unload() { if (Handle != 0) { try { AxGraph.aver_audio_soundgraph_unload(Handle); } catch (DllNotFoundException) { } } }

    /// <summary>Handle equality.</summary>
    public bool Equals(SoundGraph other) => Handle == other.Handle;
    /// <summary>Handle equality.</summary>
    public override bool Equals(object? o) => o is SoundGraph g && Equals(g);
    /// <summary>The handle itself.</summary>
    public override int GetHashCode() => Handle;
    /// <summary>Diagnostic text.</summary>
    public override string ToString() => $"SoundGraph({Handle})";
}

public readonly partial struct Voice
{
    /// <summary>True when this is a live sound-graph voice (one from <see cref="Audio.PlayGraph(SoundGraph, float, bool, Bus, uint)"/>) that has not ended.</summary>
    public bool IsGraphVoice
    {
        get { try { return Handle != 0 && AxGraph.aver_audio_soundgraph_is_voice(Handle) != 0; } catch (DllNotFoundException) { return false; } }
    }

    /// <summary>Sets a sound graph's named live input (rpm, intensity, surface). Latest value wins and the graph smooths toward it, so a jump
    /// is a ramp, not a click. False for a voice that is not a graph voice, an unknown name, or a value that is not finite.</summary>
    public bool SetGraphParam(string name, float value)
    {
        if (Handle == 0 || string.IsNullOrEmpty(name) || !float.IsFinite(value)) return false;
        try { return AxGraph.aver_audio_soundgraph_set_param(Handle, name, value) != 0; } catch (DllNotFoundException) { return false; }
    }

    /// <summary>Sets a live input by id (<see cref="SoundGraph.FindParam"/>), which skips the name lookup in a per-frame loop.</summary>
    public bool SetGraphParam(int id, float value)
    {
        if (Handle == 0 || id < 0 || !float.IsFinite(value)) return false;
        try { return AxGraph.aver_audio_soundgraph_set_param_id(Handle, id, value) != 0; } catch (DllNotFoundException) { return false; }
    }

    /// <summary>The note-off: ADSR nodes release from their current level and the voice ends once the longest release has played
    /// (a graph with no ADSR fades over 10 ms). <see cref="Stop"/> instead fades the voice out within one block.</summary>
    public void GraphNoteOff()
    {
        if (Handle == 0) return;
        try { AxGraph.aver_audio_soundgraph_note_off(Handle); } catch (DllNotFoundException) { }
    }
}

public static partial class Audio
{
    /// <summary>Loads and compiles a <c>.ocsnd</c> for real-time play. <see cref="SoundGraph.None"/> for an unreadable or invalid file,
    /// a graph over an evaluator limit, or with no audio device. The same path twice gives the same handle.</summary>
    public static SoundGraph LoadGraph(string path)
    {
        if (string.IsNullOrEmpty(path)) return SoundGraph.None;
        try { return new SoundGraph(AxGraph.aver_audio_soundgraph_load(path)); } catch (DllNotFoundException) { return SoundGraph.None; }
    }

    /// <summary>Plays a sound graph flat in both ears, evaluated on the audio thread. A non-looping play lasts the graph's duration (an
    /// ADSR releases before the end); a looping one runs until <see cref="Voice.GraphNoteOff"/> or <see cref="Voice.Stop"/>.</summary>
    /// <param name="graph">The graph from <see cref="LoadGraph"/>.</param>
    /// <param name="volume">The voice volume.</param>
    /// <param name="looping">True to run until a note-off or Stop instead of for the graph's duration.</param>
    /// <param name="bus">Which mixer bus to play on.</param>
    /// <param name="seed">0 picks a different seed every play (Noise and Random vary); any other value repeats exactly.</param>
    public static Voice PlayGraph(SoundGraph graph, float volume = 1f, bool looping = false, Bus bus = Bus.Sfx, uint seed = 0)
        => PlayGraph(graph, volume, looping, bus, seed, ReadOnlySpan<(string Name, float Value)>.Empty);

    /// <summary>As <see cref="PlayGraph(SoundGraph, float, bool, Bus, uint)"/>, with live inputs set before the first block, so a voice that
    /// starts at rpm 3000 does not ramp up from the default. Names the graph does not have are ignored.</summary>
    public static Voice PlayGraph(SoundGraph graph, float volume, bool looping, Bus bus, uint seed, ReadOnlySpan<(string Name, float Value)> inputs)
    {
        if (!graph.IsValid) return Voice.None;
        SplitInputs(inputs, out string[]? names, out float[]? values);
        try { return new Voice(AxGraph.aver_audio_soundgraph_play(graph.Handle, volume, looping ? 1 : 0, (int)bus, seed, names!, values!, names?.Length ?? 0)); }
        catch (DllNotFoundException) { return Voice.None; }
    }

    /// <summary>Plays a sound graph at a world position, panned and attenuated against the listener (centimetres).</summary>
    public static Voice PlayGraphAt(SoundGraph graph, Vec3 position, float volume = 1f, bool looping = false, Bus bus = Bus.Sfx,
                                    float innerCm = 100f, float outerCm = 2000f, uint seed = 0)
        => PlayGraphAt(graph, position, volume, looping, bus, innerCm, outerCm, seed, ReadOnlySpan<(string Name, float Value)>.Empty);

    /// <summary>As the other <c>PlayGraphAt</c>, with live inputs set before the first block.</summary>
    public static Voice PlayGraphAt(SoundGraph graph, Vec3 position, float volume, bool looping, Bus bus, float innerCm, float outerCm, uint seed,
                                    ReadOnlySpan<(string Name, float Value)> inputs)
    {
        if (!graph.IsValid) return Voice.None;
        SplitInputs(inputs, out string[]? names, out float[]? values);
        try
        {
            return new Voice(AxGraph.aver_audio_soundgraph_play_at(graph.Handle, position.X, position.Y, position.Z, volume, looping ? 1 : 0, (int)bus,
                                                                   innerCm, outerCm, seed, names!, values!, names?.Length ?? 0));
        }
        catch (DllNotFoundException) { return Voice.None; }
    }

    private static void SplitInputs(ReadOnlySpan<(string Name, float Value)> inputs, out string[]? names, out float[]? values)
    {
        names = null; values = null;
        if (inputs.IsEmpty) return;
        names = new string[inputs.Length];
        values = new float[inputs.Length];
        for (int i = 0; i < inputs.Length; i++) { names[i] = inputs[i].Name ?? string.Empty; values[i] = inputs[i].Value; }
    }
}
