// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Timer, and the UI placeholders CanvasItem and Control (stored properties; the UI runtime is a later package).

namespace Aver.Fabric;

/// <summary>Counts down and emits "timeout" (Godot's Timer). It counts the game's scaled time in the frame (or physics) pass.</summary>
public class Timer : Node
{
    public new class SignalName : Node.SignalName
    {
        public static readonly StringName Timeout = "timeout";
    }

    public enum TimerProcessCallback { Physics = 0, Idle = 1 }

    public event Action Timeout { add => Connect(SignalName.Timeout, Callable.From(value)); remove => Disconnect(SignalName.Timeout, Callable.From(value)); }

    private double _waitTime = 1.0;
    private double _timeLeft;
    private bool _running;

    public double WaitTime
    {
        get => _waitTime;
        set { _waitTime = Math.Max(value, 0.001); if (_running && _timeLeft > _waitTime) _timeLeft = _waitTime; }
    }

    public bool OneShot { get; set; }
    public bool Autostart { get; set; }
    public bool Paused { get; set; }
    private TimerProcessCallback _callback = TimerProcessCallback.Idle;

    public TimerProcessCallback ProcessCallback
    {
        get => _callback;
        set { _callback = value; RefreshProcessFlags(); }
    }
    public bool IgnoreTimeScale { get; set; }

    /// <summary>Seconds left until the next timeout; 0 when stopped.</summary>
    public double TimeLeft => _running ? Math.Max(_timeLeft, 0) : 0;

    public bool IsStopped() => !_running;

    /// <summary>Starts (or restarts) the countdown. A positive <paramref name="timeSec"/> also sets <see cref="WaitTime"/>.</summary>
    public void Start(double timeSec = -1)
    {
        if (timeSec > 0) _waitTime = timeSec;
        _timeLeft = _waitTime;
        _running = true;
    }

    public void Stop()
    {
        _running = false;
        _timeLeft = 0;
    }

    internal override bool UsesInternalProcess => ProcessCallback == TimerProcessCallback.Idle;
    internal override bool UsesInternalPhysicsProcess => ProcessCallback == TimerProcessCallback.Physics;

    // With ignore_time_scale the timer counts real seconds, not the scaled seconds the tick gives.
    internal override void InternalProcess(double delta)
    {
        if (ProcessCallback == TimerProcessCallback.Idle) Advance(IgnoreTimeScale ? SceneTree.Instance.LastRealDelta : delta);
    }

    internal override void InternalPhysicsProcess(double delta)
    {
        if (ProcessCallback == TimerProcessCallback.Physics) Advance(IgnoreTimeScale ? SceneTree.Instance.LastRealDelta : delta);
    }

    private void Advance(double delta)
    {
        if (!_running || Paused || !CanProcess()) return;
        _timeLeft -= delta;
        if (_timeLeft > 0) return;
        _timeLeft += _waitTime;
        if (OneShot) Stop();
        EmitSignal(SignalName.Timeout);
    }

    internal override void OnReadying()
    {
        base.OnReadying();
        if (Autostart) Start();
    }

}

/// <summary>Anything drawn in 2D or UI space (Godot's CanvasItem). Only the shared properties are kept here.</summary>
public class CanvasItem : Node
{
    public new class SignalName : Node.SignalName
    {
        public static readonly StringName Draw = "draw";
        public static readonly StringName VisibilityChanged = "visibility_changed";
        public static readonly StringName Hidden = "hidden";
        public static readonly StringName ItemRectChanged = "item_rect_changed";
    }

    private bool _visible = true;

    public bool Visible
    {
        get => _visible;
        set
        {
            if (_visible == value) return;
            _visible = value;
            OnShownChanged();
            EmitSignal(SignalName.VisibilityChanged);
            if (!value) EmitSignal(SignalName.Hidden);
        }
    }

    /// <summary>This item's own visible flag changed (before the signal). A <see cref="SubViewportContainer"/> shows or hides its view with it.</summary>
    internal virtual void OnShownChanged() { }

    public Color Modulate { get; set; } = new(1, 1, 1, 1);
    public Color SelfModulate { get; set; } = new(1, 1, 1, 1);
    public int ZIndex { get; set; }
    public bool TopLevel { get; set; }

    public void Show() => Visible = true;
    public void Hide() => Visible = false;

    public bool IsVisibleInTree()
    {
        if (!IsInsideTree()) return false;
        for (Node? n = this; n is CanvasItem ci; n = n.GetParent())
            if (!ci._visible) return false;
        return true;
    }

    public void QueueRedraw() { }
}

/// <summary>A UI element (Godot's Control). The layout properties are stored; the UI runtime that lays out and draws them is a later package.</summary>
public class Control : CanvasItem
{
    public enum MouseFilterEnum { Stop = 0, Pass = 1, Ignore = 2 }

    public new class SignalName : CanvasItem.SignalName
    {
        public static readonly StringName Resized = "resized";
        public static readonly StringName GuiInput = "gui_input";
        public static readonly StringName MouseEntered = "mouse_entered";
        public static readonly StringName MouseExited = "mouse_exited";
        public static readonly StringName FocusEntered = "focus_entered";
        public static readonly StringName FocusExited = "focus_exited";
        public static readonly StringName SizeFlagsChanged = "size_flags_changed";
        public static readonly StringName MinimumSizeChanged = "minimum_size_changed";
    }

    private Vector2 _position, _size;

    public Vector2 Position { get => _position; set { _position = value; OnRectChanged(); } }
    public Vector2 Size { get => _size; set { _size = value; OnRectChanged(); } }

    /// <summary>The control's rectangle changed. A <see cref="SubViewportContainer"/> keeps the view it shows in step with it.</summary>
    internal virtual void OnRectChanged() { }

    public Vector2 CustomMinimumSize { get; set; }
    public Vector2 PivotOffset { get; set; }
    public float Rotation { get; set; }
    public Vector2 Scale { get; set; } = Vector2.One;
    public float AnchorLeft { get; set; }
    public float AnchorTop { get; set; }
    public float AnchorRight { get; set; }
    public float AnchorBottom { get; set; }
    public float OffsetLeft { get; set; }
    public float OffsetTop { get; set; }
    public float OffsetRight { get; set; }
    public float OffsetBottom { get; set; }
    public MouseFilterEnum MouseFilter { get; set; } = MouseFilterEnum.Stop;
    public string TooltipText { get; set; } = "";

    public Vector2 GetMinimumSize() => CustomMinimumSize;
    public Rect2 GetRect() => new(Position, Size);
    public void GrabFocus() { }
    public bool HasFocus() => false;
    public void ReleaseFocus() { }
}
