// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// SceneTree, Viewport, Window and SceneTreeTimer: the tree API scripts reach through GetTree(). The tree owns the frame signals,
// the one-shot timers and the tweens; the framework's ticks reach it through SceneTreeDriver (engine) or RunManagedFrame (tests).

using System.Diagnostics;
using Aver.Framework;

namespace Aver.Fabric;

/// <summary>A rendering/input area (Godot's Viewport). Here it carries the input-handled switch, the screen size and the 3D world.</summary>
public class Viewport : Node
{
    /// <summary>The size in pixels. Defaults to the project's window size (WINDOW.SIZE) or 1280 by 720.</summary>
    public virtual Vector2I Size
    {
        get => _size ?? ProjectWindowSize();
        set => _size = value;
    }

    /// <summary>The picture this viewport draws as a texture. A <see cref="SubViewport"/> has one (a render-to-texture view, G4_RENDER.md R5); the
    /// window's own viewport is the screen and has none, so this is null for it.</summary>
    public virtual ViewportTexture? GetTexture() => null;

    private Vector2I? _size;

    private static Vector2I ProjectWindowSize()
    {
        try
        {
            string[] parts = Aver.Framework.ProjectSettings.Get("WINDOW.SIZE", "").Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length >= 2 && int.TryParse(parts[0], out int w) && int.TryParse(parts[1], out int h) && w > 0 && h > 0) return new Vector2I(w, h);
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { }
        return new Vector2I(1280, 720);
    }

    /// <summary>The area Controls are laid out in: the viewport's size (the window's stretched content size for the root window).</summary>
    public virtual Rect2 GetVisibleRect() => new(Vector2.Zero, new Vector2(Size.X, Size.Y));

    // ---- the GUI (G5 U1) ----

    /// <summary>The control that has keyboard focus, or null.</summary>
    public Control? GuiGetFocusOwner() => Gui.FocusOwner;

    /// <summary>Takes keyboard focus away from whichever control has it.</summary>
    public void GuiReleaseFocus() => Gui.ReleaseFocusQuiet();

    /// <summary>The control the pointer is over (the deepest one that takes mouse events), or null.</summary>
    public Control? GuiGetHoveredControl() => Gui.HoveredControl;

    public bool GuiIsDragging() => Gui.IsDragging;

    public Variant GuiGetDragData() => Gui.DragData;

    /// <summary>Whether the last drag ended with a control taking the data.</summary>
    public bool GuiIsDragSuccessful() => Gui.DragSuccessful;

    public void GuiCancelDrag() => Gui.CancelDrag();

    /// <summary>The 3D world of this viewport: its physics space.</summary>
    public World3D World3D { get; set; } = new();

    /// <summary>The camera that is drawn (the one made current last).</summary>
    public virtual Camera3D? GetCamera3D() => Camera3D.CurrentCamera;

    /// <summary>The pointer in viewport pixels. The engine gives no absolute pointer position yet: this is where script-fed events last put it.</summary>
    public Vector2 GetMousePosition() => Input.MousePosition;

    /// <summary>Stops the input event being processed by anything further (Godot's set_input_as_handled).</summary>
    public void SetInputAsHandled()
    {
        InputEvent.Current?.MarkHandled();
        if (NodeHost.Current.IsEngine) Aver.Framework.Input.SetInputAsHandled();
    }

    public bool IsInputHandled() => InputEvent.Current?.IsHandled ?? false;
}

/// <summary>The window at the top of the tree; its name is "root" (Godot's Window).</summary>
public class Window : Viewport
{
    public enum ContentScaleModeEnum { Disabled = 0, CanvasItems = 1, Viewport = 2 }

    public enum ContentScaleAspectEnum { Ignore = 0, Keep = 1, KeepWidth = 2, KeepHeight = 3, Expand = 4 }

    public enum ContentScaleStretchEnum { Fractional = 0, Integer = 1 }

    public string Title { get; set; } = "";

    private ContentScaleModeEnum? _mode;
    private ContentScaleAspectEnum? _aspect;
    private ContentScaleStretchEnum? _stretch;
    private Vector2I? _baseSize;
    private float? _factor;

