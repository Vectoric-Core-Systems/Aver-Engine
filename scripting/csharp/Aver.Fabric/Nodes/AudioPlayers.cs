// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Godot's audio nodes: AudioStreamPlayer (flat), AudioStreamPlayer3D (Godot's attenuation, panning, cone, filter and Doppler, applied by the
// mixer) and AudioListener3D. The three share AudioPlayerCore, which keeps the playbacks (max_polyphony, pause, finished) and hands them the
// volume, pitch and bus every frame. docs/plans/fabric/G5_PARITY.md, S1.

using Aver.Scene;

namespace Aver.Fabric;

/// <summary>What the players do the same way (Godot's AudioStreamPlayerInternal).</summary>
internal sealed class AudioPlayerCore
{
    private readonly List<AudioStreamPlayback> _playbacks = new();
    private readonly Func<PlayCtx> _context;
    private readonly Action _emitFinished;
    private bool _treePaused;
    private double _trimEnd = -1;
    private bool _warned;

    public AudioStream? Stream;
    public float VolumeDb;
    public float PitchScale = 1f;
    public StringName Bus = "Master";
    public int MaxPolyphony = 1;
    public bool StreamPaused;

    public AudioPlayerCore(Func<PlayCtx> context, Action emitFinished) { _context = context; _emitFinished = emitFinished; }

    public bool IsPlaying => _playbacks.Any(p => p.Active);
    public AudioStreamPlayback? Latest => _playbacks.Count > 0 ? _playbacks[^1] : null;

    /// <summary>The context a playback is started and updated with, from the player's current properties.</summary>
    public PlayCtx Base()
    {
        int bus = AudioServer.GetBusIndex(Bus);
        return new PlayCtx
        {
            Volume = VolumeDb <= -200f ? 0f : Mathf.DbToLinear(VolumeDb),
            Pitch = Math.Max(PitchScale, 0.0001f),
            Bus = bus < 0 ? 0 : bus,
            Paused = StreamPaused || _treePaused,
        };
    }

    public bool Play(double fromPosition)
    {
        if (Stream == null)
        {
            if (!_warned) { _warned = true; Aver.Scripting.Log.Warn("[Fabric] an audio player was asked to play with no stream"); }
            return false;
        }
        AudioStreamPlayback? pb = Stream.NewPlayback();
        if (pb == null) return false;
        PlayCtx c = _context();
        c.Start = (float)Math.Max(0, fromPosition);
        _trimEnd = -1;
        if (!pb.Begin(c)) return false;
        _playbacks.Add(pb);
        // max_polyphony: one more than it allows cuts the oldest (no finished signal for a cut sound).
        while (_playbacks.Count > Math.Max(1, MaxPolyphony)) { _playbacks[0].Halt(); _playbacks.RemoveAt(0); }
        return true;
    }

    public void Stop()
    {
        foreach (AudioStreamPlayback p in _playbacks) p.Halt();
        _playbacks.Clear();
        _trimEnd = -1;
    }

    public void Seek(double to)
    {
        if (Stream == null) return;
        int i = _playbacks.Count - 1;
        AudioStreamPlayback? pb = Stream.NewPlayback();
        if (pb == null) return;
        PlayCtx c = _context();
        c.Start = (float)Math.Max(0, to);
        if (i >= 0) { _playbacks[i].Halt(); _playbacks.RemoveAt(i); }
        if (pb.Begin(c)) _playbacks.Add(pb);
    }

    public double Position() => Latest?.GetPlaybackPosition() ?? 0;

    /// <summary>An animation's audio track starts a stream and wants the last <paramref name="trimEnd"/> seconds of it left out.</summary>
    public void PlayTrimmed(AudioStream s, double start, double trimEnd)
    {
        Stream = s;
        if (Play(start)) _trimEnd = trimEnd;
    }

    /// <summary>The tree was (un)paused. A paused node gets no frames, so the voices are told at once.</summary>
    public void SetTreePaused(bool paused) { _treePaused = paused; Refresh(); }

    /// <summary>Pushes the current context to every playback now (pause changes cannot wait for the next frame).</summary>
    public void Refresh()
    {
        if (_playbacks.Count == 0) return;
        PlayCtx c = _context();
        foreach (AudioStreamPlayback p in _playbacks) p.Apply(c);
    }

