// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The Input singleton and InputMap, with Godot's names. Two sources feed them: the engine (the project's input scheme, which the
// converter wrote from project.godot, read through the framework's actions and polled keys) and a managed layer that takes events
// scripts feed in (ParseInputEvent, ActionPress) the way Godot's input map does. Without the engine only the managed layer exists.

using Aver.Framework;

namespace Aver.Fabric;

/// <summary>The project's actions and the events that trigger them (Godot's InputMap).</summary>
public static class InputMap
{
    private sealed class ManagedAction
    {
        public float Deadzone = 0.5f;
        public readonly List<InputEvent> Events = new();
    }

    private static readonly Dictionary<string, ManagedAction> s_actions = new(StringComparer.Ordinal);

    internal static void ResetForTests() => s_actions.Clear();

    public static bool HasAction(StringName action) =>
        s_actions.ContainsKey(action.ToString()) || (NodeHost.Current.IsEngine && InputAction.Find(action.ToString()) != null);

    /// <summary>Adds an action with no events. A name that exists already is an error and changes nothing.</summary>
    public static void AddAction(StringName action, float deadzone = 0.5f)
    {
        if (HasAction(action))
        {
            Aver.Scripting.Log.Error($"[Fabric] InputMap.AddAction: the action \"{action}\" exists already");
            return;
        }
        s_actions[action.ToString()] = new ManagedAction { Deadzone = deadzone };
    }

    public static void EraseAction(StringName action)
    {
        if (!s_actions.Remove(action.ToString()))
            Aver.Scripting.Log.Error($"[Fabric] InputMap.EraseAction: no action \"{action}\"");
    }

    /// <summary>Every action, the project's and the ones added by script.</summary>
    public static Collections.Array<StringName> GetActions()
    {
        var names = new List<string>();
        if (NodeHost.Current.IsEngine)
            for (int i = 1, n = FwIn.aver_fw_action_count(); i <= n; i++)
                if (InputAction.FromHandle(i) is { } a) names.Add(a.Name);
        foreach (string k in s_actions.Keys) if (!names.Contains(k)) names.Add(k);
        return new Collections.Array<StringName>(names.Select(s => new StringName(s)));
    }

    public static void ActionSetDeadzone(StringName action, float deadzone)
    {
        if (s_actions.TryGetValue(action.ToString(), out ManagedAction? a)) a.Deadzone = deadzone;
        else Aver.Scripting.Log.Error($"[Fabric] InputMap.ActionSetDeadzone: no action \"{action}\" that script added");
    }

    public static float ActionGetDeadzone(StringName action) =>
        s_actions.TryGetValue(action.ToString(), out ManagedAction? a) ? a.Deadzone : 0.5f;

    public static void ActionAddEvent(StringName action, InputEvent @event)
    {
        if (!s_actions.TryGetValue(action.ToString(), out ManagedAction? a))
        {
            if (!HasAction(action)) { Aver.Scripting.Log.Error($"[Fabric] InputMap.ActionAddEvent: no action \"{action}\""); return; }
            a = s_actions[action.ToString()] = new ManagedAction();
        }
        if (!a.Events.Contains(@event)) a.Events.Add(@event);
    }

    public static bool ActionHasEvent(StringName action, InputEvent @event) =>
        s_actions.TryGetValue(action.ToString(), out ManagedAction? a) && a.Events.Any(e => ReferenceEquals(e, @event) || (e.GetType() == @event.GetType() && e.IsMatch(@event, true) && @event.IsMatch(e, true)));

    public static void ActionEraseEvent(StringName action, InputEvent @event)
    {
        if (s_actions.TryGetValue(action.ToString(), out ManagedAction? a))
            a.Events.RemoveAll(e => ReferenceEquals(e, @event) || (e.GetType() == @event.GetType() && e.IsMatch(@event, true) && @event.IsMatch(e, true)));
    }

    public static void ActionEraseEvents(StringName action)
    {
        if (s_actions.TryGetValue(action.ToString(), out ManagedAction? a)) a.Events.Clear();
    }

    /// <summary>The events that trigger the action: the ones script added, then the project's bindings (the engine's).</summary>
    public static Collections.Array<InputEvent> ActionGetEvents(StringName action)
    {
        var list = new List<InputEvent>();
        if (s_actions.TryGetValue(action.ToString(), out ManagedAction? a)) list.AddRange(a.Events);
        list.AddRange(NativeEvents(action.ToString()));
        return new Collections.Array<InputEvent>(list);
    }

