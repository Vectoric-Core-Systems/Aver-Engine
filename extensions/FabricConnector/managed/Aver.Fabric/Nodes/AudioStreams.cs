// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Godot's audio stream resources and their playbacks: WAV, Ogg Vorbis and MP3 files, AudioStreamRandomizer, Polyphonic, Playlist,
// Synchronized, Interactive and Generator. A stream makes a playback; a playback owns the engine voices that sound it. The players
// (AudioPlayers.cs) hand a playback the volume, pitch, bus and 3D parameters every frame. docs/plans/fabric/G5_PARITY.md, S1.

namespace Aver.Fabric;

/// <summary>What a player tells a playback: Aver-side units (centimetres, engine axes) and linear volume.</summary>
internal sealed class PlayCtx
{
    public float Volume = 1f;
    public float Pitch = 1f;
    public int Bus;                   // a bus graph index
    public float[]? Spatial;          // Spatial3D.pack() or null for a flat voice
    public float X, Y, Z;             // the voice's position, engine centimetres
    public float Start;               // seconds into the sound
    public bool Paused;

    public PlayCtx Clone() => (PlayCtx)MemberwiseClone();
}

internal sealed class VoiceRef
{
    public int Id;
    public float VolMul = 1f, PitchMul = 1f;
}

/// <summary>One running instance of a stream (Godot's AudioStreamPlayback).</summary>
public class AudioStreamPlayback : RefCounted
{
    internal readonly List<VoiceRef> Voices = new();
    internal PlayCtx Ctx = new();

    internal virtual bool Begin(PlayCtx c) { Ctx = c.Clone(); return false; }

    /// <summary>Pushes what changed since the last call to the voices.</summary>
    internal virtual void Apply(PlayCtx c)
    {
        PlayCtx old = Ctx;
        Ctx = c.Clone();
        if (!AudioNative.Available) return;
        foreach (VoiceRef v in Voices)
        {
            if (c.Volume != old.Volume) AudioNative.aver_audio_set_voice_volume(v.Id, c.Volume * v.VolMul);
            if (c.Pitch != old.Pitch) AudioNative.aver_audio_set_voice_pitch(v.Id, c.Pitch * v.PitchMul);
            if (c.Bus != old.Bus) AudioNative.aver_audio_set_voice_bus(v.Id, c.Bus);
            if (c.Paused != old.Paused) AudioNative.aver_audio_set_voice_paused(v.Id, c.Paused ? 1 : 0);
            if (c.Spatial != null)
            {
                AudioNative.aver_audio_set_voice_position(v.Id, c.X, c.Y, c.Z);
                AudioNative.aver_audio_set_voice_spatial(v.Id, c.Spatial);
            }
        }
    }

    /// <summary>Per-frame housekeeping (a playlist moving on).</summary>
    internal virtual void Tick() { }

    internal virtual bool Active
    {
        get
        {
            if (!AudioNative.Available) return false;
            foreach (VoiceRef v in Voices) if (AudioNative.aver_audio_playing(v.Id) != 0) return true;
            return false;
        }
    }

    internal virtual void Halt()
    {
        if (!AudioNative.Available) return;
        foreach (VoiceRef v in Voices) AudioNative.aver_audio_stop(v.Id);
        Voices.Clear();
    }

    internal virtual double Position()
    {
        if (!AudioNative.Available || Voices.Count == 0) return 0;
        double p = AudioNative.aver_audio_voice_position(Voices[0].Id);
        return p < 0 ? 0 : p;
    }

    public bool IsPlaying() => Active;

    /// <summary>Seconds played of the sound (a looping sound wraps).</summary>
    public virtual double GetPlaybackPosition() => Position();

    /// <summary>Starts a file-backed voice with this playback's context; false when the engine refused.</summary>
    internal bool StartFile(AudioStreamFile s, PlayCtx c, float volMul, float pitchMul)
    {
        if (!AudioServer.EnsureReady()) return false;
        int id = s.PlayVoice(c, volMul, pitchMul);
        if (id == 0) return false;
        Voices.Add(new VoiceRef { Id = id, VolMul = volMul, PitchMul = pitchMul });
        return true;
    }
}

/// <summary>A playback made of other playbacks (the randomizer's pick, a playlist's current clip, a synchronized set).</summary>
public class AudioStreamPlaybackMulti : AudioStreamPlayback
{
    internal readonly List<(AudioStreamPlayback Pb, float Vol, float Pitch)> Kids = new();

    internal PlayCtx Scaled(PlayCtx c, float vol, float pitch)
    {
        PlayCtx k = c.Clone();
        k.Volume = c.Volume * vol;
        k.Pitch = c.Pitch * pitch;
        return k;
    }

    internal override void Apply(PlayCtx c)
    {
        Ctx = c.Clone();
        foreach (var k in Kids) k.Pb.Apply(Scaled(c, k.Vol, k.Pitch));
    }

