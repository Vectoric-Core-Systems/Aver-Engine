// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// Action-mapped input: named actions, stacked mapping contexts, and the per-frame router.

using System;
using System.Collections.Generic;
using Aver.Scene;

namespace Aver.Framework;

/// <summary>What shape of value an <see cref="InputAction"/> carries.</summary>
public enum InputValueType
{
    /// <summary>On or off — a button.</summary>
    Digital,
    /// <summary>One axis, -1..1.</summary>
    Axis1D,
    /// <summary>Two axes, carried in X and Y of a <see cref="Vec3"/>.</summary>
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

/// <summary>A named thing the player can do — "Jump", "Fire", "Move" — independent of any key.</summary>
public sealed class InputAction
{
    /// <summary>Display name, used in logs and by the mapping context.</summary>
    public string Name { get; }

    /// <summary>The shape of this action's value.</summary>
    public InputValueType ValueType { get; }

    // This frame's value and the previous frame's, refreshed by EnhancedInput.Update.
    internal Vec3 Raw, Prev;

    private InputAction(string name, InputValueType type) { Name = name; ValueType = type; }

    /// <summary>Declares a button action.</summary>
    public static InputAction Digital(string name) => Register(new InputAction(name, InputValueType.Digital));
    /// <summary>Declares a single-axis action.</summary>
    public static InputAction Axis1D(string name) => Register(new InputAction(name, InputValueType.Axis1D));
    /// <summary>Declares a two-axis action, read through <see cref="Value2D"/>.</summary>
    public static InputAction Axis2D(string name) => Register(new InputAction(name, InputValueType.Axis2D));

    /// <summary>Registers an action with the router and returns it.</summary>
    private static InputAction Register(InputAction a) { EnhancedInput.Track(a); return a; }

    /// <summary>The raw value: X for a 1D axis, X and Y for 2D, X = 0 or 1 for a button.</summary>
    public Vec3 Value2D => Raw;

    /// <summary>The single-axis value.</summary>
    public float Value1D => Raw.X;

    /// <summary>True while the action is active.</summary>
    public bool IsHeld => Active(Raw);

    /// <summary>True on the frame the action became active.</summary>
    public bool WasPressed => Active(Raw) && !Active(Prev);

    /// <summary>True on the frame the action stopped being active.</summary>
    public bool WasReleased => !Active(Raw) && Active(Prev);

    /// <summary>True when any component is outside the dead zone.</summary>
    private static bool Active(Vec3 v) => MathF.Abs(v.X) > 0.15f || MathF.Abs(v.Y) > 0.15f || MathF.Abs(v.Z) > 0.15f;

    public override string ToString() => $"{Name} ({ValueType})";
}

/// <summary>One key-to-action mapping inside a context.</summary>
internal readonly struct InputBinding
{
    public readonly InputAction Action;
    public readonly InputSource Source;
    public readonly Key         Key;
    public readonly float       Scale;
    public readonly int         Component;  // 0=X, 1=Y

    /// <summary>Builds one binding.</summary>
    public InputBinding(InputAction action, InputSource source, Key key, float scale, int component)
    { Action = action; Source = source; Key = key; Scale = scale; Component = component; }
}

/// <summary>A set of key-to-action mappings pushed and popped as a whole. Derive it and bind in the constructor.</summary>
public abstract class InputMappingContext
{
    internal readonly List<InputBinding> Bindings = new();

    /// <summary>Name shown in logs; defaults to the class name.</summary>
    public virtual string Name => GetType().Name;

    /// <summary>Binds a key to an action. <paramref name="scale"/> can negate it for an axis.</summary>
    protected void BindKey(InputAction action, Key key, float scale = 1f, int component = 0) =>
        Bindings.Add(new InputBinding(action, InputSource.Key, key, scale, component));

    /// <summary>Binds two opposed keys to one -1..1 axis.</summary>
    protected void BindAxis1D(InputAction action, Key positive, Key negative, int component = 0)
    {
        BindKey(action, positive,  1f, component);
        BindKey(action, negative, -1f, component);
    }

    /// <summary>Binds four keys to a 2D axis: X is up/down (forward), Y is right/left.</summary>
    protected void BindAxis2D(InputAction action, Key up, Key down, Key right, Key left)
    {
        BindAxis1D(action, up, down, component: 0);
        BindAxis1D(action, right, left, component: 1);
    }

    /// <summary>Binds mouse movement to a 2D action: X gets horizontal, Y gets vertical.</summary>
    protected void BindMouseLook(InputAction action, float sensitivity = 1f)
    {
        Bindings.Add(new InputBinding(action, InputSource.MouseX, Key.A, sensitivity, 0));
        Bindings.Add(new InputBinding(action, InputSource.MouseY, Key.A, sensitivity, 1));
    }

    /// <summary>Binds the wheel to a single axis.</summary>
    protected void BindMouseWheel(InputAction action, float scale = 1f) =>
        Bindings.Add(new InputBinding(action, InputSource.MouseWheel, Key.A, scale, 0));
}

/// <summary>The input router: mapping contexts go in, action values come out, once per frame.</summary>
public static class EnhancedInput
{
    /// <summary>One context at one priority.</summary>
    private sealed class Layer
    {
        public InputMappingContext Context = null!;
        public int Priority;
    }

    private static readonly List<Layer>       s_layers  = new();
    private static readonly List<InputAction> s_actions = new();

    /// <summary>Adds an action to the set updated each frame.</summary>
    internal static void Track(InputAction a) { if (!s_actions.Contains(a)) s_actions.Add(a); }

    /// <summary>Pushes a context. Higher <paramref name="priority"/> runs first and consumes the keys it binds.</summary>
    public static void AddContext(InputMappingContext context, int priority = 0)
    {
        if (context is null) return;
        RemoveContext(context);
        s_layers.Add(new Layer { Context = context, Priority = priority });
        s_layers.Sort((a, b) => b.Priority.CompareTo(a.Priority));
    }

    /// <summary>Pops a context. Safe for one that was never added.</summary>
    public static void RemoveContext(InputMappingContext context) =>
        s_layers.RemoveAll(l => ReferenceEquals(l.Context, context));

    /// <summary>Drops every context.</summary>
    public static void ClearContexts() => s_layers.Clear();

    /// <summary>How many contexts are active.</summary>
    public static int ContextCount => s_layers.Count;

    /// <summary>Recomputes every action from the current device state. Host calls this once per frame.</summary>
    internal static void Update()
    {
        for (int i = 0; i < s_actions.Count; i++)
        {
            InputAction a = s_actions[i];
            a.Prev = a.Raw;
            a.Raw  = Vec3.Zero;
        }
        if (s_layers.Count == 0) return;

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
            // Consumption is per layer, so two bindings in one context can share a key.
            for (int bi = 0; bi < bindings.Count; bi++)
                if (bindings[bi].Source == InputSource.Key) consumed.Add(bindings[bi].Key);
        }
    }

    /// <summary>Adds a value into one component of an action.</summary>
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
