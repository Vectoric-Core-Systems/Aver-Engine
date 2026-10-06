// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Script-facing streamed audio: long files, music crossfades, occlusion and reverb.
//
// Same silent-machine contract as Audio.cs: with no audio DLL or no device every call is a no-op
// returning Voice.None. See docs/AUDIO_STREAMING.md.

using System;
using System.Runtime.InteropServices;
using Aver.Scene;

namespace Aver.Framework;

internal static class AxStream
{
    private const string Lib = "Aver.Audio.Abi";

    [DllImport(Lib)] internal static extern int aver_audio_stream_play([MarshalAs(UnmanagedType.LPUTF8Str)] string path,
        float volume, float pitch, int looping, int bus, float fadeInSeconds);
    [DllImport(Lib)] internal static extern int aver_audio_stream_play_at([MarshalAs(UnmanagedType.LPUTF8Str)] string path,
        float x, float y, float z, float volume, float pitch, int looping, int bus,
        float innerCm, float outerCm, float fadeInSeconds);
    [DllImport(Lib)] internal static extern void aver_audio_fade_voice(int voice, float targetGain, float seconds,
        int curve, int stopWhenDone);
    [DllImport(Lib)] internal static extern int  aver_audio_music_play([MarshalAs(UnmanagedType.LPUTF8Str)] string path,
        float volume, float fadeSeconds, int looping, int curve);
    [DllImport(Lib)] internal static extern void aver_audio_music_stop(float fadeSeconds);
    [DllImport(Lib)] internal static extern int  aver_audio_music_voice();
    [DllImport(Lib)] internal static extern void aver_audio_set_voice_occlusion(int voice, float occlusion);
    [DllImport(Lib)] internal static extern void aver_audio_set_voice_reverb_send(int voice, float send);
    [DllImport(Lib)] internal static extern void aver_audio_set_occlusion_curve(float minCutoffHz, float minVolume);
    [DllImport(Lib)] internal static extern void aver_audio_set_reverb(float wet, float decaySeconds, float damping);
    [DllImport(Lib)] internal static extern int  aver_audio_stream_underruns();
    [DllImport(Lib)] internal static extern int  aver_audio_active_streams();
}

/// <summary>The shape of a fade, mirroring <c>AVER_AUDIO_FADE_*</c> in audio_abi.h.</summary>
public enum FadeCurve
{
    /// <summary>Straight-line gain. Two overlapping tracks dip about 3 dB at the midpoint.</summary>
    Linear = 0,
    /// <summary>Sine and cosine legs, so two overlapping tracks keep constant power.</summary>
    EqualPower = 1,
}

public readonly partial struct Voice
{
    /// <summary>Ramps this voice's fade gain (on top of Volume) to <paramref name="gain"/>.
    /// With <paramref name="stopWhenDone"/> the voice ends when the ramp does.</summary>
    public void FadeTo(float gain, float seconds, FadeCurve curve = FadeCurve.EqualPower, bool stopWhenDone = false)
    {
        if (Handle == 0) return;
        try { AxStream.aver_audio_fade_voice(Handle, gain, seconds, (int)curve, stopWhenDone ? 1 : 0); }
        catch (DllNotFoundException) { }
    }

    /// <summary>Fades to silence over <paramref name="seconds"/> and ends the voice.</summary>
    public void FadeOut(float seconds, FadeCurve curve = FadeCurve.EqualPower) => FadeTo(0f, seconds, curve, true);

    /// <summary>0 clear to 1 fully blocked: the mixer low-passes and ducks the voice.
    /// Normally driven by an occlusion emitter (<see cref="Entity.SetAudioOcclusion"/>).</summary>
    public float Occlusion
    {
        set { if (Handle != 0) { try { AxStream.aver_audio_set_voice_occlusion(Handle, value); } catch (DllNotFoundException) { } } }
    }

    /// <summary>How much of this voice feeds the reverb, 0 to 1.</summary>
    public float ReverbSend
    {
        set { if (Handle != 0) { try { AxStream.aver_audio_set_voice_reverb_send(Handle, value); } catch (DllNotFoundException) { } } }
    }
}

public static partial class Audio
{
    /// <summary>Streams a file from disk, flat in both ears: long ambience, dialogue, music.
    /// Silent until a quarter second is buffered. Stereo or mono only.</summary>
    public static Voice PlayStream(string path, float volume = 1f, float pitch = 1f, bool looping = false,
                                   Bus bus = Bus.Sfx, float fadeInSeconds = 0f)
    {
        if (string.IsNullOrEmpty(path)) return Voice.None;
        try { return new Voice(AxStream.aver_audio_stream_play(path, volume, pitch, looping ? 1 : 0, (int)bus, fadeInSeconds)); }
        catch (DllNotFoundException) { return Voice.None; }
    }

    /// <summary>Streams a file as a positioned voice.</summary>
    public static Voice PlayStreamAt(string path, Vec3 position, float volume = 1f, float pitch = 1f,
                                     bool looping = false, Bus bus = Bus.Sfx, float innerCm = 100f,
                                     float outerCm = 2000f, float fadeInSeconds = 0f)
    {
        if (string.IsNullOrEmpty(path)) return Voice.None;
        try
        {
            return new Voice(AxStream.aver_audio_stream_play_at(path, position.X, position.Y, position.Z, volume, pitch,
                                                                looping ? 1 : 0, (int)bus, innerCm, outerCm, fadeInSeconds));
        }
        catch (DllNotFoundException) { return Voice.None; }
    }