    internal override void Tick() { foreach (var k in Kids) k.Pb.Tick(); }
    internal override bool Active => Kids.Any(k => k.Pb.Active);
    internal override void Halt() { foreach (var k in Kids) k.Pb.Halt(); Kids.Clear(); }
    internal override double Position() => Kids.Count > 0 ? Kids[0].Pb.Position() : 0;

    internal bool StartKid(AudioStream? s, PlayCtx c, float vol, float pitch)
    {
        AudioStreamPlayback? pb = s?.NewPlayback();
        if (pb == null) return false;
        if (!pb.Begin(Scaled(c, vol, pitch))) return false;
        Kids.Add((pb, vol, pitch));
        return true;
    }
}

/// <summary>The base of every audio stream (Godot's AudioStream).</summary>
public class AudioStream : Resource
{
    public virtual double GetLength() => 0;
    public virtual bool IsMonophonic() => false;
    public virtual double GetBpm() => 0;
    public virtual int GetBeatCount() => 0;
    public virtual bool CanBeSampled() => false;

    /// <summary>A playback of this stream, not yet started (an AudioStreamPlayer starts it).</summary>
    public AudioStreamPlayback? InstantiatePlayback() => NewPlayback();

    internal virtual AudioStreamPlayback? NewPlayback() => null;

    // Dynamic values a scene stores for the composite streams ("stream_0", "clip_1/name") resolve a Resource or a path.
    protected static AudioStream? AsStream(Variant v) => v.AsGodotObject() as AudioStream ?? (v.AsString() is { Length: > 0 } p ? ResourceLoader.Load<AudioStream>(p) : null);
}

// ---------------------------------------------------------------- files

/// <summary>An audio file decoded by the engine (WAV by its own reader; Ogg Vorbis, MP3 and the rest by the platform's media stack).</summary>
public class AudioStreamFile : AudioStream
{
    internal int Handle;
    internal int Generation = -1;
    internal virtual bool Looping => false;

    /// <summary>The file on disk this stream plays: <see cref="SourcePath"/> when set, else the resource's own path.</summary>
    internal string? DiskPath()
    {
        string p = SourcePath.Length > 0 ? SourcePath : ResourcePath;
        return p.Length == 0 ? null : (File.Exists(p) ? p : ResourceLoader.FindFile(p));
    }

    /// <summary>An Aver extension: the file this stream reads when it is not the resource's own path (the converter writes it for a WAV with
    /// import-time loop settings).</summary>
    public string SourcePath { get; set; } = "";

    /// <summary>Hands the sound to the engine (once per audio session) and returns its handle, 0 when it cannot be decoded.</summary>
    internal virtual int EnsureLoaded()
    {
        if (!AudioServer.EnsureReady()) return 0;
        if (Handle != 0 && Generation == AudioServer.Generation) return Handle;
        string? path = DiskPath();
        if (path == null) { Aver.Scripting.Log.Warn($"[Fabric] the audio file for '{ResourcePath}' does not exist"); return 0; }
        Handle = AudioNative.aver_audio_load(path);
        Generation = AudioServer.Generation;
        if (Handle == 0) Aver.Scripting.Log.Warn($"[Fabric] the engine could not decode '{path}' (it reads WAV, Ogg Vorbis, FLAC and MP3 itself; is the file damaged? see docs/AUDIO.md)");
        else OnLoaded();
        return Handle;
    }

    internal virtual void OnLoaded() { }

    /// <summary>Whether a file this large is read from disk as it plays rather than decoded whole.</summary>
    internal virtual bool StreamFromDisk()
    {
        string? p = DiskPath();
        try { return p != null && new FileInfo(p).Length > 6 * 1024 * 1024; }
        catch (IOException) { return false; }
    }

    internal virtual int PlayVoice(PlayCtx c, float volMul, float pitchMul)
    {
        if (StreamFromDisk() && !NeedsResident)
        {
            string? p = DiskPath();
            return p == null ? 0 : AudioNative.aver_audio_stream_play_ex(p, c.Volume * volMul, c.Pitch * pitchMul, Looping ? 1 : 0, 0, c.Bus, c.Start, c.Paused ? 1 : 0, c.X, c.Y, c.Z, c.Spatial);
        }
        int h = EnsureLoaded();
        if (h == 0) return 0;
        return AudioNative.aver_audio_play_ex(h, c.Volume * volMul, c.Pitch * pitchMul, Looping ? 1 : 0, 0, c.Bus, c.Start, c.Paused ? 1 : 0, c.X, c.Y, c.Z, c.Spatial);
    }

    /// <summary>True when something (custom loop points) only works on a sound held in memory.</summary>
    internal virtual bool NeedsResident => false;

    public override double GetLength()
    {
        int h = EnsureLoaded();
        return h == 0 ? 0 : AudioNative.aver_audio_sound_length(h);
    }

