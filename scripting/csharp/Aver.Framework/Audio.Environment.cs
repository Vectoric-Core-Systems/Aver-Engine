// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Script-facing environment audio: zone send reverbs (algorithmic or convolution), and the scene settings for sends and diffraction.
// Same silent-machine contract as Audio.cs. See docs/AUDIO.md section 15.

using System;
using System.Runtime.InteropServices;
using Aver.Scene;

namespace Aver.Framework;

/// <summary>P/Invoke for the environment calls of Aver.Audio.Abi. Internal: scripts use <see cref="Audio"/>.</summary>
internal static class AxEnv
{
    private const string Lib = "Aver.Audio.Abi";

    [DllImport(Lib)] internal static extern int  aver_audio_set_zone_reverb(int slot, float wet, float decaySeconds, float damping, int impulseSound, float predelayMs);
    [DllImport(Lib)] internal static extern void aver_audio_set_voice_zone_send(int voice, int slot, float send);
}

public readonly partial struct Voice
{
    /// <summary>How much of this voice feeds zone send reverb <paramref name="slot"/> (0 to <see cref="Audio.ZoneReverbSlots"/> - 1),
    /// 0 to 1. Zero until set. Reverb zones with sends enabled on the emitter (<see cref="Entity.SetAudioZoneSends"/>) drive this themselves.</summary>
    public void SetZoneSend(int slot, float send)
    {
        if (Handle == 0) return;
        try { AxEnv.aver_audio_set_voice_zone_send(Handle, slot, send); } catch (DllNotFoundException) { }
    }
}

public static partial class Audio
{
    /// <summary>How many zone send reverbs the mixer pre-allocates.</summary>
    public const int ZoneReverbSlots = 4;

    /// <summary>Sets zone send reverb <paramref name="slot"/>: <paramref name="wet"/> is its return level (0 turns the slot off),
    /// decay the RT60 in seconds. A loaded <paramref name="impulse"/> makes it a convolution reverb with that sound (any format the loaders
    /// decode, any rate; the engine keeps its own copy) and <paramref name="predelayMs"/> delays the wet signal. False for a bad slot or an
    /// unusable impulse, or with no audio.</summary>
    public static bool SetZoneReverb(int slot, float wet, float decaySeconds = 1.5f, float damping = 0.4f, Sound impulse = default, float predelayMs = 0f)
    {
        try { return AxEnv.aver_audio_set_zone_reverb(slot, wet, decaySeconds, damping, impulse.Handle, predelayMs) != 0; }
        catch (DllNotFoundException) { return false; }
    }
}

public readonly partial struct Entity
{
    /// <summary>Gives this reverb zone a convolution reverb: while the zone has a send reverb slot it convolves with the loaded
    /// <paramref name="impulse"/> instead of the algorithmic reverb. <see cref="Sound.None"/> goes back to the algorithmic one.</summary>
    public bool SetReverbZoneImpulse(Sound impulse, float predelayMs = 0f)
    {
        if (!HasComponent("CReverbZone")) return false;
        SetInt("CReverbZone.impulseSound", impulse.Handle);
        return SetFloat("CReverbZone.predelayMs", predelayMs);
    }

    /// <summary>Makes the voice of this entity's occlusion emitter (<see cref="SetAudioOcclusion"/>) send to the reverb zone(s) it is
    /// inside, through the mixer's send reverbs, instead of only hearing the listener's. <paramref name="reverbSendBase"/> is the voice's
    /// overall reverb level (1 on Sfx and Voice, 0 on Music and Ui by default).</summary>
    public bool SetAudioZoneSends(bool enabled = true, float reverbSendBase = 1f)
    {
        if (!HasComponent("CAudioOcclusion")) return false;
        SetInt("CAudioOcclusion.zoneSends", enabled ? 1 : 0);
        return SetFloat("CAudioOcclusion.reverbSendBase", reverbSendBase);
    }

    /// <summary>Turns diffraction on for this entity's occlusion emitter: when the direct path is blocked the probe looks for the
    /// shortest path around an edge or through an opening and uses its gain and low-pass instead of the fully occluded value.
    /// Off by default. <paramref name="maxRays"/> bounds the rays one probe may spend.</summary>
    public bool SetAudioDiffraction(bool enabled = true, int directions = 8, int maxRays = 256, float maxOffsetCm = 3200f, float maxOcclusion = 0.85f)
    {
        if (!HasComponent("CAudioOcclusion")) return false;
        SetInt("CAudioOcclusion.diffraction", enabled ? 1 : 0);
        SetInt("CAudioOcclusion.diffractionDirections", directions);
        SetInt("CAudioOcclusion.diffractionMaxRays", maxRays);
        SetFloat("CAudioOcclusion.diffractionMaxOffsetCm", maxOffsetCm);
        return SetFloat("CAudioOcclusion.diffractionMaxOcclusion", maxOcclusion);
    }
}
