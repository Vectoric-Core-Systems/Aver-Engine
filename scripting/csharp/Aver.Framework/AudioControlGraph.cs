// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The entry points the audio-control Aver Node nodes call (GraphAudioControlNodeTable.cs; palette mirror
// sandbox/src/GraphNodeDefsAudioControl.hpp): master and bus volume, live voice control, the occlusion curve and mixer
// statistics, each a thin wrapper over Audio, Voice and Music. Voice handles come from the voice pin of AN_PlayStream,
// AN_PlayStreamAt and AN_PlayMusic. Silence is a supported configuration (Audio.Ready false): writes answer false, reads
// answer 0, nothing throws.

using Aver.Scene;

namespace Aver.Framework;

internal static class AudioControlGraph
{
    private const string NoVoice = "the voice handle is 0 (a refused or finished play) or audio is not running";

    /// <summary>AN_SetMasterVolume: the volume over every bus, clamped to 0..1. False when no audio device is open.</summary>
    internal static bool SetMasterVolumeForGraph(float volume)
    {
        try
        {
            if (!Audio.Ready || !float.IsFinite(volume)) return false;
            Audio.MasterVolume = Math.Clamp(volume, 0f, 1f);
            return true;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetMasterVolume", e); }
    }

    /// <summary>AN_GetMasterVolume: 0 when no audio device is open.</summary>
    internal static float GetMasterVolumeForGraph()
    {
        try { return Audio.MasterVolume; }
        catch (Exception e) { FamiliesBLog.Failed("AN_GetMasterVolume", e); return 0f; }
    }

    /// <summary>AN_GetBusVolume: bus 0 Sfx, 1 Music, 2 Voice, 3 Ui. An unknown bus reads 0 (logged once).</summary>
    internal static float GetBusVolumeForGraph(int bus)
    {
        try
        {
            if (!FamiliesBLog.Ok("AN_GetBusVolume", Enum.IsDefined(typeof(Bus), bus), $"bus {bus} is not one of Sfx(0)/Music(1)/Voice(2)/Ui(3)"))
                return 0f;
            return Audio.GetBusVolume((Bus)bus);
        }
        catch (Exception e) { FamiliesBLog.Failed("AN_GetBusVolume", e); return 0f; }
    }

    /// <summary>AN_StopAllSounds: stops every voice at once. False when no audio device is open.</summary>
    internal static bool StopAllSoundsForGraph()
    {
        try
        {
            if (!Audio.Ready) return false;
            Audio.StopAll();
            return true;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_StopAllSounds", e); }
    }

    /// <summary>AN_SetVoiceVolume: a live voice's volume (negative reads as 0).</summary>
    internal static bool SetVoiceVolumeForGraph(int voice, float volume)
    {
        try
        {
            if (!FamiliesBLog.Ok("AN_SetVoiceVolume", voice != 0 && Audio.Ready && float.IsFinite(volume), NoVoice)) return false;
            new Voice(voice).Volume = MathF.Max(volume, 0f);
            return true;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetVoiceVolume", e); }
    }

    /// <summary>AN_SetVoicePitch: a live voice's playback rate; 1 is unaltered. Must be above 0.</summary>
    internal static bool SetVoicePitchForGraph(int voice, float pitch)
    {
        try
        {
            if (!FamiliesBLog.Ok("AN_SetVoicePitch", voice != 0 && Audio.Ready && float.IsFinite(pitch) && pitch > 0f,
                                 NoVoice + ", or the pitch is not above 0")) return false;
            new Voice(voice).Pitch = pitch;
            return true;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetVoicePitch", e); }
    }

    /// <summary>AN_SetVoicePosition: moves a positioned voice (one from a PlayAt node); a flat voice ignores it.</summary>
    internal static bool SetVoicePositionForGraph(int voice, float x, float y, float z)
    {
        try
        {
            if (!FamiliesBLog.Ok("AN_SetVoicePosition", voice != 0 && Audio.Ready && float.IsFinite(x) && float.IsFinite(y) && float.IsFinite(z), NoVoice))
                return false;
            new Voice(voice).Position = new Vec3(x, y, z);
            return true;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetVoicePosition", e); }
    }

    /// <summary>AN_IsVoicePlaying: whether the voice is still sounding. A finished or zero handle reads false.</summary>
    internal static bool IsVoicePlayingForGraph(int voice)
    {
        try { return voice != 0 && new Voice(voice).IsPlaying; }
        catch (Exception e) { return FamiliesBLog.Failed("AN_IsVoicePlaying", e); }
    }

    /// <summary>AN_StopVoice: stops one voice. Harmless if it already ended.</summary>
    internal static bool StopVoiceForGraph(int voice)
    {
        try
        {
            if (!FamiliesBLog.Ok("AN_StopVoice", voice != 0 && Audio.Ready, NoVoice)) return false;
            new Voice(voice).Stop();
            return true;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_StopVoice", e); }
    }

    /// <summary>AN_SetOcclusionCurve: the low-pass cutoff (Hz, above 0) and linear volume (0..1) a fully occluded voice reaches.</summary>
    internal static bool SetOcclusionCurveForGraph(float minCutoffHz, float minVolume)
    {
        try
        {
            if (!FamiliesBLog.Ok("AN_SetOcclusionCurve", Audio.Ready && float.IsFinite(minCutoffHz) && minCutoffHz > 0f && float.IsFinite(minVolume),
                                 "audio is not running, or the cutoff is not above 0 Hz")) return false;
            Audio.SetOcclusionCurve(minCutoffHz, Math.Clamp(minVolume, 0f, 1f));
            return true;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetOcclusionCurve", e); }
    }

    /// <summary>AN_GetAudioStats: voices sounding now, streams held by a voice, and mixer buffers that ran short since start.</summary>
    internal static void GetAudioStatsForGraph(out int activeVoices, out int activeStreams, out int underruns)
    {
        activeVoices = 0; activeStreams = 0; underruns = 0;
        try
        {
            activeVoices = Audio.ActiveVoices;
            activeStreams = Audio.ActiveStreams;
            underruns = Audio.Underruns;
        }
        catch (Exception e) { FamiliesBLog.Failed("AN_GetAudioStats", e); }
    }
}
