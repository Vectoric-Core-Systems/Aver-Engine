// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// G5 A1: Animation, AnimationLibrary and AnimationPlayer against the engine's property-track player (Aver.Anim.Abi). The sampling, loop and blend
// rules themselves are PropAnimTest's (C++); what is checked here is the Godot surface on top: the API names and their results, the order of
// signals and method calls, properties written to nodes in Godot's units and axes, libraries, scene values and the converter's own output.
// The suite needs the Aver.Anim.Abi library from build/bin; without it every test is skipped (and says so).

using Aver.Fabric;
using Collections = Aver.Fabric.Collections;

/// <summary>A node an animation drives: properties of several types and methods that record their calls.</summary>
class AnimTarget : Node3D
{
    public static readonly List<string> Calls = new();
    public float Energy;
    public Color Tint = new(1, 1, 1, 1);
    public bool Flag = true;
    public int Count;
    public string Label = "";

    public void Hit(int dmg, string tag) => Calls.Add($"hit({dmg},{tag})");
    public void Done() => Calls.Add("done");
    public void Tick(int n) => Calls.Add("tick" + n);
}

/// <summary>An audio target that knows animations.</summary>
class AudioProbe : Node, IAnimationAudioTarget
{
    public readonly List<string> Log = new();
    public void AnimationPlayStream(string streamPath, double startOffset, double endOffset) => Log.Add($"play {streamPath} {startOffset:0.###} {endOffset:0.###}");
    public void AnimationStopStream() => Log.Add("stop");
}

/// <summary>An audio-like node that does not: it has a stream and a Play(double).</summary>
class PlainAudio : Node
{
    public Resource? Stream;
    public double From = -1;
    public bool Stopped;
    public void Play(double fromPosition = 0.0) => From = fromPosition;
    public void Stop() => Stopped = true;
}

static class AnimationTests
{
    private static bool s_native;

    public static int RunAll()
    {
        s_native = EnsureNative();
        if (!s_native)
        {
            Console.WriteLine("Test: animation (SKIP: build/bin/Aver.Anim.Abi was not found; build the Aver.Anim.Abi target)");
            return 0;
        }
        return T.Suite(() =>
        {
            T.Run("Animation: tracks, keys, sampling and every typed key through Godot's API", ApiAnimation);
            T.Run("Animation: transform tracks are stored in engine space and read back in Godot's", TransformSpace);
            T.Run("AnimationLibrary and AnimationMixer: names, lists, signals, refusals", Libraries);
            T.Run("AnimationPlayer: tracks write Godot-space values to node properties, per type", DrivesProperties);
            T.Run("AnimationPlayer: method tracks call in order across frames, skipping keys, with arguments, immediate or deferred", MethodTracks);
            T.Run("AnimationPlayer: signals come in Godot's order, with queue and chaining", SignalOrder);
            T.Run("AnimationPlayer: crossfade weights, blend time table, default and custom blend", Crossfade);
            T.Run("AnimationPlayer: loop, ping-pong, speed_scale, backwards, active, manual advance", LoopsAndSpeed);
            T.Run("AnimationPlayer: seek, pause, resume, stop, autoplay", SeekPauseStop);
            T.Run("AnimationPlayer: audio and animation (sub-player) tracks", AudioAndSubPlayer);
            T.Run("AnimationPlayer: capture mode fades from the captured value", CaptureMode);
            T.Run("AnimationPlayer: scene values (libraries as .ocanim paths, autoplay, blend_times) and ResourceLoader", SceneValues);
            T.Run("AnimationPlayer: the converter's output plays (AverAssetC on the AnimGame fixture)", ConverterOutput);
        });
    }

    // ---- the native library ----

    private static string LibFile() =>
        OperatingSystem.IsWindows() ? "Aver.Anim.Abi.dll" : OperatingSystem.IsMacOS() ? "libAver.Anim.Abi.dylib" : "libAver.Anim.Abi.so";

    private static string? FindBin(string file)
    {
        for (DirectoryInfo? d = new DirectoryInfo(AppContext.BaseDirectory); d != null; d = d.Parent)
        {
            string bin = Path.Combine(d.FullName, "build", "bin");
            if (File.Exists(Path.Combine(bin, file))) return bin;
        }
        return null;
    }

    internal static bool EnsureNative()
    {
        string? bin = FindBin(LibFile());
        if (bin != null)
        {
            // the library goes beside this assembly, where the default DllImport probing looks
            string beside = Path.Combine(AppContext.BaseDirectory, LibFile());
            try { if (!File.Exists(beside) || File.GetLastWriteTimeUtc(beside) < File.GetLastWriteTimeUtc(Path.Combine(bin, LibFile()))) File.Copy(Path.Combine(bin, LibFile()), beside, true); }
            catch (IOException) { /* a copy from an earlier run is in use */ }
            PanimNative.Reprobe();
        }
        return PanimNative.Available;
    }

    // ---- builders ----

    private static Animation Anim(float length, Animation.LoopModeEnum loop = Animation.LoopModeEnum.None) => new() { Length = length, LoopMode = loop };

    private static int Track(Animation a, Animation.TrackType type, string path, Animation.InterpolationType interp = Animation.InterpolationType.Linear)
    {
        int t = a.AddTrack(type);
        a.TrackSetPath(t, path);
        a.TrackSetInterpolationType(t, interp);
        return t;
    }

    // Root / Player, Target, Box: the player's root_node is its parent.
    private static Node3D Scene(out AnimationPlayer player, out AnimTarget target, out Node3D box, params (string Name, Animation Anim)[] clips)
    {
        var root = new Node3D { Name = "Root" };
        player = new AnimationPlayer { Name = "Player" };
        target = new AnimTarget { Name = "Target" };
        box = new Node3D { Name = "Box" };
        var lib = new AnimationLibrary();
        foreach (var (name, anim) in clips) lib.AddAnimation(name, anim);
        player.AddAnimationLibrary("", lib);
        player.CallbackModeMethod = AnimationMixer.AnimationCallbackModeMethod.Immediate;
        root.AddChild(player);
        root.AddChild(target);
        root.AddChild(box);
        SceneTree.Instance.Root.AddChild(root);
        AnimTarget.Calls.Clear();
        return root;
    }

    private static Animation EnergyRamp(float length, float to, Animation.LoopModeEnum loop = Animation.LoopModeEnum.None)
    {
        Animation a = Anim(length, loop);
        int t = Track(a, Animation.TrackType.Value, "Target:energy");
        a.TrackInsertKey(t, 0, 0f);
        a.TrackInsertKey(t, length, to);
        return a;
    }

