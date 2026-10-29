// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// AnimationMixer and AnimationPlayer (Godot's). The playback itself (play, queue, crossfade, seek, loop modes, track blending, the order of method
// calls and signals) is the engine's PropPlayer (modules/anim, through Aver.Anim.Abi); this class feeds it the libraries, steps it, and carries out
// what it reports in the order it reports it: writing blended values to node properties, calling methods, starting audio and sub-players, and
// emitting Godot's signals. docs/plans/fabric/G5_PARITY.md, A1.

using Aver.Scene;

namespace Aver.Fabric;

/// <summary>A node an animation's audio track can start and stop (the contract with the audio package). An AudioStreamPlayer or
/// AudioStreamPlayer3D implements it; a class that does not is driven through its <c>stream</c> property and <c>play</c> / <c>stop</c> methods.</summary>
public interface IAnimationAudioTarget
{
    /// <summary>Starts <paramref name="streamPath"/> at <paramref name="startOffset"/> seconds into it; <paramref name="endOffset"/> seconds are trimmed from its end.</summary>
    void AnimationPlayStream(string streamPath, double startOffset, double endOffset);

    /// <summary>Stops what the animation started.</summary>
    void AnimationStopStream();
}

/// <summary>The base of AnimationPlayer: libraries of animations and the engine player that mixes them (Godot's AnimationMixer).</summary>
public class AnimationMixer : Node
{
    public enum AnimationCallbackModeProcess { Physics = 0, Idle = 1, Manual = 2 }
    public enum AnimationCallbackModeMethod { Deferred = 0, Immediate = 1 }
    public enum AnimationCallbackModeDiscrete { Dominant = 0, Recessive = 1, ForceContinuous = 2 }

    public new class SignalName : Node.SignalName
    {
        public static readonly StringName AnimationFinished = "animation_finished";
        public static readonly StringName AnimationStarted = "animation_started";
        public static readonly StringName AnimationListChanged = "animation_list_changed";
        public static readonly StringName AnimationLibrariesUpdated = "animation_libraries_updated";
        public static readonly StringName CachesCleared = "caches_cleared";
        public static readonly StringName MixerApplied = "mixer_applied";
        public static readonly StringName MixerUpdated = "mixer_updated";
    }

    public event Action<StringName> AnimationFinished { add => Connect(SignalName.AnimationFinished, Callable.From(value)); remove => Disconnect(SignalName.AnimationFinished, Callable.From(value)); }
    public event Action<StringName> AnimationStarted { add => Connect(SignalName.AnimationStarted, Callable.From(value)); remove => Disconnect(SignalName.AnimationStarted, Callable.From(value)); }
    public event Action AnimationListChanged { add => Connect(SignalName.AnimationListChanged, Callable.From(value)); remove => Disconnect(SignalName.AnimationListChanged, Callable.From(value)); }
    public event Action AnimationLibrariesUpdated { add => Connect(SignalName.AnimationLibrariesUpdated, Callable.From(value)); remove => Disconnect(SignalName.AnimationLibrariesUpdated, Callable.From(value)); }
    public event Action MixerApplied { add => Connect(SignalName.MixerApplied, Callable.From(value)); remove => Disconnect(SignalName.MixerApplied, Callable.From(value)); }

    // The engine player, 0 when the animation library is not present (a host without Aver.Anim.Abi).
    internal int Player { get; private set; }

    private readonly List<KeyValuePair<StringName, AnimationLibrary>> _libraries = new();
    private readonly Dictionary<string, Animation> _synced = new(StringComparer.Ordinal);
    private long _syncSignature = -1;
    private readonly Dictionary<string, Target> _targets = new(StringComparer.Ordinal);
    private AnimationCallbackModeProcess _processMode = AnimationCallbackModeProcess.Idle;
    private NodePath _rootNode = new("..");

    public AnimationMixer()
    {
        if (PanimNative.Available) { PanimNative.DrainDead(); Player = PplayerNative.aver_pplayer_create(); }
    }

    // A finalizer runs on another thread and the library is single-threaded: it only queues the player.
    ~AnimationMixer()
    {
        if (Player != 0) PanimNative.QueueDestroyPlayer(Player);
    }

    private void DestroyPlayer()
    {
        int p = Player;
        Player = 0;
        if (p != 0) PplayerNative.aver_pplayer_destroy(p);
    }

    public override void Free()
    {
        DestroyPlayer();
        base.Free();
    }

    // ---- properties ----

    public bool Active
    {
        get => Player == 0 || PplayerNative.aver_pplayer_active(Player) != 0;
        set { if (Player != 0) PplayerNative.aver_pplayer_set_active(Player, value ? 1 : 0); }
    }

    public AnimationCallbackModeProcess CallbackModeProcess
    {
        get => _processMode;
        set { _processMode = value; RefreshProcessFlags(); }
    }

    public AnimationCallbackModeMethod CallbackModeMethod { get; set; } = AnimationCallbackModeMethod.Deferred;

    public AnimationCallbackModeDiscrete CallbackModeDiscrete
    {
        get => Player == 0 ? AnimationCallbackModeDiscrete.Recessive : (AnimationCallbackModeDiscrete)PplayerNative.aver_pplayer_discrete_mode(Player);
        set { if (Player != 0) PplayerNative.aver_pplayer_set_discrete_mode(Player, (int)value); }
    }