    internal override AudioStreamPlayback? NewPlayback() => new FilePlayback(this);
}

internal sealed class FilePlayback : AudioStreamPlayback
{
    private readonly AudioStreamFile _s;
    public FilePlayback(AudioStreamFile s) { _s = s; }

    internal override bool Begin(PlayCtx c)
    {
        Ctx = c.Clone();
        return StartFile(_s, c, 1f, 1f);
    }
}

/// <summary>A WAV file or WAV data (Godot's AudioStreamWAV).</summary>
public class AudioStreamWav : AudioStreamFile
{
    public enum FormatEnum { Format8Bits = 0, Format16Bits = 1, FormatImaAdpcm = 2, FormatQoa = 3 }
    public enum LoopModeEnum { Disabled = 0, Forward = 1, PingPong = 2, Backward = 3 }

    private byte[] _data = System.Array.Empty<byte>();
    private LoopModeEnum _loopMode;
    private int _loopBegin, _loopEnd;
    private bool _warned;

    /// <summary>Raw samples in <see cref="Format"/>, interleaved when <see cref="Stereo"/>. Empty for a stream that plays a file.</summary>
    public byte[] Data { get => _data; set { _data = value ?? System.Array.Empty<byte>(); Generation = -1; } }
    public FormatEnum Format { get; set; } = FormatEnum.Format8Bits;
    public int MixRate { get; set; } = 44100;
    public bool Stereo { get; set; }
    public LoopModeEnum LoopMode { get => _loopMode; set { _loopMode = value; Generation = -1; } }
    public int LoopBegin { get => _loopBegin; set { _loopBegin = value; Generation = -1; } }
    public int LoopEnd { get => _loopEnd; set { _loopEnd = value; Generation = -1; } }

    internal override bool Looping => _loopMode != LoopModeEnum.Disabled;
    internal override bool NeedsResident => _loopBegin != 0 || _loopEnd != 0 || _data.Length > 0;
    public override bool IsMonophonic() => !Stereo;
    public override bool CanBeSampled() => true;

    public override double GetLength()
    {
        if (_data.Length > 0)
        {
            int bytes = Format == FormatEnum.Format16Bits ? 2 : 1;
            return (double)_data.Length / bytes / (Stereo ? 2 : 1) / Math.Max(1, MixRate);
        }
        return base.GetLength();
    }

    internal override int EnsureLoaded()
    {
        if (_data.Length == 0) return LoadFile();
        if (!AudioServer.EnsureReady()) return 0;
        if (Handle != 0 && Generation == AudioServer.Generation) return Handle;
        if (Format is FormatEnum.FormatImaAdpcm or FormatEnum.FormatQoa)
        {
            Aver.Scripting.Log.Warn($"[Fabric] AudioStreamWAV data in {Format} is not decoded; only 8 and 16 bit PCM play");
            return 0;
        }
        int ch = Stereo ? 2 : 1;
        int frames = _data.Length / (Format == FormatEnum.Format16Bits ? 2 : 1) / ch;
        float[] pcm = new float[frames * ch];
        if (Format == FormatEnum.Format16Bits)
            for (int i = 0; i < pcm.Length; i++) pcm[i] = (short)(_data[2 * i] | (_data[2 * i + 1] << 8)) / 32768f;
        else
            for (int i = 0; i < pcm.Length; i++) pcm[i] = (sbyte)_data[i] / 128f;
        Handle = AudioNative.aver_audio_load_pcm(pcm, frames, ch, MixRate);
        Generation = AudioServer.Generation;
        if (Handle != 0) OnLoaded();
        return Handle;
    }

    private int LoadFile() => base.EnsureLoaded();

    internal override void OnLoaded()
    {
        if (_loopBegin != 0 || _loopEnd != 0) AudioNative.aver_audio_sound_set_loop(Handle, _loopBegin, _loopEnd);
        if (_loopMode is LoopModeEnum.PingPong or LoopModeEnum.Backward && !_warned)
        {
            _warned = true;
            Aver.Scripting.Log.Warn($"[Fabric] AudioStreamWAV loop mode {_loopMode} plays forward; the mixer loops in one direction");
        }
    }

}

/// <summary>An Ogg Vorbis file (Godot's AudioStreamOggVorbis), decoded by the engine on every platform (docs/AUDIO.md).</summary>
public class AudioStreamOggVorbis : AudioStreamFile
{
    private bool _loop;
    public bool Loop { get => _loop; set => _loop = value; }
    public double LoopOffset { get; set; }
    public double Bpm { get; set; }
    public int BeatCount { get; set; }
    public int BarBeats { get; set; } = 4;
    internal override bool Looping => _loop;
    public override double GetBpm() => Bpm;
    public override int GetBeatCount() => BeatCount;
}