    private static Animation EnergyConst(float length, float value)
    {
        Animation a = Anim(length);
        int t = Track(a, Animation.TrackType.Value, "Target:energy");
        a.TrackInsertKey(t, 0, value);
        a.TrackInsertKey(t, length, value);
        return a;
    }

    private static Animation MethodClip(float length, params (double Time, string Name, Variant[] Args)[] calls)
    {
        Animation a = Anim(length);
        int t = Track(a, Animation.TrackType.Method, "Target");
        foreach (var (time, name, args) in calls)
            a.TrackInsertKey(t, time, new Collections.Dictionary { ["method"] = new StringName(name), ["args"] = new Collections.Array(args) });
        return a;
    }

    // ---- tests ----

    private static void ApiAnimation()
    {
        var a = new Animation { Length = 2f };
        T.CheckNum("length", 2f, a.Length);
        T.Check("a new animation has no tracks and no loop", a.GetTrackCount() == 0 && a.LoopMode == Animation.LoopModeEnum.None);
        a.LoopMode = Animation.LoopModeEnum.Linear;
        a.Step = 0.1f;
        T.Check("loop mode and step", a.LoopMode == Animation.LoopModeEnum.Linear && T.Near(a.Step, 0.1f));

        int v = Track(a, Animation.TrackType.Value, "Target:energy");
        T.Check("add_track returns the index and the type is kept", v == 0 && a.TrackGetType(v) == Animation.TrackType.Value);
        T.Check("track_get_path", a.TrackGetPath(v) == new NodePath("Target:energy"));
        T.Check("find_track", a.FindTrack("Target:energy", Animation.TrackType.Value) == 0 && a.FindTrack("Target:energy", Animation.TrackType.Method) == -1);
        a.TrackInsertKey(v, 0, 0f);
        a.TrackInsertKey(v, 2, 0f);
        T.Check("keys are inserted in time order and a key at the same time replaces", a.TrackInsertKey(v, 1, 10f) == 1 && a.TrackInsertKey(v, 1, 20f) == 1 && a.TrackGetKeyCount(v) == 3);
        T.CheckNum("track_get_key_value", 20f, a.TrackGetKeyValue(v, 1).AsSingle());
        T.Check("track_get_key_time", Math.Abs(a.TrackGetKeyTime(v, 2) - 2.0) < 1e-9);
        T.CheckNum("value_track_interpolate halfway up", 10f, a.ValueTrackInterpolate(v, 0.5).AsSingle());
        T.CheckNum("and halfway down", 10f, a.ValueTrackInterpolate(v, 1.5).AsSingle());
        a.TrackSetInterpolationType(v, Animation.InterpolationType.Nearest);
        T.CheckNum("nearest keeps the earlier key", 0f, a.ValueTrackInterpolate(v, 0.9).AsSingle());
        a.TrackSetInterpolationType(v, Animation.InterpolationType.Linear);
        a.TrackSetKeyTransition(v, 0, 2f);
        T.CheckNum("a key's transition eases its segment (ease(0.5, 2) = 0.25)", 5f, a.ValueTrackInterpolate(v, 0.5).AsSingle());
        T.CheckNum("track_get_key_transition", 2f, a.TrackGetKeyTransition(v, 0));
        a.TrackSetKeyValue(v, 0, 4f);
        T.CheckNum("track_set_key_value", 4f, a.TrackGetKeyValue(v, 0).AsSingle());
        a.TrackSetKeyTime(v, 2, 1.5);
        T.Check("track_set_key_time keeps the keys sorted", Math.Abs(a.TrackGetKeyTime(v, 2) - 1.5) < 1e-9);
        a.TrackRemoveKey(v, 2);
        a.TrackRemoveKeyAtTime(v, 1.0);
        T.Check("track_remove_key and _at_time", a.TrackGetKeyCount(v) == 1);
        T.Check("track_find_key exact and nearest", a.TrackFindKey(v, 0.0, Animation.FindMode.Exact) == 0 && a.TrackFindKey(v, 0.7, Animation.FindMode.Exact) == -1 && a.TrackFindKey(v, 0.7) == 0);

        T.Check("track flags default to enabled, loop-wrapped and not imported", a.TrackIsEnabled(v) && a.TrackGetInterpolationLoopWrap(v) && !a.TrackIsImported(v));
        a.TrackSetEnabled(v, false); a.TrackSetImported(v, true); a.TrackSetInterpolationLoopWrap(v, false);
        T.Check("and can be changed", !a.TrackIsEnabled(v) && a.TrackIsImported(v) && !a.TrackGetInterpolationLoopWrap(v));
        T.Check("update mode, and capture_included follows it", a.ValueTrackGetUpdateMode(v) == Animation.UpdateMode.Continuous && !a.CaptureIncluded);
        a.ValueTrackSetUpdateMode(v, Animation.UpdateMode.Capture);
        T.Check("capture mode", a.ValueTrackGetUpdateMode(v) == Animation.UpdateMode.Capture && a.CaptureIncluded);

        // typed tracks
        int m = a.AddTrack(Animation.TrackType.Method);
        a.TrackSetPath(m, ".");
        a.TrackInsertKey(m, 0.5, new Collections.Dictionary { ["method"] = new StringName("hit"), ["args"] = new Collections.Array { 3, "x" } });
        T.Check("method key: name and arguments", a.MethodTrackGetName(m, 0) == "hit" && a.MethodTrackGetParams(m, 0).Count == 2 && a.MethodTrackGetParams(m, 0)[0].AsInt32() == 3 && a.MethodTrackGetParams(m, 0)[1].AsString() == "x");
        Collections.Dictionary mk = a.TrackGetKeyValue(m, 0).AsGodotDictionary();
        T.Check("method key value is { method, args }", mk["method"].AsString() == "hit" && mk["args"].AsGodotArray().Count == 2);

        int bz = a.AddTrack(Animation.TrackType.Bezier);
        a.TrackSetPath(bz, "Target:energy");
        a.BezierTrackInsertKey(bz, 0, 0f, new Vector2(0, 0), new Vector2(0.25f, 0));
        a.BezierTrackInsertKey(bz, 1, 10f, new Vector2(-0.25f, 0), new Vector2(0, 0));
        T.CheckNum("bezier key value", 10f, a.BezierTrackGetKeyValue(bz, 1));
        T.Check("bezier handles", a.BezierTrackGetKeyOutHandle(bz, 0) == new Vector2(0.25f, 0) && a.BezierTrackGetKeyInHandle(bz, 1) == new Vector2(-0.25f, 0));
        T.CheckNum("bezier with symmetric flat handles passes through the middle", 5f, a.BezierTrackInterpolate(bz, 0.5), 1e-2f);
        a.BezierTrackSetKeyOutHandle(bz, 0, new Vector2(0.5f, 1f));
        T.Check("bezier set_key_out_handle", a.BezierTrackGetKeyOutHandle(bz, 0) == new Vector2(0.5f, 1f));

        int au = a.AddTrack(Animation.TrackType.Audio);
        a.TrackSetPath(au, "Snd");
        a.AudioTrackInsertKey(au, 0.5, new Resource { ResourcePath = "res://hit.wav" }, 0.25f, 0.5f);
        T.Check("audio key: stream path, offsets", a.AudioTrackGetKeyStream(au, 0)?.ResourcePath == "res://hit.wav" && T.Near(a.AudioTrackGetKeyStartOffset(au, 0), 0.25f) && T.Near(a.AudioTrackGetKeyEndOffset(au, 0), 0.5f));
        a.AudioTrackSetKeyStartOffset(au, 0, 0.75f);
        T.CheckNum("audio set_key_start_offset", 0.75f, a.AudioTrackGetKeyStartOffset(au, 0));

        int an = a.AddTrack(Animation.TrackType.Animation);
        a.TrackSetPath(an, "Sub");
        a.AnimationTrackInsertKey(an, 1.0, "run");
        T.Check("animation key", a.AnimationTrackGetKeyAnimation(an, 0) == "run");
        a.AnimationTrackSetKeyAnimation(an, 0, "walk");
        T.Check("animation set_key_animation", a.AnimationTrackGetKeyAnimation(an, 0) == "walk");

        int bs = a.AddTrack(Animation.TrackType.BlendShape);
        a.TrackSetPath(bs, "Box:smile");
        a.BlendShapeTrackInsertKey(bs, 0, 0f);
        a.BlendShapeTrackInsertKey(bs, 1, 1f);
        T.CheckNum("blend shape interpolation", 0.25f, a.BlendShapeTrackInterpolate(bs, 0.25));

        // an object value (an array) travels as a Variant
        int ov = Track(a, Animation.TrackType.Value, "Target:items");
        a.TrackInsertKey(ov, 0, new Collections.Array { 1, 2, 3 });
        T.Check("an array value comes back as an array", a.TrackGetKeyValue(ov, 0).AsGodotArray().Count == 3);

        // reordering
        int count = a.GetTrackCount();
        a.TrackMoveTo(m, 0);
        T.Check("track_move_to", a.TrackGetType(0) == Animation.TrackType.Method && a.GetTrackCount() == count);
        a.TrackSwap(0, 1);
        T.Check("track_swap", a.TrackGetType(1) == Animation.TrackType.Method && a.TrackGetType(0) == Animation.TrackType.Value);
        a.RemoveTrack(0);
        T.Check("remove_track", a.GetTrackCount() == count - 1);

        // a duplicate is its own animation
        Animation copy = (Animation)a.Duplicate();
        a.Clear();
        T.Check("clear empties the original and leaves the duplicate alone", a.GetTrackCount() == 0 && copy.GetTrackCount() == count - 1 && T.Near(copy.Length, 2f));
    }

