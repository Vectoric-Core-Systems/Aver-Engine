// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Animation and AnimationLibrary (Godot's resources). An Animation is a handle to a property-track clip in the engine's animation module (Aver.Anim.Abi,
// the same clip an .ocanim file holds); every Godot method here reads or edits that clip. Values travel as numbers where they have a numeric form
// and otherwise as Godot's text form of the Variant, so Variant keys of any type survive.
//
// Position, rotation and scale 3D tracks are stored in engine space (centimetres, +Z up, left-handed) because that is what the converter writes into
// an .ocanim; these methods take and return Godot's metres and axes, converting through FabricSpace. A value track keeps the Godot numbers it was
// given, since the property it writes is a Godot property.

using Aver.Scene;

namespace Aver.Fabric;

/// <summary>One animation: tracks of keys, a length and a loop mode (Godot's Animation).</summary>
public class Animation : Resource
{
    public enum TrackType { Value = 0, Position3D = 1, Rotation3D = 2, Scale3D = 3, BlendShape = 4, Method = 5, Bezier = 6, Audio = 7, Animation = 8 }
    public enum InterpolationType { Nearest = 0, Linear = 1, Cubic = 2, LinearAngle = 3, CubicAngle = 4 }
    public enum UpdateMode { Continuous = 0, Discrete = 1, Capture = 2 }
    public enum LoopModeEnum { None = 0, Linear = 1, Pingpong = 2 }
    public enum LoopedFlag { None = 0, End = 1, Start = 2 }
    public enum FindMode { Nearest = 0, Approx = 1, Exact = 2 }

    internal int Handle { get; private set; }

    /// <summary>Bumped by every edit, so a player can tell its copy of the clip is current.</summary>
    internal int Version { get; private set; }

    public Animation()
    {
        if (PanimNative.Available) { PanimNative.DrainDead(); Handle = PanimNative.aver_panim_create(); }
    }

    ~Animation()
    {
        if (Handle != 0) PanimNative.QueueDestroyClip(Handle);
    }

    /// <summary>Reads an .ocanim file into this animation (replacing its tracks). False when the file cannot be read.</summary>
    internal bool LoadFrom(string file)
    {
        if (!PanimNative.Available) return false;
        int h = PanimNative.aver_panim_load(file);
        if (h == 0) return false;
        if (Handle != 0) PanimNative.aver_panim_destroy(Handle);
        Handle = h;
        Version++;
        return true;
    }

    /// <summary>Writes this animation to an .ocanim file (tests and tools).</summary>
    internal bool SaveTo(string file) => Handle != 0 && PanimNative.aver_panim_save(Handle, file) != 0;

    private void Touch() { Version++; EmitChanged(); }

    public override Resource Duplicate(bool subresources = false)
    {
        var copy = new Animation();
        if (Handle != 0 && PanimNative.Available)
        {
            if (copy.Handle != 0) PanimNative.aver_panim_destroy(copy.Handle);
            copy.Handle = PanimNative.aver_panim_clone(Handle);
        }
        copy.ResourceName = ResourceName;
        return copy;
    }

    /// <summary>Removes every track and resets the length and loop mode.</summary>
    public void Clear()
    {
        if (Handle == 0) return;
        while (PanimNative.aver_panim_track_count(Handle) > 0) PanimNative.aver_panim_track_remove(Handle, 0);
        PanimNative.aver_panim_set_length(Handle, 1.0);
        PanimNative.aver_panim_set_loop_mode(Handle, 0);
        Touch();
    }

    // ---- properties ----

    public float Length
    {
        get => Handle == 0 ? 1f : (float)PanimNative.aver_panim_length(Handle);
        set { if (Handle != 0 && PanimNative.aver_panim_set_length(Handle, value) != 0) Touch(); }
    }

    public LoopModeEnum LoopMode
    {
        get => Handle == 0 ? LoopModeEnum.None : (LoopModeEnum)PanimNative.aver_panim_loop_mode(Handle);
        set { if (Handle != 0 && PanimNative.aver_panim_set_loop_mode(Handle, (int)value) != 0) Touch(); }
    }

    public float Step
    {
        get => Handle == 0 ? 1f / 30f : (float)PanimNative.aver_panim_step(Handle);
        set { if (Handle != 0 && PanimNative.aver_panim_set_step(Handle, value) != 0) Touch(); }
    }