    // The project's bindings of an action, as Godot events.
    private static IEnumerable<InputEvent> NativeEvents(string name)
    {
        if (!NodeHost.Current.IsEngine) yield break;
        for (int slot = 0; slot < 16; slot++)
        {
            if (!EnhancedInput.TryGetBindingKey(name, slot, out int raw, out InputSource source)) yield break;
            InputEvent? e = source switch
            {
                InputSource.Key when InputBridge.TryGodotKey((Aver.Framework.Key)raw, out Key g) => new InputEventKey { Keycode = g, PhysicalKeycode = g },
                InputSource.Key when InputBridge.ToMouseButton((Aver.Framework.Key)raw) is var mb && mb != MouseButton.None => new InputEventMouseButton { ButtonIndex = mb },
                InputSource.MouseWheel => new InputEventMouseButton { ButtonIndex = MouseButton.WheelUp },
                InputSource.GamepadButton => new InputEventJoypadButton { ButtonIndex = InputBridge.ToJoyButton((Aver.Framework.GamepadButton)raw) },
                InputSource.GamepadAxis => new InputEventJoypadMotion { Axis = InputBridge.ToJoyAxis((Aver.Framework.GamepadAxis)raw), AxisValue = 1f },
                _ => null,
            };
            if (e != null) yield return e;
        }
    }

    /// <summary>Whether <paramref name="event"/> triggers the action.</summary>
    public static bool EventIsAction(InputEvent @event, StringName action, bool exactMatch = false) =>
        EventGetActionStatus(@event, action, exactMatch, out _, out _, out _);

    /// <summary>The action's reading of an event: whether any of its events matches it, and if so whether it is a press and how strong.</summary>
    internal static bool EventGetActionStatus(InputEvent @event, StringName action, bool exactMatch, out bool pressed, out float strength, out float rawStrength)
    {
        pressed = false; strength = 0f; rawStrength = 0f;
        string name = action.ToString();
        float deadzone = ActionGetDeadzone(name);
        bool found = false;
        if (@event is InputEventAction ia)
        {
            // An action event names its action; no stored event is needed.
            if (ia.Action != action) return false;
            pressed = ia.Pressed;
            strength = rawStrength = pressed ? ia.Strength : 0f;
            return true;
        }
        foreach (InputEvent stored in ActionGetEvents(name))
        {
            if (!stored.ActionMatch(@event, exactMatch, deadzone, out bool p, out float s, out float r)) continue;
            // Among several matching events a press wins over a release, the stronger over the weaker.
            if (!found || (p && !pressed) || (p == pressed && s > strength)) { pressed = p; strength = s; rawStrength = r; }
            found = true;
        }
        return found;
    }
}

/// <summary>Keyboard, mouse, gamepad and action state with Godot's names (Godot's Input singleton).</summary>
public static class Input
{
    public enum MouseModeEnum { Visible = 0, Hidden = 1, Captured = 2, Confined = 3, ConfinedHidden = 4 }

    private sealed class ActionState
    {
        public bool Pressed, Exact;
        public float Strength, RawStrength;
        public long PressedProcessFrame = -1, PressedPhysicsFrame = -1, ReleasedProcessFrame = -1, ReleasedPhysicsFrame = -1;
    }

    private static readonly Dictionary<string, ActionState> s_actions = new(StringComparer.Ordinal);
    private static readonly HashSet<Key> s_keys = new();
    private static readonly HashSet<MouseButton> s_mouseButtons = new();
    private static readonly HashSet<JoyButton> s_joyButtons = new();
    private static readonly Dictionary<JoyAxis, float> s_joyAxes = new();
    private static MouseModeEnum s_mouseMode = MouseModeEnum.Captured;
    private static IDisposable? s_cursorScope;
    private static Vector2 s_mouseVelocity, s_mousePosition;

    /// <summary>True while a physics pass runs (set by the driver): "just pressed" then means this physics frame, not this process frame.</summary>
    internal static bool InPhysicsFrame;

    /// <summary>The seconds of the frame being processed.</summary>
    internal static double FrameDelta { get; private set; }

