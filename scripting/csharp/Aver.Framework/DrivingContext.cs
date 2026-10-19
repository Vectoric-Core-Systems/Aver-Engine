// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The default driving bindings. The vehicle system reads these NAMED ACTIONS from the framework while a vehicle has a driver, so a
// project either ships them in its one .ocinput (the Vehicle template does) or pushes this context from a script. Design:
// docs/VEHICLES.md.

namespace Aver.Framework;

/// <summary>The names of the input actions a vehicle with a driver reads. The system looks them up by these exact names.</summary>
public static class DriveActionNames
{
    /// <summary>Axis, -1..1: forward throttle (negative asks for reverse or brake, see the driver aids).</summary>
    public const string Throttle = "Drive.Throttle";
    /// <summary>Axis, 0..1: the brake pedal.</summary>
    public const string Brake = "Drive.Brake";
    /// <summary>Axis, -1..1: steering, +1 to the right.</summary>
    public const string Steer = "Drive.Steer";
    /// <summary>Button: the handbrake, held.</summary>
    public const string Handbrake = "Drive.Handbrake";
    /// <summary>Button: one gear up (manual gearbox only).</summary>
    public const string ShiftUp = "Drive.ShiftUp";
    /// <summary>Button: one gear down (manual gearbox only).</summary>
    public const string ShiftDown = "Drive.ShiftDown";
    /// <summary>Button: put the vehicle upright.</summary>
    public const string Reset = "Drive.Reset";
    /// <summary>Button: get out.</summary>
    public const string Exit = "Drive.Exit";
    /// <summary>Button: the next camera.</summary>
    public const string CameraNext = "Drive.CameraNext";

    /// <summary>Every name, in the order the system reads them.</summary>
    public static IReadOnlyList<string> All { get; } = new[] { Throttle, Brake, Steer, Handbrake, ShiftUp, ShiftDown, Reset, Exit, CameraNext };
}

/// <summary>One default binding as data: the action, where it reads from, the key or axis name (a member of <see cref="Key"/>,
/// <see cref="GamepadButton"/> or <see cref="GamepadAxis"/>) and the scale and component.</summary>
public readonly record struct DriveBinding(string Action, InputSource Source, string KeyName, float Scale = 1f, int Component = 0);

/// <summary>The default driving bindings: W, Up and the right trigger throttle; S, Down and the left trigger brake; A, D and the left
/// stick steer; Space or the A button the handbrake; E and Q shift; R resets; F exits; C changes camera. Rebindable through the existing
/// <c>SaveBindings</c> / <c>RebindAction</c> like any other context.</summary>
public sealed class DrivingContext : InputMappingContext
{
    /// <summary>The priority <see cref="Push"/> adds the context at: above a project's on-foot context.</summary>
    public const int Priority = 100;

    /// <summary>The bindings as plain data (no native calls), which is what the constructor replays and what a test can read in a bare process.</summary>
    public static IReadOnlyList<DriveBinding> DefaultBindings { get; } = new[]
    {
        new DriveBinding(DriveActionNames.Throttle, InputSource.Key, nameof(Key.W)),
        new DriveBinding(DriveActionNames.Throttle, InputSource.Key, nameof(Key.Up)),
        new DriveBinding(DriveActionNames.Throttle, InputSource.GamepadAxis, nameof(GamepadAxis.RightTrigger)),
        new DriveBinding(DriveActionNames.Brake, InputSource.Key, nameof(Key.S)),
        new DriveBinding(DriveActionNames.Brake, InputSource.Key, nameof(Key.Down)),
        new DriveBinding(DriveActionNames.Brake, InputSource.GamepadAxis, nameof(GamepadAxis.LeftTrigger)),
        new DriveBinding(DriveActionNames.Steer, InputSource.Key, nameof(Key.D), 1f),
        new DriveBinding(DriveActionNames.Steer, InputSource.Key, nameof(Key.A), -1f),
        new DriveBinding(DriveActionNames.Steer, InputSource.Key, nameof(Key.Right), 1f),
        new DriveBinding(DriveActionNames.Steer, InputSource.Key, nameof(Key.Left), -1f),
        new DriveBinding(DriveActionNames.Steer, InputSource.GamepadAxis, nameof(GamepadAxis.LeftX)),
        new DriveBinding(DriveActionNames.Handbrake, InputSource.Key, nameof(Key.Space)),
        new DriveBinding(DriveActionNames.Handbrake, InputSource.GamepadButton, nameof(GamepadButton.A)),
        new DriveBinding(DriveActionNames.ShiftUp, InputSource.Key, nameof(Key.E)),
        new DriveBinding(DriveActionNames.ShiftUp, InputSource.GamepadButton, nameof(GamepadButton.RightShoulder)),
        new DriveBinding(DriveActionNames.ShiftDown, InputSource.Key, nameof(Key.Q)),
        new DriveBinding(DriveActionNames.ShiftDown, InputSource.GamepadButton, nameof(GamepadButton.LeftShoulder)),
        new DriveBinding(DriveActionNames.Reset, InputSource.Key, nameof(Key.R)),
        new DriveBinding(DriveActionNames.Reset, InputSource.GamepadButton, nameof(GamepadButton.Y)),
        new DriveBinding(DriveActionNames.Exit, InputSource.Key, nameof(Key.F)),
        new DriveBinding(DriveActionNames.Exit, InputSource.GamepadButton, nameof(GamepadButton.B)),
        new DriveBinding(DriveActionNames.CameraNext, InputSource.Key, nameof(Key.C)),
        new DriveBinding(DriveActionNames.CameraNext, InputSource.GamepadButton, nameof(GamepadButton.RightThumb)),
    };