    // The project's display/window/stretch settings (the UI.* keys); a script may change any of them.
    private static string Project(string key) => ProjectSettingsKey(key);

    private static string ProjectSettingsKey(string key)
    {
        try { return Aver.Framework.ProjectSettings.Get(key, ""); }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { return ""; }
    }

    /// <summary>How the UI follows the window's size: nothing stretches (default), the canvas is drawn scaled (canvas_items), or the picture is rendered at the
    /// base size and scaled up (viewport). Project key UI.STRETCH.</summary>
    public ContentScaleModeEnum ContentScaleMode
    {
        get => _mode ??= Project("UI.STRETCH") switch { "canvas_items" => ContentScaleModeEnum.CanvasItems, "viewport" => ContentScaleModeEnum.Viewport, _ => ContentScaleModeEnum.Disabled };
        set { _mode = value; ContentChanged(); }
    }

    /// <summary>What happens when the window's shape differs from the base size's. Project key UI.ASPECT.</summary>
    public ContentScaleAspectEnum ContentScaleAspect
    {
        get => _aspect ??= Project("UI.ASPECT") switch
        {
            "ignore" => ContentScaleAspectEnum.Ignore, "keep_width" => ContentScaleAspectEnum.KeepWidth, "keep_height" => ContentScaleAspectEnum.KeepHeight,
            "expand" => ContentScaleAspectEnum.Expand, _ => ContentScaleAspectEnum.Keep,
        };
        set { _aspect = value; ContentChanged(); }
    }

    /// <summary>Fractional or integer scaling. Project key UI.STRETCHSCALEMODE.</summary>
    public ContentScaleStretchEnum ContentScaleStretch
    {
        get => _stretch ??= Project("UI.STRETCHSCALEMODE") == "integer" ? ContentScaleStretchEnum.Integer : ContentScaleStretchEnum.Fractional;
        set { _stretch = value; ContentChanged(); }
    }

    /// <summary>The size the UI is authored at; zero in either axis means none (no stretch). Project key UI.BASESIZE.</summary>
    public Vector2I ContentScaleSize
    {
        get
        {
            if (_baseSize is { } s) return s;
            string[] p = Project("UI.BASESIZE").Split(' ', StringSplitOptions.RemoveEmptyEntries);
            return (_baseSize = p.Length >= 2 && int.TryParse(p[0], out int w) && int.TryParse(p[1], out int h) ? new Vector2I(w, h) : Vector2I.Zero).Value;
        }
        set { _baseSize = value; ContentChanged(); }
    }

