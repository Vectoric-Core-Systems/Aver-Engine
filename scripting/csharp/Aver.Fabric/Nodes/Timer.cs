// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Timer.

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

// CanvasItem and Control moved to CanvasItem.cs and Control.cs (G5 U1).