    private static void TransformSpace()
    {
        var a = Anim(1f);
        int p = Track(a, Animation.TrackType.Position3D, "Box");
        a.PositionTrackInsertKey(p, 0, Vector3.Zero);
        a.PositionTrackInsertKey(p, 1, new Vector3(1, 2, 3));
        T.CheckVec("a position key reads back in metres and Godot's axes", new Vector3(1, 2, 3), a.TrackGetKeyValue(p, 1).AsVector3());
        T.CheckVec("position interpolates", new Vector3(0.5f, 1, 1.5f), a.PositionTrackInterpolate(p, 0.5));
        T.CheckVec("TrackGetKeyValue is the same as position_track", new Vector3(1, 2, 3), a.TrackGetKeyValue(p, 1).AsVector3());

        int r = Track(a, Animation.TrackType.Rotation3D, "Box");
        var q = new Quaternion(new Vector3(0, 1, 0), Mathf.Pi / 2);
        a.RotationTrackInsertKey(r, 0, Quaternion.Identity);
        a.RotationTrackInsertKey(r, 1, q);
        T.Check("a rotation key reads back", T.Near(a.TrackGetKeyValue(r, 1).AsQuaternion(), q));
        T.Check("rotation slerps: half of 90 degrees about Y", T.Near(a.RotationTrackInterpolate(r, 0.5), new Quaternion(new Vector3(0, 1, 0), Mathf.Pi / 4)));

        int s = Track(a, Animation.TrackType.Scale3D, "Box");
        a.ScaleTrackInsertKey(s, 0, Vector3.One);
        a.ScaleTrackInsertKey(s, 1, new Vector3(2, 3, 4));
        T.CheckVec("scale reads back", new Vector3(2, 3, 4), a.TrackGetKeyValue(s, 1).AsVector3());
        T.CheckVec("scale interpolates", new Vector3(1.5f, 2f, 2.5f), a.ScaleTrackInterpolate(s, 0.5));
    }

