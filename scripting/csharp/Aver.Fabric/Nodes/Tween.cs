// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tween: Godot 4's tween API in C#. A Tween is a list of steps; each step holds tweeners that run side by side, and the next step
// starts when every tweener of the step has finished. The SceneTree steps the live tweens once per frame (or physics step).

namespace Aver.Fabric;

/// <summary>Animates properties, calls and intervals in sequence or in parallel (Godot's Tween).
/// Made with <see cref="Node.CreateTween"/> (bound to the node) or <see cref="SceneTree.CreateTween"/>.</summary>
public class Tween : RefCounted
{
    public new class SignalName : GodotObject.SignalName
    {
        public static readonly StringName Finished = "finished";
        public static readonly StringName StepFinished = "step_finished";
        public static readonly StringName LoopFinished = "loop_finished";
    }

    public enum TransitionType { Linear = 0, Sine = 1, Quint = 2, Quart = 3, Quad = 4, Expo = 5, Elastic = 6, Cubic = 7, Circ = 8, Bounce = 9, Back = 10, Spring = 11 }

    public enum EaseType { In = 0, Out = 1, InOut = 2, OutIn = 3 }

    public enum TweenProcessMode { Physics = 0, Idle = 1 }

    public enum TweenPauseMode { Bound = 0, Stop = 1, Process = 2 }

    public event Action Finished { add => Connect(SignalName.Finished, Callable.From(value)); remove => Disconnect(SignalName.Finished, Callable.From(value)); }
    public event Action<long> StepFinished { add => Connect(SignalName.StepFinished, Callable.From(value)); remove => Disconnect(SignalName.StepFinished, Callable.From(value)); }
    public event Action<long> LoopFinished { add => Connect(SignalName.LoopFinished, Callable.From(value)); remove => Disconnect(SignalName.LoopFinished, Callable.From(value)); }

    private readonly List<List<Tweener>> _steps = new();
    private int _currentStep = -1;   // the step being played
    private int _appendStep = -1;    // the step the last tweener was added to
    private bool _parallelNext, _defaultParallel;
    private bool _running = true, _started, _dead, _valid = true;
    private int _loops = 1, _loopsDone;
    private double _totalTime, _speedScale = 1.0;
    private Node? _bound;
    private TransitionType _trans = TransitionType.Linear;
    private EaseType _ease = EaseType.InOut;
    private TweenProcessMode _processMode = TweenProcessMode.Idle;
    private TweenPauseMode _pauseMode;
    private bool _pauseModeSet, _ignoreTimeScale;

    public Tween() { }

    // ---- building ----

    /// <summary>Animates a property of an object from its value when the step starts to <paramref name="finalVal"/>. The property path
    /// may reach into a sub-property ("position:x", "modulate:a").</summary>
    public PropertyTweener TweenProperty(GodotObject @object, NodePath property, Variant finalVal, double duration)
    {
        if (!CanAppend()) throw new InvalidOperationException("Tween is invalid (it was killed or has finished).");
        var t = new PropertyTweener(@object, property, finalVal, duration);
        Append(t);
        return t;
    }

    /// <summary>Waits <paramref name="time"/> seconds.</summary>
    public IntervalTweener TweenInterval(double time)
    {
        if (!CanAppend()) throw new InvalidOperationException("Tween is invalid (it was killed or has finished).");
        var t = new IntervalTweener(time);
        Append(t);
        return t;
    }

    /// <summary>Calls a callable when its step is reached.</summary>
    public CallbackTweener TweenCallback(Callable callback)
    {
        if (!CanAppend()) throw new InvalidOperationException("Tween is invalid (it was killed or has finished).");
        var t = new CallbackTweener(callback);
        Append(t);
        return t;
    }

    /// <summary>Calls a method every frame with a value interpolated from <paramref name="from"/> to <paramref name="to"/>.</summary>
    public MethodTweener TweenMethod(Callable method, Variant from, Variant to, double duration)
    {
        if (!CanAppend()) throw new InvalidOperationException("Tween is invalid (it was killed or has finished).");
        var t = new MethodTweener(method, from, to, duration);
        Append(t);
        return t;
    }

