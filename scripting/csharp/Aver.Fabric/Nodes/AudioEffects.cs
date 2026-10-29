// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Godot's AudioEffect resources. Each one holds its parameters under Godot's property names; once the effect sits on a bus
// (AudioServer.AddBusEffect, or a loaded layout) the values live in the engine's mixer and a property change reaches it at once.
// The defaults written here are Godot's, and a test pins them to the engine's own table. docs/plans/fabric/G5_PARITY.md, S1.

namespace Aver.Fabric;

/// <summary>The base of every bus effect (Godot's AudioEffect).</summary>
public class AudioEffect : Resource
{
    private readonly Dictionary<string, float> _local = new(StringComparer.Ordinal);
    internal readonly List<int> NativeIds = new();

    /// <summary>The engine's class name for the effect (Godot's).</summary>
    internal virtual string NativeClass => "";

    internal float P(string name, float def)
    {
        if (NativeIds.Count > 0 && AudioNative.Available) return AudioNative.aver_audio_effect_get_param(NativeIds[0], name);
        return _local.TryGetValue(name, out float v) ? v : def;
    }

    internal void SetP(string name, float value)
    {
        _local[name] = value;
        if (!AudioNative.Available) return;
        foreach (int id in NativeIds) AudioNative.aver_audio_effect_set_param(id, name, value);
    }

    internal bool B(string name, bool def) => P(name, def ? 1f : 0f) > 0.5f;
    internal void SetB(string name, bool v) => SetP(name, v ? 1f : 0f);

    /// <summary>The effect was put on a bus: its values go to the engine.</summary>
    internal virtual void Attach(int id)
    {
        NativeIds.Add(id);
        foreach (var kv in _local) AudioNative.aver_audio_effect_set_param(id, kv.Key, kv.Value);
    }

    internal virtual void Detach(int id) => NativeIds.Remove(id);

    /// <summary>Binds this object to an engine effect that already exists (one a loaded layout made): the values are the engine's, not pushed.</summary>
    internal virtual void Adopt(int id) => NativeIds.Add(id);

    /// <summary>The effect class for an engine class name, or null.</summary>
    internal static AudioEffect? Create(string godotClass) => godotClass switch
    {
        "AudioEffectAmplify" => new AudioEffectAmplify(),
        "AudioEffectDelay" => new AudioEffectDelay(),
        "AudioEffectChorus" => new AudioEffectChorus(),
        "AudioEffectCompressor" => new AudioEffectCompressor(),
        "AudioEffectLimiter" => new AudioEffectLimiter(),
        "AudioEffectHardLimiter" => new AudioEffectHardLimiter(),
        "AudioEffectEQ6" => new AudioEffectEQ6(),
        "AudioEffectEQ10" => new AudioEffectEQ10(),
        "AudioEffectEQ21" => new AudioEffectEQ21(),
        "AudioEffectLowPassFilter" => new AudioEffectLowPassFilter(),
        "AudioEffectHighPassFilter" => new AudioEffectHighPassFilter(),
        "AudioEffectBandPassFilter" => new AudioEffectBandPassFilter(),
        "AudioEffectNotchFilter" => new AudioEffectNotchFilter(),
        "AudioEffectLowShelfFilter" => new AudioEffectLowShelfFilter(),
        "AudioEffectHighShelfFilter" => new AudioEffectHighShelfFilter(),
        "AudioEffectDistortion" => new AudioEffectDistortion(),
        "AudioEffectPhaser" => new AudioEffectPhaser(),
        "AudioEffectPitchShift" => new AudioEffectPitchShift(),
        "AudioEffectReverb" => new AudioEffectReverb(),
        "AudioEffectPanner" => new AudioEffectPanner(),
        "AudioEffectStereoEnhance" => new AudioEffectStereoEnhance(),
        "AudioEffectCapture" => new AudioEffectCapture(),
        "AudioEffectSpectrumAnalyzer" => new AudioEffectSpectrumAnalyzer(),
        _ => null,
    };

    // The scene stores indexed values ("voice/1/delay_ms", "band_db/1000_hz") under their Godot names; they go straight to the parameter.
    internal override bool TryDynamicSet(string name, Variant value)
    {
        if (name.Contains('/')) { SetP(name, (float)value.AsDouble()); return true; }
        return false;
    }
}

/// <summary>What a bus effect looks like once it runs (Godot's AudioEffectInstance).</summary>
public class AudioEffectInstance : RefCounted
{
    internal int Id;
}

