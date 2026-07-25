using System;
using System.Collections.Generic;
using Aver.Scene;

namespace Aver.Framework;

/// <summary>What shape of value an <see cref="InputAction"/> carries.</summary>
public enum InputValueType
{
    /// <summary>On or off — a button.</summary>
    Digital,
    /// <summary>One axis, -1..1 — a trigger, or a throttle.</summary>
    Axis1D,
    /// <summary>Two axes — movement, or a look delta. Carried in X and Y of a <see cref="Vec3"/>.</summary>
    Axis2D,
}

/// <summary>Where one binding reads from.</summary>
public enum InputSource
{
    /// <summary>A key or mouse button, held = 1.</summary>
    Key,
    /// <summary>Mouse movement this frame, in pixels.</summary>
    MouseX,
    /// <summary>Mouse movement this frame, in pixels.</summary>
    MouseY,
    /// <summary>Wheel notches this frame.</summary>
    MouseWheel,
}

/// <summary>
/// A thing the player can DO, named once and referred to everywhere — "Jump", "Fire", "Move" — as
/// opposed to a key, which is merely one way to ask for it.
/// </summary>
/// <remarks>
/// The point of the indirection is that gameplay never mentions a key. A pawn asks whether
/// <c>Fire</c> happened; which key that is belongs to an <see cref="InputMappingContext"/>, and can
/// change per context (on foot vs in a menu vs driving) without gameplay knowing.
///
/// <para>Declare them once, usually as <c>static readonly</c> fields on a class of your own, and
/// share that class between the pawn that reads them and the context that binds them.</para>
/// </remarks>
public sealed class InputAction
{
    /// <summary>Display name, used in logs and by the mapping context.</summary>
    public string Name { get; }

    /// <summary>The shape of this action's value.</summary>
    public InputValueType ValueType { get; }

    // Updated once per frame by EnhancedInput.Update. Two frames are kept so a script can ask what
    // CHANGED, which is what almost every button actually wants.
    internal Vec3 Raw, Prev;

    private InputAction(string name, InputValueType type) { Name = name; ValueType = type; }

    /// <summary>A button.</summary>
    public static InputAction Digital(string name) => Register(new InputAction(name, InputValueType.Digital));
    /// <summary>A single axis.</summary>
    public static InputAction Axis1D(string name) => Register(new InputAction(name, InputValueType.Axis1D));
    /// <summary>Two axes, read through <see cref="Value2D"/>.</summary>
    public static InputAction Axis2D(string name) => Register(new InputAction(name, InputValueType.Axis2D));

    private static InputAction Register(InputAction a) { EnhancedInput.Track(a); return a; }

    /// <summary>The raw value: X for a 1D axis, X and Y for 2D, X = 0 or 1 for a button.</summary>
    public Vec3 Value2D => Raw;

    /// <summary>The single-axis value.</summary>
    public float Value1D => Raw.X;

    /// <summary>True while the action is active — a button held, or an axis away from centre.</summary>
    public bool IsHeld => Active(Raw);

    /// <summary>True on the frame the action became active. The usual test for a jump or a shot.</summary>
    public bool WasPressed => Active(Raw) && !Active(Prev);

    /// <summary>True on the frame the action stopped being active.</summary>
    public bool WasReleased => !Active(Raw) && Active(Prev);

    // A small dead zone, so a stick resting slightly off centre does not read as held for ever.
    private static bool Active(Vec3 v) => MathF.Abs(v.X) > 0.15f || MathF.Abs(v.Y) > 0.15f || MathF.Abs(v.Z) > 0.15f;

    public override string ToString() => $"{Name} ({ValueType})";
}

/// <summary>One key-to-action mapping inside a context.</summary>
internal readonly struct InputBinding
{
    public readonly InputAction Action;
    public readonly InputSource Source;
    public readonly Key         Key;
    public readonly float       Scale;      // negate an axis, or scale a look
    public readonly int         Component;  // which component of the action's value this feeds: 0=X, 1=Y

    public InputBinding(InputAction action, InputSource source, Key key, float scale, int component)
    { Action = action; Source = source; Key = key; Scale = scale; Component = component; }
}

/// <summary>
/// A set of key-to-action mappings that can be pushed and popped as a whole — the "what do the
/// controls mean right now" layer.
/// </summary>
/// <remarks>
/// Derive it and bind in the constructor. Contexts stack by priority, and a HIGHER-priority context
/// consumes the keys it binds, so a lower one never sees them: push a menu or vehicle context over
/// the on-foot one and the walking bindings stop firing without anything having to disable them.
///
/// <code>
/// public sealed class OnFoot : InputMappingContext
/// {
///     public OnFoot()
///     {
///         BindAxis2D(GameActions.Move, Key.W, Key.S, Key.D, Key.A);
///         BindMouseLook(GameActions.Look);
///         BindKey(GameActions.Jump, Key.Space);
///         BindKey(GameActions.Fire, Key.MouseLeft);
///     }
/// }
/// </code>
/// </remarks>
public abstract class InputMappingContext
{
    internal readonly List<InputBinding> Bindings = new();

    /// <summary>Name shown in logs; defaults to the class name.</summary>
    public virtual string Name => GetType().Name;