/// <summary>An MP3 file (Godot's AudioStreamMP3), decoded by the engine on every platform (docs/AUDIO.md).</summary>
public class AudioStreamMP3 : AudioStreamFile
{
    private bool _loop;
    public bool Loop { get => _loop; set => _loop = value; }
    public double LoopOffset { get; set; }
    public double Bpm { get; set; }
    public int BeatCount { get; set; }
    public int BarBeats { get; set; } = 4;
    internal override bool Looping => _loop;
    public override double GetBpm() => Bpm;
    public override int GetBeatCount() => BeatCount;
}

// ---------------------------------------------------------------- the randomizer

public class AudioStreamRandomizer : AudioStream
{
    public enum PlaybackModeEnum { RandomNoRepeats = 0, Random = 1, Sequential = 2 }

    private readonly List<(AudioStream? Stream, float Weight)> _streams = new();
    private int _last = -1, _next;

    /// <summary>The generator used for every pick (tests install a fixed one).</summary>
    internal static Func<float> Rng = () => GD.Randf();

    public PlaybackModeEnum PlaybackMode { get; set; } = PlaybackModeEnum.RandomNoRepeats;
    public float RandomPitch { get; set; } = 1f;
    public float RandomVolumeOffsetDb { get; set; }

    internal override bool TryDynamicSet(string name, Variant value)
    {
        if (name == "streams_count") { StreamsCount = value.AsInt32(); return true; }
        if (name.StartsWith("stream_", StringComparison.Ordinal) && name.IndexOf('/') > 0)
        {
            int slash = name.IndexOf('/');
            if (!int.TryParse(name.AsSpan(7, slash - 7), out int i) || i < 0 || i > 4096) return false;
            while (_streams.Count <= i) _streams.Add((null, 1f));
            string what = name[(slash + 1)..];
            if (what == "stream") { SetStream(i, AsStream(value)); return true; }
            if (what == "weight") { SetStreamProbabilityWeight(i, value.AsSingle()); return true; }
        }
        return false;
    }

    public int StreamsCount
    {
        get => _streams.Count;
        set { while (_streams.Count < value) _streams.Add((null, 1f)); while (_streams.Count > Math.Max(value, 0)) _streams.RemoveAt(_streams.Count - 1); }
    }

    public void AddStream(int index, AudioStream? stream, float weight = 1f)
    {
        if (index < 0 || index > _streams.Count) index = _streams.Count;
        _streams.Insert(index, (stream, weight));
    }

    public void MoveStream(int indexFrom, int indexTo)
    {
        if (indexFrom < 0 || indexFrom >= _streams.Count || indexTo < 0 || indexTo >= _streams.Count) return;
        var e = _streams[indexFrom];
        _streams.RemoveAt(indexFrom);
        _streams.Insert(indexTo, e);
    }

    public void RemoveStream(int index) { if (index >= 0 && index < _streams.Count) _streams.RemoveAt(index); }
    public void SetStream(int index, AudioStream? stream) { if (index >= 0 && index < _streams.Count) _streams[index] = (stream, _streams[index].Weight); }
    public AudioStream? GetStream(int index) => index >= 0 && index < _streams.Count ? _streams[index].Stream : null;
    public void SetStreamProbabilityWeight(int index, float weight) { if (index >= 0 && index < _streams.Count) _streams[index] = (_streams[index].Stream, weight); }
    public float GetStreamProbabilityWeight(int index) => index >= 0 && index < _streams.Count ? _streams[index].Weight : 0f;

    public override double GetLength() => _streams.Count > 0 && _streams[0].Stream != null ? _streams[0].Stream!.GetLength() : 0;

    /// <summary>Chooses the next stream by the mode: random by weight, random but never the one just played, or in order.</summary>
    internal int Pick()
    {
        int n = _streams.Count;
        if (n == 0) return -1;
        int pick;
        if (PlaybackMode == PlaybackModeEnum.Sequential) { pick = _next % n; _next = (pick + 1) % n; }
        else
        {
            bool avoid = PlaybackMode == PlaybackModeEnum.RandomNoRepeats && n > 1 && _last >= 0;
            float total = 0;
            for (int i = 0; i < n; i++) if (!(avoid && i == _last)) total += Math.Max(0f, _streams[i].Weight);
            if (total <= 0)
            {
                pick = avoid ? (_last + 1) % n : (int)(Rng() * n) % n;
            }
            else
            {
                float r = Rng() * total;
                pick = -1;
                for (int i = 0; i < n && pick < 0; i++)
                {
                    if (avoid && i == _last) continue;
                    float w = Math.Max(0f, _streams[i].Weight);
                    if (r < w) pick = i;
                    r -= w;
                }
                if (pick < 0) for (int i = n - 1; i >= 0; i--) if (!(avoid && i == _last) && _streams[i].Weight > 0) { pick = i; break; }
            }
        }
        _last = pick;
        return pick;
    }