public class AudioEffectAmplify : AudioEffect
{
    internal override string NativeClass => "AudioEffectAmplify";
    public float VolumeDb { get => P("volume_db", 0f); set => SetP("volume_db", value); }
}

public class AudioEffectDelay : AudioEffect
{
    internal override string NativeClass => "AudioEffectDelay";
    public float Dry { get => P("dry", 1f); set => SetP("dry", value); }
    public bool Tap1Active { get => B("tap1_active", true); set => SetB("tap1_active", value); }
    public float Tap1DelayMs { get => P("tap1_delay_ms", 250f); set => SetP("tap1_delay_ms", value); }
    public float Tap1LevelDb { get => P("tap1_level_db", -6f); set => SetP("tap1_level_db", value); }
    public float Tap1Pan { get => P("tap1_pan", 0.2f); set => SetP("tap1_pan", value); }
    public bool Tap2Active { get => B("tap2_active", true); set => SetB("tap2_active", value); }
    public float Tap2DelayMs { get => P("tap2_delay_ms", 500f); set => SetP("tap2_delay_ms", value); }
    public float Tap2LevelDb { get => P("tap2_level_db", -12f); set => SetP("tap2_level_db", value); }
    public float Tap2Pan { get => P("tap2_pan", -0.4f); set => SetP("tap2_pan", value); }
    public bool FeedbackActive { get => B("feedback_active", false); set => SetB("feedback_active", value); }
    public float FeedbackDelayMs { get => P("feedback_delay_ms", 340f); set => SetP("feedback_delay_ms", value); }
    public float FeedbackLevelDb { get => P("feedback_level_db", -6f); set => SetP("feedback_level_db", value); }
    public float FeedbackLowpass { get => P("feedback_lowpass", 16000f); set => SetP("feedback_lowpass", value); }
}

public class AudioEffectChorus : AudioEffect
{
    internal override string NativeClass => "AudioEffectChorus";
    private static readonly float[] DefDelay = { 15, 20, 12, 22 }, DefRate = { 0.8f, 1.2f, 1f, 0.7f }, DefDepth = { 2, 3, 2, 3 }, DefPan = { -0.5f, 0.5f, -0.25f, 0.25f };

    public int VoiceCount { get => (int)P("voice_count", 2f); set => SetP("voice_count", Math.Clamp(value, 1, 4)); }
    public float Dry { get => P("dry", 1f); set => SetP("dry", value); }
    public float Wet { get => P("wet", 0.5f); set => SetP("wet", value); }

    private static string V(int voice, string what) => $"voice/{voice + 1}/{what}";
    private static int Idx(int voice) => Math.Clamp(voice, 0, 3);

    public void SetVoiceDelayMs(int voiceIdx, float delayMs) => SetP(V(Idx(voiceIdx), "delay_ms"), delayMs);
    public float GetVoiceDelayMs(int voiceIdx) => P(V(Idx(voiceIdx), "delay_ms"), DefDelay[Idx(voiceIdx)]);
    public void SetVoiceRateHz(int voiceIdx, float rateHz) => SetP(V(Idx(voiceIdx), "rate_hz"), rateHz);
    public float GetVoiceRateHz(int voiceIdx) => P(V(Idx(voiceIdx), "rate_hz"), DefRate[Idx(voiceIdx)]);
    public void SetVoiceDepthMs(int voiceIdx, float depthMs) => SetP(V(Idx(voiceIdx), "depth_ms"), depthMs);
    public float GetVoiceDepthMs(int voiceIdx) => P(V(Idx(voiceIdx), "depth_ms"), DefDepth[Idx(voiceIdx)]);
    public void SetVoiceLevelDb(int voiceIdx, float levelDb) => SetP(V(Idx(voiceIdx), "level_db"), levelDb);
    public float GetVoiceLevelDb(int voiceIdx) => P(V(Idx(voiceIdx), "level_db"), 0f);
    public void SetVoiceCutoffHz(int voiceIdx, float cutoffHz) => SetP(V(Idx(voiceIdx), "cutoff_hz"), cutoffHz);
    public float GetVoiceCutoffHz(int voiceIdx) => P(V(Idx(voiceIdx), "cutoff_hz"), 8000f);
    public void SetVoicePan(int voiceIdx, float pan) => SetP(V(Idx(voiceIdx), "pan"), pan);
    public float GetVoicePan(int voiceIdx) => P(V(Idx(voiceIdx), "pan"), DefPan[Idx(voiceIdx)]);
}

