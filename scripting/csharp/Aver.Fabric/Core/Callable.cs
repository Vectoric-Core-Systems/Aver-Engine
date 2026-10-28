// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Callable, Signal and the awaiter behind `await ToSignal(...)`.

using System.Reflection;
using System.Runtime.CompilerServices;
using Aver.Framework;

namespace Aver.Fabric;

/// <summary>Something that can be called: a method on an object, a delegate, or either with arguments bound (Godot's Callable).</summary>
public readonly struct Callable : IEquatable<Callable>
{
    private readonly GodotObject? _target;
    private readonly string? _method;
    private readonly Delegate? _delegate;
    private readonly bool _variadic;               // _delegate is an Action<Variant[]> that takes the arguments as they are
    private readonly object? _inner;               // a boxed Callable this one wraps (Bind / Unbind)
    private readonly Variant[]? _bound;
    private readonly int _unbind;

    /// <summary>A method of an object, by name. Names are matched as Godot names and as C# names (see Object.Call).</summary>
    public Callable(GodotObject target, StringName method)
    {
        _target = target;
        _method = method?.ToString();
        _delegate = null; _variadic = false; _inner = null; _bound = null; _unbind = 0;
    }

    private Callable(Delegate d, bool variadic)
    {
        _delegate = d; _variadic = variadic;
        _target = d.Target as GodotObject;
        _method = null; _inner = null; _bound = null; _unbind = 0;
    }

    private Callable(Callable inner, Variant[]? bound, int unbind)
    {
        _inner = inner; _bound = bound; _unbind = unbind;
        _target = null; _method = null; _delegate = null; _variadic = false;
    }

    public static Callable From(Delegate d) => new(d ?? throw new ArgumentNullException(nameof(d)), false);
    public static Callable From(Action action) => new(action, false);
    public static Callable From<T0>(Action<T0> action) => new(action, false);
    public static Callable From<T0, T1>(Action<T0, T1> action) => new(action, false);
    public static Callable From<T0, T1, T2>(Action<T0, T1, T2> action) => new(action, false);
    public static Callable From<T0, T1, T2, T3>(Action<T0, T1, T2, T3> action) => new(action, false);
    public static Callable From<TResult>(Func<TResult> func) => new(func, false);
    public static Callable From<T0, TResult>(Func<T0, TResult> func) => new(func, false);
    public static Callable From<T0, T1, TResult>(Func<T0, T1, TResult> func) => new(func, false);
    public static Callable From<T0, T1, T2, TResult>(Func<T0, T1, T2, TResult> func) => new(func, false);

    // Receives every argument as emitted, whatever their number; the awaiter and the engine's own listeners use it.
    internal static Callable FromVariadic(Action<Variant[]> action) => new(action, true);

    /// <summary>The object the method is called on, if any.</summary>
    public GodotObject? Target => _inner is Callable c ? c.Target : _target;

    /// <summary>The method name, if this is a method callable.</summary>
    public StringName? Method => _inner is Callable c ? c.Method : _method is null ? null : new StringName(_method);

    /// <summary>The delegate, if this is a delegate callable.</summary>
    public Delegate? Delegate => _inner is Callable c ? c.Delegate : _delegate;

    public bool IsNull => _inner is null && _target is null && _delegate is null;
    public bool IsCustom => _delegate != null || _inner != null;

    /// <summary>True when there is something to call: a live target, or a delegate.</summary>
    public bool IsValid()
    {
        if (_inner is Callable c) return c.IsValid();
        if (_delegate != null) return _target is null || _target.IsAlive;
        return _target != null && _target.IsAlive;
    }

    /// <summary>Calls it. Throws if the target is gone or the method does not exist.</summary>
    public Variant Call(params Variant[] args)
    {
        args ??= System.Array.Empty<Variant>();
        if (_inner is Callable inner)
        {
            Variant[] a = _unbind > 0 ? args[..Math.Max(0, args.Length - _unbind)] : args;
            if (_bound is { Length: > 0 }) a = a.Concat(_bound).ToArray();
            return inner.Call(a);
        }
        if (_delegate != null) return InvokeDelegate(args);
        if (_target is null) throw new InvalidOperationException("Cannot call a null Callable.");
        if (!_target.IsAlive) throw new ObjectDisposedException(_target.GetType().Name, "The target of the Callable was freed.");
        return _target.Call(_method!, args);
    }

    /// <summary>Calls it with the arguments in an array.</summary>
    public Variant Callv(Collections.Array arguments) => Call(arguments.ToArray());

    /// <summary>Calls it when the current tick pass ends (Godot's call_deferred).</summary>
    public void CallDeferred(params Variant[] args)
    {
        Callable self = this;
        Deferred.Call(() => { if (self.IsValid()) self.Call(args); });
    }

    /// <summary>A callable that passes <paramref name="args"/> after whatever it is called with.</summary>
    public Callable Bind(params Variant[] args) => new(this, args, 0);

    /// <summary>A callable that drops the last <paramref name="argCount"/> arguments it is called with.</summary>
    public Callable Unbind(int argCount) => new(this, null, argCount);

    private Variant InvokeDelegate(Variant[] args)
    {
        if (_variadic)
        {
            ((Action<Variant[]>)_delegate!)(args);
            return default;
        }
        MethodInfo m = _delegate!.Method;
        ParameterInfo[] ps = m.GetParameters();
        // A lambda with default parameters (func(x, y = 2)) may be called with fewer arguments.
        int required = ps.Count(p => !p.IsOptional);
        if (args.Length > ps.Length || args.Length < required)
            throw new ArgumentException($"The delegate expects {ps.Length} argument(s) but was called with {args.Length}.");
        var values = new object?[ps.Length];
        for (int i = 0; i < ps.Length; i++) values[i] = i < args.Length ? args[i].ToType(ps[i].ParameterType) : ps[i].DefaultValue;
        try
        {
            object? r = _delegate.DynamicInvoke(values);
            return r == null ? default : Variant.FromObject(r);
        }
        catch (TargetInvocationException ex) when (ex.InnerException != null)
        {
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(ex.InnerException).Throw();
            throw;
        }
    }

    public bool Equals(Callable other)
    {
        if (_inner is Callable a && other._inner is Callable b)
            return _unbind == other._unbind && a.Equals(b) && (_bound ?? System.Array.Empty<Variant>()).SequenceEqual(other._bound ?? System.Array.Empty<Variant>());
        if (_inner != null || other._inner != null) return false;
        if (_delegate != null || other._delegate != null) return _delegate != null && other._delegate != null && _delegate.Equals(other._delegate);
        return ReferenceEquals(_target, other._target) && _method == other._method;
    }

    public override bool Equals(object? obj) => obj is Callable c && Equals(c);
    public override int GetHashCode() => _delegate?.GetHashCode() ?? HashCode.Combine(_target is null ? 0 : RuntimeHelpers.GetHashCode(_target), _method, _unbind);
    public static bool operator ==(Callable l, Callable r) => l.Equals(r);
    public static bool operator !=(Callable l, Callable r) => !l.Equals(r);
    public override string ToString() => _inner != null ? "Callable(bound)" : _delegate != null ? $"Callable({_delegate.Method.Name})" : _target is null ? "null::null" : $"{_target.GetType().Name}::{_method}";
}

