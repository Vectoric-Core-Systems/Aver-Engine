// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for DrivingContext (docs/plans/wave10/VEHICLES.md section 2.10): the action names the vehicle system reads are exactly the Drive.*
// set, every one is bound to a key and a gamepad control, the steering signs are right, and every binding names a real member of the key
// enums. Pure data (DrivingContext.DefaultBindings): constructing the context declares native actions, which a bare process cannot do.
using System;
using System.Collections.Generic;
using System.Linq;
using Aver.Framework;

[AutoRun]
static class DrivingContextTests
{
    public static int RunAll()
    {
        int failures = 0;
        failures += TestActionNamesAreTheDriveSet();
        failures += TestEveryActionIsBound();
        failures += TestSteeringAndThrottleSigns();
        failures += TestBindingsNameRealControls();
        failures += TestNoDuplicateBindings();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static int TestActionNamesAreTheDriveSet()
    {
        Console.WriteLine("Test: the action names are exactly the nine Drive.* actions the vehicle system reads");
        var want = new[] { "Drive.Throttle", "Drive.Brake", "Drive.Steer", "Drive.Handbrake", "Drive.ShiftUp", "Drive.ShiftDown", "Drive.Reset", "Drive.Exit", "Drive.CameraNext" };
        if (!DriveActionNames.All.OrderBy(x => x).SequenceEqual(want.OrderBy(x => x))) return Fail($"got [{string.Join(", ", DriveActionNames.All)}]");
        if (DriveActionNames.Throttle != "Drive.Throttle" || DriveActionNames.CameraNext != "Drive.CameraNext") return Fail("constant spelling");
        if (DrivingContext.Priority != 100) return Fail("the context is pushed at priority 100");
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int TestEveryActionIsBound()
    {
        Console.WriteLine("Test: every Drive.* action has a keyboard binding and a gamepad binding, and no binding names another action");
        foreach (string name in DriveActionNames.All)
        {
            var rows = DrivingContext.DefaultBindings.Where(b => b.Action == name).ToArray();
            if (!rows.Any(b => b.Source == InputSource.Key)) return Fail($"{name} has no keyboard binding");
            if (!rows.Any(b => b.Source is InputSource.GamepadButton or InputSource.GamepadAxis)) return Fail($"{name} has no gamepad binding");
        }
        foreach (DriveBinding b in DrivingContext.DefaultBindings)
            if (!DriveActionNames.All.Contains(b.Action)) return Fail($"a binding names '{b.Action}', which is not a Drive.* action");
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int TestSteeringAndThrottleSigns()
    {
        Console.WriteLine("Test: D and Right steer right (+1), A and Left steer left (-1); W and Up throttle, S and Down brake");
        float Scale(string action, string key) => DrivingContext.DefaultBindings.First(b => b.Action == action && b.KeyName == key).Scale;
        if (Scale("Drive.Steer", nameof(Key.D)) != 1f || Scale("Drive.Steer", nameof(Key.Right)) != 1f) return Fail("D and Right must be +1");
        if (Scale("Drive.Steer", nameof(Key.A)) != -1f || Scale("Drive.Steer", nameof(Key.Left)) != -1f) return Fail("A and Left must be -1");
        if (Scale("Drive.Throttle", nameof(Key.W)) != 1f || Scale("Drive.Throttle", nameof(Key.Up)) != 1f) return Fail("W and Up are the throttle");
        if (Scale("Drive.Brake", nameof(Key.S)) != 1f || Scale("Drive.Brake", nameof(Key.Down)) != 1f) return Fail("S and Down are the brake pedal");
        var triggers = DrivingContext.DefaultBindings.Where(b => b.Source == InputSource.GamepadAxis).ToArray();
        if (!triggers.Any(b => b.Action == "Drive.Throttle" && b.KeyName == nameof(GamepadAxis.RightTrigger)) ||
            !triggers.Any(b => b.Action == "Drive.Brake" && b.KeyName == nameof(GamepadAxis.LeftTrigger)) ||
            !triggers.Any(b => b.Action == "Drive.Steer" && b.KeyName == nameof(GamepadAxis.LeftX)))
            return Fail("the triggers throttle and brake and the left stick steers");
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int TestBindingsNameRealControls()
    {
        Console.WriteLine("Test: every binding's key name is a member of the enum its source selects");
        foreach (DriveBinding b in DrivingContext.DefaultBindings)
        {
            bool ok = b.Source switch
            {
                InputSource.Key => Enum.TryParse<Key>(b.KeyName, out _),
                InputSource.GamepadButton => Enum.TryParse<GamepadButton>(b.KeyName, out _),
                InputSource.GamepadAxis => Enum.TryParse<GamepadAxis>(b.KeyName, out _),
                _ => false,
            };
            if (!ok) return Fail($"'{b.KeyName}' is not a {b.Source} name");
        }
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int TestNoDuplicateBindings()
    {
        Console.WriteLine("Test: no control is bound twice to the same action");
        var seen = new HashSet<string>();
        foreach (DriveBinding b in DrivingContext.DefaultBindings)
            if (!seen.Add($"{b.Action}|{b.Source}|{b.KeyName}")) return Fail($"{b.Action} binds {b.KeyName} twice");
        Console.WriteLine("  PASS");
        return 0;
    }
}
