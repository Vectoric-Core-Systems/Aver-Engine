// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// InputRouterTests: input propagation -- every actor's OnInput in the order given, then the UI for the
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
        T.Run("input: touch and sideways wheel events are pointer events; the key and pad numbers are stable", WiderSet);
    });

    private static void WiderSet()
    {
        s_log.Clear();
        var actors = Two(out _, out _);
        T.Check("a finger down goes to the UI when it wants the pointer",
                InputRouter.Route(new InputEventTouch(0, true, 10f, 20f), actors, true, false) == "ui");
        T.Check("so does a finger moving",
                InputRouter.Route(new InputEventTouchMotion(0, 12f, 20f, 2f, 0f), actors, true, false) == "ui");
        T.Check("a sideways wheel notch too", InputRouter.Route(new InputEventMouseWheel(0f, 1f), actors, true, false) == "ui");
        T.Check("a touch passes a UI that wants only the keyboard", InputRouter.Route(new InputEventTouch(0, false, 0f, 0f), actors, false, true) == "");
        T.Check("a finger lifting is not a press", !new InputEventTouch(1, false, 0f, 0f).Pressed && new InputEventTouch(1, true, 0f, 0f).Pressed);
        T.Check("a touch motion is never a release", !new InputEventTouchMotion(0, 1f, 1f, 0f, 0f).IsActionReleased("x"));
        T.Check("the wheel carries both directions", new InputEventMouseWheel(2f, -1f) is { Delta: 2f, Horizontal: -1f } && new InputEventMouseWheel(3f).Horizontal == 0f);

        // The original 50 keys keep their numbers (saved graphs store them); the appended ones follow.
        T.Check("the original key numbers", (int)Key.A == 0 && (int)Key.Z == 25 && (int)Key.D0 == 26 && (int)Key.Space == 36 && (int)Key.Down == 46 && (int)Key.MouseMiddle == 49);
        T.Check("the appended keys start at 50 and run to 108", (int)Key.F1 == 50 && (int)Key.F12 == 61 && (int)Key.Kp0 == 87 && (int)Key.KpEnter == 102 &&
                                                                  (int)Key.MouseX1 == 103 && (int)Key.MouseWheelRight == 108);
        T.Check("109 keys in all", Enum.GetValues<Key>().Length == 109);
        T.Check("the original 14 pad buttons keep their numbers", (int)GamepadButton.Y == 13 && (int)GamepadButton.Guide == 14 && (int)GamepadButton.Touchpad == 20);
        T.Check("21 pad buttons and 6 axes", Enum.GetValues<GamepadButton>().Length == 21 && Enum.GetValues<GamepadAxis>().Length == 6);
        T.Check("four pads and ten fingers", Gamepads.MaxPads == 4 && Touch.MaxFingers == 10);
        T.Check("present mode numbers", (int)PresentMode.Fifo == 0 && (int)PresentMode.Immediate == 1 && (int)PresentMode.Adaptive == 2 && (int)PresentMode.Mailbox == 3);
        T.Check("window mode numbers and flags", (int)WindowMode.Fullscreen == 3 && (int)WindowFlags.AlwaysOnTop == 4 && (int)PointerMode.ConfinedHidden == 5 && (int)CursorShape.Help == 16);
    }

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