    /// <summary>True when a track is in capture update mode.</summary>
    public bool CaptureIncluded
    {
        get
        {
            for (int i = 0; i < GetTrackCount(); i++)
                if (TrackGetType(i) == TrackType.Value && ValueTrackGetUpdateMode(i) == UpdateMode.Capture) return true;
            return false;
        }
    }

    // ---- tracks ----

    public int GetTrackCount() => Handle == 0 ? 0 : PanimNative.aver_panim_track_count(Handle);

    public int AddTrack(TrackType type, int atPosition = -1)
    {
        if (Handle == 0) return -1;
        int i = PanimNative.aver_panim_track_add(Handle, (int)type, atPosition);
        // Godot's defaults: transform tracks interpolate linearly; a method track has no interpolation to speak of; value tracks are linear.
        if (i >= 0) { PanimNative.aver_panim_track_set_interp(Handle, i, (int)InterpolationType.Linear); PanimNative.aver_panim_track_set_flags(Handle, i, 1, 1, 0); Touch(); }
        return i;
    }

    public void RemoveTrack(int trackIdx) { if (Handle != 0 && PanimNative.aver_panim_track_remove(Handle, trackIdx) != 0) Touch(); }

    public TrackType TrackGetType(int trackIdx) => Handle == 0 ? TrackType.Value : (TrackType)Math.Max(PanimNative.aver_panim_track_type(Handle, trackIdx), 0);

    public NodePath TrackGetPath(int trackIdx) => Handle == 0 ? new NodePath() : new NodePath(PanimNative.Str(PanimNative.aver_panim_track_path(Handle, trackIdx)));

    public void TrackSetPath(int trackIdx, NodePath path) { if (Handle != 0 && PanimNative.aver_panim_track_set_path(Handle, trackIdx, path.ToString()) != 0) Touch(); }

    public int FindTrack(NodePath path, TrackType type)
    {
        for (int i = 0; i < GetTrackCount(); i++)
            if (TrackGetType(i) == type && TrackGetPath(i) == path) return i;
        return -1;
    }

    public void TrackMoveTo(int trackIdx, int toIdx) { if (Handle != 0 && PanimNative.aver_panim_track_move(Handle, trackIdx, toIdx) != 0) Touch(); }

    public void TrackSwap(int trackIdx, int withIdx)
    {
        if (Handle == 0 || trackIdx == withIdx || trackIdx < 0 || withIdx < 0 || trackIdx >= GetTrackCount() || withIdx >= GetTrackCount()) return;
        int lo = Math.Min(trackIdx, withIdx), hi = Math.Max(trackIdx, withIdx);
        PanimNative.aver_panim_track_move(Handle, hi, lo);
        PanimNative.aver_panim_track_move(Handle, lo + 1, hi);
        Touch();
    }

    public void TrackSetImported(int trackIdx, bool imported) => SetFlag(trackIdx, 4, imported);
    public bool TrackIsImported(int trackIdx) => GetFlag(trackIdx, 4);
    public void TrackSetEnabled(int trackIdx, bool enabled) => SetFlag(trackIdx, 2, enabled);
    public bool TrackIsEnabled(int trackIdx) => GetFlag(trackIdx, 2);
    public void TrackSetInterpolationLoopWrap(int trackIdx, bool interpolation) => SetFlag(trackIdx, 1, interpolation);
    public bool TrackGetInterpolationLoopWrap(int trackIdx) => GetFlag(trackIdx, 1);

    private bool GetFlag(int track, int bit)
    {
        if (Handle == 0) return false;
        int f = PanimNative.aver_panim_track_flags(Handle, track);
        return f >= 0 && (f & bit) != 0;
    }

    private void SetFlag(int track, int bit, bool on)
    {
        if (Handle == 0) return;
        int f = PanimNative.aver_panim_track_flags(Handle, track);
        if (f < 0) return;
        f = on ? f | bit : f & ~bit;
        PanimNative.aver_panim_track_set_flags(Handle, track, f & 1, (f >> 1) & 1, (f >> 2) & 1);
        Touch();
    }

