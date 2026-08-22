// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// Script-facing audio: loading sounds, playing them flat or positioned, and steering live voices.

using System;
using System.Runtime.InteropServices;
using Aver.Scene;

namespace Aver.Framework;

/// <summary>P/Invoke into Aver.Audio.Abi. Internal: scripts use <see cref="Audio"/>.</summary>
/// <remarks>NO NativeResolver ENTRY, and that is deliberate rather than an omission. The resolver in
/// NativeResolver.cs special-cases exactly two names, "Aver.Framework" and "Aver.Scene", because
/// those two collide with MANAGED assemblies of the same name staged in bin/Scripting — default
/// probing would find the IL one and every symbol would come back missing. "Aver.Audio.Abi" has no
/// managed namesake anywhere, so ordinary probing finds bin/Aver.Audio.Abi.dll on the first try.
/// That is the same path Aver.Physics already takes; see Phys in Physics.cs.</remarks>
internal static class Ax
{
    private const string Lib = "Aver.Audio.Abi";

    [DllImport(Lib)] internal static extern int  aver_audio_ready();
    [DllImport(Lib)] internal static extern int  aver_audio_sample_rate();
    [DllImport(Lib)] internal static extern int  aver_audio_channels();
    [DllImport(Lib)] internal static extern int  aver_audio_underruns();
    [DllImport(Lib)] internal static extern void aver_audio_collect();

    [DllImport(Lib)] internal static extern int  aver_audio_load([MarshalAs(UnmanagedType.LPUTF8Str)] string utf8Path);
    [DllImport(Lib)] internal static extern void aver_audio_unload(int sound);

    [DllImport(Lib)] internal static extern int  aver_audio_play(int sound, float volume, float pitch, int looping, int bus);
    [DllImport(Lib)] internal static extern int  aver_audio_play_at(int sound, float x, float y, float z,
                                                                   float volume, float pitch, int looping, int bus,
                                                                   float innerCm, float outerCm);
    [DllImport(Lib)] internal static extern void aver_audio_stop(int voice);
    [DllImport(Lib)] internal static extern void aver_audio_stop_all();
    [DllImport(Lib)] internal static extern int  aver_audio_playing(int voice);

    [DllImport(Lib)] internal static extern void aver_audio_set_voice_volume(int voice, float volume);
    [DllImport(Lib)] internal static extern void aver_audio_set_voice_pitch(int voice, float pitch);
    [DllImport(Lib)] internal static extern void aver_audio_set_voice_position(int voice, float x, float y, float z);

    [DllImport(Lib)] internal static extern void aver_audio_set_listener(float px, float py, float pz,
                                                                        float fx, float fy, float fz,
                                                                        float rx, float ry, float rz);

    [DllImport(Lib)] internal static extern float aver_audio_bus_volume(int bus);
    [DllImport(Lib)] internal static extern void  aver_audio_set_bus_volume(int bus, float volume);
    [DllImport(Lib)] internal static extern float aver_audio_master_volume();
    [DllImport(Lib)] internal static extern void  aver_audio_set_master_volume(float volume);

    [DllImport(Lib)] internal static extern int aver_audio_active_voices();
    [DllImport(Lib)] internal static extern int aver_audio_stolen_voices();
}

/// <summary>A decoded sound, ready to play. 0 is "nothing", which every call below tolerates.</summary>
/// <remarks>A HANDLE, NOT A FILE. <see cref="Audio.Load"/> decodes once and hands back the same
/// handle for the same path forever after, so loading in a hot loop is wasteful but not wrong.</remarks>
public readonly struct Sound : IEquatable<Sound>
{
    /// <summary>The raw handle. 0 is invalid.</summary>
    public readonly int Handle;

    /// <summary>Wraps a raw handle.</summary>
    public Sound(int handle) { Handle = handle; }

    /// <summary>The absence of a sound.</summary>
    public static Sound None => new(0);

    /// <summary>True when this refers to a real loaded sound.</summary>
    public bool IsValid => Handle != 0;

    /// <summary>Releases it. The memory goes on the next <see cref="Audio.Collect"/>.</summary>
    public void Unload() { if (Handle != 0) Ax.aver_audio_unload(Handle); }

    /// <summary>Handle equality.</summary>
    public bool Equals(Sound other) => Handle == other.Handle;
    /// <summary>Handle equality.</summary>
    public override bool Equals(object? o) => o is Sound s && Equals(s);
    /// <summary>The handle itself.</summary>
    public override int GetHashCode() => Handle;
    /// <summary>Diagnostic text.</summary>
    public override string ToString() => $"Sound({Handle})";
}

