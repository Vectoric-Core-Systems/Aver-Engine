// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Godot's AudioServer: the bus layout (named buses, sends, volume, mute, solo, bypass, effect chains, peak meters) over the engine's bus graph
// in Aver.Audio. The server needs the engine's audio library (Aver.Audio.Abi); without it the calls are harmless and the layout is just Master.
// Where the sound goes is the host's business: with an output device the mixer feeds it; with none (a server, a test) the mixer runs offline
// and, in a game, is stepped by the frame loop so players still finish. docs/plans/fabric/G5_PARITY.md, S1.

using Aver.Scene;

namespace Aver.Fabric;

/// <summary>A saved bus layout (Godot's AudioBusLayout), held as the engine's .ocbus text.</summary>
public class AudioBusLayout : Resource
{
    /// <summary>The layout as .ocbus text (see aver/formats/OcBusLayout.hpp).</summary>
    public string Text { get; set; } = "";
}

/// <summary>Godot's AudioServer singleton.</summary>
public static class AudioServer
{
    public enum SpeakerMode { ModeStereo = 0, Surround31 = 1, Surround51 = 2, Surround71 = 3 }
    public enum PlaybackType { Default = 0, Stream = 1, Sample = 2, Max = 3 }

    /// <summary>Godot's project setting audio/general/3d_panning_strength (default 0.5), which scales every 3D player's panning_strength.</summary>
    internal static float GlobalPanningStrength => (float)ProjectSettings.GetSetting("audio/general/3d_panning_strength", 0.5).AsDouble();

    private static Vector3 s_lastCameraPos;
    private static bool s_haveCameraPos;
    private static bool s_ready;
    private static bool s_autoPump;
    private static float[] s_scratch = new float[8192];
    private static long s_pumpFrame = -1;
    private static readonly Dictionary<int, AudioEffect> s_wrappers = new();

    /// <summary>Bumps whenever the engine's sound table is reset (a new audio session), so a stream knows to hand its sound over again.</summary>
    internal static int Generation { get; private set; }

    /// <summary>A test points this at an .ocbus file instead of the project's AUDIO.BUSLAYOUT.</summary>
    internal static string? LayoutOverride;

    public static event Action? BusLayoutChanged;

    // ---- setup ----

    /// <summary>Makes sure the engine's audio exists and the bus graph is on. True when sound can be made.</summary>
    internal static bool EnsureReady()
    {
        if (s_ready) return true;
        if (!AudioNative.Available) return false;
        if (AudioNative.aver_audio_ready() == 0)
        {
            // No output device (a headless run): the same mixer without one, stepped by the frame loop.
            if (AudioNative.aver_audio_offline_init(48000, 2) == 0) return false;
            s_autoPump = true;
        }
        AudioNative.aver_audio_graph_set_active(1);
        s_ready = true;
        Generation++;
        LoadProjectLayout();
        return true;
    }

    private static void LoadProjectLayout()
    {
        string rel = LayoutOverride ?? "";
        if (rel.Length == 0)
        {
            try { rel = Aver.Framework.ProjectSettings.Get("AUDIO.BUSLAYOUT", ""); }
            catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { rel = ""; }
        }
        if (rel.Length == 0) return;
        string full = File.Exists(rel) ? rel : System.IO.Path.Combine(ResourceLoader.ContentRoot, rel.Replace('/', System.IO.Path.DirectorySeparatorChar));
        if (!File.Exists(full)) { Aver.Scripting.Log.Warn($"[Fabric] the bus layout '{rel}' does not exist"); return; }
        int skipped = AudioNative.aver_audio_bus_load_layout(full);
        if (skipped < 0) Aver.Scripting.Log.Warn($"[Fabric] the bus layout '{rel}' could not be read");
        else if (skipped > 0) Aver.Scripting.Log.Warn($"[Fabric] {skipped} effect(s) of the bus layout '{rel}' are not supported and were left out");
        ForgetEffects();
    }

    /// <summary>Tests: a fresh offline mixer at this rate (the device is never opened). Every sound, voice and bus is gone.</summary>
    internal static bool ResetOffline(int sampleRate = 48000, int channels = 2)
    {
        if (!AudioNative.Available) return false;
        AudioNative.aver_audio_shutdown();
        s_ready = false;
        s_autoPump = false;
        ForgetEffects();
        if (AudioNative.aver_audio_offline_init(sampleRate, channels) == 0) return false;
        AudioNative.aver_audio_graph_set_active(1);
        s_ready = true;
        Generation++;
        LoadProjectLayout();
        return true;
    }