    /// <summary>When true the weights of the animations being blended are not normalised, so a target only some of them touch fades towards its
    /// RESET value instead of keeping the value of the animation that touches it.</summary>
    public bool Deterministic
    {
        get => Player != 0 && PplayerNative.aver_pplayer_deterministic(Player) != 0;
        set { if (Player != 0) PplayerNative.aver_pplayer_set_deterministic(Player, value ? 1 : 0); }
    }

    /// <summary>The node track paths are relative to (default: the parent).</summary>
    public NodePath RootNode
    {
        get => _rootNode;
        set { _rootNode = value; ClearCaches(); }
    }

    private NodePath _rootMotionTrack = new();

    /// <summary>The position / rotation / scale track whose movement is not applied to its node but reported by <see cref="GetRootMotionPosition"/>
    /// and the other root-motion getters (a character's body movement then comes from the animation).</summary>
    public NodePath RootMotionTrack
    {
        get => _rootMotionTrack;
        set
        {
            _rootMotionTrack = value;
            if (Player != 0) PplayerNative.aver_pplayer_set_root_motion(Player, value.IsEmpty ? "" : value.ToString());
        }
    }

    /// <summary>Stored; root motion is reported in the track's own space whatever this is (Godot 4.4's local-space option is not modelled).</summary>
    public bool RootMotionLocal { get; set; }

    // The root-motion answers of the last pass, in Godot's space: deltas for the position / rotation / scale of the track, and the blended
    // absolute values at the playheads.
    private readonly double[] _rm = new double[20];
    private int _rmFlags;

    private void ReadRootMotion()
    {
        _rmFlags = Player == 0 ? 0 : Math.Max(PplayerNative.aver_pplayer_root_motion(Player, _rm), 0);
    }

    private Vector3 RmVec(int at) => FabricSpace.FromAverScale(new Vec3((float)_rm[at], (float)_rm[at + 1], (float)_rm[at + 2]));

    /// <summary>How far the root-motion position track moved in the last pass (Godot metres and axes), zero without such a track.</summary>
    public Vector3 GetRootMotionPosition()
    {
        ReadRootMotion();
        if ((_rmFlags & 1) == 0) return Vector3.Zero;
        return FabricSpace.FromAverPosition(new Vec3((float)_rm[0], (float)_rm[1], (float)_rm[2]));
    }

    /// <summary>The rotation the root-motion rotation track made in the last pass, identity without one.</summary>
    public Quaternion GetRootMotionRotation()
    {
        ReadRootMotion();
        if ((_rmFlags & 2) == 0) return Quaternion.Identity;
        return FabricSpace.FromAverRotation(new Quat((float)_rm[3], (float)_rm[4], (float)_rm[5], (float)_rm[6]));
    }

    /// <summary>How far the root-motion scale track moved in the last pass, zero without one.</summary>
    public Vector3 GetRootMotionScale()
    {
        ReadRootMotion();
        return (_rmFlags & 4) == 0 ? Vector3.Zero : RmVec(7);
    }

    /// <summary>The blended absolute position of the root-motion track (not a delta), for placing a body that is driven by the animation.</summary>
    public Vector3 GetRootMotionPositionAccumulator()
    {
        ReadRootMotion();
        if ((_rmFlags & 1) == 0) return Vector3.Zero;
        return FabricSpace.FromAverPosition(new Vec3((float)_rm[10], (float)_rm[11], (float)_rm[12]));
    }

    /// <summary>The blended absolute rotation of the root-motion track.</summary>
    public Quaternion GetRootMotionRotationAccumulator()
    {
        ReadRootMotion();
        if ((_rmFlags & 2) == 0) return Quaternion.Identity;
        return FabricSpace.FromAverRotation(new Quat((float)_rm[13], (float)_rm[14], (float)_rm[15], (float)_rm[16]));
    }

    /// <summary>The blended absolute scale of the root-motion track.</summary>
    public Vector3 GetRootMotionScaleAccumulator()
    {
        ReadRootMotion();
        return (_rmFlags & 4) == 0 ? Vector3.Zero : RmVec(17);
    }

    public bool ResetOnSave { get; set; } = true;
    public int AudioMaxPolyphony { get; set; } = 32;

    internal override bool UsesInternalProcess => _processMode == AnimationCallbackModeProcess.Idle;
    internal override bool UsesInternalPhysicsProcess => _processMode == AnimationCallbackModeProcess.Physics;

    // ---- libraries ----

    public Error AddAnimationLibrary(StringName name, AnimationLibrary library)
    {
        if (library == null) return Error.InvalidParameter;
        string n = name.ToString();
        if (n.Contains('/') || n.Contains(',') || n.Contains('[') || n.Contains(']')) return Error.InvalidParameter;
        if (_libraries.Any(kv => kv.Key == name)) return Error.AlreadyExists;
        _libraries.Add(new KeyValuePair<StringName, AnimationLibrary>(name, library));
        LibrariesChanged();
        return Error.Ok;
    }

    public void RemoveAnimationLibrary(StringName name)
    {
        int i = _libraries.FindIndex(kv => kv.Key == name);
        if (i < 0) return;
        _libraries.RemoveAt(i);
        LibrariesChanged();
    }

    public void RenameAnimationLibrary(StringName name, StringName newname)
    {
        int i = _libraries.FindIndex(kv => kv.Key == name);
        if (i < 0 || _libraries.Any(kv => kv.Key == newname)) return;
        _libraries[i] = new KeyValuePair<StringName, AnimationLibrary>(newname, _libraries[i].Value);
        LibrariesChanged();
    }