    private static void Libraries()
    {
        var lib = new AnimationLibrary();
        var events = new List<string>();
        lib.Connect(AnimationLibrary.SignalName.AnimationAdded, Callable.From<StringName>(n => events.Add("added " + n)));
        lib.Connect(AnimationLibrary.SignalName.AnimationRemoved, Callable.From<StringName>(n => events.Add("removed " + n)));
        lib.Connect(AnimationLibrary.SignalName.AnimationRenamed, Callable.From<StringName, StringName>((a, b) => events.Add("renamed " + a + ">" + b)));
        var idle = Anim(1f); var walk = Anim(2f); var spin = Anim(3f);
        T.Check("add_animation", lib.AddAnimation("idle", idle) == Error.Ok && lib.AddAnimation("walk", walk) == Error.Ok);
        T.Check("a taken name is refused", lib.AddAnimation("idle", spin) == Error.AlreadyExists);
        T.Check("a name with a slash is refused", lib.AddAnimation("a/b", spin) == Error.InvalidParameter);
        T.Check("has_animation, get_animation, the list and its size", lib.HasAnimation("idle") && lib.GetAnimation("walk") == walk && lib.GetAnimationListSize() == 2 && lib.GetAnimationList()[0] == "idle");
        lib.RenameAnimation("walk", "run");
        lib.RemoveAnimation("idle");
        T.CheckSeq("signals", new[] { "added idle", "added walk", "renamed walk>run", "removed idle" }, events);

        var p = new AnimationPlayer { Name = "P" };
        var other = new AnimationLibrary();
        other.AddAnimation("spin", spin);
        var lists = new List<string>();
        p.AnimationListChanged += () => lists.Add("list");
        p.AnimationLibrariesUpdated += () => lists.Add("libs");
        T.Check("add_animation_library", p.AddAnimationLibrary("", lib) == Error.Ok && p.AddAnimationLibrary("extra", other) == Error.Ok);
        T.CheckSeq("libraries updated then list changed, each time", new[] { "libs", "list", "libs", "list" }, lists);
        T.Check("a taken library name is refused", p.AddAnimationLibrary("extra", new AnimationLibrary()) == Error.AlreadyExists);
        T.Check("and so is a name with a slash", p.AddAnimationLibrary("a/b", new AnimationLibrary()) == Error.InvalidParameter);
        T.CheckSeq("get_animation_list: the default library bare, others as library/name", new[] { "run", "extra/spin" }, p.GetAnimationList());
        T.Check("has_animation and get_animation by either spelling", p.HasAnimation("run") && p.HasAnimation("extra/spin") && p.GetAnimation("extra/spin") == spin && !p.HasAnimation("spin"));
        T.Check("find_animation and find_animation_library", p.FindAnimation(spin) == "extra/spin" && p.FindAnimationLibrary(spin) == "extra" && p.FindAnimation(new Animation()).IsEmpty);
        T.Check("library list and has_animation_library", p.GetAnimationLibraryList().Length == 2 && p.HasAnimationLibrary("extra") && p.GetAnimationLibrary("extra") == other);
        p.RenameAnimationLibrary("extra", "more");
        T.Check("rename_animation_library", p.HasAnimation("more/spin") && !p.HasAnimation("extra/spin"));
        p.RemoveAnimationLibrary("more");
        T.Check("remove_animation_library", !p.HasAnimation("more/spin") && p.GetAnimationLibraryList().Length == 1);
        // Playing something that is not there is an error, not a crash.
        p.Play("nope");
        T.Check("play of an unknown animation logs an error and plays nothing", T.ErrorCount("no animation 'nope'") == 1 && !p.IsPlaying());
    }

    private static void DrivesProperties()
    {
        Animation a = Anim(2f);
        int energy = Track(a, Animation.TrackType.Value, "Target:energy");
        a.TrackInsertKey(energy, 0, 0f); a.TrackInsertKey(energy, 2, 10f);
        int pos = Track(a, Animation.TrackType.Position3D, "Box");
        a.PositionTrackInsertKey(pos, 0, Vector3.Zero); a.PositionTrackInsertKey(pos, 2, new Vector3(2, 4, 6));
        int rot = Track(a, Animation.TrackType.Rotation3D, "Box");
        a.RotationTrackInsertKey(rot, 0, Quaternion.Identity); a.RotationTrackInsertKey(rot, 2, new Quaternion(new Vector3(0, 1, 0), Mathf.Pi / 2));
        int scl = Track(a, Animation.TrackType.Scale3D, "Box");
        a.ScaleTrackInsertKey(scl, 0, Vector3.One); a.ScaleTrackInsertKey(scl, 2, new Vector3(2, 2, 2));
        int tint = Track(a, Animation.TrackType.Value, "Target:tint");
        a.TrackInsertKey(tint, 0, new Color(0, 0, 0, 1)); a.TrackInsertKey(tint, 2, new Color(1, 0, 0, 1));
        int count = Track(a, Animation.TrackType.Value, "Target:count");
        a.TrackInsertKey(count, 0, 0); a.TrackInsertKey(count, 2, 10);
        int flag = Track(a, Animation.TrackType.Value, "Target:flag");
        a.ValueTrackSetUpdateMode(flag, Animation.UpdateMode.Discrete);
        a.TrackInsertKey(flag, 0, true); a.TrackInsertKey(flag, 1, false);
        int label = Track(a, Animation.TrackType.Value, "Target:label");
        a.TrackInsertKey(label, 0, "a"); a.TrackInsertKey(label, 1, "b");
        int comp = Track(a, Animation.TrackType.Value, "Box:position:x");   // a sub-property of a value track
        int bezier = Track(a, Animation.TrackType.Bezier, "Target:energy");

        Scene(out AnimationPlayer p, out AnimTarget target, out Node3D box, ("idle", a));
        a.TrackSetEnabled(bezier, false);   // keep the bezier from fighting the energy value track
        a.TrackSetEnabled(comp, false);
        p.Play("idle");
        T.Check("play() starts it", p.IsPlaying() && p.CurrentAnimation == "idle");
        T.Frame(0.5);
        T.CheckNum("the first frame does not move the playhead: energy at 0", 0f, target.Energy);
        T.Check("the discrete bool is written at the start", target.Flag && target.Label == "a");
        T.Frame(0.5);
        T.CheckNum("energy at 0.5 s (value track, float)", 2.5f, target.Energy);
        T.CheckVec("position at 0.5 s is in metres and Godot's axes", new Vector3(0.5f, 1, 1.5f), box.Position);
        T.Check("rotation at 0.5 s is a quarter of the way to 90 degrees about Y", T.Near(box.Quaternion, new Quaternion(new Vector3(0, 1, 0), Mathf.Pi / 8)));
        T.CheckVec("scale at 0.5 s", new Vector3(1.25f, 1.25f, 1.25f), box.Scale);
        T.CheckNum("a colour interpolates per channel: red at 0.5 s", 0.25f, target.Tint.R);
        T.Check("an int track interpolates and truncates: 2 at 0.5 s of 0..10", target.Count == 2);
        T.Check("a discrete track has not reached its key yet", target.Flag);
        T.Frame(0.5);
        T.Check("discrete: written when the playhead reaches the key at 1 s", !target.Flag);
        T.Check("a string track steps to the key it reaches", target.Label == "b");
        T.Frame(1.0);
        T.CheckNum("the end applies the last pose in full", 10f, target.Energy);
        T.CheckVec("and the transform", new Vector3(2, 4, 6), box.Position);
        T.Check("and the clip is finished", !p.IsPlaying());
    }

