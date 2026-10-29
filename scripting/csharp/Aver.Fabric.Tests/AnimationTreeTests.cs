// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// G5 A2: AnimationTree over the engine's property-track player (Aver.Anim.Abi). The mixing rules (weights, discrete keys, method order, root motion) are
// PropAnimTest's (C++); what is checked here is the graph: every blend node's weights, the state machine (travel paths, conditions, expressions, switch
// modes, cross-fades, start / end), blend spaces, one-shot and transition, root motion through the Godot surface, parameters, and scene values.
// Reference values are computed by hand from Godot's documented behaviour. The suite needs build/bin/Aver.Anim.Abi; without it every test is skipped.

using Aver.Fabric;
using Collections = Aver.Fabric.Collections;

class TreeTarget : AnimTarget
{
    public float Other;
}

static class AnimationTreeTests
{
    public static int RunAll()
    {
        if (!AnimationTests.EnsureNative())
        {
            Console.WriteLine("Test: animation tree (SKIP: build/bin/Aver.Anim.Abi was not found; build the Aver.Anim.Abi target)");
            return 0;
        }
        return T.Suite(() =>
        {
            T.Run("AnimationTree: a leaf plays, loops, plays backwards, and the tree is inactive until asked", Leaf);
            T.Run("AnimationTree: the playhead maths matches the engine player's", StepperMatchesEngine);
            T.Run("Blend2, Blend3, Add2, Add3, Sub2: weights and filters", BlendNodes);
            T.Run("Blend2: sync decides whether an input at weight 0 stands still", SyncFlag);
            T.Run("OneShot: fire, fades, abort, fade out, autorestart, add mode", OneShot);
            T.Run("Transition node, TimeScale and TimeSeek", TransitionScaleSeek);
            T.Run("BlendSpace1D and BlendSpace2D through the tree", BlendSpaces);
            T.Run("StateMachine: start, conditions, expressions, switch modes, cross-fade", StateMachineBasics);
            T.Run("StateMachine: travel() takes the shortest path, honours priority, teleports, and next() / stop() / start()", StateMachineTravel);
            T.Run("StateMachine: end state, nested machines, blend trees inside states", StateMachineNesting);
            T.Run("AnimationTree: root motion and method tracks through the tree", RootMotionAndMethods);
            T.Run("AnimationTree: parameters, scene values (Object trees), AnimationPlayer libraries, the idle process", ParametersAndScene);
            T.Run("AnimationPlayer and AnimationTree: bone tracks pose a Skeleton3D, in Godot's space", BoneTracks);
            T.Run("the converter's output for the TreeGame fixture instantiates and plays (AverAssetC)", ConverterOutput);
        });
    }

    private static void ConverterOutput()
    {
        string exeName = "AverAssetC" + (OperatingSystem.IsWindows() ? ".exe" : "");
        string? exe = null;
        for (DirectoryInfo? d = new DirectoryInfo(AppContext.BaseDirectory); d != null && exe == null; d = d.Parent)
        {
            string p = Path.Combine(d.FullName, "build", "bin", exeName);
            if (File.Exists(p)) exe = p;
        }
        if (exe == null) { Console.WriteLine("  SKIP: build/bin/AverAssetC was not found"); return; }
        string outDir = Path.Combine(Path.GetTempPath(), "aver-fabric-tree-conv-" + System.Environment.ProcessId);
        try
        {
            var psi = new System.Diagnostics.ProcessStartInfo(exe) { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false };
            psi.ArgumentList.Add("fabric"); psi.ArgumentList.Add(Path.Combine(T.DataDir(), "TreeGame")); psi.ArgumentList.Add(outDir); psi.ArgumentList.Add("--scenes-only");
            using (var proc = System.Diagnostics.Process.Start(psi)!)
            {
                proc.StandardOutput.ReadToEnd(); proc.StandardError.ReadToEnd();
                proc.WaitForExit();
                T.Check("the converter runs on the TreeGame fixture", proc.ExitCode == 0, "exit " + proc.ExitCode);
                if (proc.ExitCode != 0) return;
            }
            ResourceLoader.ContentRootOverride = Path.Combine(outDir, "TreeGame", "Content");
            Node main = ResourceLoader.Load<PackedScene>("res://main.tscn")!.Instantiate();
            SceneTree.Instance.Root.AddChild(main);
            T.Check("the level instantiates without an error", T.ErrorCount() == 0, string.Join(" | ", T.Logs.Where(l => l.Level == Aver.Scripting.Log.Level.Error).Select(l => l.Message)));

            var tree = main.GetNode<AnimationTree>("AnimationTree");
            var lamp = main.GetNode<OmniLight3D>("Lamp");
            T.Check("the tree is active, takes the player's animations and has its state machine as root",
                tree.Active && tree.HasAnimation("idle") && tree.HasAnimation("walk") && tree.TreeRoot is AnimationNodeStateMachine { } sm && sm.HasNode("Idle") && sm.HasNode("Move") && sm.GetTransitionCount() == 3);
            T.Check("the transition's switch mode, condition, curve, expression and priority arrive",
                ((AnimationNodeStateMachine)tree.TreeRoot!).GetTransition(1)!.SwitchMode == AnimationNodeStateMachineTransition.SwitchModeEnum.Sync &&
                ((AnimationNodeStateMachine)tree.TreeRoot!).GetTransition(1)!.XfadeCurve!.PointCount == 2 && ((AnimationNodeStateMachine)tree.TreeRoot!).GetTransition(2)!.AdvanceExpression == "speed < 1.0" &&
                ((AnimationNodeStateMachine)tree.TreeRoot!).GetTransition(2)!.Priority == 2);
            T.Check("the root-motion track and the expression base node arrive", tree.RootMotionTrack == new NodePath("Skeleton3D:Root") && tree.AdvanceExpressionBaseNode == new NodePath("../Brain"));
            T.Frame(0.1);
            T.Check("the machine starts by itself in Idle", Playback(tree).GetCurrentNode() == "Idle");
            var seen = main.GetNode<VisibleOnScreenEnabler3D>("Lamp/Seen");
            T.Check("the camera of the level (made current by the converter) sees the lamp's box 2.95 m ahead: the enabler switches the lamp to its enable mode (always)",
                seen.EnableMode == VisibleOnScreenEnabler3D.EnableModeEnum.Always && seen.IsOnScreen() && lamp.ProcessMode == Node.ProcessModeEnum.Always);
            T.CheckNum("and Idle drives the lamp (light_energy 1)", 1f, lamp.LightEnergy);
            tree.Set("parameters/conditions/running", true);
            T.Frames(15, 0.1);
            T.Check("a condition set from a script moves it to the Move blend tree", Playback(tree).GetCurrentNode() == "Move");
            T.CheckNum("whose Blend2 (amount 0.5 from the scene, filtered to the lamp) averages Idle's 1 and Run's 3", 2f, lamp.LightEnergy);

            var skel = main.GetNode<Skeleton3D>("Skeleton3D");
            T.Check("the skeleton has the scene's three bones and parents", skel.GetBoneCount() == 3 && skel.GetBoneName(2) == "Hand" && skel.GetBoneParent(2) == 1 && skel.GetBoneParent(0) == -1);
            T.CheckVec("global pose of the hand", new Vector3(0, 2, 0), skel.GetBoneGlobalPose(2).Origin, 1e-4f);
            T.CheckVec("the attachment sits on it", new Vector3(0, 2, 0), main.GetNode<BoneAttachment3D>("Skeleton3D/Grip").Position, 1e-3f);
            var ik = main.GetNode<SkeletonIK3D>("Skeleton3D/Reach");
            T.Check("the IK node has its bones and iterations and is not running", ik.MaxIterations == 6 && ik.RootBone == "Root" && ik.TipBone == "Hand" && !ik.IsRunning());
            T.Check("a modifier the runtime lacks (LookAtModifier3D) is a plain Node3D carrier", main.GetNode("Skeleton3D/Look") is Node3D);

            var follow = main.GetNode<PathFollow3D>("Track/Rider");
            T.Check("the follower rides the converted curve: half of 10 m, not looping, oriented", T.Near(follow.Position, new Vector3(5, 0, 0), 2e-3f) && !follow.Loop && follow.RotationMode == PathFollow3D.RotationModeEnum.Oriented);
            T.CheckNum("progress_ratio reads 0.5", 0.5f, follow.ProgressRatio, 1e-3f);
            T.CheckNum("the curve's baked length", 10f, main.GetNode<Path3D>("Track").Curve!.GetBakedLength(), 1e-2f);

            var boom = main.GetNode<SpringArm3D>("Boom");
            T.Check("the spring arm has its sphere shape, length and margin", boom.Shape is SphereShape3D { Radius: var rad } && T.Near(rad, 0.25f) && T.Near(boom.SpringLength, 3f) && T.Near(boom.Margin, 0.05f));
            T.Frame(0.016, 1);
            T.CheckVec("with nothing to hit the camera sits at the end of the arm less the margin", new Vector3(0, 0, 2.95f), main.GetNode<Node3D>("Boom/Cam").Position, 1e-3f);

            var mirror = main.GetNode<RemoteTransform3D>("Mirror");
            mirror.Position = new Vector3(1, 2, 3);
            T.Frame(0.016);
            T.CheckVec("the remote transform carries its position to the lamp (scale is switched off)", new Vector3(1, 2, 3), lamp.Position, 1e-4f);
        }
        finally
        {
            try { Directory.Delete(outDir, true); } catch (IOException) { /* a leftover temp folder is harmless */ }
        }
    }