/// <summary>A signal of an object: its owner and name (Godot's Signal).</summary>
public readonly struct Signal : IEquatable<Signal>
{
    public GodotObject? Owner { get; }
    public StringName? Name { get; }

    public Signal(GodotObject owner, StringName name) { Owner = owner; Name = name; }

    public bool IsNull => Owner is null || Name is null;
    public Error Connect(Callable callable, uint flags = 0) => Owner is null || Name is null ? Error.InvalidParameter : Owner.Connect(Name, callable, flags);
    public void Disconnect(Callable callable) { if (Owner != null && Name != null) Owner.Disconnect(Name, callable); }
    public bool IsConnected(Callable callable) => Owner != null && Name != null && Owner.IsConnected(Name, callable);
    public Error Emit(params Variant[] args) => Owner is null || Name is null ? Error.InvalidParameter : Owner.EmitSignal(Name, args);

    public bool Equals(Signal other) => ReferenceEquals(Owner, other.Owner) && Name == other.Name;
    public override bool Equals(object? obj) => obj is Signal s && Equals(s);
    public override int GetHashCode() => HashCode.Combine(Owner is null ? 0 : RuntimeHelpers.GetHashCode(Owner), Name);
    public static bool operator ==(Signal l, Signal r) => l.Equals(r);
    public static bool operator !=(Signal l, Signal r) => !l.Equals(r);
    public override string ToString() => $"{Owner?.GetType().Name}::{Name}";
}

/// <summary>What <c>await ToSignal(source, "name")</c> awaits. It connects when created, so a signal emitted before the
/// await is not missed, and it resumes inside the emit, like Godot's.</summary>
public readonly struct SignalAwaiter : INotifyCompletion
{
    private sealed class State
    {
        public bool Done;
        public Variant[] Result = System.Array.Empty<Variant>();
        public Action? Continuation;
    }

    private readonly State _state;

    public SignalAwaiter(GodotObject source, StringName signal, GodotObject? target = null)
    {
        _state = new State();
        State s = _state;
        Callable cb = Callable.FromVariadic(args =>
        {
            s.Result = args;
            s.Done = true;
            Action? k = s.Continuation;
            s.Continuation = null;
            k?.Invoke();
        });
        source.Connect(signal, cb, (uint)GodotObject.ConnectFlags.OneShot);
    }

    public bool IsCompleted => _state.Done;
    public void OnCompleted(Action continuation)
    {
        if (_state.Done) continuation();
        else _state.Continuation += continuation;
    }
    public Variant[] GetResult() => _state.Result;
    public SignalAwaiter GetAwaiter() => this;
}