    private static void MethodTracks()
    {
        var clip = MethodClip(1f,
            (0.1, "Tick", new Variant[] { 1 }), (0.2, "Tick", new Variant[] { 2 }), (0.3, "Tick", new Variant[] { 3 }),
            (0.6, "Hit", new Variant[] { 7, "crit" }), (0.9, "Done", System.Array.Empty<Variant>()));
        Scene(out AnimationPlayer p, out AnimTarget target, out _, ("a", clip));
        p.Play("a");
        T.Frame(0.25);   // the first frame stays at 0
        T.Check("nothing yet", AnimTarget.Calls.Count == 0);
        T.Frame(0.5);
        T.CheckSeq("a frame that skips three keys calls all three, in time order", new[] { "tick1", "tick2", "tick3" }, AnimTarget.Calls);
        AnimTarget.Calls.Clear();
        T.Frame(0.25);
        T.CheckSeq("the next frame passes 0.6 only: the arguments arrive converted to the parameters' types", new[] { "hit(7,crit)" }, AnimTarget.Calls);
        AnimTarget.Calls.Clear();
        T.Frame(0.5);
        T.CheckSeq("the last key, then the clip ends", new[] { "done" }, AnimTarget.Calls);

        // Backwards: descending order.
        var back = MethodClip(1f, (0.2, "Tick", new Variant[] { 2 }), (0.4, "Tick", new Variant[] { 4 }), (0.6, "Tick", new Variant[] { 6 }));
        Scene(out AnimationPlayer q, out _, out _, ("b", back));
        q.PlayBackwards("b");
        T.Frame(0.25);
        T.Frame(0.9);
        T.CheckSeq("playing backwards calls the keys in descending order", new[] { "tick6", "tick4", "tick2" }, AnimTarget.Calls);

        // Deferred: the calls wait for the end of the pass.
        T.Reset();
        Scene(out AnimationPlayer d, out _, out _, ("a", MethodClip(1f, (0.5, "Tick", new Variant[] { 5 }))));
        d.CallbackModeMethod = AnimationMixer.AnimationCallbackModeMethod.Deferred;
        d.Play("a");
        d.Advance(0.0);
        d.Advance(0.75);
        T.Check("deferred: not called inside advance()", AnimTarget.Calls.Count == 0);
        T.Frame(0.0);
        T.CheckSeq("deferred: called when the pass ends", new[] { "tick5" }, AnimTarget.Calls);

        // A track naming a method the node lacks is an error in the log, not an exception.
        T.Reset();
        Scene(out AnimationPlayer m, out _, out _, ("a", MethodClip(1f, (0.5, "Nope", System.Array.Empty<Variant>()))));
        m.Play("a"); T.Frame(0.1); T.Frame(0.9);
        T.Check("a missing method is logged", T.ErrorCount("Nope") == 1);
    }

    private static void SignalOrder()
    {
        Animation A = EnergyRamp(1f, 1f), B = EnergyRamp(1f, 2f);
        Scene(out AnimationPlayer p, out _, out _, ("A", A), ("B", B));
        var trail = new List<string>();
        p.AnimationStarted += n => trail.Add("started:" + n);
        p.AnimationFinished += n => trail.Add("finished:" + n);
        p.AnimationChanged += (o, n) => trail.Add("changed:" + o + ">" + n);
        p.CurrentAnimationChanged += n => trail.Add("current:" + n);
        p.MixerApplied += () => trail.Add("applied");
        p.Play("A");
        T.CheckSeq("play(): current_animation_changed, then animation_started", new[] { "current:A", "started:A" }, trail);
        trail.Clear();
        p.Queue("B");
        T.Check("queue() while playing only queues", trail.Count == 0 && p.GetQueue().Length == 1 && p.GetQueue()[0] == "B");
        T.Frame(0.0);
        trail.Clear();
        T.Frame(1.0);
        T.CheckSeq("A ends and the queue takes over: started, then animation_changed (no finished for A), then the pass is over",
            new[] { "current:B", "started:B", "changed:A>B", "applied" }, trail);
        trail.Clear();
        T.Frame(0.0);
        T.Frame(1.0);
        T.CheckSeq("B ends: animation_finished is the one non-bookkeeping signal", new[] { "finished:B" }, trail.Where(s => s != "applied"));
        T.Check("animation_finished comes before mixer_applied of the same pass", trail.IndexOf("finished:B") >= 0 && trail.LastIndexOf("applied") > trail.IndexOf("finished:B"));
        T.Check("the queue is spent", p.GetQueue().Length == 0 && !p.IsPlaying());

        // animation_set_next chains without a queue call
        T.Reset();
        Scene(out AnimationPlayer n, out _, out _, ("A", EnergyRamp(1f, 1f)), ("B", EnergyRamp(1f, 2f)));
        n.AnimationSetNext("A", "B");
        T.Check("animation_get_next", n.AnimationGetNext("A") == "B");
        n.Play("A"); T.Frame(0.0); T.Frame(1.0);
        T.Check("A runs into B", n.CurrentAnimation == "B");

        // clear_queue
        T.Reset();
        Scene(out AnimationPlayer c, out _, out _, ("A", EnergyRamp(1f, 1f)), ("B", EnergyRamp(1f, 2f)));
        c.Play("A"); c.Queue("B"); c.ClearQueue(); T.Frame(0.0); T.Frame(1.0);
        T.Check("with the queue cleared A just finishes", !c.IsPlaying() && c.AssignedAnimation == "A");
    }

    private static void Crossfade()
    {
        Scene(out AnimationPlayer p, out AnimTarget target, out _, ("A", EnergyConst(10f, 0f)), ("B", EnergyConst(10f, 10f)), ("C", EnergyConst(10f, 20f)));
        p.PlaybackDefaultBlendTime = 1.0;
        p.Play("A");
        T.Frame(0.5);
        T.CheckNum("A alone", 0f, target.Energy);
        p.Play("B");
        T.Frame(0.0);
        T.CheckNum("the start of the fade is still A", 0f, target.Energy);
        T.Frame(0.25);
        T.CheckNum("a quarter in, B has weight 0.25: 2.5", 2.5f, target.Energy);
        T.Frame(0.25);
        T.CheckNum("halfway: 5", 5f, target.Energy);
        T.Frame(0.5);
        T.CheckNum("done: B alone", 10f, target.Energy);

        p.SetBlendTime("B", "C", 4.0);
        T.Check("set_blend_time and get_blend_time", Math.Abs(p.GetBlendTime("B", "C") - 4.0) < 1e-9 && p.GetBlendTime("C", "B") == 0.0);
        p.Play("C");
        T.Frame(0.0); T.Frame(1.0);
        T.CheckNum("B to C takes 4 s: a quarter of the way after 1 s", 12.5f, target.Energy);
        p.Play("A", 0.5);
        T.Frame(0.0); T.Frame(0.25);
        // At the call B is still fading out with weight 0.75 (its own 4 s) and C has 0.25; C now gets the call's 0.5 s: after 0.25 s its weight is 0,
        // B is 0.75 - 0.25 / 4 = 0.6875 and A is the rest, 0.3125: 10 * 0.6875.
        T.CheckNum("a custom blend per call (0.5 s) applies to the clip it replaces; the clips already fading keep their own times", 6.875f, target.Energy, 1e-3f);

        // A second play() mid-fade does not make the value jump.
        T.Reset();
        Scene(out AnimationPlayer m, out AnimTarget t2, out _, ("A", EnergyConst(10f, 0f)), ("B", EnergyConst(10f, 10f)), ("C", EnergyConst(10f, 20f)));
        m.PlaybackDefaultBlendTime = 1.0;
        m.Play("A"); T.Frame(0.0);
        m.Play("B"); T.Frame(0.0); T.Frame(0.5);
        T.CheckNum("half A, half B", 5f, t2.Energy);
        m.Play("C"); T.Frame(0.0);
        T.CheckNum("starting C at weight 0 changes nothing", 5f, t2.Energy);
    }