    private static void BoneTracks()
    {
        var root = new Node3D { Name = "Root" };
        var skel = new Skeleton3D { Name = "Skel" };
        skel.AddBone("Hips");
        skel.AddBone("Spine");
        skel.SetBoneParent(1, 0);
        skel.SetBoneRest(1, new Transform3D(Basis.Identity, new Vector3(0, 1, 0)));
        skel.ResetBonePoses();
        var player = new AnimationPlayer { Name = "Player" };
        var a = new Animation { Length = 1f };
        int p = a.AddTrack(Animation.TrackType.Position3D);
        a.TrackSetPath(p, "Skel:Spine");
        a.PositionTrackInsertKey(p, 0, new Vector3(0, 1, 0));
        a.PositionTrackInsertKey(p, 1, new Vector3(0, 3, 0));
        int r = a.AddTrack(Animation.TrackType.Rotation3D);
        a.TrackSetPath(r, "Skel:Hips");
        a.RotationTrackInsertKey(r, 0, Quaternion.Identity);
        a.RotationTrackInsertKey(r, 1, new Quaternion(new Vector3(0, 1, 0), MathF.PI / 2));
        int s = a.AddTrack(Animation.TrackType.Scale3D);
        a.TrackSetPath(s, "Skel:Spine");
        a.ScaleTrackInsertKey(s, 0, Vector3.One);
        a.ScaleTrackInsertKey(s, 1, new Vector3(2, 2, 2));
        int missing = a.AddTrack(Animation.TrackType.Position3D);
        a.TrackSetPath(missing, "Skel:Nope");
        a.PositionTrackInsertKey(missing, 0, Vector3.Zero);
        var lib = new AnimationLibrary();
        lib.AddAnimation("bones", a);
        player.AddAnimationLibrary("", lib);
        root.AddChild(player);
        root.AddChild(skel);
        SceneTree.Instance.Root.AddChild(root);

        player.Play("bones");
        player.Advance(0);
        player.Advance(0.5);
        T.CheckVec("a position bone track sets the bone's pose position (Godot metres)", new Vector3(0, 2, 0), skel.GetBonePosePosition(1), 1e-3f);
        T.Check("a rotation bone track sets its pose rotation (45 degrees about Y halfway)", T.Near(skel.GetBonePoseRotation(0), new Quaternion(new Vector3(0, 1, 0), MathF.PI / 4), 1e-3f));
        T.CheckVec("a scale bone track sets its pose scale", new Vector3(1.5f, 1.5f, 1.5f), skel.GetBonePoseScale(1), 1e-3f);
        T.CheckVec("the global pose follows (the head is on the rotation axis)", new Vector3(0, 2, 0), skel.GetBoneGlobalPose(1).Origin, 1e-3f);
        T.Check("a track naming a bone that is not there warns once and does not stop the others", T.Logs.Count(l => l.Message.Contains("Nope")) == 1);

        // A tree drives bones through the same path.
        var tree = new AnimationTree { Name = "Tree", CallbackModeProcess = AnimationMixer.AnimationCallbackModeProcess.Manual };
        tree.AddAnimationLibrary("", lib);
        tree.TreeRoot = new AnimationNodeAnimation { Animation = "bones" };
        tree.Active = true;
        root.AddChild(tree);
        skel.ResetBonePoses();
        tree.Advance(0.5);
        T.CheckVec("an AnimationTree poses the skeleton too", new Vector3(0, 2, 0), skel.GetBonePosePosition(1), 1e-3f);

        // A bone filter keeps an input to some bones.
        var still = new Animation { Length = 1f, LoopMode = Animation.LoopModeEnum.Linear };
        int sp = still.AddTrack(Animation.TrackType.Position3D);
        still.TrackSetPath(sp, "Skel:Spine");
        still.PositionTrackInsertKey(sp, 0, new Vector3(0, 1, 0));
        still.PositionTrackInsertKey(sp, 1, new Vector3(0, 1, 0));
        int sr = still.AddTrack(Animation.TrackType.Rotation3D);
        still.TrackSetPath(sr, "Skel:Hips");
        still.RotationTrackInsertKey(sr, 0, Quaternion.Identity);
        still.RotationTrackInsertKey(sr, 1, Quaternion.Identity);
        lib.AddAnimation("still", still);
        var mix = new AnimationNodeBlend2 { FilterEnabled = true };
        mix.SetFilterPath("Skel:Hips", true);
        var bt = Tree(("Rest", Leaf("still")), ("Move", Leaf("bones")), ("Mix", mix));
        Wire(bt, ("Mix", 0, "Rest"), ("Mix", 1, "Move"), ("output", 0, "Mix"));
        tree.TreeRoot = bt;
        tree.Set("parameters/Mix/blend_amount", 1.0);
        skel.ResetBonePoses();
        tree.Advance(0.5);
        T.Check("a bone filter: the filtered bone takes the second input (45 degrees)", T.Near(skel.GetBonePoseRotation(0), new Quaternion(new Vector3(0, 1, 0), MathF.PI / 4), 1e-3f));
        T.CheckVec("and the other bone stays with the first input (position (0, 1, 0), not animated)", new Vector3(0, 1, 0), skel.GetBonePosePosition(1), 1e-3f);
    }

    // ---- builders ----

    private static Animation Clip(float length, float energy, float other = float.NaN, Animation.LoopModeEnum loop = Animation.LoopModeEnum.None)
    {
        var a = new Animation { Length = length, LoopMode = loop };
        AddTrack(a, "Target:energy", energy, energy, length);
        if (!float.IsNaN(other)) AddTrack(a, "Target:other", other, other, length);
        return a;
    }

    private static Animation Ramp(float length, float from, float to, Animation.LoopModeEnum loop = Animation.LoopModeEnum.None)
    {
        var a = new Animation { Length = length, LoopMode = loop };
        AddTrack(a, "Target:energy", from, to, length);
        return a;
    }

    private static void AddTrack(Animation a, string path, float from, float to, float length)
    {
        int t = a.AddTrack(Animation.TrackType.Value);
        a.TrackSetPath(t, path);
        a.TrackSetInterpolationType(t, Animation.InterpolationType.Linear);
        a.TrackInsertKey(t, 0, from);
        a.TrackInsertKey(t, length, to);
    }

    private static AnimationNodeAnimation Leaf(string animation) => new() { Animation = animation };

    private static Node3D Build(out AnimationTree tree, out TreeTarget target, AnimationRootNode? root, params (string Name, Animation Anim)[] clips)
    {
        var rootNode = new Node3D { Name = "Root" };
        tree = new AnimationTree { Name = "Tree" };
        target = new TreeTarget { Name = "Target" };
        var lib = new AnimationLibrary();
        lib.AddAnimation("RESET", Clip(1f, 0f, 0f));
        foreach (var (name, anim) in clips) lib.AddAnimation(name, anim);
        tree.AddAnimationLibrary("", lib);
        tree.CallbackModeProcess = AnimationMixer.AnimationCallbackModeProcess.Manual;
        tree.CallbackModeMethod = AnimationMixer.AnimationCallbackModeMethod.Immediate;
        tree.TreeRoot = root;
        tree.Active = true;
        rootNode.AddChild(tree);
        rootNode.AddChild(target);
        SceneTree.Instance.Root.AddChild(rootNode);
        AnimTarget.Calls.Clear();
        return rootNode;
    }

    private static AnimationNodeBlendTree Tree(params (string Name, AnimationNode Node)[] nodes)
    {
        var bt = new AnimationNodeBlendTree();
        foreach (var (name, node) in nodes) bt.AddNode(name, node);
        return bt;
    }

    private static void Wire(AnimationNodeBlendTree bt, params (string To, int Port, string From)[] wires)
    {
        foreach (var (to, port, from) in wires) bt.ConnectNode(to, port, from);
    }

    private static void Advance(AnimationTree tree, double dt, int steps = 1)
    {
        for (int i = 0; i < steps; i++) tree.Advance(dt);
    }

    private static AnimationNodeStateMachineTransition Tr(AnimationNodeStateMachineTransition.SwitchModeEnum mode = AnimationNodeStateMachineTransition.SwitchModeEnum.Immediate,
        AnimationNodeStateMachineTransition.AdvanceModeEnum adv = AnimationNodeStateMachineTransition.AdvanceModeEnum.Auto, string condition = "", string expression = "",
        float xfade = 0f, bool reset = true, int priority = 1) =>
        new() { SwitchMode = mode, AdvanceMode = adv, AdvanceCondition = condition, AdvanceExpression = expression, XfadeTime = xfade, Reset = reset, Priority = priority };