    public void TrackSetInterpolationType(int trackIdx, InterpolationType interpolation) { if (Handle != 0 && PanimNative.aver_panim_track_set_interp(Handle, trackIdx, (int)interpolation) != 0) Touch(); }

    public InterpolationType TrackGetInterpolationType(int trackIdx) => Handle == 0 ? InterpolationType.Linear : (InterpolationType)PanimNative.aver_panim_track_interp(Handle, trackIdx);

    public void ValueTrackSetUpdateMode(int trackIdx, UpdateMode mode) { if (Handle != 0 && PanimNative.aver_panim_track_set_update(Handle, trackIdx, (int)mode) != 0) Touch(); }

    public UpdateMode ValueTrackGetUpdateMode(int trackIdx) => Handle == 0 ? UpdateMode.Continuous : (UpdateMode)PanimNative.aver_panim_track_update(Handle, trackIdx);

    // ---- keys ----

    public int TrackGetKeyCount(int trackIdx) => Handle == 0 ? 0 : PanimNative.aver_panim_key_count(Handle, trackIdx);

    public double TrackGetKeyTime(int trackIdx, int keyIdx) => Handle == 0 ? 0 : PanimNative.aver_panim_key_time(Handle, trackIdx, keyIdx);

    public float TrackGetKeyTransition(int trackIdx, int keyIdx) => Handle == 0 ? 1f : (float)PanimNative.aver_panim_key_transition(Handle, trackIdx, keyIdx);

    public void TrackSetKeyTransition(int trackIdx, int keyIdx, float transition) { if (Handle != 0 && PanimNative.aver_panim_key_set_transition(Handle, trackIdx, keyIdx, transition) != 0) Touch(); }

    public void TrackSetKeyTime(int trackIdx, int keyIdx, double time) { if (Handle != 0 && PanimNative.aver_panim_key_set_time(Handle, trackIdx, keyIdx, time) >= 0) Touch(); }

    public void TrackRemoveKey(int trackIdx, int keyIdx) { if (Handle != 0 && PanimNative.aver_panim_key_remove(Handle, trackIdx, keyIdx) != 0) Touch(); }

    public void TrackRemoveKeyAtTime(int trackIdx, double time)
    {
        int k = TrackFindKey(trackIdx, time, FindMode.Exact);
        if (k >= 0) TrackRemoveKey(trackIdx, k);
    }

    public int TrackFindKey(int trackIdx, double time, FindMode findMode = FindMode.Nearest, bool limit = false, bool backward = false) =>
        Handle == 0 ? -1 : PanimNative.aver_panim_key_find(Handle, trackIdx, time, findMode == FindMode.Nearest ? 0 : 1, backward ? 1 : 0, limit ? 1 : 0);