    /// <summary>The listener-side reverb. <paramref name="wet"/> 0 turns it off; decay is the RT60 in seconds.
    /// Reverb zones (<see cref="Entity.SetReverbZone"/>) drive this themselves; call it for a fixed room.</summary>
    public static void SetReverb(float wet, float decaySeconds = 1.5f, float damping = 0.4f)
    {
        try { AxStream.aver_audio_set_reverb(wet, decaySeconds, damping); } catch (DllNotFoundException) { }
    }

    /// <summary>The low-pass cutoff and linear volume a fully occluded voice reaches.</summary>
    public static void SetOcclusionCurve(float minCutoffHz, float minVolume)
    {
        try { AxStream.aver_audio_set_occlusion_curve(minCutoffHz, minVolume); } catch (DllNotFoundException) { }
    }

    /// <summary>Blocks in which a stream ran out of decoded audio. Each is an audible gap.</summary>
    public static int StreamUnderruns { get { try { return AxStream.aver_audio_stream_underruns(); } catch (DllNotFoundException) { return 0; } } }

    /// <summary>Streams currently held by a playing voice.</summary>
    public static int ActiveStreams { get { try { return AxStream.aver_audio_active_streams(); } catch (DllNotFoundException) { return 0; } } }
}

/// <summary>The music slot. Starting a track crossfades from the current one.</summary>
public static class Music
{
    /// <summary>Plays <paramref name="path"/> on the Music bus, fading the current track out as it fades in.
    /// Returns the new voice, or <see cref="Voice.None"/> (the current track keeps playing).</summary>
    public static Voice Play(string path, float fadeSeconds = 2f, float volume = 1f, bool looping = true,
                             FadeCurve curve = FadeCurve.EqualPower)
    {
        if (string.IsNullOrEmpty(path)) return Voice.None;
        try { return new Voice(AxStream.aver_audio_music_play(path, volume, fadeSeconds, looping ? 1 : 0, (int)curve)); }
        catch (DllNotFoundException) { return Voice.None; }
    }

    /// <summary>Fades the current track out and ends it.</summary>
    public static void Stop(float fadeSeconds = 1f)
    {
        try { AxStream.aver_audio_music_stop(fadeSeconds); } catch (DllNotFoundException) { }
    }

    /// <summary>The current track's voice, or <see cref="Voice.None"/> once it has ended.</summary>
    public static Voice Current
    {
        get { try { return new Voice(AxStream.aver_audio_music_voice()); } catch (DllNotFoundException) { return Voice.None; } }
    }
}

public readonly partial struct Entity
{
    /// <summary>Makes this entity a reverb volume. Inside it the listener hears the given reverb; the weight
    /// falls to zero <paramref name="blendDistanceCm"/> outside the surface. The entity's transform places and
    /// scales it. <paramref name="shape"/> 0 is a box (half extents) and 1 a sphere (radius = extents.X).
    /// Every field is written: the component arrives zero-filled.</summary>
    public bool SetReverbZone(Vec3 halfExtentsCm, float blendDistanceCm = 200f, float wet = 0.35f,
                              float decaySeconds = 1.8f, float damping = 0.4f, int priority = 0, int shape = 0)
    {
        if (!HasComponent("CReverbZone") && !AddComponent("CReverbZone")) return false;
        SetInt("CReverbZone.enabled", 1);
        SetInt("CReverbZone.shape", shape);
        SetVec3("CReverbZone.halfExtentsCm", halfExtentsCm);
        SetFloat("CReverbZone.radiusCm", halfExtentsCm.X);
        SetFloat("CReverbZone.blendDistanceCm", blendDistanceCm);
        SetInt("CReverbZone.priority", priority);
        SetFloat("CReverbZone.wet", wet);
        SetFloat("CReverbZone.decaySec", decaySeconds);
        return SetFloat("CReverbZone.damping", damping);
    }

    /// <summary>Drives <paramref name="voice"/>'s occlusion from line-of-sight rays between the listener and
    /// this entity, and moves the voice with the entity. The voice ends the component's work when it ends.</summary>
    public bool SetAudioOcclusion(Voice voice, int rayCount = 5, float probeRadiusCm = 50f)
    {
        if (!HasComponent("CAudioOcclusion") && !AddComponent("CAudioOcclusion")) return false;
        SetInt("CAudioOcclusion.enabled", 1);
        SetInt("CAudioOcclusion.voice", voice.Handle);
        SetInt("CAudioOcclusion.followEntity", 1);
        SetInt("CAudioOcclusion.rayCount", rayCount);
        SetFloat("CAudioOcclusion.probeRadiusCm", probeRadiusCm);
        SetFloat("CAudioOcclusion.thinkIntervalSec", 0.1f);
        SetFloat("CAudioOcclusion.riseSec", 0.08f);
        return SetFloat("CAudioOcclusion.fallSec", 0.25f);
    }
}
