// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// InputEvents: input as events routed through the actors (input, unhandled input and
// mark-as-handled), built each frame from the polled state.

namespace Aver.Framework;

/// <summary>One piece of input, routed to <see cref="AverActor.OnInput"/>, then the game UI, then
/// <see cref="AverActor.OnUnhandledInput"/>, until something handles it.</summary>
public abstract class InputEvent
{
    /// <summary>Set by <see cref="Input.SetInputAsHandled"/> (or the UI): the event goes no further.</summary>
    public bool Handled { get; internal set; }

    /// <summary>Whether this is a press. Keys, buttons and actions only; false for motion.</summary>
    public virtual bool Pressed => false;

    /// <summary>Whether the action was pressed this frame and this event is a press (an action-pressed test on an
    /// event). An approximation: it does not check this event is one of the action's bindings.</summary>
    public bool IsActionPressed(string action) => Pressed && InputAction.ActionPressedThisFrame(action);

    /// <summary>Whether the action was released this frame and this event is a release.</summary>
    public bool IsActionReleased(string action) => !Pressed && this is not InputEventMouseMotion and not InputEventMouseWheel
                                                   && InputAction.ActionReleasedThisFrame(action);

    // Mouse events go to the UI when it wants the pointer, keyboard events when it wants the keyboard.
    internal virtual bool IsPointer => false;
    internal virtual bool IsKeyboard => false;
}

/// <summary>A key going down or up.</summary>
public sealed class InputEventKey : InputEvent
{
    public InputEventKey(Key key, bool pressed) { Key = key; IsPressed = pressed; }
    public Key Key { get; }
    public bool IsPressed { get; }
    public override bool Pressed => IsPressed;
    internal override bool IsKeyboard => true;
    public override string ToString() => $"Key {Key} {(IsPressed ? "down" : "up")}";
}

/// <summary>A mouse button going down or up (<see cref="Key.MouseLeft"/>, <see cref="Key.MouseRight"/>, <see cref="Key.MouseMiddle"/>).</summary>
public sealed class InputEventMouseButton : InputEvent
{
    public InputEventMouseButton(Key button, bool pressed) { Button = button; IsPressed = pressed; }
    public Key Button { get; }
    public bool IsPressed { get; }
    public override bool Pressed => IsPressed;
    internal override bool IsPointer => true;
    public override string ToString() => $"Mouse {Button} {(IsPressed ? "down" : "up")}";
}

/// <summary>The mouse wheel turning; positive is away from the user.</summary>
public sealed class InputEventMouseWheel : InputEvent
{
    public InputEventMouseWheel(float delta) => Delta = delta;
    public float Delta { get; }
    internal override bool IsPointer => true;
    public override string ToString() => $"Wheel {Delta}";
}

/// <summary>The mouse moving, by this much since the last frame.</summary>
public sealed class InputEventMouseMotion : InputEvent
{
    public InputEventMouseMotion(float dx, float dy) { RelativeX = dx; RelativeY = dy; }
    public float RelativeX { get; }
    public float RelativeY { get; }
    internal override bool IsPointer => true;
    public override string ToString() => $"Motion {RelativeX},{RelativeY}";
}

/// <summary>A gamepad button going down or up.</summary>
public sealed class InputEventJoypadButton : InputEvent
{
    public InputEventJoypadButton(GamepadButton button, bool pressed, int pad = 0) { Button = button; IsPressed = pressed; Pad = pad; }
    public GamepadButton Button { get; }
    public bool IsPressed { get; }
    public int Pad { get; }
    public override bool Pressed => IsPressed;
    public override string ToString() => $"Pad{Pad} {Button} {(IsPressed ? "down" : "up")}";
}

/// <summary>A gamepad axis moving to a new value.</summary>
public sealed class InputEventJoypadMotion : InputEvent
{
    public InputEventJoypadMotion(GamepadAxis axis, float value, int pad = 0) { Axis = axis; Value = value; Pad = pad; }
    public GamepadAxis Axis { get; }
    public float Value { get; }
    public int Pad { get; }
    public override string ToString() => $"Pad{Pad} {Axis} {Value}";
}