    /// <summary>Adds a key. What <paramref name="key"/> is depends on the track: a value for a value track, a Vector3 / Quaternion / float for the
    /// transform and blend-shape tracks, a dictionary { method, args } for a method track, { stream, start_offset, end_offset } for an audio track,
    /// the animation's name for an animation track, and a float (or an array [value, in.x, in.y, out.x, out.y]) for a bezier track.
    /// A key at the same time is replaced. Returns the key's index.</summary>
    public int TrackInsertKey(int trackIdx, double time, Variant key, float transition = 1)
    {
        if (Handle == 0 || trackIdx < 0 || trackIdx >= GetTrackCount()) return -1;
        TrackType type = TrackGetType(trackIdx);
        double[] v5 = new double[5];
        string text = "";
        Collections.Array? args = null;
        double start = 0, end = 0;
        double[]? handles = null;
        switch (type)
        {
            case TrackType.Method:
            {
                Collections.Dictionary d = key.AsGodotDictionary();
                v5[0] = (double)PanimKind.String;
                text = d.TryGetValue("method", out Variant m) ? m.AsString() : "";
                args = d.TryGetValue("args", out Variant a) ? a.AsGodotArray() : null;
                break;
            }
            case TrackType.Audio:
            {
                Collections.Dictionary d = key.AsGodotDictionary();
                v5[0] = (double)PanimKind.String;
                if (d.TryGetValue("stream", out Variant s) && s.AsGodotObject() is Resource r) text = r.ResourcePath;
                start = d.TryGetValue("start_offset", out Variant so) ? so.AsDouble() : 0;
                end = d.TryGetValue("end_offset", out Variant eo) ? eo.AsDouble() : 0;
                break;
            }
            case TrackType.Animation:
                v5[0] = (double)PanimKind.String;
                text = key.AsString();
                break;
            case TrackType.Bezier:
                v5[0] = (double)PanimKind.Float;
                if (key.VariantType == Variant.Type.Array)
                {
                    Collections.Array a = key.AsGodotArray();
                    v5[1] = a.Count > 0 ? a[0].AsDouble() : 0;
                    handles = new double[4];
                    for (int i = 0; i < 4; i++) handles[i] = a.Count > i + 1 ? a[i + 1].AsDouble() : 0;
                }
                else v5[1] = key.AsDouble();
                break;
            case TrackType.BlendShape:
                v5[0] = (double)PanimKind.Float;
                v5[1] = key.AsDouble();
                break;
            case TrackType.Position3D: { Vec3 p = FabricSpace.ToAverPosition(key.AsVector3()); v5[0] = (double)PanimKind.Vec3; v5[1] = p.X; v5[2] = p.Y; v5[3] = p.Z; break; }
            case TrackType.Scale3D: { Vec3 s = FabricSpace.ToAverScale(key.AsVector3()); v5[0] = (double)PanimKind.Vec3; v5[1] = s.X; v5[2] = s.Y; v5[3] = s.Z; break; }
            case TrackType.Rotation3D: { Quat q = FabricSpace.ToAverRotation(key.AsQuaternion()); v5[0] = (double)PanimKind.Quat; v5[1] = q.X; v5[2] = q.Y; v5[3] = q.Z; v5[4] = q.W; break; }
            default:
                AnimValue.Encode(key, v5, out text);
                break;
        }
        int idx = PanimNative.aver_panim_key_insert(Handle, trackIdx, time, v5, text, transition);
        if (idx < 0) return -1;
        if (args != null)
        {
            PanimNative.aver_panim_key_args_clear(Handle, trackIdx, idx);
            for (int i = 0; i < args.Count; i++)
            {
                var a5 = new double[5];
                AnimValue.Encode(args[i], a5, out string at);
                PanimNative.aver_panim_key_arg_set(Handle, trackIdx, idx, i, a5, at);
            }
        }
        if (type == TrackType.Audio) PanimNative.aver_panim_key_set_offsets(Handle, trackIdx, idx, start, end);
        if (handles != null) PanimNative.aver_panim_key_set_handles(Handle, trackIdx, idx, handles[0], handles[1], handles[2], handles[3]);
        Touch();
        return idx;
    }

    public void TrackSetKeyValue(int trackIdx, int keyIdx, Variant value)
    {
        if (Handle == 0 || trackIdx < 0 || trackIdx >= GetTrackCount() || keyIdx < 0 || keyIdx >= TrackGetKeyCount(trackIdx)) return;
        double time = TrackGetKeyTime(trackIdx, keyIdx);
        float transition = TrackGetKeyTransition(trackIdx, keyIdx);
        TrackInsertKey(trackIdx, time, value, transition);
    }

    public Variant TrackGetKeyValue(int trackIdx, int keyIdx)
    {
        if (Handle == 0 || trackIdx < 0 || trackIdx >= GetTrackCount() || keyIdx < 0 || keyIdx >= TrackGetKeyCount(trackIdx)) return default;
        TrackType type = TrackGetType(trackIdx);
        double[] v5 = new double[5];
        PanimNative.aver_panim_key_value(Handle, trackIdx, keyIdx, v5);
        string text = PanimNative.Str(PanimNative.aver_panim_key_text(Handle, trackIdx, keyIdx));
        switch (type)
        {
            case TrackType.Method:
                return new Collections.Dictionary { ["method"] = new StringName(text), ["args"] = MethodTrackGetParams(trackIdx, keyIdx) };
            case TrackType.Audio:
            {
                double[] o = new double[2];
                PanimNative.aver_panim_key_offsets(Handle, trackIdx, keyIdx, o);
                return new Collections.Dictionary { ["stream"] = AudioTrackGetKeyStream(trackIdx, keyIdx), ["start_offset"] = o[0], ["end_offset"] = o[1] };
            }
            case TrackType.Animation: return new StringName(text);
            case TrackType.Bezier:
            {
                double[] h = new double[4];
                PanimNative.aver_panim_key_handles(Handle, trackIdx, keyIdx, h);
                return new Collections.Array { v5[1], h[0], h[1], h[2], h[3] };
            }
            case TrackType.BlendShape: return v5[1];
            case TrackType.Position3D: return FabricSpace.FromAverPosition(new Vec3((float)v5[1], (float)v5[2], (float)v5[3]));
            case TrackType.Scale3D: return FabricSpace.FromAverScale(new Vec3((float)v5[1], (float)v5[2], (float)v5[3]));
            case TrackType.Rotation3D: return FabricSpace.FromAverRotation(new Quat((float)v5[1], (float)v5[2], (float)v5[3], (float)v5[4]));
            default: return AnimValue.Decode(v5, text);
        }
    }