    private static AnimationNodeStateMachinePlayback Playback(AnimationTree tree, string path = "parameters/playback") =>
        (AnimationNodeStateMachinePlayback)tree.Get(path).AsGodotObject()!;

    private const AnimationNodeStateMachineTransition.SwitchModeEnum Imm = AnimationNodeStateMachineTransition.SwitchModeEnum.Immediate;
    private const AnimationNodeStateMachineTransition.SwitchModeEnum Sync = AnimationNodeStateMachineTransition.SwitchModeEnum.Sync;
    private const AnimationNodeStateMachineTransition.SwitchModeEnum AtEnd = AnimationNodeStateMachineTransition.SwitchModeEnum.AtEnd;
    private const AnimationNodeStateMachineTransition.AdvanceModeEnum Auto = AnimationNodeStateMachineTransition.AdvanceModeEnum.Auto;
    private const AnimationNodeStateMachineTransition.AdvanceModeEnum Enabled = AnimationNodeStateMachineTransition.AdvanceModeEnum.Enabled;
    private const AnimationNodeStateMachineTransition.AdvanceModeEnum Disabled = AnimationNodeStateMachineTransition.AdvanceModeEnum.Disabled;

    // ---- tests ----

    private static void Leaf()
    {
        Build(out AnimationTree tree, out TreeTarget target, Leaf("ramp"), ("ramp", Ramp(1f, 0f, 10f)), ("loop", Ramp(1f, 0f, 10f, Animation.LoopModeEnum.Linear)));
        tree.Active = false;
        target.Energy = -1f;
        Advance(tree, 0.25);
        T.CheckNum("an inactive tree does nothing", -1f, target.Energy);
        tree.Active = true;
        Advance(tree, 0.25);
        T.CheckNum("a leaf plays its animation: 0.25 s of a 0..10 ramp over 1 s", 2.5f, target.Energy);
        Advance(tree, 0.25);
        T.CheckNum("and goes on", 5f, target.Energy);
        Advance(tree, 5.0);
        T.CheckNum("a one-shot animation holds its last value", 10f, target.Energy);
        Advance(tree, 0.5);
        T.CheckNum("and keeps holding it", 10f, target.Energy);

        tree.TreeRoot = Leaf("loop");
        Advance(tree, 0.75);
        T.CheckNum("a looping leaf: 0.75", 7.5f, target.Energy);
        Advance(tree, 0.5);
        T.CheckNum("wraps to 0.25", 2.5f, target.Energy);

        var back = new AnimationNodeAnimation { Animation = "ramp", PlayMode = AnimationNodeAnimation.PlayModeEnum.Backward };
        tree.TreeRoot = back;
        Advance(tree, 0.25);
        T.CheckNum("playing backwards starts at the end: 0.75 of the ramp", 7.5f, target.Energy);
        Advance(tree, 0.25);
        T.CheckNum("and runs down", 5f, target.Energy);

        tree.TreeRoot = Leaf("missing");
        target.Energy = -3f;
        Advance(tree, 0.1);
        T.Check("an animation that is not in the libraries is a warning, not a crash, and writes nothing", target.Energy == -3f && T.Logs.Any(l => l.Message.Contains("missing")));
    }

    // The tree evaluates a node without playing it by stepping the playhead in managed code; the engine does the same arithmetic. They must agree.
    private static void StepperMatchesEngine()
    {
        Build(out AnimationTree tree, out _, null, ("none", Ramp(2f, 0f, 1f)), ("loop", Ramp(2f, 0f, 1f, Animation.LoopModeEnum.Linear)), ("pp", Ramp(2f, 0f, 1f, Animation.LoopModeEnum.Pingpong)));
        tree.SyncLibraries();
        int agreed = 0, total = 0;
        foreach ((string clip, int mode) in new[] { ("none", 0), ("loop", 1), ("pp", 2) })
            foreach (double prev in new[] { 0.0, 0.3, 1.9, 2.0 })
                foreach (double d in new[] { 0.0, 0.1, 0.5, 3.7, -0.4, -2.5 })
                {
                    PropStepper.Step(2.0, mode, prev, d, out double managed, out bool looped, out int bounces);
                    var r = tree.TreeAdd(clip, prev, d, 1.0, null, false, false, false);
                    PplayerNative.aver_pplayer_tree_mix(tree.Player, 0);
                    total++;
                    if (r.Ok && Math.Abs(r.Next - managed) < 1e-9 && r.Looped == looped && r.Flipped == ((bounces & 1) != 0)) agreed++;
                    else Console.WriteLine($"    {clip} prev {prev} d {d}: engine {r.Next} looped {r.Looped} flipped {r.Flipped}; managed {managed} looped {looped} bounces {bounces}");
                }
        T.Check($"the managed playhead step agrees with the engine's on {total} cases (three loop modes, wraps, bounces, backward)", agreed == total);
    }

    private static void BlendNodes()
    {
        // Blend2
        var bt = Tree(("A", Leaf("c10")), ("B", Leaf("c20")), ("Mix", new AnimationNodeBlend2()));
        Wire(bt, ("Mix", 0, "A"), ("Mix", 1, "B"), ("output", 0, "Mix"));
        Build(out AnimationTree tree, out TreeTarget target, bt, ("c10", Clip(1f, 10f, 10f, Animation.LoopModeEnum.Linear)), ("c20", Clip(1f, 20f, 20f, Animation.LoopModeEnum.Linear)),
            ("c30", Clip(1f, 30f, 30f, Animation.LoopModeEnum.Linear)), ("c5", Clip(1f, 5f, 5f, Animation.LoopModeEnum.Linear)));
        Advance(tree, 0.1);
        T.CheckNum("Blend2 at amount 0 is the first input", 10f, target.Energy);
        tree.Set("parameters/Mix/blend_amount", 0.25);
        Advance(tree, 0.1);
        T.CheckNum("at 0.25: 0.75 * 10 + 0.25 * 20", 12.5f, target.Energy);
        tree.Set("parameters/Mix/blend_amount", 1.0);
        Advance(tree, 0.1);
        T.CheckNum("at 1 the second input", 20f, target.Energy);
        T.CheckNum("a parameter reads back", 1f, tree.Get("parameters/Mix/blend_amount").AsSingle());

        // Blend2 with a filter: only the listed track is mixed, the others come from the first input.
        var mix = (AnimationNodeBlend2)bt.GetNode("Mix")!;
        mix.FilterEnabled = true;
        mix.SetFilterPath("Target:other", true);
        T.Check("filter paths read back", mix.IsPathFiltered("Target:other") && !mix.IsPathFiltered("Target:energy") && mix.Filters.Count == 1);
        tree.Set("parameters/Mix/blend_amount", 0.5);
        Advance(tree, 0.1);
        T.CheckNum("filtered track 'other' mixes: 15", 15f, target.Other);
        T.CheckNum("an unfiltered track stays with the first input: 10", 10f, target.Energy);
        mix.SetFilterPath("Target:other", false);
        mix.FilterEnabled = false;

        // Blend3: -1 .. 1, the middle input at 0.
        var b3 = Tree(("Lo", Leaf("c10")), ("Mid", Leaf("c20")), ("Hi", Leaf("c30")), ("M3", new AnimationNodeBlend3()));
        Wire(b3, ("M3", 0, "Lo"), ("M3", 1, "Mid"), ("M3", 2, "Hi"), ("output", 0, "M3"));
        tree.TreeRoot = b3;
        Advance(tree, 0.1);
        T.CheckNum("Blend3 at 0 is the middle input", 20f, target.Energy);
        tree.Set("parameters/M3/blend_amount", -0.5);
        Advance(tree, 0.1);
        T.CheckNum("at -0.5 half low, half middle: 15", 15f, target.Energy);
        tree.Set("parameters/M3/blend_amount", 0.5);
        Advance(tree, 0.1);
        T.CheckNum("at +0.5 half middle, half high: 25", 25f, target.Energy);
        tree.Set("parameters/M3/blend_amount", -1.0);
        Advance(tree, 0.1);
        T.CheckNum("at -1 the low input", 10f, target.Energy);

        // Add2: the second input is added to the first, scaled by add_amount.
        var a2 = Tree(("Base", Leaf("c10")), ("Extra", Leaf("c20")), ("Add", new AnimationNodeAdd2()));
        Wire(a2, ("Add", 0, "Base"), ("Add", 1, "Extra"), ("output", 0, "Add"));
        tree.TreeRoot = a2;
        Advance(tree, 0.1);
        T.CheckNum("Add2 at 0 is the base", 10f, target.Energy);
        tree.Set("parameters/Add/add_amount", 0.5);
        Advance(tree, 0.1);
        T.CheckNum("at 0.5: 10 + 0.5 * 20 over the RESET value 0", 20f, target.Energy);
        tree.Set("parameters/Add/add_amount", 2.0);
        Advance(tree, 0.1);
        T.CheckNum("at 2 it is amplified: 10 + 2 * 20", 50f, target.Energy);

        // Add3
        var a3 = Tree(("Down", Leaf("c5")), ("In", Leaf("c10")), ("Up", Leaf("c20")), ("A3", new AnimationNodeAdd3()));
        Wire(a3, ("A3", 0, "Down"), ("A3", 1, "In"), ("A3", 2, "Up"), ("output", 0, "A3"));
        tree.TreeRoot = a3;
        tree.Set("parameters/A3/add_amount", 0.5);
        Advance(tree, 0.1);
        T.CheckNum("Add3 at +0.5 adds half of the upper input: 10 + 0.5 * 20", 20f, target.Energy);
        tree.Set("parameters/A3/add_amount", -1.0);
        Advance(tree, 0.1);
        T.CheckNum("at -1 adds all of the lower input: 10 + 5", 15f, target.Energy);

        // Sub2
        var s2 = Tree(("Base", Leaf("c10")), ("Take", Leaf("c5")), ("Sub", new AnimationNodeSub2()));
        Wire(s2, ("Sub", 0, "Base"), ("Sub", 1, "Take"), ("output", 0, "Sub"));
        tree.TreeRoot = s2;
        tree.Set("parameters/Sub/sub_amount", 1.0);
        Advance(tree, 0.1);
        T.CheckNum("Sub2 takes the second input away: 10 - 5", 5f, target.Energy);
        tree.Set("parameters/Sub/sub_amount", 0.5);
        Advance(tree, 0.1);
        T.CheckNum("scaled: 10 - 2.5", 7.5f, target.Energy);
    }