    public bool HasAnimationLibrary(StringName name) => _libraries.Any(kv => kv.Key == name);

    public AnimationLibrary? GetAnimationLibrary(StringName name)
    {
        foreach (var kv in _libraries) if (kv.Key == name) return kv.Value;
        return null;
    }

    public StringName[] GetAnimationLibraryList() => _libraries.Select(kv => kv.Key).ToArray();

    private void LibrariesChanged()
    {
        _syncSignature = -1;
        SyncLibraries();
        EmitSignal(SignalName.AnimationLibrariesUpdated);
        EmitSignal(SignalName.AnimationListChanged);
        DispatchEvents();
    }

    /// <summary>The libraries as scripts and scenes set them: library name to library.</summary>
    public Collections.Dictionary Libraries
    {
        get
        {
            var d = new Collections.Dictionary();
            foreach (var kv in _libraries) d[kv.Key] = kv.Value;
            return d;
        }
        set
        {
            _libraries.Clear();
            foreach (KeyValuePair<Variant, Variant> kv in value)
            {
                AnimationLibrary? lib = LibraryFrom(kv.Value);
                if (lib != null) _libraries.Add(new KeyValuePair<StringName, AnimationLibrary>(kv.Key.AsStringName(), lib));
            }
            LibrariesChanged();
        }
    }

    // A library given as an AnimationLibrary, or (the converter's form) a dictionary of animation name to Animation or to the path of an .ocanim.
    private static AnimationLibrary? LibraryFrom(Variant v)
    {
        if (v.AsGodotObject() is AnimationLibrary lib) return lib;
        if (v.VariantType != Variant.Type.Dictionary) return null;
        var made = new AnimationLibrary();
        foreach (KeyValuePair<Variant, Variant> kv in v.AsGodotDictionary())
        {
            Animation? a = kv.Value.AsGodotObject() switch
            {
                Animation direct => direct,
                Resource r when r.ResourcePath.Length > 0 => ResourceLoader.Load(r.ResourcePath, typeof(Animation)) as Animation,
                _ => kv.Value.VariantType == Variant.Type.String ? ResourceLoader.Load(kv.Value.AsString(), typeof(Animation)) as Animation : null,
            };
            if (a == null) { Aver.Scripting.Log.Warn($"[Fabric] AnimationPlayer: the animation '{kv.Key}' could not be loaded"); continue; }
            made.AddAnimation(kv.Key.AsStringName(), a);
        }
        return made;
    }

    private static string KeyOf(StringName library, StringName name) => library.IsEmpty ? name.ToString() : library + "/" + name;

    // Splits "lib/name" at the first slash; no slash is the library with the empty name.
    private static void Split(string full, out StringName library, out StringName name)
    {
        int slash = full.IndexOf('/');
        if (slash < 0) { library = StringName.Empty; name = full; }
        else { library = full[..slash]; name = full[(slash + 1)..]; }
    }

    public bool HasAnimation(StringName name) => GetAnimation(name) != null;

    public Animation? GetAnimation(StringName name)
    {
        Split(name.ToString(), out StringName lib, out StringName anim);
        return GetAnimationLibrary(lib)?.GetAnimation(anim);
    }

    /// <summary>Every animation as "name" (default library) or "library/name".</summary>
    public string[] GetAnimationList()
    {
        var list = new List<string>();
        foreach (var kv in _libraries)
            foreach (StringName n in kv.Value.GetAnimationList()) list.Add(KeyOf(kv.Key, n));
        return list.ToArray();
    }

    public StringName FindAnimation(Animation animation)
    {
        foreach (var kv in _libraries)
            foreach (var e in kv.Value.Entries)
                if (ReferenceEquals(e.Value, animation)) return KeyOf(kv.Key, e.Key);
        return StringName.Empty;
    }

    public StringName FindAnimationLibrary(Animation animation)
    {
        foreach (var kv in _libraries)
            foreach (var e in kv.Value.Entries)
                if (ReferenceEquals(e.Value, animation)) return kv.Key;
        return StringName.Empty;
    }

    // Makes the engine player's clips the libraries' animations (cheap when nothing changed).
    internal void SyncLibraries()
    {
        if (Player == 0) return;
        long sig = _libraries.Count;
        foreach (var kv in _libraries) sig = unchecked(sig * 31 + kv.Key.GetHashCode() + kv.Value.ContentVersion * 7 + kv.Value.GetAnimationListSize());
        if (sig == _syncSignature) return;
        _syncSignature = sig;
        var wanted = new Dictionary<string, Animation>(StringComparer.Ordinal);
        foreach (var kv in _libraries)
            foreach (var e in kv.Value.Entries) wanted[KeyOf(kv.Key, e.Key)] = e.Value;
        foreach (string gone in _synced.Keys.Where(k => !wanted.ContainsKey(k)).ToList())
        {
            PplayerNative.aver_pplayer_remove_clip(Player, gone);
            _synced.Remove(gone);
        }
        foreach (var kv in wanted)
        {
            if (kv.Value.Handle == 0) continue;
            PplayerNative.aver_pplayer_set_clip(Player, kv.Key, kv.Value.Handle);
            _synced[kv.Key] = kv.Value;
        }
    }

    // ---- stepping ----

