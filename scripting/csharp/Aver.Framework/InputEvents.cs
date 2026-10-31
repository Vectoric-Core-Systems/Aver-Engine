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
    public bool IsActionReleased(string action) => !Pressed && this is not InputEventMouseMotion and not InputEventMouseWheel and not InputEventTouchMotion
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

/// <summary>A mouse button going down or up (<see cref="Key.MouseLeft"/>, <see cref="Key.MouseRight"/>, <see cref="Key.MouseMiddle"/>,
/// and the two side buttons <see cref="Key.MouseX1"/> and <see cref="Key.MouseX2"/>).</summary>
public sealed class InputEventMouseButton : InputEvent
{
    public InputEventMouseButton(Key button, bool pressed) { Button = button; IsPressed = pressed; }
    public Key Button { get; }
    public bool IsPressed { get; }
    public override bool Pressed => IsPressed;
    internal override bool IsPointer => true;
    public override string ToString() => $"Mouse {Button} {(IsPressed ? "down" : "up")}";
}

/// <summary>The mouse wheel turning; positive is away from the user. <see cref="Horizontal"/> is a sideways notch, positive to the right.</summary>
public sealed class InputEventMouseWheel : InputEvent
{
    public InputEventMouseWheel(float delta, float horizontal = 0f) { Delta = delta; Horizontal = horizontal; }
    public float Delta { get; }
    public float Horizontal { get; }
    internal override bool IsPointer => true;
    public override string ToString() => Horizontal != 0f ? $"Wheel {Delta},{Horizontal}" : $"Wheel {Delta}";
}

/// <summary>A finger touching down on, or lifting off, the screen.</summary>
public sealed class InputEventTouch : InputEvent
{
    public InputEventTouch(int finger, bool pressed, float x, float y, float pressure = 1f)
    { Finger = finger; IsPressed = pressed; X = x; Y = y; Pressure = pressure; }
    public int Finger { get; }
    public bool IsPressed { get; }
    /// <summary>Window pixels.</summary>
    public float X { get; }
    public float Y { get; }
    public float Pressure { get; }
    public override bool Pressed => IsPressed;
    internal override bool IsPointer => true;
    public override string ToString() => $"Touch {Finger} {(IsPressed ? "down" : "up")} {X},{Y}";
}

/// <summary>A finger that is down moving.</summary>
public sealed class InputEventTouchMotion : InputEvent
{
    public InputEventTouchMotion(int finger, float x, float y, float relativeX, float relativeY, float pressure = 1f)
    { Finger = finger; X = x; Y = y; RelativeX = relativeX; RelativeY = relativeY; Pressure = pressure; }
    public int Finger { get; }
    public float X { get; }
    public float Y { get; }
    public float RelativeX { get; }
    public float RelativeY { get; }
    public float Pressure { get; }
    internal override bool IsPointer => true;
    public override string ToString() => $"TouchMotion {Finger} {X},{Y}";
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
    private static readonly bool[][] s_padButtons = NewPads(() => new bool[Enum.GetValues<GamepadButton>().Length]);
    private static readonly float[][] s_padAxes = NewPads(() => new float[Enum.GetValues<GamepadAxis>().Length]);
    private static readonly bool[] s_touchDown = new bool[Touch.MaxFingers];
    private static readonly (float X, float Y)[] s_touchAt = new (float, float)[Touch.MaxFingers];
    private static readonly List<InputEvent> s_injected = new();

    private static T[] NewPads<T>(Func<T> make)
    {
        var a = new T[Gamepads.MaxPads];
        for (int i = 0; i < a.Length; i++) a[i] = make();
        return a;
    }

    internal static void Inject(InputEvent ev) { lock (s_injected) s_injected.Add(ev); }

    internal static List<InputEvent> Collect()
    {
        var events = new List<InputEvent>();
        foreach (Key k in Enum.GetValues<Key>())
        {
            // A wheel notch arrives as the wheel event below; its key slots are for polling and action bindings.
            if (k is Key.MouseWheelUp or Key.MouseWheelDown) continue;
            if (k is Key.MouseWheelLeft or Key.MouseWheelRight)
            {
                if (Input.GetKeyDown(k)) events.Add(new InputEventMouseWheel(0f, k == Key.MouseWheelRight ? 1f : -1f));
                continue;
            }
            bool mouse = k is Key.MouseLeft or Key.MouseRight or Key.MouseMiddle or Key.MouseX1 or Key.MouseX2;
            if (Input.GetKeyDown(k)) events.Add(mouse ? new InputEventMouseButton(k, true) : new InputEventKey(k, true));
            if (Input.GetKeyUp(k)) events.Add(mouse ? new InputEventMouseButton(k, false) : new InputEventKey(k, false));
        }
        float dx = Input.MouseDeltaX, dy = Input.MouseDeltaY, wheel = Input.MouseWheel;
        if (dx != 0f || dy != 0f) events.Add(new InputEventMouseMotion(dx, dy));
        if (wheel != 0f) events.Add(new InputEventMouseWheel(wheel));
        for (int pad = 0; pad < Gamepads.MaxPads; pad++)
        {
            // A pad that is not plugged in reads all up and zero, which is also what its remembered state ends at when it leaves.
            foreach (GamepadButton b in Enum.GetValues<GamepadButton>())
            {
                bool down = Input.GetGamepadButton(b, pad);
                if (down != s_padButtons[pad][(int)b]) { s_padButtons[pad][(int)b] = down; events.Add(new InputEventJoypadButton(b, down, pad)); }
            }
            foreach (GamepadAxis ax in Enum.GetValues<GamepadAxis>())
            {
                float v = Input.GetGamepadAxis(ax, pad);
                if (MathF.Abs(v - s_padAxes[pad][(int)ax]) > 0.01f) { s_padAxes[pad][(int)ax] = v; events.Add(new InputEventJoypadMotion(ax, v, pad)); }
            }
        }
        for (int f = 0; f < Touch.MaxFingers; f++)
        {
            bool down = Touch.IsDown(f, out float x, out float y, out float pressure);
            if (down && !s_touchDown[f]) events.Add(new InputEventTouch(f, true, x, y, pressure));
            else if (down && (x != s_touchAt[f].X || y != s_touchAt[f].Y))
                events.Add(new InputEventTouchMotion(f, x, y, x - s_touchAt[f].X, y - s_touchAt[f].Y, pressure));
            else if (!down && s_touchDown[f]) events.Add(new InputEventTouch(f, false, s_touchAt[f].X, s_touchAt[f].Y, 0f));
            s_touchDown[f] = down;
            if (down) s_touchAt[f] = (x, y);
        }
        lock (s_injected) { events.AddRange(s_injected); s_injected.Clear(); }
        return events;
    }
}