public class AudioEffectCompressor : AudioEffect
{
    internal override string NativeClass => "AudioEffectCompressor";
    private StringName _sidechain = "";

    public float Threshold { get => P("threshold", 0f); set => SetP("threshold", value); }
    public float Ratio { get => P("ratio", 4f); set => SetP("ratio", value); }
    public float Gain { get => P("gain", 0f); set => SetP("gain", value); }
    public float AttackUs { get => P("attack_us", 20f); set => SetP("attack_us", value); }
    public float ReleaseMs { get => P("release_ms", 250f); set => SetP("release_ms", value); }
    public float Mix { get => P("mix", 1f); set => SetP("mix", value); }

    /// <summary>The bus whose signal drives the detector instead of this bus's own.</summary>
    public StringName Sidechain { get => _sidechain; set { _sidechain = value; ApplySidechain(); } }

    private void ApplySidechain()
    {
        if (!AudioNative.Available) return;
        foreach (int id in NativeIds)
            if (AudioNative.aver_audio_effect_find(id, out int bus, out int idx) != 0) AudioNative.aver_audio_effect_set_sidechain(bus, idx, _sidechain.ToString());
    }

    internal override void Attach(int id) { base.Attach(id); ApplySidechain(); }

    internal override void Adopt(int id)
    {
        base.Adopt(id);
        if (AudioNative.aver_audio_effect_find(id, out int bus, out int idx) != 0)
            _sidechain = AudioNative.Text((b, n) => AudioNative.aver_audio_effect_sidechain(bus, idx, b, n), 32);
    }
}

public class AudioEffectLimiter : AudioEffect
{
    internal override string NativeClass => "AudioEffectLimiter";
    public float CeilingDb { get => P("ceiling_db", -0.1f); set => SetP("ceiling_db", value); }
    public float ThresholdDb { get => P("threshold_db", 0f); set => SetP("threshold_db", value); }
    public float SoftClipDb { get => P("soft_clip_db", 2f); set => SetP("soft_clip_db", value); }
    public float SoftClipRatio { get => P("soft_clip_ratio", 10f); set => SetP("soft_clip_ratio", value); }
}

public class AudioEffectHardLimiter : AudioEffect
{
    internal override string NativeClass => "AudioEffectHardLimiter";
    public float PreGainDb { get => P("pre_gain_db", 0f); set => SetP("pre_gain_db", value); }
    public float CeilingDb { get => P("ceiling_db", -0.3f); set => SetP("ceiling_db", value); }
    public float Release { get => P("release", 0.1f); set => SetP("release", value); }
}

/// <summary>The common part of the three equalisers: bands by index, in dB.</summary>
public class AudioEffectEQ : AudioEffect
{
    private readonly float[] _hz;
    internal AudioEffectEQ(float[] hz) { _hz = hz; }
    public int GetBandCount() => _hz.Length;
    private string Band(int i) => $"band_db/{(int)_hz[Math.Clamp(i, 0, _hz.Length - 1)]}_hz";
    public void SetBandGainDb(int bandIdx, float volumeDb) { if (bandIdx >= 0 && bandIdx < _hz.Length) SetP(Band(bandIdx), volumeDb); }
    public float GetBandGainDb(int bandIdx) => bandIdx >= 0 && bandIdx < _hz.Length ? P(Band(bandIdx), 0f) : 0f;
    /// <summary>The centre frequency of a band in Hz (an Aver extension; Godot names the bands by it).</summary>
    public float GetBandFrequency(int bandIdx) => bandIdx >= 0 && bandIdx < _hz.Length ? _hz[bandIdx] : 0f;
}

public class AudioEffectEQ6 : AudioEffectEQ
{
    public AudioEffectEQ6() : base(new float[] { 32, 100, 320, 1000, 3200, 10000 }) { }
    internal override string NativeClass => "AudioEffectEQ6";
}

public class AudioEffectEQ10 : AudioEffectEQ
{
    public AudioEffectEQ10() : base(new float[] { 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 }) { }
    internal override string NativeClass => "AudioEffectEQ10";
}

public class AudioEffectEQ21 : AudioEffectEQ
{
    public AudioEffectEQ21() : base(new float[] { 22, 32, 44, 63, 90, 125, 175, 250, 350, 500, 700, 1000, 1400, 2000, 2800, 4000, 5600, 8000, 11000, 16000, 22000 }) { }
    internal override string NativeClass => "AudioEffectEQ21";
}