    /// <summary>Runs one pass of the mixer now, as if <paramref name="delta"/> seconds had passed.</summary>
    public virtual void Advance(double delta)
    {
        if (Player == 0) return;
        SyncLibraries();
        PplayerNative.aver_pplayer_advance(Player, delta);
        DispatchEvents();
    }

    // Whether the idle / physics pass has anything to do (a player: an animation is playing; a tree: it is active).
    internal virtual bool ShouldStep() => Player != 0 && PplayerNative.aver_pplayer_is_playing(Player) != 0;

    internal override void InternalProcess(double delta)
    {
        if (_processMode == AnimationCallbackModeProcess.Idle) Step(delta);
    }

    internal override void InternalPhysicsProcess(double delta)
    {
        if (_processMode == AnimationCallbackModeProcess.Physics) Step(delta);
    }

    private void Step(double delta)
    {
        if (Player == 0 || !CanProcess() || !ShouldStep()) return;
        Advance(delta);
    }

    /// <summary>Forgets the nodes and properties tracks were resolved to; they are looked up again on the next pass.</summary>
    public void ClearCaches()
    {
        _targets.Clear();
        EmitSignal(SignalName.CachesCleared);
    }

    // ---- capture ----

    /// <summary>Snapshots the current values of the capture-mode tracks of an animation and fades that snapshot out over <paramref name="duration"/>
    /// seconds while the animation comes in. Does nothing for an animation with no capture-mode track. The transition and ease types are accepted;
    /// the fade is linear.</summary>
    public void Capture(StringName name, double duration, Tween.TransitionType transType = Tween.TransitionType.Linear, Tween.EaseType easeType = Tween.EaseType.In)
    {
        if (Player == 0) return;
        SyncLibraries();
        string key = name.ToString();
        int n = PplayerNative.aver_pplayer_capture_track_count(Player, key);
        if (n <= 0) return;
        if (duration < 0)
        {
            // The interval to the first key of the earliest capture track.
            Animation? anim = GetAnimation(name);
            duration = double.MaxValue;
            if (anim != null)
                for (int t = 0; t < anim.GetTrackCount(); t++)
                    if (anim.TrackGetType(t) == Animation.TrackType.Value && anim.ValueTrackGetUpdateMode(t) == Animation.UpdateMode.Capture && anim.TrackGetKeyCount(t) > 0)
                        duration = Math.Min(duration, anim.TrackGetKeyTime(t, 0));
            if (duration == double.MaxValue) duration = 0;
        }
        if (duration <= 0) return;
        PplayerNative.aver_pplayer_capture_begin(Player);
        for (int i = 0; i < n; i++)
        {
            string path = PplayerNative.Str(PplayerNative.aver_pplayer_capture_track_path(Player, key, i));
            int type = PplayerNative.aver_pplayer_capture_track_type(Player, key, i);
            Target t = Resolve(path, (PanimTrackType)type);
            if (t.Node == null || t.Sub.Length == 0) continue;
            var v5 = new double[5];
            AnimValue.Encode(GetIndexed(t.Node, t.Sub), v5, out string text);
            PplayerNative.aver_pplayer_capture_add(Player, type, path, v5, text);
        }
        PplayerNative.aver_pplayer_capture_end(Player, duration);
    }

    // ---- what the engine player reports ----

    private readonly record struct Ev(PanimEvent Type, string A, string B, string Path, double T0, double T1, Variant[] Args);
    private readonly record struct Out(int Id, double[] V5, string Text, bool Discrete, PanimTrackType Type, string Path);

    /// <summary>Carries out everything the engine player has reported since the last call, in order.</summary>
    internal void DispatchEvents()
    {
        int p = Player;
        if (p == 0) return;
        int n = PplayerNative.aver_pplayer_event_count(p);
        if (n == 0) return;
        var events = new List<Ev>(n);
        bool applied = false;
        for (int i = 0; i < n; i++)
        {
            var type = (PanimEvent)PplayerNative.aver_pplayer_event_type(p, i);
            Variant[] args = System.Array.Empty<Variant>();
            if (type == PanimEvent.Method)
            {
                int argc = PplayerNative.aver_pplayer_event_arg_count(p, i);
                args = new Variant[argc];
                for (int a = 0; a < argc; a++)
                {
                    var v5 = new double[5];
                    PplayerNative.aver_pplayer_event_arg(p, i, a, v5);
                    args[a] = AnimValue.Decode(v5, PplayerNative.Str(PplayerNative.aver_pplayer_event_arg_text(p, i, a)));
                }
            }
            if (type == PanimEvent.Applied) applied = true;
            events.Add(new Ev(type, PplayerNative.Str(PplayerNative.aver_pplayer_event_a(p, i)), PplayerNative.Str(PplayerNative.aver_pplayer_event_b(p, i)),
                PplayerNative.Str(PplayerNative.aver_pplayer_event_path(p, i)), PplayerNative.aver_pplayer_event_t0(p, i), PplayerNative.aver_pplayer_event_t1(p, i), args));
        }
        List<Out>? outputs = applied ? ReadOutputs(p) : null;
        PplayerNative.aver_pplayer_clear_events(p);

        foreach (Ev e in events)
        {
            if (!IsAlive) return;
            switch (e.Type)
            {
                case PanimEvent.Started: EmitSignal(SignalName.AnimationStarted, new StringName(e.A)); OnStarted(e.A); break;
                case PanimEvent.Finished: EmitSignal(SignalName.AnimationFinished, new StringName(e.A)); break;
                case PanimEvent.Changed: OnChanged(e.A, e.B); break;
                case PanimEvent.CurrentChanged: OnCurrentChanged(e.A); break;
                case PanimEvent.Looped: OnLooped(e.A); break;
                case PanimEvent.Method: CallMethod(e); break;
                case PanimEvent.AudioPlay: AudioPlay(e); break;
                case PanimEvent.AudioStop: AudioStop(e); break;
                case PanimEvent.AnimPlay: SubPlayerPlay(e); break;
                case PanimEvent.AnimStop: SubPlayerStop(e); break;
                case PanimEvent.Applied: if (outputs != null) Apply(outputs); break;
                case PanimEvent.MixerApplied: EmitSignal(SignalName.MixerApplied); break;
            }
        }
    }