    private static DrivingContext? s_pushed;

    /// <summary>Creates the context: declares the nine actions (idempotent by name) and binds <see cref="DefaultBindings"/>.</summary>
    public DrivingContext()
    {
        foreach (DriveBinding b in DefaultBindings)
        {
            InputAction action = ActionFor(b.Action);
            switch (b.Source)
            {
                case InputSource.Key: BindKey(action, Enum.Parse<Key>(b.KeyName), b.Scale, b.Component); break;
                case InputSource.GamepadButton: BindGamepadButton(action, Enum.Parse<GamepadButton>(b.KeyName), b.Scale, b.Component); break;
                case InputSource.GamepadAxis: BindGamepadAxis(action, Enum.Parse<GamepadAxis>(b.KeyName), b.Scale, b.Component); break;
            }
        }
    }

    /// <inheritdoc/>
    public override string Name => "Driving";

    private static InputAction ActionFor(string name) => name is DriveActionNames.Throttle or DriveActionNames.Brake or DriveActionNames.Steer
        ? InputAction.Axis1D(name)
        : InputAction.Digital(name);

    /// <summary>Adds the default driving context at <see cref="Priority"/> (once; a second push is a no-op).</summary>
    public static void Push()
    {
        if (s_pushed != null) return;
        s_pushed = new DrivingContext();
        EnhancedInput.AddContext(s_pushed, Priority);
    }

    /// <summary>Removes what <see cref="Push"/> added. Safe when nothing was pushed.</summary>
    public static void Pop()
    {
        if (s_pushed == null) return;
        EnhancedInput.RemoveContext(s_pushed);
        s_pushed = null;
    }

    /// <summary>The action for throttle ("Drive.Throttle").</summary>
    public static InputAction Throttle => InputAction.Axis1D(DriveActionNames.Throttle);
    /// <summary>The action for the brake pedal ("Drive.Brake").</summary>
    public static InputAction Brake => InputAction.Axis1D(DriveActionNames.Brake);
    /// <summary>The action for steering ("Drive.Steer").</summary>
    public static InputAction Steer => InputAction.Axis1D(DriveActionNames.Steer);
    /// <summary>The action for the handbrake ("Drive.Handbrake").</summary>
    public static InputAction Handbrake => InputAction.Digital(DriveActionNames.Handbrake);
    /// <summary>The action for shifting up ("Drive.ShiftUp").</summary>
    public static InputAction ShiftUp => InputAction.Digital(DriveActionNames.ShiftUp);
    /// <summary>The action for shifting down ("Drive.ShiftDown").</summary>
    public static InputAction ShiftDown => InputAction.Digital(DriveActionNames.ShiftDown);
    /// <summary>The action for putting the vehicle upright ("Drive.Reset").</summary>
    public static InputAction Reset => InputAction.Digital(DriveActionNames.Reset);
    /// <summary>The action for getting out ("Drive.Exit").</summary>
    public static InputAction Exit => InputAction.Digital(DriveActionNames.Exit);
    /// <summary>The action for the next camera ("Drive.CameraNext").</summary>
    public static InputAction CameraNext => InputAction.Digital(DriveActionNames.CameraNext);
}