    // ---- sampling ----

    /// <summary>The value of a track at a time: its interpolation, update mode and this animation's loop mode applied.</summary>
    public Variant ValueTrackInterpolate(int trackIdx, double timeSec, bool backward = false) => Sample(trackIdx, timeSec, backward);

    private Variant Sample(int track, double time, bool backward)
    {
        if (Handle == 0) return default;
        double[] v5 = new double[5];
        if (PanimNative.aver_panim_sample(Handle, track, time, backward ? 1 : 0, v5) == 0) return default;
        string text = PanimNative.Str(PanimNative.aver_panim_sample_text());
        switch (TrackGetType(track))
        {
            case TrackType.Position3D: return FabricSpace.FromAverPosition(new Vec3((float)v5[1], (float)v5[2], (float)v5[3]));
            case TrackType.Scale3D: return FabricSpace.FromAverScale(new Vec3((float)v5[1], (float)v5[2], (float)v5[3]));
            case TrackType.Rotation3D: return FabricSpace.FromAverRotation(new Quat((float)v5[1], (float)v5[2], (float)v5[3], (float)v5[4]));
            default: return AnimValue.Decode(v5, text);
        }
    }

    public Vector3 PositionTrackInterpolate(int trackIdx, double timeSec, bool backward = false) => Sample(trackIdx, timeSec, backward).AsVector3();
    public Quaternion RotationTrackInterpolate(int trackIdx, double timeSec, bool backward = false) => Sample(trackIdx, timeSec, backward).AsQuaternion();
    public Vector3 ScaleTrackInterpolate(int trackIdx, double timeSec, bool backward = false) => Sample(trackIdx, timeSec, backward).AsVector3();
    public float BlendShapeTrackInterpolate(int trackIdx, double timeSec, bool backward = false) => Sample(trackIdx, timeSec, backward).AsSingle();
    public float BezierTrackInterpolate(int trackIdx, double timeSec) => Sample(trackIdx, timeSec, false).AsSingle();

    // ---- typed keys ----

    public int PositionTrackInsertKey(int trackIdx, double time, Vector3 position) => TrackInsertKey(trackIdx, time, position);
    public int RotationTrackInsertKey(int trackIdx, double time, Quaternion rotation) => TrackInsertKey(trackIdx, time, rotation);
    public int ScaleTrackInsertKey(int trackIdx, double time, Vector3 scale) => TrackInsertKey(trackIdx, time, scale);
    public int BlendShapeTrackInsertKey(int trackIdx, double time, float amount) => TrackInsertKey(trackIdx, time, amount);

    public int BezierTrackInsertKey(int trackIdx, double time, float value, Vector2 inHandle = default, Vector2 outHandle = default) =>
        TrackInsertKey(trackIdx, time, new Collections.Array { value, inHandle.X, inHandle.Y, outHandle.X, outHandle.Y });

    public void BezierTrackSetKeyValue(int trackIdx, int keyIdx, float value)
    {
        if (Handle == 0) return;
        Vector2 i = BezierTrackGetKeyInHandle(trackIdx, keyIdx), o = BezierTrackGetKeyOutHandle(trackIdx, keyIdx);
        TrackInsertKey(trackIdx, TrackGetKeyTime(trackIdx, keyIdx), new Collections.Array { value, i.X, i.Y, o.X, o.Y });
    }