    internal virtual void OnStarted(string name) { }
    internal virtual void OnChanged(string oldName, string newName) { }
    internal virtual void OnCurrentChanged(string name) { }
    internal virtual void OnLooped(string name) { }

    private static List<Out> ReadOutputs(int p)
    {
        int n = PplayerNative.aver_pplayer_output_count(p);
        var list = new List<Out>(n);
        for (int i = 0; i < n; i++)
        {
            int id = PplayerNative.aver_pplayer_output_track(p, i);
            var v5 = new double[5];
            PplayerNative.aver_pplayer_output_value(p, i, v5);
            list.Add(new Out(id, v5, PplayerNative.Str(PplayerNative.aver_pplayer_output_text(p, i)), PplayerNative.aver_pplayer_output_discrete(p, i) != 0,
                (PanimTrackType)PplayerNative.aver_pplayer_target_type(p, id), PplayerNative.Str(PplayerNative.aver_pplayer_target_path(p, id))));
        }
        return list;
    }

    // ---- targets ----

    private sealed class Target
    {
        public Node? Node;
        public string[] Sub = System.Array.Empty<string>();
        public bool Warned;
    }

    private Node? RootOfTracks() => _rootNode.IsEmpty ? this : GetNodeOrNull(_rootNode);

    private Target Resolve(string path, PanimTrackType type)
    {
        var t = new Target();
        var np = new NodePath(path);
        Node? root = RootOfTracks();
        string names = np.GetConcatenatedNames();
        t.Node = root == null ? null : names.Length == 0 ? root : root.GetNodeOrNull(names);
        string subs = np.GetConcatenatedSubnames();
        t.Sub = subs.Length == 0 ? System.Array.Empty<string>() : subs.Split(':', StringSplitOptions.RemoveEmptyEntries);
        return t;
    }

    private Target TargetOf(string path)
    {
        if (_targets.TryGetValue(path, out Target? t) && (t.Node == null ? t.Warned : t.Node.IsAlive)) return t;
        t = Resolve(path, PanimTrackType.Value);
        if (t.Node == null)
        {
            Aver.Scripting.Log.Warn($"[Fabric] AnimationPlayer '{Name}': the track '{path}' names no node (relative to '{RootNode}')");
            t.Warned = true;
        }
        _targets[path] = t;
        return t;
    }

    private static Variant GetIndexed(GodotObject target, string[] sub)
    {
        if (sub.Length == 1) return target.Get(sub[0]);
        Variant head = target.Get(sub[0]);
        if (head.AsGodotObject() is GodotObject inner) return GetIndexed(inner, sub[1..]);
        return target.Get(string.Join(':', sub));
    }

    private static void SetIndexed(GodotObject target, string[] sub, Variant value)
    {
        if (sub.Length == 1) { target.Set(sub[0], value); return; }
        Variant head = target.Get(sub[0]);
        if (head.AsGodotObject() is GodotObject inner) { SetIndexed(inner, sub[1..], value); return; }
        target.Set(string.Join(':', sub), value);
    }

    // ---- applying values ----

    private void Apply(List<Out> outputs)
    {
        foreach (Out o in outputs)
        {
            Target t = TargetOf(o.Path);
            if (t.Node == null) continue;
            try
            {
                switch (o.Type)
                {
                    case PanimTrackType.Value:
                    case PanimTrackType.Bezier:
                        if (t.Sub.Length == 0) { WarnOnce(t, o.Path, "a value track needs a property in its path"); break; }
                        SetIndexed(t.Node, t.Sub, AnimValue.Decode(o.V5, o.Text));
                        break;
                    case PanimTrackType.Position3D:
                    case PanimTrackType.Rotation3D:
                    case PanimTrackType.Scale3D:
                        ApplyTransform(t, o);
                        break;
                    case PanimTrackType.BlendShape:
                        if (t.Sub.Length == 0) { WarnOnce(t, o.Path, "a blend-shape track needs the shape's name in its path"); break; }
                        if (t.Node is MeshInstance3D mi) mi.SetBlendShapeValueByName(t.Sub[0], (float)o.V5[1]);
                        else t.Node.Set("blend_shapes/" + t.Sub[0], (float)o.V5[1]);
                        break;
                }
            }
            catch (Exception ex)
            {
                Aver.Scripting.Log.Error($"[Fabric] AnimationPlayer '{Name}': writing '{o.Path}' threw {ex.GetType().Name}: {ex.Message}");
            }
        }
    }

