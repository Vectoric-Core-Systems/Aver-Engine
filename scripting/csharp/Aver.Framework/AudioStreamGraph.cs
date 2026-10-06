// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Graph-callable surface for streamed audio: one scalar-shaped static per node, the shape
// GraphInterop's audio section uses, so the compiler can reflect them by name.
//
// THE NODE REGISTRATION IS NOT HERE. The catalog rows, parser pin defaults and compiler emit
// cases live in shared files; docs/AUDIO_STREAMING.md lists the exact entries to add.

using Aver.Scene;

namespace Aver.Framework;

internal static class AudioStreamGraph
{
    private static Bus BusOf(int bus, string node)
    {
        if (System.Enum.IsDefined(typeof(Bus), bus)) return (Bus)bus;
        Aver.Scripting.Log.Warn($"[{node}] bus {bus} is not one of Sfx(0)/Music(1)/Voice(2)/Ui(3); it plays on Sfx.");
        return Bus.Sfx;
    }

    private static FadeCurve CurveOf(int curve) => curve == 0 ? FadeCurve.Linear : FadeCurve.EqualPower;

    /// <summary>PlayStream: streams the sound= file flat. voice is 0 with no device.</summary>
    internal static bool PlayStreamForGraph(string path, float volume, float pitch, bool looping, int bus,
                                            float fadeInSeconds, out int voice)
    {
        Voice v = Audio.PlayStream(path, volume, pitch, looping, BusOf(bus, "PlayStream"), fadeInSeconds);
        voice = v.Handle;
        return v.IsValid;
    }

    /// <summary>PlayStreamAt: the positioned counterpart.</summary>
    internal static bool PlayStreamAtForGraph(string path, float x, float y, float z, float volume, float pitch,
                                              bool looping, int bus, float innerCm, float outerCm,
                                              float fadeInSeconds, out int voice)
    {
        Voice v = Audio.PlayStreamAt(path, new Vec3(x, y, z), volume, pitch, looping, BusOf(bus, "PlayStreamAt"),
                                     innerCm, outerCm, fadeInSeconds);
        voice = v.Handle;
        return v.IsValid;
    }

    /// <summary>PlayMusic: crossfades the music slot to the sound= file.</summary>
    internal static bool PlayMusicForGraph(string path, float volume, float fadeSeconds, bool looping, int curve,
                                           out int voice)
    {
        Voice v = Music.Play(path, fadeSeconds, volume, looping, CurveOf(curve));
        voice = v.Handle;
        return v.IsValid;
    }

    /// <summary>StopMusic: fades the current track out. True when a track was playing.</summary>
    internal static bool StopMusicForGraph(float fadeSeconds)
    {
        bool had = Music.Current.IsValid;
        Music.Stop(fadeSeconds);
        return had;
    }

    /// <summary>IsMusicPlaying: pure read.</summary>
    internal static bool IsMusicPlayingForGraph() => Music.Current.IsValid;

    /// <summary>FadeSound: ramps a voice's fade gain.</summary>
    internal static bool FadeSoundForGraph(int voice, float targetGain, float seconds, int curve, bool stopWhenDone)
    {
        if (voice == 0) return false;
        new Voice(voice).FadeTo(targetGain, seconds, CurveOf(curve), stopWhenDone);
        return true;
    }

    /// <summary>SetVoiceOcclusion: sets occlusion by hand (a trigger volume, a door state).</summary>
    internal static bool SetVoiceOcclusionForGraph(int voice, float occlusion)
    {
        if (voice == 0) return false;
        new Voice(voice).Occlusion = occlusion;
        return true;
    }

    /// <summary>SetReverb: a fixed listener reverb.</summary>
    internal static bool SetReverbForGraph(float wet, float decaySeconds, float damping)
    {
        Audio.SetReverb(wet, decaySeconds, damping);
        return true;
    }

    /// <summary>AttachAudioOcclusion: raycast-driven occlusion for a voice, following the entity.</summary>
    internal static bool AttachAudioOcclusionForGraph(int entity, int voice, int rayCount, float probeRadiusCm)
    {
        Entity e = new Entity(entity);
        if (!e.IsAlive || voice == 0) return false;
        return e.SetAudioOcclusion(new Voice(voice), rayCount, probeRadiusCm);
    }

    /// <summary>SetReverbZone: makes the entity a box reverb volume.</summary>
    internal static bool SetReverbZoneForGraph(int entity, float halfX, float halfY, float halfZ,
                                               float blendDistanceCm, float wet, float decaySeconds,
                                               float damping, int priority)
    {
        Entity e = new Entity(entity);
        if (!e.IsAlive) return false;
        return e.SetReverbZone(new Vec3(halfX, halfY, halfZ), blendDistanceCm, wet, decaySeconds, damping, priority, 0);
    }
}