    /// <summary>The extra scale applied on top of the stretch. Project key UI.STRETCHSCALE.</summary>
    public float ContentScaleFactor
    {
        get => _factor ??= float.TryParse(Project("UI.STRETCHSCALE"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float f) && f > 0 ? f : 1f;
        set { _factor = Math.Max(0.01f, value); ContentChanged(); }
    }

    public override Vector2I Size
    {
        get => base.Size;
        set { base.Size = value; ContentChanged(); }
    }

    // The window's size in device pixels: the engine's UI rectangle when there is one, else the window's size.
    internal Vector2 WindowPixels => EnginePixels ?? new Vector2(Size.X, Size.Y);

    private static Vector2? EnginePixels
    {
        get
        {
            if (!NodeHost.Current.IsEngine || !UiNative.Available) return null;
            try
            {
                var xywh = new float[4];
                UiNative.aver_ui_viewport(xywh);
                return xywh[2] > 0 && xywh[3] > 0 ? new Vector2(xywh[2], xywh[3]) : null;
            }
            catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { return null; }
        }
    }

    private (Rect2 Screen, Vector2 Content, Vector2 Render, Vector2 Scale, bool Letterboxed) Compute()
    {
        Vector2 px = WindowPixels;
        Vector2I b = ContentScaleSize;
        return UiNative.Stretch((UiStretchModeId)(int)ContentScaleMode, (UiStretchAspectId)(int)ContentScaleAspect, (UiStretchScaleModeId)(int)ContentScaleStretch,
                                b.X, b.Y, ContentScaleFactor, px.X, px.Y);
    }

    /// <summary>The area Controls are laid out in: the window's pixels divided by the content scale factor, or the base size (letterboxed or grown) when the
    /// stretch mode is on. Without the engine library the stretch is not computed and this is the window's size.</summary>
    public override Rect2 GetVisibleRect()
    {
        if (ContentScaleMode == ContentScaleModeEnum.Disabled && ContentScaleFactor == 1f) return base.GetVisibleRect();
        Vector2 c = Compute().Content;
        return new Rect2(Vector2.Zero, c);
    }

    /// <summary>Where the stretched content lands in the window, in window pixels (the letterbox is the rest).</summary>
    public Rect2 GetContentScreenRect() => Compute().Screen;

    public bool IsLetterboxed() => Compute().Letterboxed;

    /// <summary>A point in window pixels (the pointer) in UI space; a point in a letterbox bar comes out outside the visible rectangle.</summary>
    public Vector2 WindowToContent(Vector2 p)
    {
        var r = Compute();
        return new Vector2((p.X - r.Screen.Position.X) / (r.Scale.X > 0 ? r.Scale.X : 1f), (p.Y - r.Screen.Position.Y) / (r.Scale.Y > 0 ? r.Scale.Y : 1f));
    }

    public Vector2 ContentToWindow(Vector2 p)
    {
        var r = Compute();
        return new Vector2(p.X * r.Scale.X + r.Screen.Position.X, p.Y * r.Scale.Y + r.Screen.Position.Y);
    }

    /// <summary>UI space to window pixels as a transform: scale, then the letterbox offset.</summary>
    internal Transform2D ContentToWindowTransform()
    {
        var r = Compute();
        return new Transform2D(new Vector2(r.Scale.X, 0), new Vector2(0, r.Scale.Y), r.Screen.Position);
    }

    // The visible area changed: Controls that hang from the window lay themselves out again.
    private void ContentChanged()
    {
        foreach (Node n in ChildrenSnapshot()) RelayoutFrom(n);
    }

    private static void RelayoutFrom(Node n)
    {
        if (n is Control c) { c.SizeChanged(); return; }
        foreach (Node k in n.ChildrenSnapshot()) RelayoutFrom(k);
    }
}

/// <summary>The scene tree (Godot's SceneTree): the root window, the current scene, groups, pause, timers, tweens and scene changes.</summary>
public partial class SceneTree : GodotObject
{
    public new class SignalName : GodotObject.SignalName
    {
        public static readonly StringName ProcessFrame = "process_frame";
        public static readonly StringName PhysicsFrame = "physics_frame";
        public static readonly StringName NodeAdded = "node_added";
        public static readonly StringName NodeRemoved = "node_removed";
        public static readonly StringName NodeRenamed = "node_renamed";
        public static readonly StringName TreeChanged = "tree_changed";
        public static readonly StringName TreeProcessModeChanged = "tree_process_mode_changed";
    }

    /// <summary>Godot's GROUP_CALL_* flags for <see cref="CallGroupFlags(uint, StringName, StringName, Variant[])"/>.</summary>
    [Flags]
    public enum GroupCallFlags : uint { Default = 0, Reverse = 1, Deferred = 2, Unique = 4 }

    public event Action ProcessFrame { add => Connect(SignalName.ProcessFrame, Callable.From(value)); remove => Disconnect(SignalName.ProcessFrame, Callable.From(value)); }
    public event Action PhysicsFrame { add => Connect(SignalName.PhysicsFrame, Callable.From(value)); remove => Disconnect(SignalName.PhysicsFrame, Callable.From(value)); }
    public event Action<Node> NodeAdded { add => Connect(SignalName.NodeAdded, Callable.From(value)); remove => Disconnect(SignalName.NodeAdded, Callable.From(value)); }
    public event Action<Node> NodeRemoved { add => Connect(SignalName.NodeRemoved, Callable.From(value)); remove => Disconnect(SignalName.NodeRemoved, Callable.From(value)); }
    public event Action<Node> NodeRenamed { add => Connect(SignalName.NodeRenamed, Callable.From(value)); remove => Disconnect(SignalName.NodeRenamed, Callable.From(value)); }
    public event Action TreeChanged { add => Connect(SignalName.TreeChanged, Callable.From(value)); remove => Disconnect(SignalName.TreeChanged, Callable.From(value)); }

