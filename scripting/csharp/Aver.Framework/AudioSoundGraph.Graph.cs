// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The entry points the real-time sound-graph Aver Node nodes call (GraphProceduralSoundNodeTable.cs; palette mirror
// sandbox/src/GraphNodeDefsProceduralSound.hpp), each a thin wrapper over Audio.PlayGraph and Voice. Silence is a supported
// configuration (Audio.Ready false): plays answer false with voice 0, writes answer false, nothing throws.

using Aver.Scene;

namespace Aver.Framework;

internal static class ProceduralSoundGraph
{
    private const string NoVoice = "the voice handle is 0 (a refused or finished play), not a sound-graph voice, or audio is not running";

    private static Bus BusOf(int bus, string node)
    {
        if (System.Enum.IsDefined(typeof(Bus), bus)) return (Bus)bus;
        Aver.Scripting.Log.Warn($"[{node}] bus {bus} is not one of Sfx(0)/Music(1)/Voice(2)/Ui(3); it plays on Sfx.");
        return Bus.Sfx;
    }

    /// <summary>AN_PlaySoundGraph: plays the sound= .ocsnd flat on the audio thread. seed 0 varies every play. voice is 0 with no device.</summary>
    internal static bool PlaySoundGraphForGraph(string path, float volume, bool looping, int bus, int seed, out int voice)
    {
        voice = 0;
        try
        {
            if (!FamiliesBLog.Ok("AN_PlaySoundGraph", Audio.Ready && float.IsFinite(volume), "audio is not running, or the volume is not a number")) return false;
            SoundGraph g = Audio.LoadGraph(path);
            if (!FamiliesBLog.Ok("AN_PlaySoundGraph", g.IsValid, $"'{path}' could not be loaded as a sound graph (see the Output Log)")) return false;
            Voice v = Audio.PlayGraph(g, volume, looping, BusOf(bus, "AN_PlaySoundGraph"), unchecked((uint)seed));
            voice = v.Handle;
            return v.IsValid;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_PlaySoundGraph", e); }
    }

    /// <summary>AN_PlaySoundGraphAt: the positioned counterpart (centimetres; full volume inside innerCm, silent beyond outerCm).</summary>
    internal static bool PlaySoundGraphAtForGraph(string path, float x, float y, float z, float volume, bool looping, int bus,
                                                  float innerCm, float outerCm, int seed, out int voice)
    {
        voice = 0;
        try
        {
            if (!FamiliesBLog.Ok("AN_PlaySoundGraphAt", Audio.Ready && float.IsFinite(x) && float.IsFinite(y) && float.IsFinite(z) && float.IsFinite(volume),
                                 "audio is not running, or a position or the volume is not a number")) return false;
            SoundGraph g = Audio.LoadGraph(path);
            if (!FamiliesBLog.Ok("AN_PlaySoundGraphAt", g.IsValid, $"'{path}' could not be loaded as a sound graph (see the Output Log)")) return false;
            Voice v = Audio.PlayGraphAt(g, new Vec3(x, y, z), volume, looping, BusOf(bus, "AN_PlaySoundGraphAt"), innerCm, outerCm, unchecked((uint)seed));
            voice = v.Handle;
            return v.IsValid;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_PlaySoundGraphAt", e); }
    }

    /// <summary>AN_SetSoundGraphParam: sets the voice's named live input (the param= attribute); the graph smooths toward it. False for an unknown name.</summary>
    internal static bool SetSoundGraphParamForGraph(int voice, string name, float value)
    {
        try
        {
            if (!FamiliesBLog.Ok("AN_SetSoundGraphParam", voice != 0 && Audio.Ready && float.IsFinite(value) && !string.IsNullOrEmpty(name),
                                 NoVoice + ", the param= name is empty, or the value is not a number")) return false;
            return new Voice(voice).SetGraphParam(name, value);
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetSoundGraphParam", e); }
    }

    /// <summary>AN_NoteOffSoundGraph: the note-off. ADSR nodes release from their level and the voice ends after the longest release.</summary>
    internal static bool NoteOffSoundGraphForGraph(int voice)
    {
        try
        {
            if (!FamiliesBLog.Ok("AN_NoteOffSoundGraph", voice != 0 && Audio.Ready, NoVoice)) return false;
            new Voice(voice).GraphNoteOff();
            return true;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_NoteOffSoundGraph", e); }
    }
}