    private static void SyncFlag()
    {
        foreach (bool sync in new[] { false, true })
        {
            var bt = Tree(("A", Leaf("rampA")), ("B", Leaf("rampB")), ("Mix", new AnimationNodeBlend2 { Sync = sync }));
            Wire(bt, ("Mix", 0, "A"), ("Mix", 1, "B"), ("output", 0, "Mix"));
            Build(out AnimationTree tree, out TreeTarget target, bt, ("rampA", Clip(1f, 0f)), ("rampB", Ramp(1f, 0f, 10f)));
            tree.Set("parameters/Mix/blend_amount", 0.0);
            Advance(tree, 0.5);
            tree.Set("parameters/Mix/blend_amount", 1.0);
            Advance(tree, 0.1);
            T.CheckNum(sync ? "sync on: the second input kept playing while unseen (0.6 s into its ramp)" : "sync off: the second input stood still at weight 0 (0.1 s into its ramp)",
                sync ? 6f : 1f, target.Energy);
            T.Reset();
        }
    }

    private static void OneShot()
    {
        AnimationNodeBlendTree Make(AnimationNodeOneShot shot)
        {
            var bt = Tree(("Base", Leaf("c10")), ("Shot", Leaf("c20")), ("OS", shot));
            Wire(bt, ("OS", 0, "Base"), ("OS", 1, "Shot"), ("output", 0, "OS"));
            return bt;
        }
        (string, Animation)[] clips = { ("c10", Clip(1f, 10f, 10f, Animation.LoopModeEnum.Linear)), ("c20", Clip(1f, 20f, 20f)) };

        Build(out AnimationTree tree, out TreeTarget target, Make(new AnimationNodeOneShot()), clips);
        Advance(tree, 0.1);
        T.Check("idle: the base plays", T.Near(target.Energy, 10f) && !tree.Get("parameters/OS/active").AsBool());
        tree.Set("parameters/OS/request", (int)AnimationNodeOneShot.OneShotRequest.Fire);
        Advance(tree, 0.1);
        T.CheckNum("fire: the shot replaces the base (no fade)", 20f, target.Energy);
        T.Check("active is true, and the request cleared itself", tree.Get("parameters/OS/active").AsBool() && tree.Get("parameters/OS/request").AsInt32() == 0);
        Advance(tree, 0.1, 8);
        T.Check("still active after 0.9 s", tree.Get("parameters/OS/active").AsBool() && T.Near(target.Energy, 20f));
        Advance(tree, 0.1);
        T.Check("the shot ends at 1 s: inactive", !tree.Get("parameters/OS/active").AsBool());
        Advance(tree, 0.1);
        T.CheckNum("and the base is back", 10f, target.Energy);

        // Fade in.
        T.Reset();
        Build(out tree, out target, Make(new AnimationNodeOneShot { FadeinTime = 0.5 }), clips);
        Advance(tree, 0.1);
        tree.Set("parameters/OS/request", 1);
        float[] seen = new float[3];
        for (int i = 0; i < 3; i++) { Advance(tree, 0.25); seen[i] = target.Energy; }
        T.Check("fade in 0.5 s: the shot comes in over two steps (10, 15, 20)", T.Near(seen[0], 10f) && T.Near(seen[1], 15f) && T.Near(seen[2], 20f), string.Join(",", seen));

        // Fade out.
        T.Reset();
        Build(out tree, out target, Make(new AnimationNodeOneShot { FadeoutTime = 0.5 }), clips);
        Advance(tree, 0.1);
        tree.Set("parameters/OS/request", 1);
        float[] fo = new float[4];
        for (int i = 0; i < 4; i++) { Advance(tree, 0.25); fo[i] = target.Energy; }
        T.Check("fade out 0.5 s at the end of a 1 s shot: 20, 20, 15, 10", T.Near(fo[0], 20f) && T.Near(fo[1], 20f) && T.Near(fo[2], 15f) && T.Near(fo[3], 10f), string.Join(",", fo));
        T.Check("and it is inactive after", !tree.Get("parameters/OS/active").AsBool());

        // Abort and the fade-out request.
        T.Reset();
        Build(out tree, out target, Make(new AnimationNodeOneShot { FadeoutTime = 0.5 }), clips);
        tree.Set("parameters/OS/request", 1);
        Advance(tree, 0.1);
        T.CheckNum("fired", 20f, target.Energy);
        tree.Set("parameters/OS/request", (int)AnimationNodeOneShot.OneShotRequest.Abort);
        Advance(tree, 0.1);
        T.Check("abort stops it at once, with no fade", T.Near(target.Energy, 10f) && !tree.Get("parameters/OS/active").AsBool());
        tree.Set("parameters/OS/request", 1);
        Advance(tree, 0.1);
        tree.Set("parameters/OS/request", (int)AnimationNodeOneShot.OneShotRequest.FadeOut);
        Advance(tree, 0.25);
        T.CheckNum("fade_out starts the fade: halfway, 15", 15f, target.Energy);
        Advance(tree, 0.25);
        T.Check("and finishes it", T.Near(target.Energy, 10f) && !tree.Get("parameters/OS/active").AsBool());

        // Autorestart.
        T.Reset();
        Build(out tree, out target, Make(new AnimationNodeOneShot { Autorestart = true, AutorestartDelay = 0.2, AutorestartRandomDelay = 0 }), clips);
        tree.Set("parameters/OS/request", 1);
        Advance(tree, 0.1, 10);
        T.Check("it ended after 1 s", !tree.Get("parameters/OS/active").AsBool());
        Advance(tree, 0.1);
        T.Check("and waits for the delay", !tree.Get("parameters/OS/active").AsBool());
        Advance(tree, 0.1);
        T.Check("then fires again by itself", tree.Get("parameters/OS/active").AsBool());

        // Add mix mode: the shot is added on top of the base.
        T.Reset();
        Build(out tree, out target, Make(new AnimationNodeOneShot { MixMode = AnimationNodeOneShot.MixModeEnum.Add }), clips);
        tree.Set("parameters/OS/request", 1);
        Advance(tree, 0.1);
        T.CheckNum("add mode: 10 + 20", 30f, target.Energy);
    }