    /// <summary>Tests: renders frames from the offline mixer into a buffer (interleaved, channels as initialised).</summary>
    internal static float[] RenderOffline(int frames)
    {
        int ch = Math.Max(1, AudioNative.aver_audio_channels());
        float[] buf = new float[frames * ch];
        AudioNative.aver_audio_offline_mix(buf, frames);
        return buf;
    }

    /// <summary>Called by every player each frame: steps a device-less mixer, collects freed sounds, once per frame.</summary>
    internal static void FrameUpdate(double delta)
    {
        long frame = SceneTree.Instance.GetFrame();
        if (frame == s_pumpFrame) return;
        s_pumpFrame = frame;
        if (!s_ready) return;
        AudioNative.aver_audio_collect();
        UpdateListener(delta);
        if (!s_autoPump) return;
        int frames = (int)Math.Min(4096, Math.Max(0, delta * GetMixRate()));
        if (frames * 2 > s_scratch.Length) s_scratch = new float[frames * 2];
        if (frames > 0) AudioNative.aver_audio_offline_mix(s_scratch, frames);
    }

    /// <summary>The listener: the current AudioListener3D, else the current Camera3D, else the origin looking down -Z. Its velocity (for Doppler)
    /// is the camera's, tracked only while the camera's <c>doppler_tracking</c> is on, as in Godot.</summary>
    internal static void UpdateListener(double delta)
    {
        Camera3D? cam = Camera3D.CurrentCamera;
        Node3D? source = AudioListener3D.Current is { IsAlive: true } l && l.IsInsideTree() ? l : cam is { IsAlive: true } c && c.IsInsideTree() ? c : null;
        Transform3D t = source != null ? source.GlobalTransform : Transform3D.Identity;
        Vec3 pos = FabricSpace.ToAverPosition(t.Origin);
        Vec3 fwd = FabricSpace.ToAverDirection((-t.Basis.Z).Normalized());
        Vec3 right = FabricSpace.ToAverDirection(t.Basis.X.Normalized());
        AudioNative.aver_audio_set_listener(pos.X, pos.Y, pos.Z, fwd.X, fwd.Y, fwd.Z, right.X, right.Y, right.Z);

        Vector3 vel = Vector3.Zero;
        if (cam is { IsAlive: true, DopplerTracking: not Camera3D.DopplerTrackingEnum.Disabled } tracked && tracked.IsInsideTree())
        {
            Vector3 p = tracked.GlobalPosition;
            if (s_haveCameraPos && delta > 1e-6) vel = (p - s_lastCameraPos) / (float)delta;
            s_lastCameraPos = p;
            s_haveCameraPos = true;
        }
        else s_haveCameraPos = false;
        Vec3 v = FabricSpace.ToAverPosition(vel);
        AudioNative.aver_audio_set_listener_velocity(v.X, v.Y, v.Z);
    }

    private static void ForgetEffects()
    {
        foreach (AudioEffect e in s_wrappers.Values) e.NativeIds.Clear();
        s_wrappers.Clear();
    }

    private static bool Live => EnsureReady();
    private static void Changed() => BusLayoutChanged?.Invoke();

    // ---- mix information ----

    public static int GetMixRate() => EnsureReady() ? AudioNative.aver_audio_sample_rate() : 48000;
    public static SpeakerMode GetSpeakerMode() => SpeakerMode.ModeStereo;
    public static double GetOutputLatency() => 0.015;
    public static double GetTimeToNextMix() => 0.0;
    public static double GetTimeSinceLastMix() => 0.0;
    public static string GetDriverName() => !EnsureReady() ? "Dummy" : AudioNative.aver_audio_is_offline() != 0 ? "Dummy" : "Aver";
    public static void Lock() { }
    public static void Unlock() { }

    /// <summary>Scales how fast every sound plays (0.5 = half speed, an octave down).</summary>
    public static float PlaybackSpeedScale
    {
        get => Live ? AudioNative.aver_audio_speed_scale() : 1f;
        set { if (Live) AudioNative.aver_audio_set_speed_scale(value); }
    }

    // ---- buses ----

    public static int BusCount
    {
        get => Live ? AudioNative.aver_audio_bus_count() : 1;
        set { if (Live) { AudioNative.aver_audio_bus_set_count(value); Changed(); } }
    }

    public static int GetBusCount() => BusCount;
    public static void SetBusCount(int amount) => BusCount = amount;