    private static SceneTree? s_instance;
    private bool _paused;
    private Window? _root;
    private Node? _currentScene;
    private string _currentScenePath = "";
    private long _managedFrames;
    private readonly List<SceneTreeTimer> _timers = new();
    private readonly List<Tween> _tweens = new();
    private readonly Stopwatch _clock = Stopwatch.StartNew();
    private long _lastTicks = -1;
    private bool _driverRequested;

    /// <summary>The one tree. A test host gets a fresh one with <see cref="Reset"/>.</summary>
    public static SceneTree Instance
    {
        get
        {
            if (s_instance != null) return s_instance;
            s_instance = new SceneTree();
            s_instance.EnsureDriver();   // the first node to enter the tree starts the frame signals, timers and tweens
            return s_instance;
        }
    }

    internal static void Reset()
    {
        s_instance = null;
        Node.ResetGroups();
        Gui.ResetForTests();
        ThemeDB.ResetForTests();
        Input.ResetForTests();
        Camera3D.ResetCurrent();
        DirectionalLight3D.ResetSun();
    }

    /// <summary>The window every scene hangs from. Always in the tree; it has no entity.</summary>
    public Window Root
    {
        get
        {
            if (_root == null)
            {
                _root = new Window();
                _root.SetNameInternal("root");
                _root.MarkAsRoot();
            }
            return _root;
        }
    }

    // ---- scenes ----

    /// <summary>The scene's root node (Godot's current_scene): the one a scene change set, else the first node under the window that
    /// is not an autoload.</summary>
    public Node? CurrentScene
    {
        get
        {
            if (_currentScene is { IsAlive: true } cs && cs.IsInsideTree()) return cs;
            foreach (Node n in Root.ChildrenSnapshot())
                if (!n.Persistent && n.IsAlive && !n.IsQueuedForDeletion() && n is not SceneTreeDriver) return n;
            return null;
        }
        set => _currentScene = value;
    }

    /// <summary>Maps a scene path to the level file the converter wrote for it ("res://levels/a.tscn" to "Maps/levels/a.ocworld").</summary>
    public static Func<string, string> ScenePathMapper { get; set; } = static path =>
    {
        string p = path.StartsWith("res://", StringComparison.Ordinal) ? path["res://".Length..] : path;
        return "Maps/" + System.IO.Path.ChangeExtension(p, ".ocworld");
    };

    /// <summary>Ends the current scene and loads another at the end of the frame (the engine travels to the converted level; a scene
    /// loaded without the engine is instanced into the tree).</summary>
    public Error ChangeSceneToFile(string path)
    {
        if (NodeHost.Current.IsEngine)
        {
            _currentScenePath = path;
            Game.ChangeLevel(ScenePathMapper(path));
            return Error.Ok;
        }
        PackedScene? scene = ResourceLoader.Load(path, typeof(PackedScene)) as PackedScene;
        if (scene == null || !scene.CanInstantiate()) return Error.FileNotFound;
        _currentScenePath = path;
        return ChangeSceneToPacked(scene);
    }

    /// <summary>Replaces the current scene with an instance of a packed scene, at the end of the frame.</summary>
    public Error ChangeSceneToPacked(PackedScene packedScene)
    {
        ArgumentNullException.ThrowIfNull(packedScene);
        if (NodeHost.Current.IsEngine && packedScene.ResourcePath.Length > 0 && packedScene.IsLevelBacked)
        {
            _currentScenePath = packedScene.ResourcePath;
            Game.ChangeLevel(ScenePathMapper(packedScene.ResourcePath));
            return Error.Ok;
        }
        Node instance = packedScene.Instantiate();
        if (packedScene.ResourcePath.Length > 0) _currentScenePath = packedScene.ResourcePath;
        Deferred.Call(() =>
        {
            Node? old = CurrentScene;
            if (old != null && old.IsAlive) old.Free();
            Root.AddChild(instance);
            _currentScene = instance;
        });
        return Error.Ok;
    }