    /// <summary>Runs another tween as one step of this one: it plays for its own length and this tween waits for it.</summary>
    public SubtweenTweener TweenSubtween(Tween subtween)
    {
        if (!CanAppend()) throw new InvalidOperationException("Tween is invalid (it was killed or has finished).");
        var t = new SubtweenTweener(subtween);
        Append(t);
        return t;
    }

    private bool CanAppend() => _valid && !_dead;

    private void Append(Tweener t)
    {
        t.Owner = this;
        if (_parallelNext) _appendStep = Math.Max(_appendStep, 0);
        else _appendStep++;
        _parallelNext = _defaultParallel;
        while (_steps.Count <= _appendStep) _steps.Add(new List<Tweener>());
        _steps[_appendStep].Add(t);
    }

    /// <summary>Makes every tweener from now on run alongside the previous one (or, with false, one after the other).</summary>
    public Tween SetParallel(bool parallel = true)
    {
        _defaultParallel = parallel;
        _parallelNext = parallel;
        return this;
    }

    /// <summary>Makes only the next tweener run alongside the previous one.</summary>
    public Tween Parallel()
    {
        _parallelNext = true;
        return this;
    }

    /// <summary>Makes the next tweener start after the previous step (undoes <see cref="SetParallel"/> for one tweener).</summary>
    public Tween Chain()
    {
        _parallelNext = false;
        return this;
    }

    /// <summary>How many times the tween plays; 0 is forever. The default is 1.</summary>
    public Tween SetLoops(int loops = 0)
    {
        _loops = Math.Max(loops, 0);
        return this;
    }

    public int GetLoopsLeft() => _loops == 0 ? -1 : Math.Max(_loops - _loopsDone, 0);

    public Tween SetSpeedScale(float speed) { _speedScale = speed; return this; }

    /// <summary>The transition tweeners use unless they name their own (default Linear).</summary>
    public Tween SetTrans(TransitionType trans) { _trans = trans; return this; }

    /// <summary>The ease tweeners use unless they name their own (default InOut).</summary>
    public Tween SetEase(EaseType ease) { _ease = ease; return this; }

    public Tween SetProcessMode(TweenProcessMode mode) { _processMode = mode; return this; }

    public Tween SetPauseMode(TweenPauseMode mode) { _pauseMode = mode; _pauseModeSet = true; return this; }

    public Tween SetIgnoreTimeScale(bool ignore = true) { _ignoreTimeScale = ignore; return this; }

    /// <summary>Binds the tween to a node: it stops when the node is freed, waits while the node is out of the tree, and (by default)
    /// pauses when the node does.</summary>
    public Tween BindNode(Node node) { _bound = node; return this; }

    internal TransitionType Trans => _trans;
    internal EaseType Ease => _ease;
    internal TweenProcessMode ProcessMode => _processMode;
    internal bool IgnoreTimeScale => _ignoreTimeScale;

    // ---- control ----

    /// <summary>Starts or resumes the tween.</summary>
    public void Play() { if (_valid) _running = true; }

    public void Pause() { _running = false; }

    /// <summary>Stops the tween and rewinds it to its beginning; <see cref="Play"/> starts it again.</summary>
    public void Stop()
    {
        _running = false;
        _started = false;
        _currentStep = -1;
        _loopsDone = 0;
        _totalTime = 0;
    }

    /// <summary>Ends the tween now; it cannot be used again.</summary>
    public void Kill()
    {
        _running = false;
        _dead = true;
        _valid = false;
    }

    public bool IsRunning() => _running;

    public bool IsValid() => _valid;

    public double GetTotalElapsedTime() => _totalTime;

    /// <summary>Steps the tween by hand (for a tween that was stopped, or one driven from a script). Returns false when it has finished.</summary>
    public bool CustomStep(double delta)
    {
        bool wasRunning = _running;
        _running = true;
        bool alive = StepInternal(delta);
        _running = wasRunning && !_dead;
        return alive;
    }