    public static void AddBus(int atPosition = -1)
    {
        if (!Live) return;
        AudioNative.aver_audio_bus_add(atPosition);
        Changed();
    }

    public static void RemoveBus(int index)
    {
        if (!Live || index <= 0) return;
        ForgetBusEffects(index);
        if (AudioNative.aver_audio_bus_remove(index) != 0) Changed();
    }

    /// <summary>Moves a bus; <paramref name="toIndex"/> is numbered before the move (Godot's rule), -1 appends. Master cannot move.</summary>
    public static void MoveBus(int index, int toIndex)
    {
        if (Live && AudioNative.aver_audio_bus_move(index, toIndex) != 0) Changed();
    }

    public static int GetBusIndex(StringName busName) => Live ? AudioNative.aver_audio_bus_find(busName.ToString()) : (busName.ToString() == "Master" ? 0 : -1);

    public static StringName GetBusName(int busIdx) => Live ? AudioNative.Text((b, n) => AudioNative.aver_audio_bus_name(busIdx, b, n), 64) : (busIdx == 0 ? "Master" : "");

    public static void SetBusName(int busIdx, string name)
    {
        if (!Live) return;
        AudioNative.aver_audio_bus_set_name(busIdx, name);
        Changed();
    }

    public static void SetBusVolumeDb(int busIdx, float volumeDb) { if (Live) AudioNative.aver_audio_bus_set_volume_db(busIdx, volumeDb); }
    public static float GetBusVolumeDb(int busIdx) => Live ? AudioNative.aver_audio_bus_get_volume_db(busIdx) : 0f;
    public static void SetBusVolumeLinear(int busIdx, float linear) => SetBusVolumeDb(busIdx, linear <= 0f ? -80f : Mathf.LinearToDb(linear));
    public static float GetBusVolumeLinear(int busIdx) => Mathf.DbToLinear(GetBusVolumeDb(busIdx));

    public static void SetBusSend(int busIdx, StringName send) { if (Live) AudioNative.aver_audio_bus_set_send(busIdx, send.ToString()); }
    public static StringName GetBusSend(int busIdx) => Live ? AudioNative.Text((b, n) => AudioNative.aver_audio_bus_send_name(busIdx, b, n), 64) : "";

    public static void SetBusSolo(int busIdx, bool enable) { if (Live) AudioNative.aver_audio_bus_set_flag(busIdx, (int)AudioBusFlag.Solo, enable ? 1 : 0); }
    public static bool IsBusSolo(int busIdx) => Live && AudioNative.aver_audio_bus_get_flag(busIdx, (int)AudioBusFlag.Solo) != 0;
    public static void SetBusMute(int busIdx, bool enable) { if (Live) AudioNative.aver_audio_bus_set_flag(busIdx, (int)AudioBusFlag.Mute, enable ? 1 : 0); }
    public static bool IsBusMute(int busIdx) => Live && AudioNative.aver_audio_bus_get_flag(busIdx, (int)AudioBusFlag.Mute) != 0;
    public static void SetBusBypassEffects(int busIdx, bool enable) { if (Live) AudioNative.aver_audio_bus_set_flag(busIdx, (int)AudioBusFlag.Bypass, enable ? 1 : 0); }
    public static bool IsBusBypassingEffects(int busIdx) => Live && AudioNative.aver_audio_bus_get_flag(busIdx, (int)AudioBusFlag.Bypass) != 0;

    /// <summary>Peak of the bus's last block after its effects and volume, in dB (-200 when silent). Channel 0 is the stereo pair's left.</summary>
    public static float GetBusPeakVolumeLeftDb(int busIdx, int channel) => Live ? AudioNative.aver_audio_bus_peak_db(busIdx, 0) : -200f;
    public static float GetBusPeakVolumeRightDb(int busIdx, int channel) => Live ? AudioNative.aver_audio_bus_peak_db(busIdx, 1) : -200f;
    public static bool IsBusChannelActive(int busIdx, int channel) => Live && channel == 0 && busIdx >= 0 && busIdx < AudioNative.aver_audio_bus_count();
    public static int GetBusChannels(int busIdx) => 2;

    // ---- effects ----

    private static void ForgetBusEffects(int bus)
    {
        for (int i = 0; i < AudioNative.aver_audio_effect_count(bus); i++)
        {
            int id = AudioNative.aver_audio_effect_id(bus, i);
            if (s_wrappers.Remove(id, out AudioEffect? w)) w.Detach(id);
        }
    }