    private static void TransitionScaleSeek()
    {
        var tn = new AnimationNodeTransition { InputCount = 2, XfadeTime = 0.5 };
        tn.SetInputName(0, "a");
        tn.SetInputName(1, "b");
        var bt = Tree(("A", Leaf("c10")), ("B", Leaf("c20")), ("Tr", tn));
        Wire(bt, ("Tr", 0, "A"), ("Tr", 1, "B"), ("output", 0, "Tr"));
        Build(out AnimationTree tree, out TreeTarget target, bt, ("c10", Clip(1f, 10f, 10f, Animation.LoopModeEnum.Linear)), ("c20", Clip(1f, 20f, 20f, Animation.LoopModeEnum.Linear)),
            ("ramp", Ramp(1f, 0f, 10f)));
        Advance(tree, 0.1);
        T.Check("Transition starts on its first input", T.Near(target.Energy, 10f) && tree.Get("parameters/Tr/current_state").AsString() == "a" && tree.Get("parameters/Tr/current_index").AsInt32() == 0);
        tree.Set("parameters/Tr/transition_request", "b");
        Advance(tree, 0.25);
        T.CheckNum("a request cross-fades: a quarter second into a half second fade is halfway", 15f, target.Energy);
        T.Check("the request cleared itself and the state names the new input", tree.Get("parameters/Tr/transition_request").AsString() == "" && tree.Get("parameters/Tr/current_state").AsString() == "b");
        Advance(tree, 0.25);
        T.CheckNum("the fade ends", 20f, target.Energy);
        tree.Set("parameters/Tr/transition_request", "nope");
        Advance(tree, 0.1);
        T.Check("an unknown input is a warning and changes nothing", T.Near(target.Energy, 20f) && tree.Get("parameters/Tr/current_index").AsInt32() == 1 && T.Logs.Any(l => l.Message.Contains("nope")));
        tn.XfadeTime = 0;
        tree.Set("parameters/Tr/transition_request", "a");
        Advance(tree, 0.1);
        T.CheckNum("with no cross-fade it switches at once", 10f, target.Energy);
        T.Check("input names and flags", tn.GetInputName(1) == "b" && tn.FindInput("b") == 1 && !tn.IsInputSetAsAutoAdvance(0) && tn.IsInputResetEnabled(0));
        tn.Set("input_1/auto_advance", true);
        T.Check("input_N/... as a scene stores them", tn.IsInputSetAsAutoAdvance(1));

        // TimeScale and TimeSeek.
        var ts = Tree(("R", Leaf("ramp")), ("Scale", new AnimationNodeTimeScale()));
        Wire(ts, ("Scale", 0, "R"), ("output", 0, "Scale"));
        tree.TreeRoot = ts;
        tree.Set("parameters/Scale/scale", 2.0);
        Advance(tree, 0.25);
        T.CheckNum("TimeScale 2: a quarter second plays half a second of the ramp", 5f, target.Energy);
        tree.Set("parameters/Scale/scale", 0.0);
        Advance(tree, 0.25);
        T.CheckNum("scale 0 stops it", 5f, target.Energy);
        tree.Set("parameters/Scale/scale", 0.5);
        Advance(tree, 0.2);
        T.CheckNum("scale 0.5 plays at half speed: another 0.1 s", 6f, target.Energy);

        var sk = Tree(("R", Leaf("ramp")), ("Seek", new AnimationNodeTimeSeek()));
        Wire(sk, ("Seek", 0, "R"), ("output", 0, "Seek"));
        tree.TreeRoot = sk;
        Advance(tree, 0.1);
        tree.Set("parameters/Seek/seek_request", 0.8);
        Advance(tree, 0.0);
        T.CheckNum("TimeSeek jumps the input to 0.8 s", 8f, target.Energy);
        T.CheckNum("and the request clears", -1f, tree.Get("parameters/Seek/seek_request").AsSingle());
        Advance(tree, 0.1);
        T.CheckNum("playback goes on from there", 9f, target.Energy);
    }

    private static void BlendSpaces()
    {
        var space = new AnimationNodeBlendSpace1D { MinSpace = 0, MaxSpace = 3 };
        space.AddBlendPoint(Leaf("c10"), 0f);
        space.AddBlendPoint(Leaf("c20"), 1f);
        space.AddBlendPoint(Leaf("c40"), 3f);
        Build(out AnimationTree tree, out TreeTarget target, space, ("c10", Clip(1f, 10f, 10f, Animation.LoopModeEnum.Linear)), ("c20", Clip(1f, 20f, 20f, Animation.LoopModeEnum.Linear)),
            ("c40", Clip(1f, 40f, 40f, Animation.LoopModeEnum.Linear)));
        tree.Set("parameters/blend_position", 0.5);
        Advance(tree, 0.1);
        T.CheckNum("1D at 0.5: halfway between the first two points (10, 20)", 15f, target.Energy);
        tree.Set("parameters/blend_position", 2.0);
        Advance(tree, 0.1);
        T.CheckNum("at 2: halfway between 20 and 40", 30f, target.Energy);
        tree.Set("parameters/blend_position", 7.0);
        Advance(tree, 0.1);
        T.CheckNum("beyond the range it is the last point alone", 40f, target.Energy);
        space.BlendMode = AnimationNodeBlendSpace1D.BlendModeEnum.Discrete;
        tree.Set("parameters/blend_position", 0.4);
        Advance(tree, 0.1);
        T.CheckNum("discrete mode takes the nearest point (0 is nearer 0.4 than 1)", 10f, target.Energy);
        tree.Set("parameters/blend_position", 0.6);
        Advance(tree, 0.1);
        T.CheckNum("and 0.6 is nearer 1", 20f, target.Energy);

        var s2 = new AnimationNodeBlendSpace2D();
        s2.AddBlendPoint(Leaf("c10"), new Vector2(0, 0));
        s2.AddBlendPoint(Leaf("c20"), new Vector2(1, 0));
        s2.AddBlendPoint(Leaf("c40"), new Vector2(0, 1));
        T.Check("three points make one triangle", s2.GetTriangleCount() == 1);
        tree.TreeRoot = s2;
        tree.Set("parameters/blend_position", new Vector2(0.25f, 0.25f));
        Advance(tree, 0.1);
        T.CheckNum("2D inside the triangle: 0.5 * 10 + 0.25 * 20 + 0.25 * 40", 20f, target.Energy);
        tree.Set("parameters/blend_position", new Vector2(2f, 0f));
        Advance(tree, 0.1);
        T.CheckNum("2D beyond the edge snaps to the nearest corner (20)", 20f, target.Energy);
        tree.Set("parameters/blend_position", new Vector2(0.5f, -3f));
        Advance(tree, 0.1);
        T.CheckNum("below the bottom edge it is the point on the edge: half 10, half 20", 15f, target.Energy);
        s2.BlendMode = AnimationNodeBlendSpace2D.BlendModeEnum.Discrete;
        tree.Set("parameters/blend_position", new Vector2(0.1f, 0.9f));
        Advance(tree, 0.1);
        T.CheckNum("2D discrete: the nearest point (0, 1)", 40f, target.Energy);
    }

    private static AnimationNodeStateMachine Machine(out AnimationNodeStateMachine sm, params (string Name, string Anim)[] states)
    {
        sm = new AnimationNodeStateMachine();
        foreach (var (name, anim) in states) sm.AddNode(name, Leaf(anim), new Vector2(100, 100));
        return sm;
    }

    private static (string, Animation)[] StdClips() => new[]
    {
        ("idle", Clip(1f, 1f, float.NaN, Animation.LoopModeEnum.Linear)), ("run", Clip(1f, 2f, float.NaN, Animation.LoopModeEnum.Linear)), ("atk", Clip(0.5f, 3f)),
        ("long", Ramp(2f, 0f, 20f)), ("short", Ramp(1f, 0f, 10f)),
    };