    /// <summary>Loads the current scene again.</summary>
    public Error ReloadCurrentScene()
    {
        string path = CurrentScene?.SceneFilePath is { Length: > 0 } p ? p : _currentScenePath;
        if (path.Length == 0) return Error.Unconfigured;
        return ChangeSceneToFile(path);
    }

    /// <summary>Frees the current scene without loading another.</summary>
    public Error UnloadCurrentScene()
    {
        Node? cs = CurrentScene;
        if (cs == null) return Error.Unconfigured;
        cs.QueueFree();
        _currentScene = null;
        return Error.Ok;
    }

    // ---- pause, quit, frames ----

    /// <summary>Whether the tree is paused: nodes whose process mode is Pausable stop (see Node.ProcessMode).</summary>
    public bool Paused
    {
        get => NodeHost.Current.IsEngine ? Game.IsPaused : _paused;
        set
        {
            if (Paused == value) return;
            var before = AllNodes().Select(n => (Node: n, Could: n.CanProcess())).ToList();
            if (NodeHost.Current.IsEngine) Game.SetPaused(value);
            else _paused = value;
            // As Godot does: a node that stops processing is told it is paused, one that starts (WhenPaused) that it is unpaused.
            foreach ((Node n, bool could) in before)
            {
                bool can = n.CanProcess();
                if (!n.IsAlive || can == could) continue;
                n.Notification(could ? Node.NotificationPaused : Node.NotificationUnpaused);
            }
        }
    }

    /// <summary>Set once <see cref="Quit"/> was called.</summary>
    public bool QuitRequested { get; private set; }

    /// <summary>Asks the game to quit at the end of the frame.</summary>
    public void Quit(int exitCode = 0)
    {
        QuitRequested = true;
        if (NodeHost.Current.IsEngine) Game.Quit();
    }

    public int GetNodeCount() => AllNodes().Count();

    /// <summary>The process frame number: the engine's count, or the count of frames the managed loop ran.</summary>
    public long GetFrame() => NodeHost.Current.IsEngine ? Aver.Framework.Engine.ProcessFrames : _managedFrames;

    internal long PhysicsFrameCount => NodeHost.Current.IsEngine ? Aver.Framework.Engine.PhysicsFrames : _managedPhysicsFrames;
    private long _managedPhysicsFrames;

    private IEnumerable<Node> AllNodes() => NodeHost.PreOrder(Root).Skip(1);

    internal void EmitNodeAdded(Node n)
    {
        EmitSignal(SignalName.NodeAdded, n);
        EmitSignal(SignalName.TreeChanged);
    }

    internal void EmitNodeRemoved(Node n)
    {
        EmitSignal(SignalName.NodeRemoved, n);
        EmitSignal(SignalName.TreeChanged);
    }

    internal void EmitNodeRenamed(Node n) => EmitSignal(SignalName.NodeRenamed, n);

    // ---- groups ----

    public Collections.Array<Node> GetNodesInGroup(StringName group) => new(Node.NodesInGroup(group.ToString()));

    public Node? GetFirstNodeInGroup(StringName group) => Node.NodesInGroup(group.ToString()).FirstOrDefault();

    public bool HasGroup(StringName name) => Node.GroupExists(name.ToString());

    public int GetNodeCountInGroup(StringName group) => Node.NodesInGroup(group.ToString()).Count;

    /// <summary>Calls a method on every node in the group, in tree order. A node without the method is skipped.</summary>
    public void CallGroup(StringName group, StringName method, params Variant[] args) =>
        CallGroupFlags((uint)GroupCallFlags.Default, group, method, args);