    /// <summary>The frame: housekeeping on every playback, the new context pushed, and finished when the last one ended.</summary>
    public void Tick(double delta)
    {
        AudioServer.FrameUpdate(delta);
        if (_playbacks.Count == 0) return;
        PlayCtx c = _context();
        foreach (AudioStreamPlayback p in _playbacks) { p.Tick(); p.Apply(c); }
        if (_trimEnd > 0 && Latest is { } last)
        {
            double len = Stream?.GetLength() ?? 0;
            if (len > 0 && last.Position() >= len - _trimEnd) Stop();
        }
        int before = _playbacks.Count;
        _playbacks.RemoveAll(p => !p.Active);
        if (before > 0 && _playbacks.Count == 0) _emitFinished();
    }
}

/// <summary>Plays an audio stream with no position (Godot's AudioStreamPlayer).</summary>
public class AudioStreamPlayer : Node, IAnimationAudioTarget
{
    public enum MixTargetEnum { Stereo = 0, Surround = 1, Center = 2 }

    public new class SignalName : Node.SignalName
    {
        public static readonly StringName Finished = "finished";
    }

    public event Action Finished { add => Connect(SignalName.Finished, Callable.From(value)); remove => Disconnect(SignalName.Finished, Callable.From(value)); }

    private readonly AudioPlayerCore _core;

    public AudioStreamPlayer()
    {
        _core = new AudioPlayerCore(() => { PlayCtx c = _core!.Base(); return c; }, () => EmitSignal(SignalName.Finished));
    }

    public AudioStream? Stream { get => _core.Stream; set { _core.Stream = value; } }
    public float VolumeDb { get => _core.VolumeDb; set => _core.VolumeDb = value; }
    public float PitchScale { get => _core.PitchScale; set => _core.PitchScale = value; }
    public StringName Bus { get => _core.Bus; set => _core.Bus = value; }
    public bool Autoplay { get; set; }
    public MixTargetEnum MixTarget { get; set; } = MixTargetEnum.Stereo;
    public int MaxPolyphony { get => _core.MaxPolyphony; set => _core.MaxPolyphony = Math.Max(1, value); }
    public AudioServer.PlaybackType PlaybackType { get; set; } = AudioServer.PlaybackType.Default;

    public bool StreamPaused { get => _core.StreamPaused; set { _core.StreamPaused = value; _core.Refresh(); } }

    /// <summary>True while a sound of this player is sounding. Setting it plays or stops.</summary>
    public bool Playing
    {
        get => _core.IsPlaying;
        set
        {
            if (value) { if (IsInsideTree()) Play(); else _playOnReady = true; }   // a scene's playing = true starts when the node is ready
            else { _playOnReady = false; Stop(); }
        }
    }
    private bool _playOnReady;

    public void Play(double fromPosition = 0.0) => _core.Play(fromPosition);
    public void Seek(double toPosition) => _core.Seek(toPosition);
    public void Stop() => _core.Stop();
    public double GetPlaybackPosition() => _core.Position();
    public bool HasStreamPlayback() => _core.Latest != null;
    public AudioStreamPlayback? GetStreamPlayback() => _core.Latest;

    void IAnimationAudioTarget.AnimationPlayStream(string streamPath, double startOffset, double endOffset)
    {
        if (ResourceLoader.Load<AudioStream>(streamPath) is { } s) _core.PlayTrimmed(s, startOffset, endOffset);
        else Aver.Scripting.Log.Warn($"[Fabric] the animation's audio track names '{streamPath}', which is not a loadable audio stream");
    }

    void IAnimationAudioTarget.AnimationStopStream() => Stop();

    internal override bool UsesInternalProcess => true;
    internal override void InternalProcess(double delta) => _core.Tick(delta);

    internal override void OnReadying()
    {
        base.OnReadying();
        if ((Autoplay || _playOnReady) && Stream != null) Play();
    }

    internal override void OnExitingTree()
    {
        _core.Stop();
        base.OnExitingTree();
    }

    public override void _Notification(int what)
    {
        base._Notification(what);
        if (what == NotificationPaused) _core.SetTreePaused(true);
        else if (what == NotificationUnpaused) _core.SetTreePaused(false);
    }
}