    internal override AudioStreamPlayback? NewPlayback() => new RandomizerPlayback(this);

    private sealed class RandomizerPlayback : AudioStreamPlaybackMulti
    {
        private readonly AudioStreamRandomizer _r;
        public RandomizerPlayback(AudioStreamRandomizer r) { _r = r; }

        internal override bool Begin(PlayCtx c)
        {
            Ctx = c.Clone();
            int i = _r.Pick();
            if (i < 0) return false;
            // pitch in [1/random_pitch, random_pitch]; volume offset in [-x, +x] dB
            float rp = Math.Max(_r.RandomPitch, 0.0001f);
            float pitch = rp == 1f ? 1f : 1f / rp + Rng() * (rp - 1f / rp);
            float vol = _r.RandomVolumeOffsetDb == 0f ? 1f : Mathf.DbToLinear((Rng() * 2f - 1f) * _r.RandomVolumeOffsetDb);
            return StartKid(_r._streams[i].Stream, c, vol, pitch);
        }
    }
}

// ---------------------------------------------------------------- polyphonic, synchronized, playlist, interactive

public class AudioStreamPolyphonic : AudioStream
{
    public int Polyphony { get; set; } = 32;
    public override bool IsMonophonic() => false;
    internal override AudioStreamPlayback? NewPlayback() => new AudioStreamPlaybackPolyphonic(this);
}

/// <summary>Plays several streams at once on one player (Godot's AudioStreamPlaybackPolyphonic). Get it from the player after Play().</summary>
public class AudioStreamPlaybackPolyphonic : AudioStreamPlaybackMulti
{
    public const long InvalidId = -1;

    private readonly AudioStreamPolyphonic _owner;
    private readonly Dictionary<long, int> _ids = new();   // id -> index in Kids
    private readonly List<long> _order = new();
    private long _nextId = 1;
    private bool _active;

    internal AudioStreamPlaybackPolyphonic(AudioStreamPolyphonic owner) { _owner = owner; }

    internal override bool Begin(PlayCtx c) { Ctx = c.Clone(); _active = true; return true; }
    internal override bool Active => _active;
    internal override void Halt() { base.Halt(); _ids.Clear(); _order.Clear(); _active = false; }

    internal override void Tick()
    {
        base.Tick();
        // Playbacks that ended free their slot (and their id).
        for (int i = Kids.Count - 1; i >= 0; i--)
        {
            if (Kids[i].Pb.Active) continue;
            long id = _ids.First(kv => kv.Value == i).Key;
            RemoveKid(i, id);
        }
    }

    private void RemoveKid(int index, long id)
    {
        Kids.RemoveAt(index);
        _ids.Remove(id);
        _order.Remove(id);
        foreach (long k in _ids.Keys.ToList()) if (_ids[k] > index) _ids[k]--;
    }

    /// <summary>Starts a stream on this playback; the id names it for the calls below. -1 when the stream is null or cannot start.</summary>
    public long PlayStream(AudioStream? stream, float fromOffset = 0f, float volumeDb = 0f, float pitchScale = 1f)
    {
        if (stream == null || !_active) return InvalidId;
        // Polyphony is a limit: starting one past it cuts the oldest.
        while (Kids.Count >= Math.Max(1, _owner.Polyphony) && _order.Count > 0) StopStream(_order[0]);
        PlayCtx c = Ctx.Clone();
        c.Start = fromOffset;
        int before = Kids.Count;
        if (!StartKid(stream, c, Mathf.DbToLinear(volumeDb), pitchScale)) return InvalidId;
        long id = _nextId++;
        _ids[id] = before;
        _order.Add(id);
        return id;
    }

    public void SetStreamPitchScale(long stream, float pitchScale)
    {
        if (!_ids.TryGetValue(stream, out int i)) return;
        var k = Kids[i];
        Kids[i] = (k.Pb, k.Vol, pitchScale);
        k.Pb.Apply(Scaled(Ctx, k.Vol, pitchScale));
    }

    public void SetStreamVolumeDb(long stream, float volumeDb)
    {
        if (!_ids.TryGetValue(stream, out int i)) return;
        var k = Kids[i];
        float vol = Mathf.DbToLinear(volumeDb);
        Kids[i] = (k.Pb, vol, k.Pitch);
        k.Pb.Apply(Scaled(Ctx, vol, k.Pitch));
    }

    public bool IsStreamPlaying(long stream) => _ids.TryGetValue(stream, out int i) && Kids[i].Pb.Active;

    public void StopStream(long stream)
    {
        if (!_ids.TryGetValue(stream, out int i)) return;
        Kids[i].Pb.Halt();
        RemoveKid(i, stream);
    }
}

public class AudioStreamSynchronized : AudioStream
{
    public const int MaxStreams = 32;
    private readonly List<(AudioStream? Stream, float VolumeDb)> _streams = new();