    /// <summary>Calls a method on every node in the group. <paramref name="flags"/>: Reverse walks the group last node first, Deferred
    /// makes the calls when the pass ends, Unique (with Deferred) makes one call per group and method however often it is asked.</summary>
    public void CallGroupFlags(uint flags, StringName group, StringName method, params Variant[] args)
    {
        List<Node> nodes = Node.NodesInGroup(group.ToString());
        if ((flags & (uint)GroupCallFlags.Reverse) != 0) nodes.Reverse();
        bool deferred = (flags & (uint)GroupCallFlags.Deferred) != 0;
        if (deferred && (flags & (uint)GroupCallFlags.Unique) != 0)
        {
            string key = group + "\0" + method;
            if (!_uniqueCalls.Add(key)) return;
            Deferred.Call(() => _uniqueCalls.Remove(key));
        }
        foreach (Node n in nodes)
        {
            Node target = n;
            if (deferred) Deferred.Call(() => CallIfAble(target, method, args));
            else CallIfAble(target, method, args);
        }
    }

    public void CallGroupFlags(GroupCallFlags flags, StringName group, StringName method, params Variant[] args) =>
        CallGroupFlags((uint)flags, group, method, args);

    private readonly HashSet<string> _uniqueCalls = new();

    private static void CallIfAble(Node n, StringName method, Variant[] args)
    {
        if (!n.IsAlive || !n.HasMethod(method)) return;
        try { n.Call(method, args); }
        catch (Exception ex) { Aver.Scripting.Log.Error($"[Fabric] call_group: {n.GetClass()}.{method} threw: {ex.GetType().Name}: {ex.Message}"); }
    }

    public void SetGroup(StringName group, string property, Variant value) => SetGroupFlags((uint)GroupCallFlags.Default, group, property, value);

    public void SetGroupFlags(uint flags, StringName group, string property, Variant value)
    {
        List<Node> nodes = Node.NodesInGroup(group.ToString());
        if ((flags & (uint)GroupCallFlags.Reverse) != 0) nodes.Reverse();
        foreach (Node n in nodes)
        {
            Node target = n;
            if ((flags & (uint)GroupCallFlags.Deferred) != 0) Deferred.Call(() => { if (target.IsAlive) target.Set(property, value); });
            else n.Set(property, value);
        }
    }

    public void NotifyGroup(StringName group, int notification) => NotifyGroupFlags((uint)GroupCallFlags.Default, group, notification);

    public void NotifyGroupFlags(uint callFlags, StringName group, int notification)
    {
        List<Node> nodes = Node.NodesInGroup(group.ToString());
        if ((callFlags & (uint)GroupCallFlags.Reverse) != 0) nodes.Reverse();
        foreach (Node n in nodes)
        {
            Node target = n;
            if ((callFlags & (uint)GroupCallFlags.Deferred) != 0) Deferred.Call(() => { if (target.IsAlive) target.Notification(notification); });
            else n.Notification(notification);
        }
    }

    // ---- timers and tweens ----

    /// <summary>A one-shot timer that emits "timeout" after <paramref name="timeSec"/> seconds. With <paramref name="processAlways"/> it
    /// keeps counting while the tree is paused; <paramref name="processInPhysics"/> counts it in physics steps;
    /// <paramref name="ignoreTimeScale"/> counts real seconds. Await it with
    /// <c>await ToSignal(GetTree().CreateTimer(1), SceneTreeTimer.SignalName.Timeout)</c>.</summary>
    public SceneTreeTimer CreateTimer(double timeSec, bool processAlways = true, bool processInPhysics = false, bool ignoreTimeScale = false)
    {
        EnsureDriver();
        var timer = new SceneTreeTimer(timeSec, processAlways, processInPhysics, ignoreTimeScale);
        _timers.Add(timer);
        return timer;
    }

    /// <summary>A tween that is not bound to a node (it stops while the tree is paused, by default).</summary>
    public Tween CreateTween()
    {
        EnsureDriver();
        var tween = new Tween();
        _tweens.Add(tween);
        return tween;
    }

    /// <summary>The tweens that are still alive.</summary>
    public Collections.Array<Tween> GetProcessedTweens() => new(_tweens.Where(t => t.IsValid()));

    internal int TimerCount => _timers.Count;

    /// <summary>The real seconds the last process frame took (what ignore_time_scale timers and tweens count).</summary>
    internal double LastRealDelta { get; private set; } = 1.0 / 60.0;