/// <summary>Plays an audio stream from a point in the world (Godot's AudioStreamPlayer3D). The mixer applies Godot's distance models, the
/// stereo panning law, the emission cone, the attenuation filter and the Doppler shift to it.</summary>
public class AudioStreamPlayer3D : Node3D, IAnimationAudioTarget
{
    public enum AttenuationModelEnum { InverseDistance = 0, InverseSquareDistance = 1, Logarithmic = 2, Disabled = 3 }
    public enum DopplerTrackingEnum { Disabled = 0, IdleStep = 1, PhysicsStep = 2 }

    public new class SignalName : Node3D.SignalName
    {
        public static readonly StringName Finished = "finished";
    }

    public event Action Finished { add => Connect(SignalName.Finished, Callable.From(value)); remove => Disconnect(SignalName.Finished, Callable.From(value)); }

    private readonly AudioPlayerCore _core;
    private Vector3 _lastPos;
    private Vector3 _velocity;       // Godot metres per second
    private bool _haveLast;

    public AudioStreamPlayer3D()
    {
        _core = new AudioPlayerCore(BuildContext, () => EmitSignal(SignalName.Finished));
    }

    public AudioStream? Stream { get => _core.Stream; set { _core.Stream = value; } }
    public float VolumeDb { get => _core.VolumeDb; set => _core.VolumeDb = value; }
    /// <summary>The old name of <see cref="VolumeDb"/>.</summary>
    public float UnitDb { get => _core.VolumeDb; set => _core.VolumeDb = value; }
    public float PitchScale { get => _core.PitchScale; set => _core.PitchScale = value; }
    public StringName Bus { get => _core.Bus; set => _core.Bus = value; }
    public bool Autoplay { get; set; }
    public int MaxPolyphony { get => _core.MaxPolyphony; set => _core.MaxPolyphony = Math.Max(1, value); }
    public bool StreamPaused { get => _core.StreamPaused; set { _core.StreamPaused = value; _core.Refresh(); } }
    public AudioServer.PlaybackType PlaybackType { get; set; } = AudioServer.PlaybackType.Default;

    public AttenuationModelEnum AttenuationModel { get; set; } = AttenuationModelEnum.InverseDistance;
    public float UnitSize { get; set; } = 10f;
    public float MaxDb { get; set; } = 3f;
    public float MaxDistance { get; set; }
    public float PanningStrength { get; set; } = 1f;
    public bool EmissionAngleEnabled { get; set; }
    public float EmissionAngleDegrees { get; set; } = 45f;
    public float EmissionAngleFilterAttenuationDb { get; set; } = -12f;
    public float AttenuationFilterCutoffHz { get; set; } = 5000f;
    public float AttenuationFilterDb { get; set; } = -24f;
    public DopplerTrackingEnum DopplerTracking { get; set; } = DopplerTrackingEnum.Disabled;
    /// <summary>Which areas' reverb buses apply (stored; area reverb buses are not routed).</summary>
    public uint AreaMask { get; set; } = 1;

    public bool Playing
    {
        get => _core.IsPlaying;
        set
        {
            if (value) { if (IsInsideTree()) Play(); else _playOnReady = true; }   // a scene's playing = true starts when the node is ready
            else { _playOnReady = false; Stop(); }
        }
    }
    private bool _playOnReady;

    public void Play(double fromPosition = 0.0) { TrackVelocity(0); _core.Play(fromPosition); }
    public void Seek(double toPosition) => _core.Seek(toPosition);
    public void Stop() => _core.Stop();
    public double GetPlaybackPosition() => _core.Position();
    public bool HasStreamPlayback() => _core.Latest != null;
    public AudioStreamPlayback? GetStreamPlayback() => _core.Latest;

    /// <summary>The emitter's tracked velocity, metres per second (zero when Doppler tracking is off).</summary>
    internal Vector3 TrackedVelocity => DopplerTracking == DopplerTrackingEnum.Disabled ? Vector3.Zero : _velocity;