/// <summary>The common part of the filters (Godot's AudioEffectFilter): cutoff, resonance, gain and the steepness in stages of 6 dB per octave.</summary>
public class AudioEffectFilter : AudioEffect
{
    public enum FilterDB { Filter6Db = 0, Filter12Db = 1, Filter18Db = 2, Filter24Db = 3 }

    public float CutoffHz { get => P("cutoff_hz", 2000f); set => SetP("cutoff_hz", value); }
    public float Resonance { get => P("resonance", 0.5f); set => SetP("resonance", value); }
    public float Gain { get => P("gain", 1f); set => SetP("gain", value); }
    public FilterDB Db { get => (FilterDB)(int)P("db", 0f); set => SetP("db", (float)(int)value); }
}

public class AudioEffectLowPassFilter : AudioEffectFilter { internal override string NativeClass => "AudioEffectLowPassFilter"; }
public class AudioEffectHighPassFilter : AudioEffectFilter { internal override string NativeClass => "AudioEffectHighPassFilter"; }
public class AudioEffectBandPassFilter : AudioEffectFilter { internal override string NativeClass => "AudioEffectBandPassFilter"; }
public class AudioEffectNotchFilter : AudioEffectFilter { internal override string NativeClass => "AudioEffectNotchFilter"; }
public class AudioEffectLowShelfFilter : AudioEffectFilter { internal override string NativeClass => "AudioEffectLowShelfFilter"; }
public class AudioEffectHighShelfFilter : AudioEffectFilter { internal override string NativeClass => "AudioEffectHighShelfFilter"; }

public class AudioEffectDistortion : AudioEffect
{
    public enum ModeEnum { Clip = 0, Atan = 1, Lofi = 2, Overdrive = 3, WaveShape = 4 }

    internal override string NativeClass => "AudioEffectDistortion";
    public ModeEnum Mode { get => (ModeEnum)(int)P("mode", 0f); set => SetP("mode", (float)(int)value); }
    public float PreGain { get => P("pre_gain", 0f); set => SetP("pre_gain", value); }
    public float KeepHfHz { get => P("keep_hf_hz", 16000f); set => SetP("keep_hf_hz", value); }
    public float Drive { get => P("drive", 0f); set => SetP("drive", value); }
    public float PostGain { get => P("post_gain", 0f); set => SetP("post_gain", value); }
}

public class AudioEffectPhaser : AudioEffect
{
    internal override string NativeClass => "AudioEffectPhaser";
    public float RangeMinHz { get => P("range_min_hz", 440f); set => SetP("range_min_hz", value); }
    public float RangeMaxHz { get => P("range_max_hz", 1600f); set => SetP("range_max_hz", value); }
    public float RateHz { get => P("rate_hz", 0.5f); set => SetP("rate_hz", value); }
    public float Feedback { get => P("feedback", 0.7f); set => SetP("feedback", value); }
    public float Depth { get => P("depth", 1f); set => SetP("depth", value); }
}

public class AudioEffectPitchShift : AudioEffect
{
    public enum FFTSize { Size256 = 0, Size512 = 1, Size1024 = 2, Size2048 = 3, Size4096 = 4, Max = 5 }

    internal override string NativeClass => "AudioEffectPitchShift";
    public float PitchScale { get => P("pitch_scale", 1f); set => SetP("pitch_scale", value); }
    public int Oversampling { get => (int)P("oversampling", 4f); set => SetP("oversampling", value); }
    public FFTSize FftSize { get => (FFTSize)(int)P("fft_size", 3f); set => SetP("fft_size", (float)(int)value); }
}

public class AudioEffectReverb : AudioEffect
{
    internal override string NativeClass => "AudioEffectReverb";
    public float PredelayMsec { get => P("predelay_msec", 150f); set => SetP("predelay_msec", value); }
    public float PredelayFeedback { get => P("predelay_feedback", 0.4f); set => SetP("predelay_feedback", value); }
    public float RoomSize { get => P("room_size", 0.8f); set => SetP("room_size", value); }
    public float Damping { get => P("damping", 0.5f); set => SetP("damping", value); }
    public float Spread { get => P("spread", 1f); set => SetP("spread", value); }
    public float Hipass { get => P("hipass", 0f); set => SetP("hipass", value); }
    public float Dry { get => P("dry", 1f); set => SetP("dry", value); }
    public float Wet { get => P("wet", 0.5f); set => SetP("wet", value); }
}