    // Head of a pass: the frame signal. Runs before any node's _Process / _PhysicsProcess.
    internal void BeginPass(double dt, bool physics, double? realDelta = null)
    {
        if (!physics) { LastRealDelta = realDelta ?? RealDelta(); RenderingServer.FrameStarting(); }
        PollMultiplayers(dt, physics);   // G6 NET2: Godot polls its multiplayer APIs at the head of the process frame, before the frame signal
        EmitSignal(physics ? SignalName.PhysicsFrame : SignalName.ProcessFrame);
    }

    // Tail of a pass: the timers, then the tweens, as Godot does after the nodes.
    internal void EndPass(double dt, double realDt, bool physics)
    {
        bool paused = Paused;
        foreach (SceneTreeTimer t in _timers.ToArray())
        {
            if (t.ProcessInPhysics != physics) continue;
            if (paused && !t.ProcessAlways) continue;
            t.TimeLeft -= t.IgnoreTimeScale ? realDt : dt;
            if (t.TimeLeft > 0) continue;
            _timers.Remove(t);
            t.Fire();
        }
        Tween.TweenProcessMode mode = physics ? Tween.TweenProcessMode.Physics : Tween.TweenProcessMode.Idle;
        foreach (Tween tw in _tweens.ToArray())
        {
            if (tw.ProcessMode != mode) continue;
            if (!tw.Advance(dt, realDt, paused)) _tweens.Remove(tw);
        }
        // Like Godot's physics_frames, the count of steps that have finished: a press made between steps is "just pressed" in the next one.
        if (physics) _managedPhysicsFrames++;
        else
        {
            Gui.FrameUpdate(dt);   // G5 U1: layout, tooltips and Control drawing at the end of the idle frame
            RenderingServer.FrameEnding();
        }
    }

    // Seconds of real time since the last call (the driver's clock for ignore_time_scale timers and tweens).
    internal double RealDelta()
    {
        long now = _clock.ElapsedTicks;
        double dt = _lastTicks < 0 ? 0 : (now - _lastTicks) / (double)Stopwatch.Frequency;
        _lastTicks = now;
        return Math.Min(dt, 0.25);
    }

    // The engine drives a pass through two hidden nodes, one first and one last in the order; added at the end of the pass, again
    // if a play session ended them.
    private SceneTreeDriver? _head, _tail;

    private void EnsureDriver()
    {
        if (_driverRequested || !NodeHost.Current.IsEngine || _head is { IsAlive: true }) return;
        _driverRequested = true;
        Deferred.Call(() =>
        {
            _driverRequested = false;
            if (_head is { IsAlive: true }) return;
            Root.AddChild(_head = new SceneTreeDriver { Name = "SceneTreeHead", Tail = false });
            Root.AddChild(_tail = new SceneTreeDriver { Name = "SceneTreeTail", Tail = true });
        });
    }

    // ---- the managed loop (tests and tools: no engine to tick the nodes) ----

    internal void RunManagedFrame(double dt, int physicsSteps = 0, double fixedStep = 1.0 / 60.0)
    {
        for (int i = 0; i < physicsSteps; i++) RunManagedPass(fixedStep, physics: true);
        RunManagedPass(dt, physics: false);
        _managedFrames++;
    }

    private void RunManagedPass(double dt, bool physics)
    {
        Aver.Framework.Hosting.ExtensionHost.BeginPass(physics);
        Input.InPhysicsFrame = physics;
        BeginPass(dt, physics, dt);
        var ticking = AllNodes().Where(n => physics ? n.PhysicsTickEnabled : n.TickEnabled).ToList();
        ticking.Sort((a, b) =>
        {
            int pa = physics ? a.PhysicsTickPriority : a.TickPriority, pb = physics ? b.PhysicsTickPriority : b.TickPriority;
            return pa != pb ? pa.CompareTo(pb) : Node.TreeOrderCompare(a, b);
        });
        foreach (Node n in ticking)
        {
            if (!n.IsAlive || !n.IsInsideTree() || !n.CanProcess()) continue;
            if (physics) n.OnPhysicsTick((float)dt);
            else n.OnTick((float)dt);
        }
        EndPass(dt, dt, physics);
        Input.InPhysicsFrame = false;
        Aver.Framework.Hosting.ExtensionHost.EndFrame();
    }