    private static void LoopsAndSpeed()
    {
        Scene(out AnimationPlayer p, out AnimTarget target, out _, ("pp", EnergyRamp(1f, 10f, Animation.LoopModeEnum.Pingpong)), ("lin", EnergyRamp(1f, 10f, Animation.LoopModeEnum.Linear)));
        var loops = new List<string>();
        p.AnimationLooped += n => loops.Add(n);
        p.Play("pp");
        T.Frame(0.75); T.Frame(0.75);
        T.CheckNum("ping-pong forward: 0.75 -> 7.5", 7.5f, target.Energy);
        T.Frame(0.75);
        T.CheckNum("bounced off the end: 1.5 -> 0.5 -> 5", 5f, target.Energy);
        T.Check("the bounce is reported (animation_looped, an Aver extension) and the direction flipped", loops.Count == 1 && p.GetPlayingSpeed() < 0);
        T.Frame(0.75);
        T.CheckNum("bounced off the start: -0.25 -> 0.25 -> 2.5", 2.5f, target.Energy);
        T.Check("a ping-pong clip never finishes", p.IsPlaying() && loops.Count == 2);

        loops.Clear();
        p.Play("lin");
        T.Frame(0.0); T.Frame(0.75);
        T.CheckNum("linear loop: 0.75 -> 7.5", 7.5f, target.Energy);
        T.Frame(0.75);
        T.CheckNum("wraps to 0.5 -> 5", 5f, target.Energy);
        T.Check("and reports the loop", loops.Count == 1 && p.IsPlaying());

        // speed_scale, custom speed, backwards, active, manual
        T.Reset();
        Scene(out AnimationPlayer s, out AnimTarget t2, out _, ("a", EnergyRamp(4f, 40f)));
        s.SpeedScale = 2f;
        s.Play("a", -1, 0.5f);
        T.Check("playing speed is speed_scale times the custom speed", T.Near(s.GetPlayingSpeed(), 1f));
        T.Frame(0.0); T.Frame(1.0);
        T.CheckNum("1 s at speed 1 is 10 of 40", 10f, t2.Energy);
        s.SpeedScale = 0f;
        T.Frame(1.0);
        T.CheckNum("speed_scale 0 stands still", 10f, t2.Energy);
        s.SpeedScale = -1f;
        s.Play("a", -1, 1f);
        T.Frame(0.5);
        // play() of the animation already playing does nothing, so the custom speed 0.5 from the first call is still in force: -1 * 0.5 per second.
        T.CheckNum("negative speed_scale runs backwards from where it was: 1 s - 0.25 s", 7.5f, t2.Energy);

        T.Reset();
        Scene(out AnimationPlayer b, out AnimTarget t3, out _, ("a", EnergyRamp(4f, 40f)));
        b.PlayBackwards("a");
        T.Frame(0.0); T.Frame(1.0);
        T.CheckNum("play_backwards starts at the end", 30f, t3.Energy);
        T.Frame(10.0);
        T.CheckNum("and ends at the start", 0f, t3.Energy);
        T.Check("finished at the start", !b.IsPlaying());

        T.Reset();
        Scene(out AnimationPlayer m, out AnimTarget t4, out _, ("a", EnergyRamp(4f, 40f)));
        m.CallbackModeProcess = AnimationMixer.AnimationCallbackModeProcess.Manual;
        m.Play("a");
        T.Frame(0.0); T.Frame(1.0);
        T.CheckNum("manual mode: the frame loop does not step the player", 0f, t4.Energy);
        m.Advance(0.0); m.Advance(1.0);
        T.CheckNum("advance() does", 10f, t4.Energy);
        m.Active = false;
        m.Advance(1.0);
        T.CheckNum("an inactive player does not", 10f, t4.Energy);
        m.Active = true;
        m.CallbackModeProcess = AnimationMixer.AnimationCallbackModeProcess.Physics;
        T.Frame(0.0, 1);
        T.Check("physics mode steps in the physics pass", t4.Energy > 10f - 1e-3f);
    }

    private static void SeekPauseStop()
    {
        var clip = EnergyRamp(2f, 10f);
        int mt = clip.AddTrack(Animation.TrackType.Method);
        clip.TrackSetPath(mt, "Target");
        clip.TrackInsertKey(mt, 0.5, new Collections.Dictionary { ["method"] = new StringName("Tick"), ["args"] = new Collections.Array { 5 } });
        Scene(out AnimationPlayer p, out AnimTarget target, out _, ("a", clip));
        p.Play("a");
        T.Frame(0.0);
        AnimTarget.Calls.Clear();
        p.Seek(1.0);
        T.CheckNum("seek without update changes nothing yet", 0f, target.Energy);
        p.Seek(1.0, true);
        T.CheckNum("seek with update applies the pose", 5f, target.Energy);
        T.CheckNum("current_animation_position", 1f, p.CurrentAnimationPosition);
        T.CheckNum("current_animation_length", 2f, p.CurrentAnimationLength);
        AnimTarget.Calls.Clear();
        p.Seek(0.25, true, true);
        T.Check("update_only does not call methods", AnimTarget.Calls.Count == 0);
        T.CheckNum("but applies values", 1.25f, target.Energy);
        p.Pause();
        T.Check("pause() keeps the animation", !p.IsPlaying() && p.AssignedAnimation == "a");
        T.Frame(1.0);
        T.CheckNum("a paused player does not move", 1.25f, target.Energy);
        p.Play();
        T.Frame(0.0); T.Frame(0.5);
        T.CheckNum("play() with no name resumes where it paused", 3.75f, target.Energy);
        p.Stop();
        T.Check("stop(): not playing, current_animation empty, assigned_animation kept", !p.IsPlaying() && p.CurrentAnimation.IsEmpty && p.AssignedAnimation == "a");
        T.CheckNum("stop() resets the pose to the first frame", 0f, target.Energy);
        T.CheckNum("and the position to 0", 0f, p.CurrentAnimationPosition);

        // autoplay starts when the node is ready
        T.Reset();
        var root = new Node3D { Name = "Root" };
        var player = new AnimationPlayer { Name = "P", Autoplay = "a" };
        var lib = new AnimationLibrary();
        lib.AddAnimation("a", EnergyRamp(2f, 10f));
        player.AddAnimationLibrary("", lib);
        root.AddChild(player);
        T.Check("autoplay does nothing before the node is ready", !player.IsPlaying());
        SceneTree.Instance.Root.AddChild(root);
        T.Check("autoplay plays when ready", player.IsPlaying() && player.CurrentAnimation == "a");
    }

