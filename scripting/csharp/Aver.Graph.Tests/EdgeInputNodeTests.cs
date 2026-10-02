// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
// Tests for InputKeyPressed / InputKeyReleased -- the EDGE of a key, where InputKey gives the STATE.
//
// WHY A SEPARATE NODE AND NOT A FLAG ON InputKey. `down` is true every frame a key is held, which is
// the wrong answer for jumping, firing a semi-auto, or toggling anything: all of them want one true
// per press. Building that from InputKey needs a DoOnce and a variable per key, while the framework
// ABI has answered it directly all along (aver_fw_input_key_pressed / _released, which is what
// Input.GetKeyDown/GetKeyUp already wrap for C#).
//
// WHAT THESE TESTS PROVE, AND THE ONE THING THEY CANNOT. Same limitation as NewNodeTests.cs's own
// InputKey/Raycast cases, for the same reason -- this bare process has no native scripting host, so
// invoking the compiled delegate throws EntryPointNotFoundException at the P/Invoke. That exception
// is the assertion rather than an obstacle: it names the symbol the emitted IL actually tried to
// call, which is the only way from here to tell a real native call from a stub -- AND, uniquely
// useful here, to tell `pressed` apart from `released`, since the two nodes differ in nothing else.
using System;
using Aver.Graph;

static class EdgeInputNodeTests
{
    public static int RunAll()
    {
        int failures = 0;

        failures += TestPressedDefaultPinsShape();
        failures += TestReleasedDefaultPinsShape();
        failures += TestPressedCallsPressedNotHeld("pull");
        failures += TestPressedCallsPressedNotHeld("push");
        failures += TestReleasedCallsReleasedNotPressed();

        return failures;
    }

    private static int PinsShape(string type)
    {
        Console.WriteLine($"Test: {type}'s default pins are key:int-in, triggered:bool-out");
        try
        {
            var text = $"OCGRAPH 1\nNODE k {type}\nOUT k triggered\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["k"];
            bool ok =
                node.Pins.Find(p => p.Name == "key" && !p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Find(p => p.Name == "triggered" && p.IsOutput && p.Type == PinType.Bool) != null &&
                node.Pins.Count == 2;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set: [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            // The output is NOT called `down`. That is deliberate and worth asserting: a graph author
            // reading `down` on this node would reasonably expect held-means-true, which is the exact
            // confusion the node exists to remove.
            if (node.Pins.Find(p => p.Name == "down") != null)
            {
                Console.WriteLine("  FAIL: this node has a 'down' pin -- it reports an EVENT, not a state");
                return 1;
            }
            Console.WriteLine("  PASS: 2 pins, no exec, and the output is 'triggered' rather than 'down'");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestPressedDefaultPinsShape() => PinsShape("InputKeyPressed");
    private static int TestReleasedDefaultPinsShape() => PinsShape("InputKeyReleased");

    // THE SYMBOL NAME IS THE ASSERTION. Both compilers are exercised because a node present in one
    // and absent from the other is this file's recurring defect shape -- it compiles in some graphs
    // and throws in others, depending only on whether the graph has an ENTRY.
    private static int TestPressedCallsPressedNotHeld(string path)
    {
        Console.WriteLine($"Test: InputKeyPressed ({path.ToUpperInvariant()}) calls aver_fw_input_key_pressed, not aver_fw_input_key");
        return ExpectSymbol(path, "InputKeyPressed", "aver_fw_input_key_pressed");
    }

    private static int TestReleasedCallsReleasedNotPressed()
    {
        Console.WriteLine("Test: InputKeyReleased calls aver_fw_input_key_released");
        return ExpectSymbol("pull", "InputKeyReleased", "aver_fw_input_key_released");
    }

    private static int ExpectSymbol(string path, string nodeType, string symbol)
    {
        try
        {
            string text = path == "push"
                ? $"OCGRAPH 1\nPARAM key int\nNODE tick OnTick\nNODE pk Param param=key\nNODE ik {nodeType}\n" +
                  "LINK pk.value ik.key\nENTRY tick OnTick\nOUT ik triggered\n"
                : $"OCGRAPH 1\nPARAM key int\nNODE pk Param param=key\nNODE ik {nodeType}\n" +
                  "LINK pk.value ik.key\nOUT ik triggered\n";

            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            Delegate? compiled = path == "push"
                ? compiler.CompileEntryPoint("OnTick", out var pushErr) is { } d ? d : Fail(pushErr)
                : compiler.Compile(out var pullErr) is { } d2 ? d2 : Fail(pullErr);
            if (compiled is null) return 1;

            try
            {
                compiled.DynamicInvoke(5);
                Console.WriteLine("  FAIL: expected the P/Invoke to throw in this hostless process, but it returned");
                return 1;
            }
            catch (Exception ex)
            {
                // DynamicInvoke wraps, so the EntryPointNotFoundException arrives as an inner
                // exception. Unwrapping rather than matching on the outer type keeps this assertion
                // about the SYMBOL, which is the thing under test.
                Exception? at = ex;
                while (at is not null && at is not EntryPointNotFoundException) at = at.InnerException;
                if (at is null)
                {
                    Console.WriteLine($"  FAIL: threw, but not an EntryPointNotFoundException: {ex}");
                    return 1;
                }
                if (!at.Message.Contains(symbol))
                {
                    Console.WriteLine($"  FAIL: called something else -- expected '{symbol}', got: {at.Message}");
                    return 1;
                }
                // Asserting the FULL symbol matters because `aver_fw_input_key` is a prefix of both
                // edge symbols: a test that only checked for the prefix would pass just as happily
                // if these nodes had been wired to the held-state call by mistake, which is exactly
                // the mistake worth catching.
                Console.WriteLine($"  PASS: real call attempted '{symbol}'");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static Delegate? Fail(string? why)
    {
        Console.WriteLine($"  FAIL: Compile error: {why ?? "(none reported)"}");
        return null;
    }
}