    /// <summary>Sends an event down the tree the way the engine's chain does: _Input to every node, deepest and last first, then
    /// _UnhandledInput to those that were not stopped. Used without the engine (tests, tools).</summary>
    internal void DispatchInputManaged(InputEvent e)
    {
        InputEvent? outer = InputEvent.Current;
        InputEvent.Current = e;
        try
        {
            var nodes = AllNodes().Reverse().ToList();
            foreach (Node n in nodes)
            {
                if (!n.IsAlive || !n.IsInsideTree() || !n.CanProcess()) continue;
                n.DeliverInput(e, unhandled: false);
                if (e.IsHandled) return;
            }
            if (Gui.HandleInput(e)) return;   // G5 U1: Controls take their turn between _Input and _UnhandledInput, as in Godot
            foreach (Node n in nodes)
            {
                if (!n.IsAlive || !n.IsInsideTree() || !n.CanProcess()) continue;
                n.DeliverInput(e, unhandled: true);
                if (e.IsHandled) return;
            }
        }
        finally { InputEvent.Current = outer; }
    }

    // Moves every pending SceneTreeTimer on by dt (the managed host's clock; kept for tests that drive timers only).
    internal static void AdvanceTimers(double dt) => Instance.EndPass(dt, dt, physics: false);
}

/// <summary>A one-shot timer from <see cref="SceneTree.CreateTimer"/> (Godot's SceneTreeTimer).</summary>
public class SceneTreeTimer : RefCounted
{
    public new class SignalName : GodotObject.SignalName
    {
        public static readonly StringName Timeout = "timeout";
    }

    public event Action Timeout { add => Connect(SignalName.Timeout, Callable.From(value)); remove => Disconnect(SignalName.Timeout, Callable.From(value)); }

    private bool _fired;

    internal SceneTreeTimer(double timeSec, bool processAlways, bool processInPhysics, bool ignoreTimeScale)
    {
        TimeLeft = timeSec;
        ProcessAlways = processAlways;
        ProcessInPhysics = processInPhysics;
        IgnoreTimeScale = ignoreTimeScale;
    }

    /// <summary>Seconds until the timeout; it can be changed while the timer runs.</summary>
    public double TimeLeft { get; set; }

    internal bool ProcessAlways { get; }
    internal bool ProcessInPhysics { get; }
    internal bool IgnoreTimeScale { get; }

    internal void Fire()
    {
        if (_fired) return;
        _fired = true;
        TimeLeft = 0;
        EmitSignal(SignalName.Timeout);
    }
}

/// <summary>Drives the tree's frame signal, timers and tweens from the engine's tick: one instance runs first in each pass, one last.
/// Added by <see cref="SceneTree"/> on its own; a script never makes one.</summary>
public class SceneTreeDriver : Node
{
    /// <summary>True for the instance that runs last (timers and tweens); false for the one that runs first (the frame signals).</summary>
    public bool Tail { get; set; }

    public SceneTreeDriver()
    {
        ProcessMode = ProcessModeEnum.Always;
        Persistent = true;   // a scene change must not end the clock
        SetProcess(true);
        SetPhysicsProcess(true);
    }

    internal override void OnEnteringTree()
    {
        base.OnEnteringTree();
        ProcessPriority = Tail ? int.MaxValue : int.MinValue;
        ProcessPhysicsPriority = Tail ? int.MaxValue : int.MinValue;
    }

    public override void _Process(double delta)
    {
        SceneTree tree = SceneTree.Instance;
        if (Tail) tree.EndPass(delta, tree.LastRealDelta, physics: false);
        else { Input.BeginFrame(delta); tree.BeginPass(delta, physics: false); }
    }

    public override void _PhysicsProcess(double delta)
    {
        SceneTree tree = SceneTree.Instance;
        if (Tail) { tree.EndPass(delta, delta, physics: true); Input.InPhysicsFrame = false; }
        else { Input.InPhysicsFrame = true; tree.BeginPass(delta, physics: true); }
    }
}