    // ---- stepping ----

    // Called by the SceneTree once per frame (idle) or physics step. Returns false when the tween is finished and can be dropped.
    internal bool Advance(double delta, double realDelta, bool treePaused)
    {
        if (_dead) return false;
        if (_bound != null)
        {
            if (!_bound.IsAlive) return false;
            if (!_bound.IsInsideTree()) return true;
        }
        TweenPauseMode mode = _pauseModeSet ? _pauseMode : _bound != null ? TweenPauseMode.Bound : TweenPauseMode.Stop;
        bool paused = mode switch
        {
            TweenPauseMode.Bound => _bound != null ? !_bound.CanProcess() : treePaused,
            TweenPauseMode.Stop => treePaused,
            _ => false,
        };
        if (paused) return true;
        return StepInternal(_ignoreTimeScale ? realDelta : delta);
    }

    private bool StepInternal(double delta)
    {
        if (_dead) return false;
        if (!_running) return true;
        if (!_started)
        {
            if (_steps.Count == 0) { _dead = true; _valid = false; return false; }
            _currentStep = 0;
            _loopsDone = 0;
            _totalTime = 0;
            StartTweeners();
            _started = true;
        }
        double rem = delta * _speedScale;
        _totalTime += rem;
        bool stepActive;
        double remAtLoopStart = rem;
        while (rem > 0 && _running)
        {
            double stepDelta = rem;
            stepActive = false;
            foreach (Tweener t in _steps[_currentStep].ToArray())
            {
                double temp = rem;
                if (t.Step(ref temp)) stepActive = true;
                stepDelta = Math.Min(temp, stepDelta);
            }
            rem = stepDelta;
            if (stepActive) continue;

            EmitSignal(SignalName.StepFinished, _currentStep);
            _currentStep++;
            if (_currentStep == _steps.Count)
            {
                _loopsDone++;
                if (_loops != 0 && _loopsDone >= _loops)
                {
                    _running = false;
                    _dead = true;
                    _valid = false;
                    EmitSignal(SignalName.Finished);
                    break;
                }
                EmitSignal(SignalName.LoopFinished, _loopsDone);
                _currentStep = 0;
                StartTweeners();
                // An infinite tween whose loop took no time would spin forever: one loop per frame.
                if (_loops == 0 && Math.Abs(rem - remAtLoopStart) < 1e-12) return true;
                remAtLoopStart = rem;
            }
            else StartTweeners();
        }
        return true;
    }

    private void StartTweeners()
    {
        foreach (Tweener t in _steps[_currentStep]) t.Start();
    }

    // ---- interpolation ----

    /// <summary>The value <paramref name="elapsed"/> seconds into a <paramref name="duration"/>-second move from
    /// <paramref name="initial"/> by <paramref name="delta"/> along the curve (Godot's Tween.interpolate_value).</summary>
    public static Variant InterpolateValue(Variant initial, Variant delta, double elapsed, double duration, TransitionType trans, EaseType ease) =>
        Interpolate(initial, delta, elapsed, duration, trans, ease);