    public int StreamCount
    {
        get => _streams.Count;
        set { while (_streams.Count < Math.Clamp(value, 0, MaxStreams)) _streams.Add((null, 0f)); while (_streams.Count > Math.Max(value, 0)) _streams.RemoveAt(_streams.Count - 1); }
    }

    public void SetSyncStream(int streamIdx, AudioStream? stream) { if (streamIdx >= 0 && streamIdx < _streams.Count) _streams[streamIdx] = (stream, _streams[streamIdx].VolumeDb); }
    public AudioStream? GetSyncStream(int streamIdx) => streamIdx >= 0 && streamIdx < _streams.Count ? _streams[streamIdx].Stream : null;
    public void SetSyncStreamVolume(int streamIdx, float volumeDb) { if (streamIdx >= 0 && streamIdx < _streams.Count) _streams[streamIdx] = (_streams[streamIdx].Stream, volumeDb); }
    public float GetSyncStreamVolume(int streamIdx) => streamIdx >= 0 && streamIdx < _streams.Count ? _streams[streamIdx].VolumeDb : 0f;
    public override double GetLength() => _streams.Select(s => s.Stream?.GetLength() ?? 0).DefaultIfEmpty(0).Max();

    internal override bool TryDynamicSet(string name, Variant value)
    {
        if (!name.StartsWith("stream_", StringComparison.Ordinal)) return false;
        string rest = name[7..];
        int slash = rest.IndexOf('/');
        if (!int.TryParse(slash < 0 ? rest : rest[..slash], out int i) || i < 0 || i >= MaxStreams) return false;
        if (i >= _streams.Count) StreamCount = i + 1;
        if (slash < 0) { SetSyncStream(i, AsStream(value)); return true; }
        if (rest[(slash + 1)..] == "volume") { SetSyncStreamVolume(i, value.AsSingle()); return true; }
        return false;
    }

    internal override AudioStreamPlayback? NewPlayback() => new SyncPlayback(this);

    private sealed class SyncPlayback : AudioStreamPlaybackMulti
    {
        private readonly AudioStreamSynchronized _s;
        public SyncPlayback(AudioStreamSynchronized s) { _s = s; }

        internal override bool Begin(PlayCtx c)
        {
            Ctx = c.Clone();
            bool any = false;
            foreach (var e in _s._streams) any |= StartKid(e.Stream, c, Mathf.DbToLinear(e.VolumeDb), 1f);
            return any;
        }
    }
}

public class AudioStreamPlaylist : AudioStream
{
    public const int MaxStreams = 64;
    private readonly List<AudioStream?> _streams = new();

    public bool Shuffle { get; set; }
    public bool Loop { get; set; } = true;
    /// <summary>Godot cross-fades this long between entries; the engine plays them back to back (approximated).</summary>
    public float FadeTime { get; set; } = 0.3f;
    public double Bpm { get; set; }
    public int BeatCount { get; set; }

    public int StreamCount
    {
        get => _streams.Count;
        set { while (_streams.Count < Math.Clamp(value, 0, MaxStreams)) _streams.Add(null); while (_streams.Count > Math.Max(value, 0)) _streams.RemoveAt(_streams.Count - 1); }
    }

    public void SetListStream(int streamIdx, AudioStream? stream) { if (streamIdx >= 0 && streamIdx < _streams.Count) _streams[streamIdx] = stream; }
    public AudioStream? GetListStream(int streamIdx) => streamIdx >= 0 && streamIdx < _streams.Count ? _streams[streamIdx] : null;
    public override double GetLength() => _streams.Sum(s => s?.GetLength() ?? 0);

    internal override bool TryDynamicSet(string name, Variant value)
    {
        if (!name.StartsWith("stream_", StringComparison.Ordinal) || !int.TryParse(name.AsSpan(7), out int i) || i < 0 || i >= MaxStreams) return false;
        if (i >= _streams.Count) StreamCount = i + 1;
        SetListStream(i, AsStream(value));
        return true;
    }

    internal override AudioStreamPlayback? NewPlayback() => new PlaylistPlayback(this);

    private sealed class PlaylistPlayback : AudioStreamPlaybackMulti
    {
        private readonly AudioStreamPlaylist _p;
        private List<int> _order = new();
        private int _at;
        private bool _done;

        public PlaylistPlayback(AudioStreamPlaylist p) { _p = p; }

        internal override bool Begin(PlayCtx c)
        {
            Ctx = c.Clone();
            _order = Enumerable.Range(0, _p._streams.Count).Where(i => _p._streams[i] != null).ToList();
            if (_order.Count == 0) return false;
            if (_p.Shuffle) for (int i = _order.Count - 1; i > 0; i--) { int j = (int)(AudioStreamRandomizer.Rng() * (i + 1)) % (i + 1); (_order[i], _order[j]) = (_order[j], _order[i]); }
            _at = 0;
            return StartKid(_p._streams[_order[0]], c, 1f, 1f);
        }