/// <summary>One playing instance of a <see cref="Sound"/>. Stops mattering the moment it ends.</summary>
/// <remarks>A voice handle goes stale on its own: the mixer reclaims the slot when the sound
/// finishes, and every call here answers harmlessly for a handle that has already ended, so a script
/// holding one across frames never has to guard. Ask <see cref="IsPlaying"/> if it needs to know.</remarks>
public readonly struct Voice : IEquatable<Voice>
{
    /// <summary>The raw handle. 0 is invalid — which is what a refused play returns.</summary>
    public readonly int Handle;

    /// <summary>Wraps a raw handle.</summary>
    public Voice(int handle) { Handle = handle; }

    /// <summary>The absence of a voice, and what a refused play gives back.</summary>
    public static Voice None => new(0);

    /// <summary>True when the mixer accepted this play. NOT the same as still sounding.</summary>
    public bool IsValid => Handle != 0;

    /// <summary>Whether it is still sounding right now.</summary>
    public bool IsPlaying => Handle != 0 && Ax.aver_audio_playing(Handle) != 0;

    /// <summary>Stops it. Harmless if it already ended.</summary>
    public void Stop() { if (Handle != 0) Ax.aver_audio_stop(Handle); }

    /// <summary>Its volume, 0..1 and beyond at your own risk.</summary>
    public float Volume { set { if (Handle != 0) Ax.aver_audio_set_voice_volume(Handle, value); } }

    /// <summary>Its playback rate. 1 is unaltered; 2 is an octave up and twice as fast.</summary>
    public float Pitch { set { if (Handle != 0) Ax.aver_audio_set_voice_pitch(Handle, value); } }

    /// <summary>Moves a positioned voice. Does nothing to one started with <see cref="Audio.Play"/>.</summary>
    public Vec3 Position { set { if (Handle != 0) Ax.aver_audio_set_voice_position(Handle, value.X, value.Y, value.Z); } }

    /// <summary>Handle equality.</summary>
    public bool Equals(Voice other) => Handle == other.Handle;
    /// <summary>Handle equality.</summary>
    public override bool Equals(object? o) => o is Voice v && Equals(v);
    /// <summary>The handle itself.</summary>
    public override int GetHashCode() => Handle;
    /// <summary>Diagnostic text.</summary>
    public override string ToString() => $"Voice({Handle})";
}

/// <summary>The mixer as a script sees it: load a sound, play it, steer it, place the listener.</summary>
/// <remarks>Engine units throughout: centimetres, +X forward, +Y right, +Z up.
///
/// SILENCE IS A SUPPORTED CONFIGURATION. A machine with no output device, or a build with the audio
/// modules switched off, leaves <see cref="Ready"/> false and every call here a harmless no-op that
/// returns <see cref="Voice.None"/> — the same contract the native layer states for
/// aver_audio_init. A game never has to branch on whether sound exists.</remarks>
public static class Audio
{
    /// <summary>True when a device is open and the mixer thread is running. False, rather than
    /// throwing, on a build with no audio DLL at all — matching Physics.Ready's own guard.</summary>
    public static bool Ready
    {
        get { try { return Ax.aver_audio_ready() != 0; } catch (DllNotFoundException) { return false; } }
    }

    /// <summary>The device's sample rate in Hz, or 0 before audio has started.</summary>
    public static int SampleRate { get { try { return Ax.aver_audio_sample_rate(); } catch (DllNotFoundException) { return 0; } } }

    /// <summary>The mixer's channel count (1 or 2), or 0 before audio has started.</summary>
    public static int Channels { get { try { return Ax.aver_audio_channels(); } catch (DllNotFoundException) { return 0; } } }

    /// <summary>Buffers the device asked for and did not get in time. Every one is an audible gap,
    /// so this is worth watching in a profiler HUD rather than ignoring.</summary>
    public static int Underruns { get { try { return Ax.aver_audio_underruns(); } catch (DllNotFoundException) { return 0; } } }