    internal static Variant Interpolate(Variant initial, Variant delta, double t, double d, TransitionType trans, EaseType ease)
    {
        double R(double b, double c) => Easing.Run(trans, ease, t, b, c, d);
        switch (initial.VariantType)
        {
            case Variant.Type.Int:
                return (long)R(initial.AsInt64(), delta.AsInt64());
            case Variant.Type.Float:
                return R(initial.AsDouble(), delta.AsDouble());
            case Variant.Type.Vector2:
            {
                Vector2 i = initial.AsVector2(), dl = delta.AsVector2();
                return new Vector2((float)R(i.X, dl.X), (float)R(i.Y, dl.Y));
            }
            case Variant.Type.Vector2I:
            {
                Vector2I i = initial.AsVector2I(), dl = delta.AsVector2I();
                return new Vector2I((int)R(i.X, dl.X), (int)R(i.Y, dl.Y));
            }
            case Variant.Type.Vector3:
            {
                Vector3 i = initial.AsVector3(), dl = delta.AsVector3();
                return new Vector3((float)R(i.X, dl.X), (float)R(i.Y, dl.Y), (float)R(i.Z, dl.Z));
            }
            case Variant.Type.Vector3I:
            {
                Vector3I i = initial.AsVector3I(), dl = delta.AsVector3I();
                return new Vector3I((int)R(i.X, dl.X), (int)R(i.Y, dl.Y), (int)R(i.Z, dl.Z));
            }
            case Variant.Type.Vector4:
            {
                Vector4 i = initial.AsVector4(), dl = delta.AsVector4();
                return new Vector4((float)R(i.X, dl.X), (float)R(i.Y, dl.Y), (float)R(i.Z, dl.Z), (float)R(i.W, dl.W));
            }
            case Variant.Type.Color:
            {
                Color i = initial.AsColor(), dl = delta.AsColor();
                return new Color((float)R(i.R, dl.R), (float)R(i.G, dl.G), (float)R(i.B, dl.B), (float)R(i.A, dl.A));
            }
            case Variant.Type.Quaternion:
            {
                // The delta of a rotation is a rotation: ease the fraction, then slerp from the start to start * delta.
                Quaternion i = initial.AsQuaternion(), dl = delta.AsQuaternion();
                float f = (float)Easing.Run(trans, ease, t, 0, 1, d);
                return i.Slerp(i * dl, f);
            }
            case Variant.Type.Rect2:
            {
                Rect2 i = initial.AsRect2(), dl = delta.AsRect2();
                return new Rect2((float)R(i.Position.X, dl.Position.X), (float)R(i.Position.Y, dl.Position.Y),
                                 (float)R(i.Size.X, dl.Size.X), (float)R(i.Size.Y, dl.Size.Y));
            }
            default:
                // A value that has no in-between (a bool, a string, an object) keeps its start until the end.
                return t >= d ? Add(initial, delta) : initial;
        }
    }

    // initial + delta, for the types a Tween can move.
    internal static Variant Add(Variant a, Variant b)
    {
        switch (a.VariantType)
        {
            case Variant.Type.Int: return a.AsInt64() + b.AsInt64();
            case Variant.Type.Float: return a.AsDouble() + b.AsDouble();
            case Variant.Type.Vector2: return a.AsVector2() + b.AsVector2();
            case Variant.Type.Vector2I: return a.AsVector2I() + b.AsVector2I();
            case Variant.Type.Vector3: return a.AsVector3() + b.AsVector3();
            case Variant.Type.Vector3I: return a.AsVector3I() + b.AsVector3I();
            case Variant.Type.Vector4: return a.AsVector4() + b.AsVector4();
            case Variant.Type.Color: return a.AsColor() + b.AsColor();
            case Variant.Type.Quaternion: return a.AsQuaternion() * b.AsQuaternion();
            case Variant.Type.Rect2: { Rect2 x = a.AsRect2(), y = b.AsRect2(); return new Rect2(x.Position + y.Position, x.Size + y.Size); }
            default: return b;
        }
    }

    // final - initial.
    internal static Variant Subtract(Variant a, Variant b)
    {
        switch (a.VariantType)
        {
            case Variant.Type.Int: return a.AsInt64() - b.AsInt64();
            case Variant.Type.Float: return a.AsDouble() - b.AsDouble();
            case Variant.Type.Vector2: return a.AsVector2() - b.AsVector2();
            case Variant.Type.Vector2I: return a.AsVector2I() - b.AsVector2I();
            case Variant.Type.Vector3: return a.AsVector3() - b.AsVector3();
            case Variant.Type.Vector3I: return a.AsVector3I() - b.AsVector3I();
            case Variant.Type.Vector4: return a.AsVector4() - b.AsVector4();
            case Variant.Type.Color: return a.AsColor() - b.AsColor();
            case Variant.Type.Quaternion: return b.AsQuaternion().Inverse() * a.AsQuaternion();
            case Variant.Type.Rect2: { Rect2 x = a.AsRect2(), y = b.AsRect2(); return new Rect2(x.Position - y.Position, x.Size - y.Size); }
            default: return a;
        }
    }
}

