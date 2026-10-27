// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// InputRouterTests: Godot's input propagation -- every actor's OnInput in the order given, then the UI for the
// device it wants, then OnUnhandledInput, stopping at the first that handles it. No host.

using Aver.Framework;

static class InputRouterTests
{
    private static readonly List<string> s_log = new();

    private sealed class Listener : AverActor
    {
        public string Tag = "";
        public bool TakeInput, TakeUnhandled, Throw;
        public override void OnInput(InputEvent e)
        {
            s_log.Add(Tag + ".input");
            if (Throw) throw new InvalidOperationException("boom");
            if (TakeInput) Input.SetInputAsHandled();
        }
        public override void OnUnhandledInput(InputEvent e)
        {
            s_log.Add(Tag + ".unhandled");
            if (TakeUnhandled) Input.SetInputAsHandled();
        }
    }

    private sealed class Deaf : AverActor { }

    public static int RunAll() => T.Suite(() =>
    {
        T.Run("input: OnInput in order, then OnUnhandledInput, when nothing handles it", Unclaimed);
        T.Run("input: SetInputAsHandled stops the event where it is", HandledStops);
        T.Run("input: the UI takes pointer events when it wants the pointer, keys when it wants the keyboard", UiTakes);
        T.Run("input: an actor without input hooks is not called; InputEnabled turns one off", Enablement);
        T.Run("input: a hook that throws does not stop the rest", Throws);
    });

    private static List<AverActor> Two(out Listener a, out Listener b)
    {
        a = new Listener { Tag = "a" };
        b = new Listener { Tag = "b" };
        return new List<AverActor> { a, b };
    }

    private static void Unclaimed()
    {
        s_log.Clear();
        var actors = Two(out _, out _);
        var ev = new InputEventKey(Key.Space, true);
        T.Check("nobody took it", InputRouter.Route(ev, actors, false, false) == "" && !ev.Handled);
        T.Check("a and b saw OnInput, then OnUnhandledInput", s_log.SequenceEqual(new[] { "a.input", "b.input", "a.unhandled", "b.unhandled" }));
    }

    private static void HandledStops()
    {
        s_log.Clear();
        var actors = Two(out Listener a, out _);
        a.TakeInput = true;
        var ev = new InputEventKey(Key.Space, true);
        T.Check("taken in OnInput", InputRouter.Route(ev, actors, false, false) == "input" && ev.Handled);
        T.Check("nobody after a saw it", s_log.SequenceEqual(new[] { "a.input" }));
        s_log.Clear();
        a.TakeInput = false;
        ((Listener)actors[1]).TakeUnhandled = true;
        T.Check("taken in OnUnhandledInput by b", InputRouter.Route(new InputEventKey(Key.A, true), actors, false, false) == "unhandled");
        T.Check("and the route stopped there", s_log.SequenceEqual(new[] { "a.input", "b.input", "a.unhandled", "b.unhandled" }));
        T.Check("SetInputAsHandled outside a route does nothing", Throwless(Input.SetInputAsHandled));
    }

    private static void UiTakes()
    {
        s_log.Clear();
        var actors = Two(out _, out _);
        T.Check("a click goes to the UI when it wants the pointer",
                InputRouter.Route(new InputEventMouseButton(Key.MouseLeft, true), actors, true, false) == "ui");
        T.Check("after the actors' OnInput, before OnUnhandledInput", s_log.SequenceEqual(new[] { "a.input", "b.input" }));
        T.Check("a key passes a UI that wants only the pointer",
                InputRouter.Route(new InputEventKey(Key.W, true), actors, true, false) == "");
        T.Check("a key goes to a UI that wants the keyboard",
                InputRouter.Route(new InputEventKey(Key.W, true), actors, false, true) == "ui");
        T.Check("a gamepad button never goes to the UI",
                InputRouter.Route(new InputEventJoypadButton(GamepadButton.A, true), actors, true, true) == "");
    }

    private static void Enablement()
    {
        T.Check("an actor with no input hooks starts with input off", !new Deaf().InputEnabled);
        T.Check("one with them starts on", new Listener().InputEnabled);
        s_log.Clear();
        var actors = Two(out Listener a, out _);
        a.InputEnabled = false;
        InputRouter.Route(new InputEventKey(Key.Space, true), actors, false, false);
        T.Check("a disabled actor hears nothing", !s_log.Any(l => l.StartsWith("a.")));
    }

    private static void Throws()
    {
        s_log.Clear();
        var actors = Two(out Listener a, out _);
        a.Throw = true;
        int errors = 0;
        InputRouter.Route(new InputEventKey(Key.Space, true), actors, false, false, (_, _, _) => ++errors);
        T.Check("the error is reported", errors == 1);
        T.Check("and b still heard it", s_log.Contains("b.input"));
    }

    private static bool Throwless(Action act)
    {
        try { act(); return true; } catch { return false; }
    }
}