    public float BezierTrackGetKeyValue(int trackIdx, int keyIdx)
    {
        if (Handle == 0) return 0;
        double[] v5 = new double[5];
        PanimNative.aver_panim_key_value(Handle, trackIdx, keyIdx, v5);
        return (float)v5[1];
    }

    public void BezierTrackSetKeyInHandle(int trackIdx, int keyIdx, Vector2 inHandle, float balancedValueTimeRatio = 1.0f)
    {
        if (Handle == 0) return;
        Vector2 o = BezierTrackGetKeyOutHandle(trackIdx, keyIdx);
        if (PanimNative.aver_panim_key_set_handles(Handle, trackIdx, keyIdx, inHandle.X, inHandle.Y, o.X, o.Y) != 0) Touch();
    }

    public void BezierTrackSetKeyOutHandle(int trackIdx, int keyIdx, Vector2 outHandle, float balancedValueTimeRatio = 1.0f)
    {
        if (Handle == 0) return;
        Vector2 i = BezierTrackGetKeyInHandle(trackIdx, keyIdx);
        if (PanimNative.aver_panim_key_set_handles(Handle, trackIdx, keyIdx, i.X, i.Y, outHandle.X, outHandle.Y) != 0) Touch();
    }

    public Vector2 BezierTrackGetKeyInHandle(int trackIdx, int keyIdx)
    {
        double[] h = new double[4];
        if (Handle == 0 || PanimNative.aver_panim_key_handles(Handle, trackIdx, keyIdx, h) == 0) return default;
        return new Vector2((float)h[0], (float)h[1]);
    }

    public Vector2 BezierTrackGetKeyOutHandle(int trackIdx, int keyIdx)
    {
        double[] h = new double[4];
        if (Handle == 0 || PanimNative.aver_panim_key_handles(Handle, trackIdx, keyIdx, h) == 0) return default;
        return new Vector2((float)h[2], (float)h[3]);
    }

    public StringName MethodTrackGetName(int trackIdx, int keyIdx) =>
        Handle == 0 ? StringName.Empty : new StringName(PanimNative.Str(PanimNative.aver_panim_key_text(Handle, trackIdx, keyIdx)));

    /// <summary>The arguments a method key calls with.</summary>
    public Collections.Array MethodTrackGetParams(int trackIdx, int keyIdx)
    {
        var result = new Collections.Array();
        if (Handle == 0) return result;
        int n = PanimNative.aver_panim_key_arg_count(Handle, trackIdx, keyIdx);
        for (int i = 0; i < n; i++)
        {
            double[] v5 = new double[5];
            PanimNative.aver_panim_key_arg_get(Handle, trackIdx, keyIdx, i, v5);
            result.Add(AnimValue.Decode(v5, PanimNative.Str(PanimNative.aver_panim_key_arg_text(Handle, trackIdx, keyIdx, i))));
        }
        return result;
    }

    public int AudioTrackInsertKey(int trackIdx, double time, Resource stream, float startOffset = 0, float endOffset = 0) =>
        TrackInsertKey(trackIdx, time, new Collections.Dictionary { ["stream"] = stream, ["start_offset"] = startOffset, ["end_offset"] = endOffset });

    public Resource? AudioTrackGetKeyStream(int trackIdx, int keyIdx)
    {
        if (Handle == 0) return null;
        string path = PanimNative.Str(PanimNative.aver_panim_key_text(Handle, trackIdx, keyIdx));
        return path.Length == 0 ? null : ResourceLoader.Load(path);
    }

    public void AudioTrackSetKeyStream(int trackIdx, int keyIdx, Resource stream)
    {
        if (Handle == 0) return;
        double[] v5 = { (double)PanimKind.String, 0, 0, 0, 0 };
        if (PanimNative.aver_panim_key_set_value(Handle, trackIdx, keyIdx, v5, stream?.ResourcePath ?? "") != 0) Touch();
    }

    public float AudioTrackGetKeyStartOffset(int trackIdx, int keyIdx) => AudioOffset(trackIdx, keyIdx, 0);
    public float AudioTrackGetKeyEndOffset(int trackIdx, int keyIdx) => AudioOffset(trackIdx, keyIdx, 1);
    public void AudioTrackSetKeyStartOffset(int trackIdx, int keyIdx, float offset) => SetAudioOffset(trackIdx, keyIdx, 0, offset);
    public void AudioTrackSetKeyEndOffset(int trackIdx, int keyIdx, float offset) => SetAudioOffset(trackIdx, keyIdx, 1, offset);