    private PlayCtx BuildContext()
    {
        PlayCtx c = _core!.Base();
        Transform3D t = GlobalTransform;
        Vec3 pos = FabricSpace.ToAverPosition(t.Origin);
        c.X = pos.X; c.Y = pos.Y; c.Z = pos.Z;
        Vector3 forward = (-t.Basis.Z).Normalized();           // the sound is projected along -Z
        Vec3 dir = FabricSpace.ToAverDirection(forward);
        Vec3 vel = FabricSpace.ToAverPosition(TrackedVelocity);
        var s = new Aver.Fabric.SpatialPacker
        {
            Model = (int)AttenuationModel, UnitSize = UnitSize, MaxDb = MaxDb, MaxDistance = MaxDistance, PanningStrength = PanningStrength,
            GlobalPanning = AudioServer.GlobalPanningStrength, ConeEnabled = EmissionAngleEnabled, ConeAngle = EmissionAngleDegrees,
            ConeFilterDb = EmissionAngleFilterAttenuationDb, FilterCutoff = AttenuationFilterCutoffHz, FilterDb = AttenuationFilterDb,
            DirX = dir.X, DirY = dir.Y, DirZ = dir.Z, Doppler = DopplerTracking != DopplerTrackingEnum.Disabled, VelX = vel.X, VelY = vel.Y, VelZ = vel.Z,
        };
        c.Spatial = s.Pack();
        return c;
    }

    private void TrackVelocity(double delta)
    {
        Vector3 p = GlobalPosition;
        if (_haveLast && delta > 1e-6) _velocity = (p - _lastPos) / (float)delta;
        _lastPos = p;
        _haveLast = true;
    }

    internal override bool UsesInternalProcess => true;
    internal override bool UsesInternalPhysicsProcess => DopplerTracking == DopplerTrackingEnum.PhysicsStep;

    internal override void InternalProcess(double delta)
    {
        if (DopplerTracking == DopplerTrackingEnum.IdleStep) TrackVelocity(delta);
        _core.Tick(delta);
    }

    internal override void InternalPhysicsProcess(double delta)
    {
        if (DopplerTracking == DopplerTrackingEnum.PhysicsStep) TrackVelocity(delta);
    }

    void IAnimationAudioTarget.AnimationPlayStream(string streamPath, double startOffset, double endOffset)
    {
        if (ResourceLoader.Load<AudioStream>(streamPath) is { } s) _core.PlayTrimmed(s, startOffset, endOffset);
        else Aver.Scripting.Log.Warn($"[Fabric] the animation's audio track names '{streamPath}', which is not a loadable audio stream");
    }

    void IAnimationAudioTarget.AnimationStopStream() => Stop();

    internal override void OnReadying()
    {
        base.OnReadying();
        if ((Autoplay || _playOnReady) && Stream != null) Play();
    }

    internal override void OnExitingTree()
    {
        _core.Stop();
        base.OnExitingTree();
    }

    public override void _Notification(int what)
    {
        base._Notification(what);
        if (what == NotificationPaused) _core.SetTreePaused(true);
        else if (what == NotificationUnpaused) _core.SetTreePaused(false);
    }
}

/// <summary>The 18 numbers aver_audio_play_ex / aver_audio_set_voice_spatial take (aver::audio::Spatial3D::pack, which AbiEnumTest-style
/// tests in AudioTests.cs pin).</summary>
internal struct SpatialPacker
{
    public int Model;
    public float UnitSize, MaxDb, MaxDistance, PanningStrength, GlobalPanning;
    public bool ConeEnabled;
    public float ConeAngle, ConeFilterDb, FilterCutoff, FilterDb, DirX, DirY, DirZ;
    public bool Doppler;
    public float VelX, VelY, VelZ;

    public readonly float[] Pack() => new[]
    {
        (float)Model, UnitSize, MaxDb, MaxDistance, PanningStrength, GlobalPanning, ConeEnabled ? 1f : 0f, ConeAngle, ConeFilterDb, FilterCutoff, FilterDb,
        DirX, DirY, DirZ, Doppler ? 1f : 0f, VelX, VelY, VelZ,
    };
}

/// <summary>Makes the 3D players hear from this node instead of from the camera (Godot's AudioListener3D). The first one to enter the tree is current.</summary>
public class AudioListener3D : Node3D
{
    internal static AudioListener3D? Current { get; private set; }

    /// <summary>Forgets the current listener (a new scene tree).</summary>
    internal static void Reset() => Current = null;

    public void MakeCurrent() => Current = this;
    public void ClearCurrent() { if (Current == this) Current = null; }
    public bool IsCurrent() => Current == this;
    public Transform3D GetListenerTransform() => GlobalTransform.Orthonormalized();

    internal override void OnEnteringTree()
    {
        base.OnEnteringTree();
        if (Current == null || !Current.IsAlive || !Current.IsInsideTree()) Current = this;
    }

    internal override void OnExitingTree()
    {
        if (Current == this) Current = null;
        base.OnExitingTree();
    }
}