    internal static Vector2 MousePosition => s_mousePosition;

    internal static void ResetForTests()
    {
        s_actions.Clear(); s_keys.Clear(); s_mouseButtons.Clear(); s_joyButtons.Clear(); s_joyAxes.Clear();
        s_mouseVelocity = s_mousePosition = default;
        InPhysicsFrame = false;
        s_mouseMode = MouseModeEnum.Captured;
        InputMap.ResetForTests();
    }

    // Per-frame bookkeeping, run first in each process pass by the tree's driver.
    internal static void BeginFrame(double delta)
    {
        FrameDelta = delta;
        InPhysicsFrame = false;
    }

    private static long ProcessFrameNow => NodeHost.Current.IsEngine ? Aver.Framework.Engine.ProcessFrames : SceneTree.Instance.GetFrame();
    private static long PhysicsFrameNow => NodeHost.Current.IsEngine ? Aver.Framework.Engine.PhysicsFrames : SceneTree.Instance.PhysicsFrameCount;

    internal static bool HeldModifier(KeyModifierMask m) => m switch
    {
        KeyModifierMask.Shift => IsKeyPressed(Key.Shift),
        KeyModifierMask.Ctrl => IsKeyPressed(Key.Ctrl),
        KeyModifierMask.Alt => IsKeyPressed(Key.Alt),
        _ => false,
    };

    // ---- actions ----

    private static InputAction? Native(string name)
    {
        if (!NodeHost.Current.IsEngine) return null;
        try { return InputAction.Find(name); }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { return null; }
    }

    public static bool IsActionPressed(StringName action, bool exactMatch = false)
    {
        string name = action.ToString();
        if (s_actions.TryGetValue(name, out ActionState? st) && st.Pressed && (!exactMatch || st.Exact)) return true;
        return Native(name)?.IsHeld ?? false;
    }

    public static bool IsActionJustPressed(StringName action, bool exactMatch = false)
    {
        string name = action.ToString();
        if (s_actions.TryGetValue(name, out ActionState? st) && st.Pressed && (!exactMatch || st.Exact))
        {
            if (InPhysicsFrame ? st.PressedPhysicsFrame == PhysicsFrameNow : st.PressedProcessFrame == ProcessFrameNow) return true;
        }
        return Native(name)?.WasPressed ?? false;
    }

    public static bool IsActionJustReleased(StringName action, bool exactMatch = false)
    {
        string name = action.ToString();
        if (s_actions.TryGetValue(name, out ActionState? st) && !st.Pressed && (!exactMatch || st.Exact))
        {
            if (InPhysicsFrame ? st.ReleasedPhysicsFrame == PhysicsFrameNow : st.ReleasedProcessFrame == ProcessFrameNow) return true;
        }
        return Native(name)?.WasReleased ?? false;
    }

    /// <summary>How strongly the action is pressed, 0 to 1, after the action's dead zone.</summary>
    public static float GetActionStrength(StringName action, bool exactMatch = false)
    {
        string name = action.ToString();
        float managed = s_actions.TryGetValue(name, out ActionState? st) && (!exactMatch || st.Exact) ? st.Strength : 0f;
        float native = Native(name) is { } a ? Math.Clamp(MathF.Abs(a.Value1D), 0f, 1f) : 0f;
        return MathF.Max(managed, native);
    }

    /// <summary>The strength with no dead zone applied: a stick pushed less far than the dead zone still reads its value here.</summary>
    public static float GetActionRawStrength(StringName action, bool exactMatch = false)
    {
        string name = action.ToString();
        float managed = s_actions.TryGetValue(name, out ActionState? st) && (!exactMatch || st.Exact) ? st.RawStrength : 0f;
        float native = Native(name) is { } a ? Math.Clamp(MathF.Abs(a.Value1D), 0f, 1f) : 0f;
        return MathF.Max(managed, native);
    }

    /// <summary>Positive strength minus negative strength: -1 to 1.</summary>
    public static float GetAxis(StringName negativeAction, StringName positiveAction) =>
        GetActionStrength(positiveAction) - GetActionStrength(negativeAction);