// Routes events through the actors, then the UI, then the actors again (input, GUI, unhandled input).
internal static class InputRouter
{
    [ThreadStatic] private static InputEvent? s_current;

    internal static InputEvent? Current => s_current;

    // `actors` in reverse scene-tree order (the deepest, last-added first, the usual propagation order). Returns who took it:
    // "input", "ui", "unhandled" or "" for nobody.
    internal static string Route(InputEvent ev, IReadOnlyList<AverActor> actors, bool uiWantsPointer, bool uiWantsKeyboard,
                                 Action<AverActor, string, Exception>? onError = null)
    {
        InputEvent? outer = s_current;
        s_current = ev;
        try
        {
            if (Deliver(ev, actors, unhandled: false, onError)) return "input";
            if ((ev.IsPointer && uiWantsPointer) || (ev.IsKeyboard && uiWantsKeyboard)) { ev.Handled = true; return "ui"; }
            if (Deliver(ev, actors, unhandled: true, onError)) return "unhandled";
            return "";
        }
        finally { s_current = outer; }
    }

    private static bool Deliver(InputEvent ev, IReadOnlyList<AverActor> actors, bool unhandled, Action<AverActor, string, Exception>? onError)
    {
        foreach (AverActor a in actors)
        {
            if (!a.InputEnabled) continue;
            try
            {
                if (unhandled) a.OnUnhandledInput(ev);
                else a.OnInput(ev);
            }
            catch (Exception ex) { onError?.Invoke(a, unhandled ? "OnUnhandledInput" : "OnInput", ex); }
            if (ev.Handled) return true;
        }
        return false;
    }
}

// Turns the frame's polled input into events: the presses and releases the native state reports, mouse motion
// and wheel, and gamepad changes against what it saw last frame.
internal static class InputEventSource
{
    private static readonly bool[] s_padButtons = new bool[Enum.GetValues<GamepadButton>().Length];
    private static readonly float[] s_padAxes = new float[Enum.GetValues<GamepadAxis>().Length];
    private static readonly List<InputEvent> s_injected = new();

    internal static void Inject(InputEvent ev) { lock (s_injected) s_injected.Add(ev); }

    internal static List<InputEvent> Collect()
    {
        var events = new List<InputEvent>();
        foreach (Key k in Enum.GetValues<Key>())
        {
            bool mouse = k is Key.MouseLeft or Key.MouseRight or Key.MouseMiddle;
            if (Input.GetKeyDown(k)) events.Add(mouse ? new InputEventMouseButton(k, true) : new InputEventKey(k, true));
            if (Input.GetKeyUp(k)) events.Add(mouse ? new InputEventMouseButton(k, false) : new InputEventKey(k, false));
        }
        float dx = Input.MouseDeltaX, dy = Input.MouseDeltaY, wheel = Input.MouseWheel;
        if (dx != 0f || dy != 0f) events.Add(new InputEventMouseMotion(dx, dy));
        if (wheel != 0f) events.Add(new InputEventMouseWheel(wheel));
        foreach (GamepadButton b in Enum.GetValues<GamepadButton>())
        {
            bool down = Input.GetGamepadButton(b);
            if (down != s_padButtons[(int)b]) { s_padButtons[(int)b] = down; events.Add(new InputEventJoypadButton(b, down)); }
        }
        foreach (GamepadAxis ax in Enum.GetValues<GamepadAxis>())
        {
            float v = Input.GetGamepadAxis(ax);
            if (MathF.Abs(v - s_padAxes[(int)ax]) > 0.01f) { s_padAxes[(int)ax] = v; events.Add(new InputEventJoypadMotion(ax, v)); }
        }
        lock (s_injected) { events.AddRange(s_injected); s_injected.Clear(); }
        return events;
    }
}