    private static void AudioAndSubPlayer()
    {
        Animation main = Anim(2f);
        int sub = Track(main, Animation.TrackType.Animation, "Sub");
        main.AnimationTrackInsertKey(sub, 0.25, "run");
        main.AnimationTrackInsertKey(sub, 1.0, "[stop]");
        int au = Track(main, Animation.TrackType.Audio, "Snd");
        main.AudioTrackInsertKey(au, 0.5, new Resource { ResourcePath = "res://hit.wav" }, 0.25f, 0.5f);
        int plain = Track(main, Animation.TrackType.Audio, "Plain");
        main.AudioTrackInsertKey(plain, 0.5, new Resource { ResourcePath = "res://plain.wav" }, 0.5f, 0f);

        var root = new Node3D { Name = "Root" };
        var p = new AnimationPlayer { Name = "Player", CallbackModeMethod = AnimationMixer.AnimationCallbackModeMethod.Immediate };
        var lib = new AnimationLibrary(); lib.AddAnimation("main", main);
        p.AddAnimationLibrary("", lib);
        var subPlayer = new AnimationPlayer { Name = "Sub" };
        var runLib = new AnimationLibrary(); runLib.AddAnimation("run", EnergyRamp(10f, 1f, Animation.LoopModeEnum.Linear));
        subPlayer.AddAnimationLibrary("", runLib);
        var snd = new AudioProbe { Name = "Snd" };
        var pl = new PlainAudio { Name = "Plain" };
        root.AddChild(p); root.AddChild(subPlayer); root.AddChild(snd); root.AddChild(pl);
        SceneTree.Instance.Root.AddChild(root);

        p.Play("main");
        T.Frame(0.1);   // the first frame stays at 0
        T.Frame(0.2);
        T.Check("at 0.2 s nothing has started yet", !subPlayer.IsPlaying() && snd.Log.Count == 0);
        T.Frame(0.1);
        T.Check("at 0.3 s the animation track has started the sub-player's animation", subPlayer.IsPlaying() && subPlayer.CurrentAnimation == "run");
        T.Frame(0.25);
        T.CheckSeq("at 0.55 s the audio track has told its target: stream path, start offset, end offset", new[] { "play res://hit.wav 0.25 0.5" }, snd.Log);
        T.Check("a node that is not an IAnimationAudioTarget gets its stream set and play(offset) called", pl.Stream?.ResourcePath == "res://plain.wav" && T.Near((float)pl.From, 0.5f));
        T.Frame(0.6);
        T.Check("past 1 s the [stop] key has stopped the sub-player", !subPlayer.IsPlaying());
        p.Pause();
        T.Check("pause() stops the audio tracks' streams", snd.Log.Last() == "stop" && pl.Stopped);

        // a seek into the middle of a sub-animation key puts the sub-player where it would be
        p.Seek(0.0, true);
        p.Play("main");
        T.Frame(0.0);
        p.Seek(0.75, true);
        T.Check("seek with update starts the sub-animation the playhead is inside, at the seek offset", subPlayer.IsPlaying() && T.Near(subPlayer.CurrentAnimationPosition, 0.5f, 1e-3f));
    }

    private static void CaptureMode()
    {
        Animation a = Anim(10f);
        int t = Track(a, Animation.TrackType.Value, "Target:energy");
        a.ValueTrackSetUpdateMode(t, Animation.UpdateMode.Capture);
        a.TrackInsertKey(t, 0, 100f); a.TrackInsertKey(t, 10, 100f);
        Scene(out AnimationPlayer p, out AnimTarget target, out _, ("cap", a));
        target.Energy = 40f;
        p.PlayWithCapture("cap", 1.0);
        T.Frame(0.0);
        T.CheckNum("play_with_capture starts from the value the node had", 40f, target.Energy);
        T.Frame(0.5);
        T.CheckNum("and fades into the animation", 70f, target.Energy);
        T.Frame(0.5);
        T.CheckNum("done", 100f, target.Energy);

        // playback_auto_capture (default) does the same for plain play() when a different animation was assigned
        T.Reset();
        Scene(out AnimationPlayer q, out AnimTarget t2, out _, ("other", EnergyConst(1f, 0f)), ("cap", a));
        q.AssignedAnimation = "other";
        t2.Energy = 40f;
        q.PlaybackAutoCaptureDuration = 2.0;
        q.Play("cap");
        T.Frame(0.0); T.Frame(1.0);
        T.CheckNum("auto capture: halfway through a 2 s fade", 70f, t2.Energy);
    }