    private void ApplyTransform(Target t, Out o)
    {
        var v = new Vec3((float)o.V5[1], (float)o.V5[2], (float)o.V5[3]);
        if (t.Sub.Length > 0)
        {
            // "Skeleton3D:Bone": the pose of that bone of a Skeleton3D (Godot space).
            if (t.Node is not Skeleton3D skel) { WarnOnce(t, o.Path, "a bone track needs a Skeleton3D node"); return; }
            int bone = skel.FindBone(t.Sub[0]);
            if (bone < 0) { WarnOnce(t, o.Path, $"the skeleton has no bone '{t.Sub[0]}'"); return; }
            switch (o.Type)
            {
                case PanimTrackType.Position3D: skel.SetBonePosePosition(bone, FabricSpace.FromAverPosition(v)); break;
                case PanimTrackType.Scale3D: skel.SetBonePoseScale(bone, FabricSpace.FromAverScale(v)); break;
                case PanimTrackType.Rotation3D: skel.SetBonePoseRotation(bone, FabricSpace.FromAverRotation(new Quat(v.X, v.Y, v.Z, (float)o.V5[4]))); break;
            }
            return;
        }
        if (t.Node is not Node3D n3) { WarnOnce(t, o.Path, "a transform track needs a Node3D"); return; }
        switch (o.Type)
        {
            case PanimTrackType.Position3D: n3.Position = FabricSpace.FromAverPosition(v); break;
            case PanimTrackType.Scale3D: n3.Scale = FabricSpace.FromAverScale(v); break;
            case PanimTrackType.Rotation3D: n3.Quaternion = FabricSpace.FromAverRotation(new Quat(v.X, v.Y, v.Z, (float)o.V5[4])); break;
        }
    }

    private void WarnOnce(Target t, string path, string why)
    {
        if (t.Warned) return;
        t.Warned = true;
        Aver.Scripting.Log.Warn($"[Fabric] AnimationPlayer '{Name}': the track '{path}' is not applied: {why}");
    }

    // ---- method, audio and sub-player tracks ----

    private void CallMethod(Ev e)
    {
        Target t = TargetOf(e.Path);
        if (t.Node == null) return;
        if (!t.Node.HasMethod(e.A))
        {
            Aver.Scripting.Log.Error($"[Fabric] AnimationPlayer '{Name}': a method track calls '{e.A}' on '{e.Path}', which has no such method");
            return;
        }
        if (CallbackModeMethod == AnimationCallbackModeMethod.Deferred) { t.Node.CallDeferred(e.A, e.Args); return; }
        try { t.Node.Call(e.A, e.Args); }
        catch (Exception ex) { Aver.Scripting.Log.Error($"[Fabric] AnimationPlayer '{Name}': the method track call {e.A} threw {ex.GetType().Name}: {ex.Message}"); }
    }

    private void AudioPlay(Ev e)
    {
        Target t = TargetOf(e.Path);
        if (t.Node == null) return;
        if (t.Node is IAnimationAudioTarget a) { a.AnimationPlayStream(e.A, e.T0, e.T1); return; }
        // Not a node that knows animations: set its stream and play it from the offset.
        if (e.A.Length > 0 && MemberAccess.HasMember(t.Node.GetType(), "stream")) t.Node.Set("stream", ResourceLoader.Load(e.A));
        if (t.Node.HasMethod("play")) t.Node.Call("play", e.T0);
        else WarnOnce(t, e.Path, "an audio track needs an AudioStreamPlayer (IAnimationAudioTarget) or a node with play()");
    }

    private void AudioStop(Ev e)
    {
        Target t = TargetOf(e.Path);
        if (t.Node is IAnimationAudioTarget a) a.AnimationStopStream();
        else if (t.Node != null && t.Node.HasMethod("stop")) t.Node.Call("stop");
    }

    private void SubPlayerPlay(Ev e)
    {
        Target t = TargetOf(e.Path);
        if (t.Node is not AnimationPlayer sub) { if (t.Node != null) WarnOnce(t, e.Path, "an animation track needs an AnimationPlayer"); return; }
        if (!sub.HasAnimation(e.A)) return;
        sub.Play(e.A);
        if (e.T1 != 0)
        {
            // A seek into the middle of the key: where the sub-animation is by now, by its own loop mode.
            Animation? a = sub.GetAnimation(e.A);
            if (a == null) return;
            double len = a.Length, pos = e.T0;
            switch (a.LoopMode)
            {
                case Animation.LoopModeEnum.None: if (pos > len) { sub.Stop(); return; } break;
                case Animation.LoopModeEnum.Linear: pos = len > 0 ? Mathf.PosMod(pos, len) : 0; break;
                case Animation.LoopModeEnum.Pingpong:
                    if (len > 0) { pos = Mathf.PosMod(pos, len * 2); if (pos > len) pos = len * 2 - pos; }
                    break;
            }
            sub.Seek(pos, true);
        }
    }

    private void SubPlayerStop(Ev e)
    {
        Target t = TargetOf(e.Path);
        if (t.Node is AnimationPlayer sub && sub.IsPlaying()) sub.Stop();
    }

    // Values the node a track names currently holds, for tests and capture.
    internal Variant ReadTrackTarget(string path)
    {
        Target t = Resolve(path, PanimTrackType.Value);
        return t.Node == null || t.Sub.Length == 0 ? default : GetIndexed(t.Node, t.Sub);
    }
}