    private static void StateMachineBasics()
    {
        Machine(out var sm, ("Idle", "idle"), ("Run", "run"), ("Attack", "atk"));
        sm.AddTransition("Start", "Idle", Tr());
        sm.AddTransition("Idle", "Run", Tr(condition: "running"));
        sm.AddTransition("Run", "Idle", Tr(expression: "speed < 1.0"));
        sm.AddTransition("Idle", "Attack", Tr(adv: Enabled));
        sm.AddTransition("Attack", "Idle", Tr(mode: AtEnd));
        Build(out AnimationTree tree, out TreeTarget target, sm, StdClips());
        var brain = new ExprBase { Name = "Brain", Speed = 5f };
        tree.GetParent()!.AddChild(brain);
        tree.AdvanceExpressionBaseNode = "../Brain";
        var pb = Playback(tree);

        T.Check("before the first pass nothing is playing", !pb.IsPlaying() && pb.GetCurrentNode() == "");
        Advance(tree, 0.1);
        T.Check("a root machine starts by itself and leaves Start for Idle", pb.IsPlaying() && pb.GetCurrentNode() == "Idle");
        T.CheckNum("Idle plays", 1f, target.Energy);
        Advance(tree, 0.1, 3);
        T.Check("it stays while no condition holds", pb.GetCurrentNode() == "Idle");
        T.Check("play position and length are reported", T.Near(pb.GetCurrentLength(), 1f) && pb.GetCurrentPlayPosition() > 0.3f);

        tree.Set("parameters/conditions/running", true);
        Advance(tree, 0.1);
        T.Check("a true advance_condition takes the transition", pb.GetCurrentNode() == "Run");
        T.CheckNum("Run plays", 2f, target.Energy);
        tree.Set("parameters/conditions/running", false);
        Advance(tree, 0.1);
        T.Check("with its expression 'speed < 1.0' false (speed is 5) the machine stays in Run", pb.GetCurrentNode() == "Run");
        brain.Speed = 0.5f;
        Advance(tree, 0.1);
        T.Check("and with it true goes back to Idle", pb.GetCurrentNode() == "Idle");
        T.Check("conditions are case-insensitive parameters", tree.Get("PARAMETERS/conditions/RUNNING").VariantType == Variant.Type.Bool);

        // Enabled-mode transitions are for travel only: Idle has one to Attack and does not take it.
        Advance(tree, 0.1, 3);
        T.Check("an Enabled transition is not taken automatically", pb.GetCurrentNode() == "Idle");
        pb.Travel("Attack");
        Advance(tree, 0.1);
        T.Check("but travel() uses it", pb.GetCurrentNode() == "Attack");
        T.CheckNum("Attack plays", 3f, target.Energy);
        Advance(tree, 0.1, 3);
        T.Check("Attack is 0.5 s long; an at-end transition waits for it (0.4 s in)", pb.GetCurrentNode() == "Attack");
        Advance(tree, 0.1);
        T.Check("and leaves when it ends", pb.GetCurrentNode() == "Idle");

        // Cross-fade.
        T.Reset();
        Machine(out sm, ("Idle", "idle"), ("Run", "run"));
        sm.AddTransition("Start", "Idle", Tr());
        sm.AddTransition("Idle", "Run", Tr(condition: "go", xfade: 0.5f));
        Build(out tree, out target, sm, StdClips());
        pb = Playback(tree);
        Advance(tree, 0.1);
        tree.Set("parameters/conditions/go", true);
        Advance(tree, 0.1);
        T.Check("fading: Run is current, Idle is fading out", pb.GetCurrentNode() == "Run" && pb.GetFadingFromNode() == "Idle");
        T.CheckNum("the first fade frame weights Run by 0.1 / 0.5 = 0.2: 0.8 * 1 + 0.2 * 2", 1.2f, target.Energy);
        Advance(tree, 0.1, 2);
        T.CheckNum("after 0.3 s of fade: 1.6", 1.6f, target.Energy);
        Advance(tree, 0.1, 3);
        T.CheckNum("after 0.6 s the fade (0.5 s) is over", 2f, target.Energy);
        T.Check("and nothing fades out any more", pb.GetFadingFromNode() == "");

        // A fade curve shapes the weight.
        T.Reset();
        Machine(out sm, ("Idle", "idle"), ("Run", "run"));
        var late = new Curve();
        late.AddPoint(new Vector2(0, 0), 0, 0);
        late.AddPoint(new Vector2(1, 0), 0, 0);
        sm.AddTransition("Start", "Idle", Tr());
        sm.AddTransition("Idle", "Run", new AnimationNodeStateMachineTransition { AdvanceCondition = "go", XfadeTime = 0.5f, XfadeCurve = late });
        Build(out tree, out target, sm, StdClips());
        Advance(tree, 0.1);
        tree.Set("parameters/conditions/go", true);
        Advance(tree, 0.1, 2);
        T.CheckNum("a flat zero xfade curve keeps the old state at full weight", 1f, target.Energy);

        // Sync: the next state starts at the same fraction of its length; Immediate starts it at the beginning.
        foreach (bool sync in new[] { true, false })
        {
            T.Reset();
            Machine(out sm, ("Short", "short"), ("Long", "long"));
            sm.AddTransition("Start", "Short", Tr());
            sm.AddTransition("Short", "Long", Tr(mode: sync ? Sync : Imm, condition: "go"));
            Build(out tree, out target, sm, StdClips());
            pb = Playback(tree);
            Advance(tree, 0.5);
            tree.Set("parameters/conditions/go", true);
            Advance(tree, 0.1);
            float ratio = pb.GetCurrentPlayPosition() / pb.GetCurrentLength();
            T.Check(sync ? $"sync: the long state continues at the same fraction of its length (0.7 -> {ratio:0.00})" : $"immediate: the long state starts from its beginning (0.1 / 2 -> {ratio:0.00})",
                pb.GetCurrentNode() == "Long" && (sync ? T.Near(ratio, 0.65f, 0.06f) : T.Near(ratio, 0.05f, 0.01f)));
        }
    }

    private static void StateMachineTravel()
    {
        // Idle -> Run -> Boss (via Run) and Idle -> Alt -> Boss (via Alt) have the same length; the lower priority numbers win.
        Machine(out var sm, ("Idle", "idle"), ("Run", "run"), ("Alt", "run"), ("Boss", "atk"), ("Orphan", "idle"), ("Direct", "atk"));
        sm.AddTransition("Start", "Idle", Tr());
        sm.AddTransition("Idle", "Run", Tr(adv: Enabled, priority: 1));
        sm.AddTransition("Idle", "Alt", Tr(adv: Enabled, priority: 5));
        sm.AddTransition("Run", "Boss", Tr(adv: Enabled, priority: 1, mode: AtEnd));
        sm.AddTransition("Alt", "Boss", Tr(adv: Enabled, priority: 5));
        sm.AddTransition("Idle", "Direct", Tr(adv: Disabled));
        sm.AddTransition("Run", "Direct", Tr(adv: Enabled, mode: AtEnd));
        sm.AddTransition("Boss", "Idle", Tr(mode: AtEnd));
        Build(out AnimationTree tree, out TreeTarget target, sm, StdClips());
        var pb = Playback(tree);
        Advance(tree, 0.1);
        T.Check("running in Idle", pb.GetCurrentNode() == "Idle");

        pb.Travel("Boss");
        Advance(tree, 0.1);
        T.Check("travel to Boss goes through Run (the priority 1 path), not Alt", pb.GetCurrentNode() == "Run");
        T.CheckSeq("and the rest of the path is [Boss]", new[] { "Boss" }, pb.GetTravelPath().Select(p => p.ToString()));
        Advance(tree, 0.1, 8);
        T.Check("Run -> Boss is at-end: Run is 1 s long, still in Run at 0.9 s", pb.GetCurrentNode() == "Run");
        Advance(tree, 0.1);
        T.Check("it arrives when Run ends", pb.GetCurrentNode() == "Boss" && pb.GetTravelPath().Length == 0);

        // A disabled transition is no way; the other route is taken, the missing one teleports.
        pb.Start("Idle");
        Advance(tree, 0.1);
        pb.Travel("Direct");
        Advance(tree, 0.1);
        T.CheckSeq("travel to Direct finds Idle -> Run -> Direct (Idle -> Direct is disabled)", new[] { "Direct" }, pb.GetTravelPath().Select(p => p.ToString()));
        Advance(tree, 1.0);
        T.Check("and gets there when Run ends", pb.GetCurrentNode() == "Direct" && pb.GetTravelPath().Length == 0);

        pb.Start("Idle");
        Advance(tree, 0.1);
        pb.Travel("Orphan");
        Advance(tree, 0.1);
        T.Check("no path at all: the machine jumps (teleports) to the state", pb.GetCurrentNode() == "Orphan" && pb.GetTravelPath().Length == 0);
        T.CheckNum("and plays it", 1f, target.Energy);
        pb.Travel("Nowhere");
        Advance(tree, 0.1);
        T.Check("an unknown state is a warning and nothing happens", pb.GetCurrentNode() == "Orphan" && T.Logs.Any(l => l.Message.Contains("Nowhere")));
        pb.Travel("Orphan");
        Advance(tree, 0.1);
        T.Check("travel to the current state does nothing", pb.GetCurrentNode() == "Orphan");

        // next(): take the transition now, without waiting for the end.
        pb.Start("Boss");
        Advance(tree, 0.1);
        T.Check("Boss -> Idle waits for the end of Boss", pb.GetCurrentNode() == "Boss");
        pb.Next();
        Advance(tree, 0.1);
        T.Check("next() takes it now", pb.GetCurrentNode() == "Idle");

        // stop() and start().
        pb.Stop();
        target.Energy = -1f;
        Advance(tree, 0.1);
        T.Check("a stopped machine plays nothing", !pb.IsPlaying() && pb.GetCurrentNode() == "" && target.Energy == -1f);
        pb.Start("Run");
        Advance(tree, 0.1);
        T.Check("start() begins at a state", pb.IsPlaying() && pb.GetCurrentNode() == "Run");
        T.CheckNum("and plays it", 2f, target.Energy);

        // Transitions to self.
        pb.Travel("Run");
        Advance(tree, 0.1);
        T.Check("travel to the state it is in is a no-op without allow_transition_to_self", pb.GetCurrentNode() == "Run");

        T.Check("API surface", sm.HasNode("Idle") && sm.HasTransition("Idle", "Run") && sm.GetTransitionCount() == 8 && sm.GetTransitionFrom(1) == "Idle" && sm.GetTransitionTo(1) == "Run" &&
                sm.GetNodeName(sm.GetNode("Run")!) == "Run" && sm.GetNodeList().Length == 8);
        sm.RemoveTransition("Idle", "Run");
        sm.RenameNode("Alt", "Other");
        T.Check("removing a transition and renaming a state keep the rest consistent", !sm.HasTransition("Idle", "Run") && sm.HasTransition("Idle", "Other") && sm.HasTransition("Other", "Boss"));
        sm.RemoveNode("Other");
        T.Check("removing a state removes its transitions", !sm.HasNode("Other") && !sm.HasTransition("Idle", "Other"));
        sm.AddTransition("Boss", "Start", Tr());
        sm.AddTransition("End", "Idle", Tr());
        T.Check("nothing leads into Start or out of End", !sm.HasTransition("Boss", "Start") && !sm.HasTransition("End", "Idle"));
    }