/// <summary>One thing a Tween does (Godot's Tweener).</summary>
public abstract class Tweener : RefCounted
{
    public new class SignalName : GodotObject.SignalName
    {
        public static readonly StringName Finished = "finished";
    }

    public event Action Finished { add => Connect(SignalName.Finished, Callable.From(value)); remove => Disconnect(SignalName.Finished, Callable.From(value)); }

    internal Tween Owner = null!;
    protected double Elapsed;
    protected bool Done;

    internal virtual void Start() { Elapsed = 0; Done = false; }

    // Advances by `delta`, leaving in it what is unused. True while the tweener is still active.
    internal abstract bool Step(ref double delta);

    protected void Complete() { Done = true; EmitSignal(SignalName.Finished); }
}

/// <summary>Animates one property (Godot's PropertyTweener).</summary>
public class PropertyTweener : Tweener
{
    private readonly GodotObject _target;
    private readonly string _property;
    private readonly Variant _baseFinal;
    private readonly double _duration;
    private Variant _initial, _final, _delta;
    private bool _doContinue = true, _doContinueDelayed, _relative;
    private double _delay;
    private Tween.TransitionType? _trans;
    private Tween.EaseType? _ease;
    private Func<float, float>? _custom;

    internal PropertyTweener(GodotObject target, NodePath property, Variant finalVal, double duration)
    {
        _target = target;
        _property = property.ToString();
        _baseFinal = finalVal;
        _duration = duration;
        _initial = target.Get(_property);
        _final = finalVal;
        _delta = Tween.Subtract(finalVal, _initial);
    }

    /// <summary>Starts from this value instead of the property's value when the tweener begins.</summary>
    public PropertyTweener From(Variant value) { _initial = value; _doContinue = false; return this; }

    /// <summary>Starts from the property's value at the moment the tweener begins (the default).</summary>
    public PropertyTweener FromCurrent() { _doContinue = true; return this; }

    /// <summary>Takes the final value as an offset from the start.</summary>
    public PropertyTweener AsRelative() { _relative = true; return this; }

    public PropertyTweener SetTrans(Tween.TransitionType trans) { _trans = trans; return this; }

    public PropertyTweener SetEase(Tween.EaseType ease) { _ease = ease; return this; }

    /// <summary>Waits this long before the property starts to change.</summary>
    public PropertyTweener SetDelay(double delay) { _delay = delay; return this; }

    /// <summary>Replaces the curve with a function of the progress (0 to 1) that returns the eased progress.</summary>
    public PropertyTweener SetCustomInterpolator(Func<float, float> interpolator) { _custom = interpolator; return this; }

    internal override void Start()
    {
        base.Start();
        if (!GodotObject.IsInstanceValid(_target)) return;
        if (_doContinue)
        {
            if (Math.Abs(_delay) < 1e-9) _initial = _target.Get(_property);
            else _doContinueDelayed = true;
        }
        _final = _relative ? Tween.Add(_initial, _baseFinal) : _baseFinal;
        _delta = Tween.Subtract(_final, _initial);
    }

    internal override bool Step(ref double delta)
    {
        if (Done) { delta = 0; return false; }
        if (!GodotObject.IsInstanceValid(_target)) return false;
        Elapsed += delta;
        if (Elapsed < _delay) { delta = 0; return true; }
        if (_doContinueDelayed && Math.Abs(_delay) >= 1e-9)
        {
            _initial = _target.Get(_property);
            _delta = Tween.Subtract(_final, _initial);
            _doContinueDelayed = false;
        }
        double time = Math.Min(Elapsed - _delay, _duration);
        if (time < _duration)
        {
            if (_custom != null)
            {
                float f = _custom((float)(time / _duration));
                _target.Set(_property, Tween.Interpolate(_initial, _delta, f, 1.0, Tween.TransitionType.Linear, Tween.EaseType.In));
            }
            else
                _target.Set(_property, Tween.Interpolate(_initial, _delta, time, _duration, _trans ?? Owner.Trans, _ease ?? Owner.Ease));
            delta = 0;
            return true;
        }
        _target.Set(_property, _final);
        delta = Elapsed - _delay - _duration;
        Complete();
        return false;
    }
}