    private static void SceneValues()
    {
        string dir = Path.Combine(Path.GetTempPath(), "aver-fabric-anim-test-" + System.Environment.ProcessId);
        Directory.CreateDirectory(Path.Combine(dir, "Animations"));
        try
        {
            Animation saved = EnergyRamp(2f, 10f);
            T.Check("an animation can be written as .ocanim", saved.SaveTo(Path.Combine(dir, "Animations", "ramp.ocanim")));
            ResourceLoader.ContentRootOverride = dir;
            var loaded = ResourceLoader.Load<Animation>("Animations/ramp.ocanim");
            T.Check("ResourceLoader loads an .ocanim as an Animation", loaded != null && loaded.GetTrackCount() == 1 && T.Near(loaded.Length, 2f) && loaded.ResourcePath == "Animations/ramp.ocanim");
            T.Check("and caches it", ReferenceEquals(loaded, ResourceLoader.Load<Animation>("Animations/ramp.ocanim")));
            T.CheckNum("with its keys", 10f, loaded!.TrackGetKeyValue(0, 1).AsSingle());

            var root = new Node3D { Name = "Root" };
            var player = new AnimationPlayer { Name = "Player" };
            var target = new AnimTarget { Name = "Target" };
            player.PendingValues = new List<(string, string)>
            {
                ("libraries", "{ \"\": { \"ramp\": Resource(\"Animations/ramp.ocanim\") }, \"extra\": { \"again\": Resource(\"Animations/ramp.ocanim\") } }"),
                ("autoplay", "\"ramp\""),
                ("speed_scale", "2.0"),
                ("playback_default_blend_time", "0.25"),
                ("blend_times", "[&\"ramp\", &\"other\", 0.5]"),
                ("root_node", "NodePath(\"..\")"),
                ("callback_mode_method", "1"),
                ("deterministic", "true"),
            };
            root.AddChild(player);
            root.AddChild(target);
            SceneTree.Instance.Root.AddChild(root);
            T.CheckSeq("the libraries property builds libraries from paths", new[] { "ramp", "extra/again" }, player.GetAnimationList());
            T.Check("the animations are the loaded ones", ReferenceEquals(player.GetAnimation("ramp"), loaded) && ReferenceEquals(player.GetAnimation("extra/again"), loaded));
            T.Check("autoplay started it", player.IsPlaying() && player.CurrentAnimation == "ramp");
            T.Check("speed_scale, default blend time, blend_times, callback mode and deterministic", T.Near(player.SpeedScale, 2f) && Math.Abs(player.PlaybackDefaultBlendTime - 0.25) < 1e-9 &&
                    Math.Abs(player.GetBlendTime("ramp", "other") - 0.5) < 1e-9 && player.CallbackModeMethod == AnimationMixer.AnimationCallbackModeMethod.Immediate && player.Deterministic);
            T.Frame(0.0); T.Frame(0.5);
            T.CheckNum("and it drives the target: 0.5 s at speed 2 is 1 s of 2: 5", 5f, target.Energy);
        }
        finally
        {
            try { Directory.Delete(dir, true); } catch (IOException) { /* a leftover temp folder is harmless */ }
        }
    }

    private static void ConverterOutput()
    {
        string? bin = FindBin("AverAssetC" + (OperatingSystem.IsWindows() ? ".exe" : ""));
        if (bin == null) { Console.WriteLine("  SKIP: build/bin/AverAssetC was not found"); return; }
        string data = Path.Combine(T.DataDir(), "AnimGame");
        string outDir = Path.Combine(Path.GetTempPath(), "aver-fabric-anim-conv-" + System.Environment.ProcessId);
        try
        {
            var psi = new System.Diagnostics.ProcessStartInfo(Path.Combine(bin, "AverAssetC" + (OperatingSystem.IsWindows() ? ".exe" : "")), $"fabric \"{data}\" \"{outDir}\" --scenes-only")
            { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false };
            using var proc = System.Diagnostics.Process.Start(psi)!;
            proc.StandardOutput.ReadToEnd(); proc.StandardError.ReadToEnd();
            proc.WaitForExit();
            T.Check("the converter runs on the AnimGame fixture", proc.ExitCode == 0, "exit " + proc.ExitCode);
            if (proc.ExitCode != 0) return;

            ResourceLoader.ContentRootOverride = Path.Combine(outDir, "AnimGame", "Content");
            var idle = ResourceLoader.Load<Animation>("Animations/main_idle.ocanim");
            T.Check("main_idle.ocanim loads", idle != null && idle.GetTrackCount() == 11 && T.Near(idle.Length, 2f) && idle.LoopMode == Animation.LoopModeEnum.Linear);
            if (idle == null) return;
            T.Check("track 0 is the cubic value track on Lamp:light_energy", idle.TrackGetType(0) == Animation.TrackType.Value && idle.TrackGetPath(0) == new NodePath("Lamp:light_energy") && idle.TrackGetInterpolationType(0) == Animation.InterpolationType.Cubic);
            T.CheckVec("the converter's position key (engine centimetres) reads back as Godot's (1, 2, 3) metres", new Vector3(1, 2, 3), idle.TrackGetKeyValue(1, 1).AsVector3());
            T.Check("the rotation key reads back as Godot's quaternion", T.Near(idle.TrackGetKeyValue(2, 1).AsQuaternion(), new Quaternion(0, 0.7071068f, 0, 0.7071068f)));
            T.CheckVec("the scale key reads back as Godot's (2, 3, 4)", new Vector3(2, 3, 4), idle.TrackGetKeyValue(3, 1).AsVector3());
            T.Check("the method keys keep their names and arguments", idle.MethodTrackGetName(4, 0) == "hit" && idle.MethodTrackGetParams(4, 0).Count == 2 && idle.MethodTrackGetParams(4, 0)[1].AsString() == "x" && idle.MethodTrackGetName(4, 1) == "done");
            T.Check("the audio key keeps the stream path and offsets", idle.AudioTrackGetKeyStream(6, 0)?.ResourcePath == "res://sfx/hit.wav" && T.Near(idle.AudioTrackGetKeyStartOffset(6, 0), 0.25f));
            T.Check("the discrete bool track reads back", idle.ValueTrackGetUpdateMode(10) == Animation.UpdateMode.Discrete && idle.TrackGetKeyValue(10, 0).AsBool() && !idle.TrackGetKeyValue(10, 1).AsBool());

            // The whole chain: the converted animation on a player with the Godot-space nodes it names.
            var root = new Node3D { Name = "Main" };
            var player = new AnimationPlayer { Name = "Anim" };
            var box = new Node3D { Name = "Box" };
            var lamp = new AnimTarget { Name = "Lamp" };
            var lib = new AnimationLibrary(); lib.AddAnimation("idle", idle);
            player.AddAnimationLibrary("", lib);
            root.AddChild(player); root.AddChild(box); root.AddChild(lamp);
            SceneTree.Instance.Root.AddChild(root);
            player.Play("idle");
            T.Frame(0.0); T.Frame(1.0);
            T.CheckVec("playing the converted clip to 1 s of 2 puts Box halfway to the keyed Godot-space position", new Vector3(0.5f, 1, 1.5f), box.Position);
            T.CheckVec("and halfway to the keyed scale (1, 1, 1) to (2, 3, 4)", new Vector3(1.5f, 2f, 2.5f), box.Scale);
        }
        finally
        {
            try { Directory.Delete(outDir, true); } catch (IOException) { /* a leftover temp folder is harmless */ }
        }
    }
}