    private static void StateMachineNesting()
    {
        // A state that ends the machine.
        Machine(out var sm, ("A", "atk"));
        sm.AddTransition("Start", "A", Tr());
        sm.AddTransition("A", "End", Tr(mode: AtEnd));
        Build(out AnimationTree tree, out TreeTarget target, sm, StdClips());
        var pb = Playback(tree);
        Advance(tree, 0.1, 4);
        T.Check("playing A (0.5 s long)", pb.IsPlaying() && pb.GetCurrentNode() == "A");
        Advance(tree, 0.1);
        T.Check("reaching End stops the machine", !pb.IsPlaying() && pb.GetCurrentNode() == "End");
        pb.Start("A");
        Advance(tree, 0.1);
        T.Check("and start() plays it again", pb.IsPlaying() && pb.GetCurrentNode() == "A");

        // A nested machine: the parent leaves it at its end, travel("Sub/Q") goes inside.
        T.Reset();
        Machine(out var inner, ("P", "idle"), ("Q", "run"));
        inner.AddTransition("Start", "P", Tr());
        inner.AddTransition("P", "Q", Tr(adv: Enabled));
        inner.StateMachineType = AnimationNodeStateMachine.StateMachineTypeEnum.Nested;
        var outer = new AnimationNodeStateMachine();
        outer.AddNode("Sub", inner);
        outer.AddNode("After", Leaf("atk"));
        outer.AddTransition("Start", "Sub", Tr());
        Build(out tree, out target, outer, StdClips());
        pb = Playback(tree);
        Advance(tree, 0.1);
        T.Check("the outer machine enters the nested one", pb.GetCurrentNode() == "Sub");
        var innerPb = Playback(tree, "parameters/Sub/playback");
        T.Check("which has a playback of its own and started at P", innerPb.IsPlaying() && innerPb.GetCurrentNode() == "P");
        T.CheckNum("P plays", 1f, target.Energy);
        pb.Travel("Sub/Q");
        Advance(tree, 0.1, 2);
        T.Check("travel('Sub/Q') goes into the nested machine and on to Q", innerPb.GetCurrentNode() == "Q");
        T.CheckNum("Q plays", 2f, target.Energy);

        // A blend tree inside a state, with a parameter below the state's path.
        T.Reset();
        var bt = Tree(("A", Leaf("idle")), ("B", Leaf("run")), ("Mix", new AnimationNodeBlend2()));
        Wire(bt, ("Mix", 0, "A"), ("Mix", 1, "B"), ("output", 0, "Mix"));
        var machine = new AnimationNodeStateMachine();
        machine.AddNode("Move", bt);
        machine.AddTransition("Start", "Move", Tr());
        Build(out tree, out target, machine, StdClips());
        tree.Set("parameters/Move/Mix/blend_amount", 0.5);
        Advance(tree, 0.1);
        T.CheckNum("a blend tree in a state: parameters/Move/Mix/blend_amount decides (1 and 2: 1.5)", 1.5f, target.Energy);
        T.CheckNum("and the parameter reads back", 0.5f, tree.Get("parameters/Move/Mix/blend_amount").AsSingle());
    }

    private static void RootMotionAndMethods()
    {
        var walk = new Animation { Length = 1f, LoopMode = Animation.LoopModeEnum.Linear };
        int pt = walk.AddTrack(Animation.TrackType.Position3D);
        walk.TrackSetPath(pt, "Rig");
        walk.PositionTrackInsertKey(pt, 0, Vector3.Zero);
        walk.PositionTrackInsertKey(pt, 1, new Vector3(4, 0, 0));
        int rt = walk.AddTrack(Animation.TrackType.Rotation3D);
        walk.TrackSetPath(rt, "Rig");
        walk.RotationTrackInsertKey(rt, 0, Quaternion.Identity);
        walk.RotationTrackInsertKey(rt, 1, new Quaternion(new Vector3(0, 1, 0), MathF.PI / 2));
        Build(out AnimationTree tree, out TreeTarget target, Leaf("walk"), ("walk", walk));
        tree.RootMotionTrack = "Rig";
        T.Check("root_motion_track reads back", tree.RootMotionTrack == new NodePath("Rig"));
        Advance(tree, 0.5);
        T.CheckVec("root motion position: the track moved 2 m in the step (Godot axes)", new Vector3(2, 0, 0), tree.GetRootMotionPosition(), 1e-3f);
        T.CheckVec("the accumulator is the absolute blended position", new Vector3(2, 0, 0), tree.GetRootMotionPositionAccumulator(), 1e-3f);
        T.Check("root motion rotation: 45 degrees about Y in the step", T.Near(tree.GetRootMotionRotation(), new Quaternion(new Vector3(0, 1, 0), MathF.PI / 4), 1e-3f));
        Advance(tree, 0.75);
        T.CheckVec("across the loop point the step adds the end and the start: 2 + 1 m", new Vector3(3, 0, 0), tree.GetRootMotionPosition(), 1e-3f);
        T.Check("and the rotation turns another 67.5 degrees about Y", T.Near(tree.GetRootMotionRotation(), new Quaternion(new Vector3(0, 1, 0), MathF.PI * 0.375f), 1e-3f));
        Advance(tree, 0.0);
        T.CheckVec("no time passing, no movement", Vector3.Zero, tree.GetRootMotionPosition(), 1e-6f);
        tree.RootMotionTrack = new NodePath();
        Advance(tree, 0.1);
        T.CheckVec("without a root-motion track nothing is reported", Vector3.Zero, tree.GetRootMotionPosition());

        // An AnimationPlayer reports root motion too.
        T.Reset();
        var root = new Node3D { Name = "Root" };
        var player = new AnimationPlayer { Name = "Player" };
        var lib = new AnimationLibrary();
        lib.AddAnimation("walk", walk);
        player.AddAnimationLibrary("", lib);
        player.RootMotionTrack = "Rig";
        root.AddChild(player);
        SceneTree.Instance.Root.AddChild(root);
        player.Play("walk");
        player.Advance(0);
        player.Advance(0.25);
        T.CheckVec("AnimationPlayer root motion: a quarter of 4 m", new Vector3(1, 0, 0), player.GetRootMotionPosition(), 1e-3f);

        // Method tracks fire through the tree, once per crossing.
        T.Reset();
        var calls = new Animation { Length = 1f, LoopMode = Animation.LoopModeEnum.Linear };
        int mt = calls.AddTrack(Animation.TrackType.Method);
        calls.TrackSetPath(mt, "Target");
        calls.TrackInsertKey(mt, 0.5, new Collections.Dictionary { ["method"] = new StringName("Done"), ["args"] = new Collections.Array() });
        Build(out tree, out target, Leaf("calls"), ("calls", calls));
        Advance(tree, 0.4);
        T.Check("no call before the key", AnimTarget.Calls.Count == 0);
        Advance(tree, 0.2);
        T.CheckSeq("the key at 0.5 is called as the playhead passes it", new[] { "done" }, AnimTarget.Calls);
        Advance(tree, 0.1);
        T.Check("and not again", AnimTarget.Calls.Count == 1);
        Advance(tree, 0.9);
        T.Check("the next lap calls it again", AnimTarget.Calls.Count == 2);
    }