        internal override bool Active => !_done && (Kids.Count > 0 || _order.Count > 0);

        internal override void Tick()
        {
            if (_done || Kids.Count == 0 || Kids[0].Pb.Active) return;
            Kids.Clear();
            _at++;
            if (_at >= _order.Count)
            {
                if (!_p.Loop) { _done = true; return; }
                _at = 0;
            }
            PlayCtx c = Ctx.Clone();
            c.Start = 0;
            if (!StartKid(_p._streams[_order[_at]], c, 1f, 1f)) _done = true;
        }
    }
}

public class AudioStreamInteractive : AudioStream
{
    public enum TransitionFromTime { Immediate = 0, NextBeat = 1, NextBar = 2, End = 3 }
    public enum TransitionToTime { SamePosition = 0, Start = 1 }
    public enum FadeMode { Disabled = 0, FadeIn = 1, FadeOut = 2, Cross = 3, Automatic = 4 }
    public enum AutoAdvanceMode { Disabled = 0, Enabled = 1, ReturnToHold = 2 }
    public const int ClipAny = -1;

    internal sealed class Clip
    {
        public string Name = "";
        public AudioStream? Stream;
        public AutoAdvanceMode AutoAdvance;
        public int NextClip;
    }

    internal readonly List<Clip> Clips = new();
    internal readonly List<(int From, int To, TransitionFromTime FromTime, TransitionToTime ToTime, FadeMode Fade, float Beats, bool UseFiller, int FillerClip, bool HoldPrevious)> Transitions = new();

    public int InitialClip { get; set; }

    public int ClipCount
    {
        get => Clips.Count;
        set { while (Clips.Count < Math.Max(value, 0)) Clips.Add(new Clip { Name = $"Clip {Clips.Count}" }); while (Clips.Count > Math.Max(value, 0)) Clips.RemoveAt(Clips.Count - 1); }
    }

    private Clip? C(int i) => i >= 0 && i < Clips.Count ? Clips[i] : null;
    public void SetClipName(int clipIndex, StringName name) { if (C(clipIndex) is { } c) c.Name = name.ToString(); }
    public StringName GetClipName(int clipIndex) => C(clipIndex)?.Name ?? "";
    public void SetClipStream(int clipIndex, AudioStream? stream) { if (C(clipIndex) is { } c) c.Stream = stream; }
    public AudioStream? GetClipStream(int clipIndex) => C(clipIndex)?.Stream;
    public void SetClipAutoAdvance(int clipIndex, AutoAdvanceMode mode) { if (C(clipIndex) is { } c) c.AutoAdvance = mode; }
    public AutoAdvanceMode GetClipAutoAdvance(int clipIndex) => C(clipIndex)?.AutoAdvance ?? AutoAdvanceMode.Disabled;
    public void SetClipAutoAdvanceNextClip(int clipIndex, int autoAdvanceNextClip) { if (C(clipIndex) is { } c) c.NextClip = autoAdvanceNextClip; }
    public int GetClipAutoAdvanceNextClip(int clipIndex) => C(clipIndex)?.NextClip ?? 0;

    /// <summary>Records a transition rule. The engine switches clips at once on a request (no beat grid), with the clip's own start; the rule is kept.</summary>
    public void AddTransition(int fromClip, int toClip, TransitionFromTime fromTime, TransitionToTime toTime, FadeMode fadeMode, float fadeBeats, bool useFillerClip = false, int fillerClip = -1, bool holdPrevious = false)
    {
        Transitions.RemoveAll(t => t.From == fromClip && t.To == toClip);
        Transitions.Add((fromClip, toClip, fromTime, toTime, fadeMode, fadeBeats, useFillerClip, fillerClip, holdPrevious));
    }
    public bool HasTransition(int fromClip, int toClip) => Transitions.Any(t => t.From == fromClip && t.To == toClip);
    public void EraseTransition(int fromClip, int toClip) => Transitions.RemoveAll(t => t.From == fromClip && t.To == toClip);

    internal override bool TryDynamicSet(string name, Variant value)
    {
        if (name == "clip_count") { ClipCount = value.AsInt32(); return true; }
        if (!name.StartsWith("clip_", StringComparison.Ordinal)) return false;
        int slash = name.IndexOf('/');
        if (slash < 0 || !int.TryParse(name.AsSpan(5, slash - 5), out int i) || i < 0 || i > 255) return false;
        if (i >= Clips.Count) ClipCount = i + 1;
        switch (name[(slash + 1)..])
        {
            case "name": SetClipName(i, value.AsStringName()); return true;
            case "stream": SetClipStream(i, AsStream(value)); return true;
            case "auto_advance": SetClipAutoAdvance(i, (AutoAdvanceMode)value.AsInt32()); return true;
            case "next_clip": SetClipAutoAdvanceNextClip(i, value.AsInt32()); return true;
        }
        return false;
    }