    public static int GetBusEffectCount(int busIdx) => Live ? AudioNative.aver_audio_effect_count(busIdx) : 0;

    /// <summary>Puts an effect on a bus (at the end by default). The effect object then controls the engine's effect: change its properties and the sound changes.</summary>
    public static void AddBusEffect(int busIdx, AudioEffect effect, int atPosition = -1)
    {
        if (!Live || effect == null) return;
        if (effect.NativeClass.Length == 0) { Aver.Scripting.Log.Warn($"[Fabric] {effect.GetClass()} cannot be put on a bus"); return; }
        int id = AudioNative.aver_audio_effect_add(busIdx, effect.NativeClass, atPosition);
        if (id == 0) { Aver.Scripting.Log.Warn($"[Fabric] cannot add {effect.GetClass()} to bus {busIdx}"); return; }
        effect.Attach(id);
        s_wrappers[id] = effect;
        Changed();
    }

    public static void RemoveBusEffect(int busIdx, int effectIdx)
    {
        if (!Live) return;
        int id = AudioNative.aver_audio_effect_id(busIdx, effectIdx);
        if (id == 0) return;
        if (s_wrappers.Remove(id, out AudioEffect? w)) w.Detach(id);
        AudioNative.aver_audio_effect_remove(busIdx, effectIdx);
        Changed();
    }

    /// <summary>The effect at an index. One that came from a loaded layout is wrapped on first request, with the engine's values.</summary>
    public static AudioEffect? GetBusEffect(int busIdx, int effectIdx)
    {
        if (!Live) return null;
        int id = AudioNative.aver_audio_effect_id(busIdx, effectIdx);
        if (id == 0) return null;
        if (s_wrappers.TryGetValue(id, out AudioEffect? w)) return w;
        string cls = AudioNative.Text((b, n) => AudioNative.aver_audio_effect_class(id, b, n), 64);
        AudioEffect? made = AudioEffect.Create(cls);
        if (made == null) return null;
        made.Adopt(id);
        s_wrappers[id] = made;
        return made;
    }

    public static void SwapBusEffects(int busIdx, int effectIdx, int byEffectIdx)
    {
        if (!Live || effectIdx == byEffectIdx) return;
        AudioNative.aver_audio_effect_move(busIdx, effectIdx, byEffectIdx);
        Changed();
    }

    public static void SetBusEffectEnabled(int busIdx, int effectIdx, bool enabled) { if (Live) AudioNative.aver_audio_effect_set_enabled(busIdx, effectIdx, enabled ? 1 : 0); }
    public static bool IsBusEffectEnabled(int busIdx, int effectIdx) => Live && AudioNative.aver_audio_effect_get_enabled(busIdx, effectIdx) != 0;

    /// <summary>The running form of an effect (what a spectrum analyser is read through).</summary>
    public static AudioEffectInstance? GetBusEffectInstance(int busIdx, int effectIdx, int channel = 0)
    {
        if (!Live) return null;
        int id = AudioNative.aver_audio_effect_id(busIdx, effectIdx);
        if (id == 0) return null;
        AudioEffect? fx = GetBusEffect(busIdx, effectIdx);
        return fx is AudioEffectSpectrumAnalyzer ? new AudioEffectSpectrumAnalyzerInstance { Id = id } : new AudioEffectInstance { Id = id };
    }

    // ---- layouts ----

    public static AudioBusLayout GenerateBusLayout()
    {
        string text = Live ? AudioNative.Text((b, n) => AudioNative.aver_audio_bus_layout_text(b, n), 2048) : "";
        return new AudioBusLayout { Text = text };
    }

    /// <summary>Replaces every bus and effect with the layout's. Effect objects fetched before are no longer on the buses.</summary>
    public static void SetBusLayout(AudioBusLayout layout)
    {
        if (!Live || layout == null) return;
        ForgetEffects();
        int skipped = AudioNative.aver_audio_bus_apply_layout_text(layout.Text);
        if (skipped > 0) Aver.Scripting.Log.Warn($"[Fabric] {skipped} effect(s) of the bus layout are not supported and were left out");
        Changed();
    }

    // ---- unsupported device calls ----

    public static string[] GetInputDeviceList() => System.Array.Empty<string>();
    public static string GetInputDevice() => "Default";
    public static string[] GetOutputDeviceList() => new[] { "Default" };
    public static string GetOutputDevice() => "Default";
}