    private static void ParametersAndScene()
    {
        var bt = Tree(("A", Leaf("c10")), ("B", Leaf("c20")), ("Mix", new AnimationNodeBlend2()));
        Wire(bt, ("Mix", 0, "A"), ("Mix", 1, "B"), ("output", 0, "Mix"));
        Build(out AnimationTree tree, out TreeTarget target, bt, ("c10", Clip(1f, 10f, 10f, Animation.LoopModeEnum.Linear)), ("c20", Clip(1f, 20f, 20f, Animation.LoopModeEnum.Linear)));
        T.CheckNum("a parameter has its default once the tree is set", 0f, tree.Get("parameters/Mix/blend_amount").AsSingle());
        tree.Set("PARAMETERS/mix/BLEND_AMOUNT", 0.5);
        T.CheckNum("parameter names are matched without regard to case (a level reader lower-cases keys)", 0.5f, tree.Get("parameters/Mix/blend_amount").AsSingle());
        Advance(tree, 0.1);
        T.CheckNum("and the value is used", 15f, target.Energy);
        T.Check("an unknown parameter reads as nil", tree.Get("parameters/Nope/x").VariantType == Variant.Type.Nil);

        // The idle process: a tree that is active steps itself every frame.
        T.Reset();
        Build(out tree, out target, Leaf("ramp"), ("ramp", Ramp(1f, 0f, 10f)));
        tree.CallbackModeProcess = AnimationMixer.AnimationCallbackModeProcess.Idle;
        T.Frame(0.25);
        T.CheckNum("idle process: a frame moves the tree", 2.5f, target.Energy);
        tree.Active = false;
        T.Frame(0.25);
        T.CheckNum("an inactive tree stands still", 2.5f, target.Energy);
        tree.Active = true;
        tree.CallbackModeProcess = AnimationMixer.AnimationCallbackModeProcess.Manual;
        T.Frame(0.25);
        T.CheckNum("manual mode leaves the stepping to the script", 2.5f, target.Energy);

        // A tree as a scene stores it: Object(...) values and parameters/ keys.
        T.Reset();
        var root = new Node3D { Name = "Root" };
        var t2 = new AnimationTree { Name = "Tree" };
        var target2 = new TreeTarget { Name = "Target" };
        var lib = new AnimationLibrary();
        lib.AddAnimation("RESET", Clip(1f, 0f));
        lib.AddAnimation("c10", Clip(1f, 10f, float.NaN, Animation.LoopModeEnum.Linear));
        lib.AddAnimation("c20", Clip(1f, 20f, float.NaN, Animation.LoopModeEnum.Linear));
        lib.AddAnimation("atk", Clip(0.5f, 3f));
        t2.AddAnimationLibrary("", lib);
        t2.CallbackModeProcess = AnimationMixer.AnimationCallbackModeProcess.Manual;
        t2.PendingValues = new List<(string, string)>
        {
            ("tree_root", "Object(AnimationNodeBlendTree, \"graph_offset\": Vector2(0, 0), \"nodes/A/node\": Object(AnimationNodeAnimation, \"animation\": &\"c10\"), \"nodes/A/position\": Vector2(0, 0), " +
                          "\"nodes/B/node\": Object(AnimationNodeAnimation, \"animation\": &\"c20\"), \"nodes/B/position\": Vector2(0, 100), \"nodes/Mix/node\": Object(AnimationNodeBlend2, \"sync\": false), " +
                          "\"nodes/Mix/position\": Vector2(200, 50), \"nodes/output/position\": Vector2(400, 50), \"node_connections\": [&\"Mix\", 0, &\"A\", &\"Mix\", 1, &\"B\", &\"output\", 0, &\"Mix\"])"),
            ("active", "true"),
            ("parameters/mix/blend_amount", "0.5"),
        };
        root.AddChild(t2);
        root.AddChild(target2);
        SceneTree.Instance.Root.AddChild(root);
        T.Check("a tree_root given as Object(AnimationNodeBlendTree, ...) is built", t2.TreeRoot is AnimationNodeBlendTree b && b.HasNode("Mix") && b.GetNode("Mix") is AnimationNodeBlend2);
        T.Check("with its positions and wiring", ((AnimationNodeBlendTree)t2.TreeRoot!).GetNodePosition("B") == new Vector2(0, 100) && ((AnimationNodeBlendTree)t2.TreeRoot!).NodeConnections.Count == 9);
        t2.Advance(0.1);
        T.CheckNum("and it plays with the parameter the scene set: 15", 15f, target2.Energy);

        // A state machine as a scene stores it.
        T.Reset();
        root = new Node3D { Name = "Root" };
        var t3 = new AnimationTree { Name = "Tree" };
        target2 = new TreeTarget { Name = "Target" };
        lib = new AnimationLibrary();
        lib.AddAnimation("RESET", Clip(1f, 0f));
        lib.AddAnimation("c10", Clip(1f, 10f, float.NaN, Animation.LoopModeEnum.Linear));
        lib.AddAnimation("c20", Clip(1f, 20f, float.NaN, Animation.LoopModeEnum.Linear));
        t3.AddAnimationLibrary("", lib);
        t3.CallbackModeProcess = AnimationMixer.AnimationCallbackModeProcess.Manual;
        t3.PendingValues = new List<(string, string)>
        {
            ("tree_root", "Object(AnimationNodeStateMachine, \"states/Idle/node\": Object(AnimationNodeAnimation, \"animation\": &\"c10\"), \"states/Idle/position\": Vector2(300, 100), " +
                          "\"states/Run/node\": Object(AnimationNodeAnimation, \"animation\": &\"c20\"), \"states/Run/position\": Vector2(500, 100), " +
                          "\"transitions\": [\"Start\", \"Idle\", Object(AnimationNodeStateMachineTransition, \"priority\": 1), \"Idle\", \"Run\", Object(AnimationNodeStateMachineTransition, \"advance_condition\": &\"running\", " +
                          "\"xfade_time\": 0.0, \"xfade_curve\": Object(Curve, \"_data\": [Vector2(0, 0), 0.0, 0.0, 0, 0, Vector2(1, 1), 0.0, 0.0, 0, 0], \"point_count\": 2)), " +
                          "\"Run\", \"Idle\", Object(AnimationNodeStateMachineTransition, \"switch_mode\": 2, \"advance_mode\": 2, \"advance_expression\": \"false\")])"),
            ("active", "true"),
            ("parameters/conditions/running", "false"),
        };
        root.AddChild(t3);
        root.AddChild(target2);
        SceneTree.Instance.Root.AddChild(root);
        var sm = (AnimationNodeStateMachine)t3.TreeRoot!;
        T.Check("a state machine from text: states, positions, transitions", sm.HasNode("Idle") && sm.HasNode("Run") && sm.GetNodePosition("Run") == new Vector2(500, 100) && sm.GetTransitionCount() == 3);
        T.Check("transition properties and a nested Curve resource arrive", sm.GetTransition(1)!.AdvanceCondition == "running" && sm.GetTransition(1)!.XfadeCurve!.PointCount == 2 &&
                sm.GetTransition(2)!.SwitchMode == AnimationNodeStateMachineTransition.SwitchModeEnum.AtEnd && sm.GetTransition(2)!.AdvanceMode == AnimationNodeStateMachineTransition.AdvanceModeEnum.Auto);
        t3.Advance(0.1);
        T.Check("it starts by itself", Playback(t3).GetCurrentNode() == "Idle");
        t3.Set("parameters/conditions/running", true);
        t3.Advance(0.1);
        T.Check("and obeys the condition", Playback(t3).GetCurrentNode() == "Run");

        // AnimationTree.anim_player: the player's libraries and root.
        T.Reset();
        root = new Node3D { Name = "Root" };
        var player = new AnimationPlayer { Name = "Player" };
        var plib = new AnimationLibrary();
        plib.AddAnimation("RESET", Clip(1f, 0f));
        plib.AddAnimation("c10", Clip(1f, 10f, float.NaN, Animation.LoopModeEnum.Linear));
        player.AddAnimationLibrary("", plib);
        var t4 = new AnimationTree { Name = "Tree", CallbackModeProcess = AnimationMixer.AnimationCallbackModeProcess.Manual };
        target2 = new TreeTarget { Name = "Target" };
        root.AddChild(player);
        root.AddChild(t4);
        root.AddChild(target2);
        SceneTree.Instance.Root.AddChild(root);
        // How converted GDScript reaches the tree: animation_tree["parameters/playback"].travel("Run"), animation_tree["parameters/conditions/x"] = true.
        {
            Machine(out var gsm, ("Idle", "c10"), ("Run", "c20"));
            gsm.AddTransition("Start", "Idle", Tr());
            gsm.AddTransition("Idle", "Run", Tr(adv: Enabled));
            var gtree = new AnimationTree { Name = "GTree", CallbackModeProcess = AnimationMixer.AnimationCallbackModeProcess.Manual };
            gtree.AddAnimationLibrary("", plib);
            gtree.TreeRoot = gsm;
            gtree.Active = true;
            root.AddChild(gtree);
            Variant pbv = GDScript.Index(gtree, "parameters/playback");
            T.Check("GDScript indexing a tree by 'parameters/playback' gives the playback object", pbv.AsGodotObject() is AnimationNodeStateMachinePlayback);
            gtree.Advance(0.1);
            GDScript.CallMethod(pbv, "travel", "Run");
            gtree.Advance(0.1);
            T.Check("and calling travel('Run') on it by name works", Playback(gtree).GetCurrentNode() == "Run");
            GDScript.SetIndex(gtree, "parameters/conditions/anything", true);
            T.Check("assigning to an index sets the parameter", gtree.Get("parameters/conditions/anything").AsBool());
            gtree.Free();
        }

        t4.AnimPlayer = "../Player";
        T.Check("the tree takes the player's libraries", t4.HasAnimation("c10") && ReferenceEquals(t4.GetAnimationLibrary(""), plib));
        t4.TreeRoot = Leaf("c10");
        t4.Active = true;
        t4.Advance(0.1);
        T.CheckNum("and plays them, on the nodes the player's root_node names", 10f, target2.Energy);
        plib.AddAnimation("c99", Clip(1f, 99f, float.NaN, Animation.LoopModeEnum.Linear));
        t4.TreeRoot = Leaf("c99");
        t4.Advance(0.1);
        T.CheckNum("an animation added to the player later is seen", 99f, target2.Energy);
    }
}