    private float AudioOffset(int track, int key, int which)
    {
        double[] o = new double[2];
        return Handle != 0 && PanimNative.aver_panim_key_offsets(Handle, track, key, o) != 0 ? (float)o[which] : 0f;
    }

    private void SetAudioOffset(int track, int key, int which, float value)
    {
        double[] o = new double[2];
        if (Handle == 0 || PanimNative.aver_panim_key_offsets(Handle, track, key, o) == 0) return;
        o[which] = value;
        PanimNative.aver_panim_key_set_offsets(Handle, track, key, o[0], o[1]);
        Touch();
    }

    public int AnimationTrackInsertKey(int trackIdx, double time, StringName animation) => TrackInsertKey(trackIdx, time, animation);

    public StringName AnimationTrackGetKeyAnimation(int trackIdx, int keyIdx) =>
        Handle == 0 ? StringName.Empty : new StringName(PanimNative.Str(PanimNative.aver_panim_key_text(Handle, trackIdx, keyIdx)));

    public void AnimationTrackSetKeyAnimation(int trackIdx, int keyIdx, StringName animation)
    {
        if (Handle == 0) return;
        double[] v5 = { (double)PanimKind.String, 0, 0, 0, 0 };
        if (PanimNative.aver_panim_key_set_value(Handle, trackIdx, keyIdx, v5, animation.ToString()) != 0) Touch();
    }
}

/// <summary>Variant to and from the library's value form.</summary>
internal static class AnimValue
{
    private const char TokenMark = '\u0001';
    private static readonly List<Variant> s_opaque = new();

    // A Variant with no numeric or text form (an object, an array) is kept here and the clip holds a token. Such a value is lost with the process,
    // which is right: an .ocanim written by the converter holds real Variant text instead.
    private static string Token(Variant v)
    {
        lock (s_opaque) { s_opaque.Add(v); return TokenMark + (s_opaque.Count - 1).ToString(System.Globalization.CultureInfo.InvariantCulture); }
    }

    internal static void Encode(Variant v, double[] v5, out string text)
    {
        text = "";
        System.Array.Clear(v5);
        switch (v.VariantType)
        {
            case Variant.Type.Nil: break;
            case Variant.Type.Bool: v5[0] = (double)PanimKind.Bool; v5[1] = v.AsBool() ? 1 : 0; break;
            case Variant.Type.Int: v5[0] = (double)PanimKind.Int; v5[1] = v.AsInt64(); break;
            case Variant.Type.Float: v5[0] = (double)PanimKind.Float; v5[1] = v.AsDouble(); break;
            case Variant.Type.Vector2: { Vector2 a = v.AsVector2(); v5[0] = (double)PanimKind.Vec2; v5[1] = a.X; v5[2] = a.Y; break; }
            case Variant.Type.Vector3: { Vector3 a = v.AsVector3(); v5[0] = (double)PanimKind.Vec3; v5[1] = a.X; v5[2] = a.Y; v5[3] = a.Z; break; }
            case Variant.Type.Vector4: { Vector4 a = v.AsVector4(); v5[0] = (double)PanimKind.Vec4; v5[1] = a.X; v5[2] = a.Y; v5[3] = a.Z; v5[4] = a.W; break; }
            case Variant.Type.Quaternion: { Quaternion a = v.AsQuaternion(); v5[0] = (double)PanimKind.Quat; v5[1] = a.X; v5[2] = a.Y; v5[3] = a.Z; v5[4] = a.W; break; }
            case Variant.Type.Color: { Color a = v.AsColor(); v5[0] = (double)PanimKind.Color; v5[1] = a.R; v5[2] = a.G; v5[3] = a.B; v5[4] = a.A; break; }
            case Variant.Type.String:
            case Variant.Type.StringName: v5[0] = (double)PanimKind.String; text = v.AsString(); break;
            case Variant.Type.NodePath: v5[0] = (double)PanimKind.Text; text = "^\"" + v.AsNodePath() + "\""; break;
            default: v5[0] = (double)PanimKind.Text; text = Token(v); break;
        }
    }