public class AudioEffectPanner : AudioEffect
{
    internal override string NativeClass => "AudioEffectPanner";
    public float Pan { get => P("pan", 0f); set => SetP("pan", value); }
}

public class AudioEffectStereoEnhance : AudioEffect
{
    internal override string NativeClass => "AudioEffectStereoEnhance";
    public float PanPullout { get => P("pan_pullout", 1f); set => SetP("pan_pullout", value); }
    public float TimePulloutMs { get => P("time_pullout_ms", 0f); set => SetP("time_pullout_ms", value); }
    public float Surround { get => P("surround", 0f); set => SetP("surround", value); }
}

/// <summary>Keeps what plays through its bus so a script can read it (Godot's AudioEffectCapture). Frames are stereo.</summary>
public class AudioEffectCapture : AudioEffect
{
    internal override string NativeClass => "AudioEffectCapture";

    /// <summary>How many seconds the buffer holds (the engine keeps up to two).</summary>
    public float BufferLength { get => P("buffer_length", 0.1f); set => SetP("buffer_length", value); }

    private int Id => NativeIds.Count > 0 && AudioNative.Available ? NativeIds[0] : 0;

    public int GetFramesAvailable() => Id != 0 ? AudioNative.aver_audio_capture_available(Id) : 0;
    public bool CanGetBuffer(int frames) => GetFramesAvailable() >= frames;
    public int GetBufferLengthFrames() => Id != 0 ? AudioNative.aver_audio_capture_capacity(Id) : 0;
    public int GetDiscardedFrames() => Id != 0 ? AudioNative.aver_audio_capture_discarded(Id) : 0;
    public long GetPushedFrames() => Id != 0 ? _pushedBase + GetFramesAvailable() : 0;
    private long _pushedBase;

    public void ClearBuffer() { if (Id != 0) AudioNative.aver_audio_capture_clear(Id); }

    /// <summary>Takes <paramref name="frames"/> stereo frames (left, right) out of the buffer; an empty array when fewer are queued.</summary>
    public Vector2[] GetBuffer(int frames)
    {
        if (Id == 0 || frames <= 0 || !CanGetBuffer(frames)) return System.Array.Empty<Vector2>();
        float[] raw = new float[frames * 2];
        int n = AudioNative.aver_audio_capture_read(Id, raw, frames);
        _pushedBase += n;
        var result = new Vector2[n];
        for (int i = 0; i < n; i++) result[i] = new Vector2(raw[2 * i], raw[2 * i + 1]);
        return result;
    }
}

/// <summary>Measures the spectrum of what plays through its bus (Godot's AudioEffectSpectrumAnalyzer); read it through the instance
/// <see cref="AudioServer.GetBusEffectInstance"/> returns.</summary>
public class AudioEffectSpectrumAnalyzer : AudioEffect
{
    public enum FFTSize { Size256 = 0, Size512 = 1, Size1024 = 2, Size2048 = 3, Size4096 = 4, Max = 5 }

    internal override string NativeClass => "AudioEffectSpectrumAnalyzer";
    public float BufferLength { get => P("buffer_length", 2f); set => SetP("buffer_length", value); }
    public FFTSize FftSize { get => (FFTSize)(int)P("fft_size", 2f); set => SetP("fft_size", (float)(int)value); }
    public float TapBackPos { get => P("tap_back_pos", 0.01f); set => SetP("tap_back_pos", value); }
}

/// <summary>The reader of a spectrum analyser on a bus (Godot's AudioEffectSpectrumAnalyzerInstance).</summary>
public class AudioEffectSpectrumAnalyzerInstance : AudioEffectInstance
{
    public enum MagnitudeMode { Average = 0, Max = 1 }

    /// <summary>The magnitude of the left and right channels over a frequency range (linear; a full-scale sine reads about its amplitude).</summary>
    public Vector2 GetMagnitudeForFrequencyRange(float fromHz, float toHz, MagnitudeMode mode = MagnitudeMode.Max)
    {
        if (Id == 0 || !AudioNative.Available) return Vector2.Zero;
        AudioNative.aver_audio_spectrum(Id, fromHz, toHz, (int)mode, out float l, out float r);
        return new Vector2(l, r);
    }
}