    /// <summary>How many voices are sounding right now.</summary>
    public static int ActiveVoices { get { try { return Ax.aver_audio_active_voices(); } catch (DllNotFoundException) { return 0; } } }

    /// <summary>How many voices have been cut short to make room for a newer one. A TUNING FACT,
    /// not an error: it means the voice pool is smaller than the game's busiest moment.</summary>
    public static int StolenVoices { get { try { return Ax.aver_audio_stolen_voices(); } catch (DllNotFoundException) { return 0; } } }

    /// <summary>Reclaims unloaded sounds whose last voice has ended. Call once a frame.</summary>
    public static void Collect() { try { Ax.aver_audio_collect(); } catch (DllNotFoundException) { } }

    /// <summary>Loads a sound file and returns a handle, or <see cref="Sound.None"/>.
    /// The same path twice gives the SAME handle and does not decode again.</summary>
    public static Sound Load(string path)
    {
        if (string.IsNullOrEmpty(path)) return Sound.None;
        try { return new Sound(Ax.aver_audio_load(path)); } catch (DllNotFoundException) { return Sound.None; }
    }

    /// <summary>Plays a sound the same in both ears — UI clicks, music, narration.</summary>
    /// <param name="bus">Which bus to mix through, for group volume control. 0 is the default bus.</param>
    public static Voice Play(Sound sound, float volume = 1f, float pitch = 1f, bool looping = false, int bus = 0)
    {
        if (!sound.IsValid) return Voice.None;
        try { return new Voice(Ax.aver_audio_play(sound.Handle, volume, pitch, looping ? 1 : 0, bus)); }
        catch (DllNotFoundException) { return Voice.None; }
    }

    /// <summary>Plays a sound panned and attenuated against the listener.</summary>
    /// <param name="innerCm">Full volume at or inside this radius.</param>
    /// <param name="outerCm">Exactly silent at or beyond it.</param>
    public static Voice PlayAt(Sound sound, Vec3 position, float volume = 1f, float pitch = 1f,
                               bool looping = false, int bus = 0,
                               float innerCm = 100f, float outerCm = 2000f)
    {
        if (!sound.IsValid) return Voice.None;
        try
        {
            return new Voice(Ax.aver_audio_play_at(sound.Handle, position.X, position.Y, position.Z,
                                                   volume, pitch, looping ? 1 : 0, bus, innerCm, outerCm));
        }
        catch (DllNotFoundException) { return Voice.None; }
    }

    /// <summary>Loads and plays in one call, for the common "just make this noise" case.</summary>
    public static Voice PlayFile(string path, float volume = 1f, float pitch = 1f, bool looping = false, int bus = 0) =>
        Play(Load(path), volume, pitch, looping, bus);

    /// <summary>Stops every voice at once.</summary>
    public static void StopAll() { try { Ax.aver_audio_stop_all(); } catch (DllNotFoundException) { } }

    /// <summary>Where the ears are and which way they face. Push once a frame from whatever owns
    /// the camera; without it every positioned sound is panned against the origin.</summary>
    public static void SetListener(Vec3 position, Vec3 forward, Vec3 right)
    {
        try
        {
            Ax.aver_audio_set_listener(position.X, position.Y, position.Z,
                                       forward.X, forward.Y, forward.Z,
                                       right.X, right.Y, right.Z);
        }
        catch (DllNotFoundException) { }
    }

    /// <summary>The master volume, 0..1, applied over every bus.</summary>
    public static float MasterVolume
    {
        get { try { return Ax.aver_audio_master_volume(); } catch (DllNotFoundException) { return 0f; } }
        set { try { Ax.aver_audio_set_master_volume(value); } catch (DllNotFoundException) { } }
    }

    /// <summary>Reads one bus's volume, 0..1.</summary>
    public static float GetBusVolume(int bus)
    {
        try { return Ax.aver_audio_bus_volume(bus); } catch (DllNotFoundException) { return 0f; }
    }

    /// <summary>Sets one bus's volume, 0..1 — how a game gives the player separate SFX and music sliders.</summary>
    public static void SetBusVolume(int bus, float volume)
    {
        try { Ax.aver_audio_set_bus_volume(bus, volume); } catch (DllNotFoundException) { }
    }
}