/// <summary>Plays animations: property, transform, method, audio and sub-player tracks on any node (Godot's AnimationPlayer).</summary>
public class AnimationPlayer : AnimationMixer
{
    public new class SignalName : AnimationMixer.SignalName
    {
        public static readonly StringName AnimationChanged = "animation_changed";
        public static readonly StringName CurrentAnimationChanged = "current_animation_changed";
        /// <summary>An Aver extension (Godot has no such signal): the playhead wrapped round a looping animation or bounced off an end of a ping-pong one.</summary>
        public static readonly StringName AnimationLooped = "animation_looped";
    }

    public event Action<StringName, StringName> AnimationChanged { add => Connect(SignalName.AnimationChanged, Callable.From(value)); remove => Disconnect(SignalName.AnimationChanged, Callable.From(value)); }
    public event Action<StringName> CurrentAnimationChanged { add => Connect(SignalName.CurrentAnimationChanged, Callable.From(value)); remove => Disconnect(SignalName.CurrentAnimationChanged, Callable.From(value)); }
    public event Action<StringName> AnimationLooped { add => Connect(SignalName.AnimationLooped, Callable.From(value)); remove => Disconnect(SignalName.AnimationLooped, Callable.From(value)); }

    public enum AnimationMethodCallMode { Deferred = 0, Immediate = 1 }
    public enum AnimationProcessCallback { Physics = 0, Idle = 1, Manual = 2 }

    private double _autoCaptureDuration = -1.0;

    /// <summary>The animation played when the node becomes ready.</summary>
    public StringName Autoplay { get; set; } = StringName.Empty;

    public bool PlaybackAutoCapture { get; set; } = true;

    public double PlaybackAutoCaptureDuration { get => _autoCaptureDuration; set => _autoCaptureDuration = value; }

    public Tween.TransitionType PlaybackAutoCaptureTransitionType { get; set; } = Tween.TransitionType.Linear;

    public Tween.EaseType PlaybackAutoCaptureEaseType { get; set; } = Tween.EaseType.In;

    public bool MovieQuitOnFinish { get; set; }

    /// <summary>The blend time between two animations that have no entry of their own, in seconds.</summary>
    public double PlaybackDefaultBlendTime
    {
        get => Player == 0 ? 0 : PplayerNative.aver_pplayer_default_blend(Player);
        set { if (Player != 0) PplayerNative.aver_pplayer_set_default_blend(Player, Math.Clamp(value, 0, 4096)); }
    }

    public float SpeedScale
    {
        get => Player == 0 ? 1f : (float)PplayerNative.aver_pplayer_speed_scale(Player);
        set { if (Player != 0) PplayerNative.aver_pplayer_set_speed_scale(Player, value); }
    }

    /// <summary>The key of the animation playing now, or empty when none is. Setting it plays that animation (or stops on empty).</summary>
    public StringName CurrentAnimation
    {
        get => Player == 0 ? StringName.Empty : new StringName(PplayerNative.Str(PplayerNative.aver_pplayer_current(Player)));
        set
        {
            if (value.IsEmpty) { Stop(); return; }
            if (CurrentAnimation != value) Play(value);
        }
    }

    /// <summary>The key of the animation playing now or last played. Setting it changes the animation without playing it, unless one is playing.</summary>
    public StringName AssignedAnimation
    {
        get => Player == 0 ? StringName.Empty : new StringName(PplayerNative.Str(PplayerNative.aver_pplayer_assigned(Player)));
        set
        {
            if (Player == 0) return;
            SyncLibraries();
            PplayerNative.aver_pplayer_set_assigned(Player, value.ToString());
            DispatchEvents();
        }
    }

    public float CurrentAnimationLength => Player == 0 ? 0f : (float)PplayerNative.aver_pplayer_length(Player);

    public float CurrentAnimationPosition => Player == 0 ? 0f : (float)PplayerNative.aver_pplayer_position(Player);

    /// <summary>Blend times as scenes store them: a flat array of from, to, seconds triples.</summary>
    public Collections.Array BlendTimes
    {
        set
        {
            for (int i = 0; i + 2 < value.Count; i += 3) SetBlendTime(value[i].AsStringName(), value[i + 1].AsStringName(), value[i + 2].AsDouble());
        }
    }

    public bool IsPlaying() => Player != 0 && PplayerNative.aver_pplayer_is_playing(Player) != 0;

    public bool IsAnimationActive() => Active;

    public float GetPlayingSpeed() => Player == 0 ? 0f : (float)PplayerNative.aver_pplayer_playing_speed(Player);

    // ---- playing ----

    /// <summary>Plays an animation. An empty name resumes the assigned one. <paramref name="customBlend"/> below zero uses the blend time table;
    /// <paramref name="customSpeed"/> multiplies <see cref="SpeedScale"/>; <paramref name="fromEnd"/> starts at the end. The pose updates on the next
    /// frame; call <c>Advance(0)</c> for an immediate update.</summary>
    public void Play(StringName? name = null, double customBlend = -1, float customSpeed = 1.0f, bool fromEnd = false)
    {
        if (Player == 0) return;
        SyncLibraries();
        string n = name?.ToString() ?? "";
        string target = n.Length == 0 ? AssignedAnimation.ToString() : n;
        if (target.Length == 0 || !HasAnimation(target))
        {
            Aver.Scripting.Log.Error($"[Fabric] AnimationPlayer '{Name}': there is no animation '{target}' to play");
            return;
        }
        if (PlaybackAutoCapture && AssignedAnimation.ToString() != target) Capture(target, PlaybackAutoCaptureDuration, PlaybackAutoCaptureTransitionType, PlaybackAutoCaptureEaseType);
        PplayerNative.aver_pplayer_play(Player, target, customBlend, customSpeed, fromEnd ? 1 : 0);
        DispatchEvents();
    }