    internal override AudioStreamPlayback? NewPlayback() => new AudioStreamPlaybackInteractive(this);
}

public class AudioStreamPlaybackInteractive : AudioStreamPlaybackMulti
{
    private readonly AudioStreamInteractive _s;
    private int _current = -1;
    private bool _alive;

    internal AudioStreamPlaybackInteractive(AudioStreamInteractive s) { _s = s; }

    internal override bool Begin(PlayCtx c)
    {
        Ctx = c.Clone();
        _alive = true;
        return Enter(Math.Clamp(_s.InitialClip, 0, Math.Max(_s.Clips.Count - 1, 0)), c);
    }

    private bool Enter(int clip, PlayCtx c)
    {
        foreach (var k in Kids) k.Pb.Halt();
        Kids.Clear();
        _current = clip;
        if (clip < 0 || clip >= _s.Clips.Count) return false;
        PlayCtx start = c.Clone();
        start.Start = 0;
        return StartKid(_s.Clips[clip].Stream, start, 1f, 1f);
    }

    internal override bool Active => _alive && Kids.Count > 0;

    /// <summary>Switches to a clip now (the transition rules are stored, not timed).</summary>
    public void SwitchToClip(int clipIndex) { if (_alive) Enter(clipIndex, Ctx); }
    public void SwitchToClipByName(StringName clipName)
    {
        for (int i = 0; i < _s.Clips.Count; i++) if (_s.Clips[i].Name == clipName.ToString()) { SwitchToClip(i); return; }
    }
    public int GetCurrentClipIndex() => _current;

    internal override void Tick()
    {
        base.Tick();
        if (!_alive || Kids.Count == 0 || Kids[0].Pb.Active) return;
        AudioStreamInteractive.Clip clip = _s.Clips[_current];
        if (clip.AutoAdvance == AudioStreamInteractive.AutoAdvanceMode.Disabled) { Kids.Clear(); return; }
        Enter(clip.NextClip, Ctx);
    }
}

// ---------------------------------------------------------------- the generator

public class AudioStreamGenerator : AudioStream
{
    public enum Mode { Output = 0, Input = 1, Custom = 2 }

    public float MixRate { get; set; } = 44100f;
    public Mode MixRateMode { get; set; } = Mode.Custom;
    public float BufferLength { get; set; } = 0.5f;

    internal override AudioStreamPlayback? NewPlayback() => new AudioStreamGeneratorPlayback(this);
}

/// <summary>Frames a script pushes to the player (Godot's AudioStreamGeneratorPlayback).</summary>
public class AudioStreamGeneratorPlayback : AudioStreamPlayback
{
    private readonly AudioStreamGenerator _g;
    private int _voice;

    internal AudioStreamGeneratorPlayback(AudioStreamGenerator g) { _g = g; }

    internal override bool Begin(PlayCtx c)
    {
        Ctx = c.Clone();
        if (!AudioServer.EnsureReady()) return false;
        int rate = _g.MixRateMode == AudioStreamGenerator.Mode.Output ? AudioServer.GetMixRate() : Math.Max(1, (int)_g.MixRate);
        int ring = Math.Max(256, (int)(_g.BufferLength * rate));
        _voice = AudioNative.aver_audio_live_play(c.Volume, c.Pitch, 0, c.Bus, c.X, c.Y, c.Z, c.Spatial, rate, 2, ring);
        if (_voice == 0) return false;
        Voices.Add(new VoiceRef { Id = _voice });
        return true;
    }

    private bool Live => _voice != 0 && AudioNative.Available && AudioNative.aver_audio_playing(_voice) != 0;

    /// <summary>Frames the buffer can still take.</summary>
    public int GetFramesAvailable() => Live ? AudioNative.aver_audio_live_free(_voice) : 0;
    public bool CanPushBuffer(int amount) => GetFramesAvailable() >= amount;
    public int GetSkips() => _voice != 0 && AudioNative.Available ? AudioNative.aver_audio_voice_underruns(_voice) : 0;
    public void ClearBuffer() { if (Live) AudioNative.aver_audio_live_flush(_voice); }

    public bool PushFrame(Vector2 frame) => PushBuffer(new[] { frame });

    /// <summary>Queues frames (left, right); false, and nothing queued, when they would not all fit.</summary>
    public bool PushBuffer(Vector2[] frames)
    {
        if (!Live || frames.Length == 0 || !CanPushBuffer(frames.Length)) return false;
        float[] raw = new float[frames.Length * 2];
        for (int i = 0; i < frames.Length; i++) { raw[2 * i] = frames[i].X; raw[2 * i + 1] = frames[i].Y; }
        return AudioNative.aver_audio_live_push(_voice, raw, frames.Length) == frames.Length;
    }

    internal override bool Active => Live;
    public override double GetPlaybackPosition() => 0;
}