/// <summary>Waits (Godot's IntervalTweener).</summary>
public class IntervalTweener : Tweener
{
    private readonly double _duration;

    internal IntervalTweener(double duration) { _duration = duration; }

    internal override bool Step(ref double delta)
    {
        if (Done) { delta = 0; return false; }
        Elapsed += delta;
        if (Elapsed < _duration) { delta = 0; return true; }
        delta = Elapsed - _duration;
        Complete();
        return false;
    }
}

/// <summary>Calls a callable (Godot's CallbackTweener).</summary>
public class CallbackTweener : Tweener
{
    private readonly Callable _callback;
    private double _delay;

    internal CallbackTweener(Callable callback) { _callback = callback; }

    /// <summary>Waits this long after its step starts before calling.</summary>
    public CallbackTweener SetDelay(double delay) { _delay = delay; return this; }

    internal override bool Step(ref double delta)
    {
        if (Done) { delta = 0; return false; }
        Elapsed += delta;
        if (Elapsed < _delay) { delta = 0; return true; }
        try { if (_callback.IsValid()) _callback.Call(); }
        catch (Exception ex) { Aver.Scripting.Log.Error($"[Fabric] a tween callback threw: {ex.GetType().Name}: {ex.Message}"); }
        delta = Elapsed - _delay;
        Complete();
        return false;
    }
}

/// <summary>Calls a method with an interpolated value every frame (Godot's MethodTweener).</summary>
public class MethodTweener : Tweener
{
    private readonly Callable _method;
    private readonly Variant _from, _delta, _to;
    private readonly double _duration;
    private double _delay;
    private Tween.TransitionType? _trans;
    private Tween.EaseType? _ease;

    internal MethodTweener(Callable method, Variant from, Variant to, double duration)
    {
        _method = method; _from = from; _to = to; _duration = duration;
        _delta = Tween.Subtract(to, from);
    }

    public MethodTweener SetDelay(double delay) { _delay = delay; return this; }
    public MethodTweener SetTrans(Tween.TransitionType trans) { _trans = trans; return this; }
    public MethodTweener SetEase(Tween.EaseType ease) { _ease = ease; return this; }

    internal override bool Step(ref double delta)
    {
        if (Done) { delta = 0; return false; }
        Elapsed += delta;
        if (Elapsed < _delay) { delta = 0; return true; }
        double time = Math.Min(Elapsed - _delay, _duration);
        Variant current = time < _duration ? Tween.Interpolate(_from, _delta, time, _duration, _trans ?? Owner.Trans, _ease ?? Owner.Ease) : _to;
        try { if (_method.IsValid()) _method.Call(current); }
        catch (Exception ex) { Aver.Scripting.Log.Error($"[Fabric] a tween method threw: {ex.GetType().Name}: {ex.Message}"); }
        if (time < _duration) { delta = 0; return true; }
        delta = Elapsed - _delay - _duration;
        Complete();
        return false;
    }
}

/// <summary>Plays another tween as a step (Godot's SubtweenTweener).</summary>
public class SubtweenTweener : Tweener
{
    private readonly Tween _sub;
    private double _delay;

    internal SubtweenTweener(Tween sub) { _sub = sub; sub.Stop(); }

    public SubtweenTweener SetDelay(double delay) { _delay = delay; return this; }

    internal override void Start() { base.Start(); _sub.Stop(); }

    internal override bool Step(ref double delta)
    {
        if (Done) { delta = 0; return false; }
        Elapsed += delta;
        if (Elapsed < _delay) { delta = 0; return true; }
        double use = Math.Min(delta, Elapsed - _delay);
        bool alive = _sub.CustomStep(use);
        if (alive && _sub.IsValid()) { delta = 0; return true; }
        Complete();
        return false;
    }
}