    /// <summary>Bind a key to an action. <paramref name="scale"/> can negate it for an axis.</summary>
    protected void BindKey(InputAction action, Key key, float scale = 1f, int component = 0) =>
        Bindings.Add(new InputBinding(action, InputSource.Key, key, scale, component));

    /// <summary>Bind two opposed keys to one axis — the classic "A and D make a -1..1".</summary>
    protected void BindAxis1D(InputAction action, Key positive, Key negative, int component = 0)
    {
        BindKey(action, positive,  1f, component);
        BindKey(action, negative, -1f, component);
    }

    /// <summary>
    /// Bind four keys to a 2D axis. X is the <paramref name="up"/>/<paramref name="down"/> pair and Y
    /// the <paramref name="right"/>/<paramref name="left"/> pair, which matches the engine's own axes:
    /// X is forward, Y is right.
    /// </summary>
    protected void BindAxis2D(InputAction action, Key up, Key down, Key right, Key left)
    {
        BindAxis1D(action, up, down, component: 0);
        BindAxis1D(action, right, left, component: 1);
    }

    /// <summary>Bind mouse movement to a 2D action: X gets horizontal, Y gets vertical.</summary>
    protected void BindMouseLook(InputAction action, float sensitivity = 1f)
    {
        Bindings.Add(new InputBinding(action, InputSource.MouseX, Key.A, sensitivity, 0));
        Bindings.Add(new InputBinding(action, InputSource.MouseY, Key.A, sensitivity, 1));
    }

    /// <summary>Bind the wheel to a single axis.</summary>
    protected void BindMouseWheel(InputAction action, float scale = 1f) =>
        Bindings.Add(new InputBinding(action, InputSource.MouseWheel, Key.A, scale, 0));
}

/// <summary>
/// The input router: mapping contexts go in, action values come out.
/// </summary>
/// <remarks>
/// Evaluated once per frame, before any gameplay tick, so every actor in a frame reads the same
/// input — a per-actor poll would let two pawns disagree about whether a key was pressed this frame
/// depending on their tick order.
/// </remarks>
public static class EnhancedInput
{
    private sealed class Layer
    {
        public InputMappingContext Context = null!;
        public int Priority;
    }

    private static readonly List<Layer>       s_layers  = new();
    private static readonly List<InputAction> s_actions = new();

    internal static void Track(InputAction a) { if (!s_actions.Contains(a)) s_actions.Add(a); }

    /// <summary>
    /// Push a context. Higher <paramref name="priority"/> is evaluated first and CONSUMES the keys it
    /// binds, so a lower context never sees them.
    /// </summary>
    public static void AddContext(InputMappingContext context, int priority = 0)
    {
        if (context is null) return;
        RemoveContext(context);
        s_layers.Add(new Layer { Context = context, Priority = priority });
        // Highest first, so evaluation order IS priority order.
        s_layers.Sort((a, b) => b.Priority.CompareTo(a.Priority));
    }

    /// <summary>Pop a context. Safe to call for one that was never added.</summary>
    public static void RemoveContext(InputMappingContext context) =>
        s_layers.RemoveAll(l => ReferenceEquals(l.Context, context));

    /// <summary>Drop every context. Called when a play session ends.</summary>
    public static void ClearContexts() => s_layers.Clear();

    /// <summary>How many contexts are active.</summary>
    public static int ContextCount => s_layers.Count;

    /// <summary>
    /// Recompute every action from the current device state. Called by the host once per frame,
    /// before the first tick group.
    /// </summary>
    internal static void Update()
    {
        for (int i = 0; i < s_actions.Count; i++)
        {
            InputAction a = s_actions[i];
            a.Prev = a.Raw;
            a.Raw  = Vec3.Zero;
        }
        if (s_layers.Count == 0) return;

        // A key bound by a higher-priority context is consumed and invisible to lower ones. This is
        // the behaviour that makes contexts worth having: pushing a menu context suppresses the
        // walking bindings without anything having to know they exist.
        var consumed = new HashSet<Key>();

        for (int li = 0; li < s_layers.Count; li++)
        {
            List<InputBinding> bindings = s_layers[li].Context.Bindings;
            for (int bi = 0; bi < bindings.Count; bi++)
            {
                InputBinding b = bindings[bi];
                if (b.Source == InputSource.Key && consumed.Contains(b.Key)) continue;

                float v = b.Source switch
                {
                    InputSource.Key        => Input.GetKey(b.Key) ? 1f : 0f,
                    InputSource.MouseX     => Input.MouseDeltaX,
                    InputSource.MouseY     => Input.MouseDeltaY,
                    InputSource.MouseWheel => Input.MouseWheel,
                    _                      => 0f,
                };
                if (v == 0f) continue;

                Accumulate(b.Action, b.Component, v * b.Scale);
            }
            // Consumption is per LAYER, never per binding, and so it happens only once the whole
            // context has been evaluated. Marking a key consumed as it is read would let one binding
            // hide the same key from another binding in the SAME context -- which is a real shape:
            // a key can legitimately feed two actions at once.
            for (int bi = 0; bi < bindings.Count; bi++)
                if (bindings[bi].Source == InputSource.Key) consumed.Add(bindings[bi].Key);
        }
    }

    private static void Accumulate(InputAction a, int component, float value)
    {
        switch (component)
        {
            case 0:  a.Raw.X += value; break;
            case 1:  a.Raw.Y += value; break;
            default: a.Raw.Z += value; break;
        }
    }
}