    /// <summary>Plays an animation backwards from its end.</summary>
    public void PlayBackwards(StringName? name = null, double customBlend = -1) => Play(name, customBlend, -1.0f, true);

    /// <summary>Plays an animation after capturing the current values of its capture-mode tracks and fading from them over <paramref name="duration"/>.</summary>
    public void PlayWithCapture(StringName? name = null, double duration = -1.0, double customBlend = -1, float customSpeed = 1.0f, bool fromEnd = false,
        Tween.TransitionType transType = Tween.TransitionType.Linear, Tween.EaseType easeType = Tween.EaseType.In)
    {
        if (Player == 0) return;
        string n = name?.ToString() ?? "";
        string target = n.Length == 0 ? AssignedAnimation.ToString() : n;
        if (target.Length > 0 && HasAnimation(target)) Capture(target, duration, transType, easeType);
        bool auto = PlaybackAutoCapture;
        PlaybackAutoCapture = false;
        try { Play(target, customBlend, customSpeed, fromEnd); }
        finally { PlaybackAutoCapture = auto; }
    }

    public void Stop(bool keepState = false)
    {
        if (Player == 0) return;
        SyncLibraries();
        PplayerNative.aver_pplayer_stop(Player, keepState ? 1 : 0);
        DispatchEvents();
    }

    public void Pause()
    {
        if (Player == 0) return;
        PplayerNative.aver_pplayer_pause(Player);
        DispatchEvents();
    }

    /// <summary>Plays the animation after the current one and the ones queued before it finish (at once if nothing is playing). A looping animation holds the queue.</summary>
    public void Queue(StringName name)
    {
        if (Player == 0) return;
        SyncLibraries();
        if (PplayerNative.aver_pplayer_queue(Player, name.ToString()) == 0)
            Aver.Scripting.Log.Error($"[Fabric] AnimationPlayer '{Name}': there is no animation '{name}' to queue");
        DispatchEvents();
    }

    public void ClearQueue() { if (Player != 0) PplayerNative.aver_pplayer_clear_queue(Player); }

    public StringName[] GetQueue()
    {
        if (Player == 0) return System.Array.Empty<StringName>();
        int n = PplayerNative.aver_pplayer_queue_count(Player);
        var q = new StringName[n];
        for (int i = 0; i < n; i++) q[i] = new StringName(PplayerNative.Str(PplayerNative.aver_pplayer_queue_at(Player, i)));
        return q;
    }

    /// <summary>Moves the playhead. The events between the old and the new position are skipped. <paramref name="update"/> applies the pose at once;
    /// <paramref name="updateOnly"/> leaves method, audio and sub-player tracks alone.</summary>
    public void Seek(double seconds, bool update = false, bool updateOnly = false)
    {
        if (Player == 0) return;
        SyncLibraries();
        PplayerNative.aver_pplayer_seek(Player, seconds, update ? 1 : 0, updateOnly ? 1 : 0);
        DispatchEvents();
    }

    // ---- blend times and chaining ----

    public void SetBlendTime(StringName animationFrom, StringName animationTo, double sec)
    {
        if (Player != 0) PplayerNative.aver_pplayer_set_blend_time(Player, animationFrom.ToString(), animationTo.ToString(), sec);
    }

    public double GetBlendTime(StringName animationFrom, StringName animationTo) =>
        Player == 0 ? 0 : PplayerNative.aver_pplayer_blend_time(Player, animationFrom.ToString(), animationTo.ToString());

    public void AnimationSetNext(StringName animationFrom, StringName animationTo)
    {
        if (Player != 0) PplayerNative.aver_pplayer_set_next(Player, animationFrom.ToString(), animationTo.ToString());
    }

    public StringName AnimationGetNext(StringName animationFrom) =>
        Player == 0 ? StringName.Empty : new StringName(PplayerNative.Str(PplayerNative.aver_pplayer_next(Player, animationFrom.ToString())));

    // ---- signals from the engine player ----

    internal override void OnChanged(string oldName, string newName) => EmitSignal(SignalName.AnimationChanged, new StringName(oldName), new StringName(newName));
    internal override void OnCurrentChanged(string name) => EmitSignal(SignalName.CurrentAnimationChanged, new StringName(name));
    internal override void OnLooped(string name) => EmitSignal(SignalName.AnimationLooped, new StringName(name));

    internal override void OnReadying()
    {
        base.OnReadying();
        if (!Autoplay.IsEmpty && HasAnimation(Autoplay)) Play(Autoplay);
    }

    // Deprecated Godot names for the mixer's callback modes.
    public AnimationMethodCallMode GetMethodCallMode() => (AnimationMethodCallMode)(int)CallbackModeMethod;
    public void SetMethodCallMode(AnimationMethodCallMode mode) => CallbackModeMethod = (AnimationCallbackModeMethod)(int)mode;
    public AnimationProcessCallback GetProcessCallback() => (AnimationProcessCallback)(int)CallbackModeProcess;
    public void SetProcessCallback(AnimationProcessCallback mode) => CallbackModeProcess = (AnimationCallbackModeProcess)(int)mode;
    public NodePath GetRoot() => RootNode;
    public void SetRoot(NodePath path) => RootNode = path;
}