    /// <summary>A direction from four actions, with a circular dead zone (a negative <paramref name="deadzone"/> averages the four actions' own).</summary>
    public static Vector2 GetVector(StringName negativeX, StringName positiveX, StringName negativeY, StringName positiveY, float deadzone = -1f)
    {
        var v = new Vector2(GetActionRawStrength(positiveX) - GetActionRawStrength(negativeX),
                            GetActionRawStrength(positiveY) - GetActionRawStrength(negativeY));
        if (deadzone < 0f)
            deadzone = 0.25f * (InputMap.ActionGetDeadzone(positiveX) + InputMap.ActionGetDeadzone(negativeX) +
                                InputMap.ActionGetDeadzone(positiveY) + InputMap.ActionGetDeadzone(negativeY));
        float length = v.Length();
        if (length <= deadzone) return Vector2.Zero;
        if (length > 1f) return v / length;
        return v * ((length - deadzone) / (length * (1f - deadzone)));
    }

    /// <summary>Presses an action as if an event had (Godot's action_press). It stays pressed until released.</summary>
    public static void ActionPress(StringName action, float strength = 1f)
    {
        ActionState st = StateOf(action.ToString());
        if (!st.Pressed) MarkPressed(st, pressed: true);
        st.Exact = true;
        st.Strength = st.RawStrength = Math.Clamp(strength, 0f, 1f);
    }

    public static void ActionRelease(StringName action)
    {
        ActionState st = StateOf(action.ToString());
        if (st.Pressed) MarkPressed(st, pressed: false);
        st.Strength = st.RawStrength = 0f;
    }

    private static ActionState StateOf(string name)
    {
        if (!s_actions.TryGetValue(name, out ActionState? st)) s_actions[name] = st = new ActionState();
        return st;
    }

    private static void MarkPressed(ActionState st, bool pressed)
    {
        st.Pressed = pressed;
        if (pressed) { st.PressedProcessFrame = ProcessFrameNow; st.PressedPhysicsFrame = PhysicsFrameNow; }
        else { st.ReleasedProcessFrame = ProcessFrameNow; st.ReleasedPhysicsFrame = PhysicsFrameNow; }
    }

    public static bool IsAnythingPressed()
    {
        if (s_keys.Count > 0 || s_mouseButtons.Count > 0 || s_joyButtons.Count > 0) return true;
        foreach (ActionState st in s_actions.Values) if (st.Pressed) return true;
        return false;
    }

    // ---- keys, mouse, pads ----