    internal static Variant Decode(double[] v5, string text)
    {
        switch ((PanimKind)(int)v5[0])
        {
            case PanimKind.Bool: return v5[1] != 0;
            case PanimKind.Int: return (long)v5[1];
            case PanimKind.Float: return v5[1];
            case PanimKind.Vec2: return new Vector2((float)v5[1], (float)v5[2]);
            case PanimKind.Vec3: return new Vector3((float)v5[1], (float)v5[2], (float)v5[3]);
            case PanimKind.Vec4: return new Vector4((float)v5[1], (float)v5[2], (float)v5[3], (float)v5[4]);
            case PanimKind.Quat: return new Quaternion((float)v5[1], (float)v5[2], (float)v5[3], (float)v5[4]);
            case PanimKind.Color: return new Color((float)v5[1], (float)v5[2], (float)v5[3], (float)v5[4]);
            case PanimKind.String: return text;
            case PanimKind.Text:
                if (text.Length > 0 && text[0] == TokenMark)
                {
                    if (int.TryParse(text.AsSpan(1), System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out int i))
                        lock (s_opaque) { if (i >= 0 && i < s_opaque.Count) return s_opaque[i]; }
                    return default;
                }
                return VariantText.TryParse(text, out Variant parsed) ? parsed : default;
            default: return default;
        }
    }
}

/// <summary>A named set of animations (Godot's AnimationLibrary). An AnimationPlayer holds libraries; its animations are called "library/name",
/// or just "name" for the library with the empty name.</summary>
public class AnimationLibrary : Resource
{
    public new class SignalName : Resource.SignalName
    {
        public static readonly StringName AnimationAdded = "animation_added";
        public static readonly StringName AnimationRemoved = "animation_removed";
        public static readonly StringName AnimationRenamed = "animation_renamed";
        public static readonly StringName AnimationChanged = "animation_changed";
    }

    private readonly List<KeyValuePair<StringName, Animation>> _animations = new();

    /// <summary>Bumped by every change of the set (and of its animations when the owner asks), so a player knows to refresh its copy.</summary>
    internal int Version { get; private set; }

    internal int ContentVersion
    {
        get
        {
            int v = Version;
            foreach (var kv in _animations) v += kv.Value.Version;
            return v;
        }
    }

    public Error AddAnimation(StringName name, Animation animation)
    {
        if (animation == null) return Error.InvalidParameter;
        string n = name.ToString();
        if (n.Length > 0 && (n.Contains('/') || n.Contains(',') || n.Contains('[') || n.Contains(']'))) return Error.InvalidParameter;
        foreach (var kv in _animations) if (kv.Key == name) return Error.AlreadyExists;
        _animations.Add(new KeyValuePair<StringName, Animation>(name, animation));
        Version++;
        EmitSignal(SignalName.AnimationAdded, name);
        EmitChanged();
        return Error.Ok;
    }

    public void RemoveAnimation(StringName name)
    {
        int i = _animations.FindIndex(kv => kv.Key == name);
        if (i < 0) return;
        _animations.RemoveAt(i);
        Version++;
        EmitSignal(SignalName.AnimationRemoved, name);
        EmitChanged();
    }

    public void RenameAnimation(StringName name, StringName newname)
    {
        int i = _animations.FindIndex(kv => kv.Key == name);
        if (i < 0 || _animations.Any(kv => kv.Key == newname)) return;
        _animations[i] = new KeyValuePair<StringName, Animation>(newname, _animations[i].Value);
        Version++;
        EmitSignal(SignalName.AnimationRenamed, name, newname);
        EmitChanged();
    }

    public bool HasAnimation(StringName name) => _animations.Any(kv => kv.Key == name);

    public Animation? GetAnimation(StringName name)
    {
        foreach (var kv in _animations) if (kv.Key == name) return kv.Value;
        return null;
    }

    /// <summary>The animation names, in the order they were added.</summary>
    public StringName[] GetAnimationList() => _animations.Select(kv => kv.Key).ToArray();

    public int GetAnimationListSize() => _animations.Count;

    internal IEnumerable<KeyValuePair<StringName, Animation>> Entries => _animations;
}