    private static bool NativeKey(Key key)
    {
        if (!NodeHost.Current.IsEngine || !InputBridge.TryAverKey(key, out Aver.Framework.Key ak)) return false;
        try { return Aver.Framework.Input.GetKey(ak); }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    public static bool IsKeyPressed(Key keycode) => s_keys.Contains(keycode) || NativeKey(keycode);

    public static bool IsPhysicalKeyPressed(Key keycode) => IsKeyPressed(keycode);

    public static bool IsKeyLabelPressed(Key keycode) => IsKeyPressed(keycode);

    public static bool IsMouseButtonPressed(MouseButton button)
    {
        if (s_mouseButtons.Contains(button)) return true;
        if (!NodeHost.Current.IsEngine || !InputBridge.TryAverMouse(button, out Aver.Framework.Key mk)) return false;
        try { return Aver.Framework.Input.GetKey(mk); }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    public static bool IsJoyButtonPressed(int device, JoyButton button)
    {
        if (s_joyButtons.Contains(button)) return true;
        if (!NodeHost.Current.IsEngine || !InputBridge.TryAverButton(button, out Aver.Framework.GamepadButton gb)) return false;
        try { return Aver.Framework.Input.GetGamepadButton(gb, device); }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>An axis reading, -1 to 1 (a stick pushed down reads positive, as in Godot).</summary>
    public static float GetJoyAxis(int device, JoyAxis axis)
    {
        if (s_joyAxes.TryGetValue(axis, out float v) && v != 0f) return v;
        if (!NodeHost.Current.IsEngine) return 0f;
        foreach (Aver.Framework.GamepadAxis a in Enum.GetValues<Aver.Framework.GamepadAxis>())
        {
            if (InputBridge.ToJoyAxis(a) != axis) continue;
            try { return InputBridge.AxisValueFromAver(axis, Aver.Framework.Input.GetGamepadAxis(a, device)); }
            catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { return 0f; }
        }
        return 0f;
    }

    public static MouseButtonMask GetMouseButtonMask()
    {
        MouseButtonMask m = 0;
        if (IsMouseButtonPressed(MouseButton.Left)) m |= MouseButtonMask.Left;
        if (IsMouseButtonPressed(MouseButton.Right)) m |= MouseButtonMask.Right;
        if (IsMouseButtonPressed(MouseButton.Middle)) m |= MouseButtonMask.Middle;
        return m;
    }

    /// <summary>The pointer's velocity in pixels per second, from the last motion event.</summary>
    public static Vector2 GetLastMouseVelocity() => s_mouseVelocity;

    public static Vector2 GetLastMouseScreenVelocity() => s_mouseVelocity;

    /// <summary>The connected gamepads' device numbers.</summary>
    public static Collections.Array<int> GetConnectedJoypads()
    {
        var pads = new Collections.Array<int>();
        if (s_joyButtons.Count > 0 || s_joyAxes.Count > 0) pads.Add(0);
        return pads;
    }

    public static void StartJoyVibration(int device, float weakMagnitude, float strongMagnitude, float duration = 0f) { }

    public static void StopJoyVibration(int device) { }

    /// <summary>Whether the pointer is shown, hidden or captured by the game window. The engine shows or hides it through the cursor request.</summary>
    public static MouseModeEnum MouseMode
    {
        get => s_mouseMode;
        set
        {
            s_mouseMode = value;
            if (!NodeHost.Current.IsEngine) return;
            try
            {
                bool free = value is MouseModeEnum.Visible or MouseModeEnum.Confined;
                if (free && s_cursorScope == null) s_cursorScope = Game.FreeCursor();
                else if (!free && s_cursorScope != null) { s_cursorScope.Dispose(); s_cursorScope = null; }
            }
            catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { }
        }
    }

    // ---- feeding events in ----

    /// <summary>Feeds an event to the game as if it had come from a device (Godot's parse_input_event): input and action state change,
    /// and the event goes down the tree to _Input and _UnhandledInput.</summary>
    public static void ParseInputEvent(InputEvent @event)
    {
        ArgumentNullException.ThrowIfNull(@event);
        Absorb(@event);
        Aver.Framework.InputEvent? engine = NodeHost.Current.IsEngine ? InputBridge.ToFramework(@event) : null;
        if (engine != null) Aver.Framework.Input.ParseInputEvent(engine);
        else SceneTree.Instance.DispatchInputManaged(@event);
    }

    /// <summary>Updates the held keys and buttons and the actions the event triggers. Events from the engine's own chain are not absorbed
    /// (the engine already polls its devices); this is for events fed by script.</summary>
    internal static void Absorb(InputEvent e)
    {
        switch (e)
        {
            case InputEventKey k:
                Key key = k.GetKeycodeWithModifiers();
                if (k.Pressed) s_keys.Add(key); else s_keys.Remove(key);
                break;
            case InputEventMouseButton b:
                if (b.Pressed) s_mouseButtons.Add(b.ButtonIndex); else s_mouseButtons.Remove(b.ButtonIndex);
                s_mousePosition = b.Position;
                break;
            case InputEventMouseMotion m:
                s_mousePosition = m.Position;
                s_mouseVelocity = m.Velocity;
                break;
            case InputEventJoypadButton jb:
                if (jb.Pressed) s_joyButtons.Add(jb.ButtonIndex); else s_joyButtons.Remove(jb.ButtonIndex);
                break;
            case InputEventJoypadMotion jm:
                s_joyAxes[jm.Axis] = jm.AxisValue;
                break;
        }
        if (e is InputEventMouseMotion) return;
        foreach (StringName action in InputMap.GetActions())
        {
            string name = action.ToString();
            if (!InputMap.EventGetActionStatus(e, name, exactMatch: false, out bool pressed, out float strength, out float raw)) continue;
            ActionState st = StateOf(name);
            if (!e.IsEcho() && st.Pressed != pressed)
            {
                MarkPressed(st, pressed);
                st.Strength = st.RawStrength = 0f;
                st.Exact = InputMap.EventGetActionStatus(e, name, exactMatch: true, out bool exactPressed, out _, out _) && exactPressed == pressed;
            }
            st.Strength = strength;
            st.RawStrength = raw;
        }
    }
}
