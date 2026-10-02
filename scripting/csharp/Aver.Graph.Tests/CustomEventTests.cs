// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for CustomEvent -- an entry point whose event NAME is the author's rather than one of the
// three the palette shipped (OnStart / OnTick / OnHit).
//
// THE POINT OF THESE TESTS is that CustomEvent needs no runtime machinery at all, and proving that
// is worth more than the node is. What fires an event has always been the top-level `ENTRY <nodeId>
// <eventName>` record -- CompileEntryPoint matches on it and nothing else -- so a graph could ALWAYS
// have declared `ENTRY mine Whatever`, and FireEventNodeTests' own read-back fixture does exactly
// that with an OnStart node named `Query`. What a graph could not do was say so from the PALETTE,
// and the editor half (a `name=` attribute kept in step with the record) is where the gap really was.
//
// These run for real: no native host is involved, because a CustomEvent chain through GetVar/Add/
// SetVar is pure managed work. That is the difference from NewNodeTests' input and raycast cases,
// which can only assert which symbol they tried to call.
using System;
using Aver.Graph;

static class CustomEventTests
{
    public static int RunAll()
    {
        int failures = 0;

        failures += TestDefaultPinsShape();
        failures += TestFiresUnderItsOwnName();
        failures += TestTwoCustomEventsInOneGraphStayApart();
        failures += TestFiringAnUndeclaredNameIsRefusedNotIgnored();

        return failures;
    }

    private static int TestDefaultPinsShape()
    {
        Console.WriteLine("Test: CustomEvent's default pins are one exec output and nothing else");
        try
        {
            if (!OcGraphParser.Parse("OCGRAPH 1\nNODE e CustomEvent name=Boom\nENTRY e Boom\n",
                                     out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["e"];
            bool ok = node.Pins.Count == 1 &&
                      node.Pins[0].Name == "exec" && node.Pins[0].IsOutput && node.Pins[0].Type == PinType.Exec;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set: [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            Console.WriteLine("  PASS: same shape as OnStart/OnTick/OnHit, which is the whole design");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // The end-to-end claim: a graph declares its own event name, and firing THAT name runs the chain.
    // Read back through a second entry point whose chain ends in an OUT -- the Bump/Query shape
    // GraphVarTests and FireEventNodeTests already use, here with both halves as CustomEvents.
    private static int TestFiresUnderItsOwnName()
    {
        Console.WriteLine("Test: firing a graph's own event name runs the chain hanging off it");
        try
        {
            var text = @"
OCGRAPH 1
VAR hits float 0

NODE e CustomEvent name=Scored
ENTRY e Scored

NODE cur GetVar var=hits
NODE one ConstFloat value=1.0
NODE sum Add
LINK cur.value sum.a
LINK one.value sum.b

NODE store SetVar var=hits
LINK e.exec store.exec
LINK sum.result store.value

NODE q CustomEvent name=Query
ENTRY q Query
NODE report GetVar var=hits
OUT report value
";
            var host = new GraphHost();
            if (!host.LoadFromText(text, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }

            for (int i = 0; i < 4; i++)
            {
                if (!host.Fire("Scored", Array.Empty<object>(), out _))
                {
                    Console.WriteLine($"  FAIL: Fire('Scored') refused on iteration {i}");
                    return 1;
                }
            }

            if (!host.Fire("Query", Array.Empty<object>(), out var seen) ||
                seen is not float f || Math.Abs(f - 4f) > 1e-4f)
            {
                Console.WriteLine($"  FAIL: expected 4 after four fires, got {seen}");
                return 1;
            }
            Console.WriteLine("  PASS: four fires of an author-named event ran the chain four times");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: threw {ex}");
            return 1;
        }
    }

    // TWO CUSTOM EVENTS IN ONE GRAPH is the case where a design that keyed events off the node TYPE
    // rather than the ENTRY record would collapse -- every one of these nodes is `CustomEvent`, and
    // only the record tells them apart.
    private static int TestTwoCustomEventsInOneGraphStayApart()
    {
        Console.WriteLine("Test: two CustomEvents in one graph fire independently");
        try
        {
            var text = @"
OCGRAPH 1
VAR score float 0

NODE up CustomEvent name=Gain
ENTRY up Gain
NODE down CustomEvent name=Lose
ENTRY down Lose

NODE curA GetVar var=score
NODE ten ConstFloat value=10.0
NODE plus Add
LINK curA.value plus.a
LINK ten.value plus.b
NODE storeA SetVar var=score
LINK up.exec storeA.exec
LINK plus.result storeA.value

NODE curB GetVar var=score
NODE three ConstFloat value=3.0
NODE minus Subtract
LINK curB.value minus.a
LINK three.value minus.b
NODE storeB SetVar var=score
LINK down.exec storeB.exec
LINK minus.result storeB.value

NODE q CustomEvent name=Query
ENTRY q Query
NODE report GetVar var=score
OUT report value
";
            var host = new GraphHost();
            if (!host.LoadFromText(text, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }

            host.Fire("Gain", Array.Empty<object>(), out _);
            host.Fire("Gain", Array.Empty<object>(), out _);
            host.Fire("Lose", Array.Empty<object>(), out _);

            if (!host.Fire("Query", Array.Empty<object>(), out var seen) ||
                seen is not float f || Math.Abs(f - 17f) > 1e-4f)
            {
                Console.WriteLine($"  FAIL: expected 10 + 10 - 3 = 17, got {seen}");
                return 1;
            }
            Console.WriteLine("  PASS: 17 -- each name reached its own chain, though every node is CustomEvent");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: threw {ex}");
            return 1;
        }
    }

    // A NAME NOTHING DECLARES MUST BE REFUSED, not quietly treated as a no-op. This is the failure a
    // graph author actually hits -- a typo on the firing side -- and a silent success there means
    // staring at a chain that looks correct because it is.
    private static int TestFiringAnUndeclaredNameIsRefusedNotIgnored()
    {
        Console.WriteLine("Test: firing an event no ENTRY declares is refused");
        try
        {
            var text = "OCGRAPH 1\nVAR v float 0\nNODE e CustomEvent name=Real\nENTRY e Real\n" +
                       "NODE c ConstFloat value=1.0\nNODE s SetVar var=v\nLINK e.exec s.exec\nLINK c.value s.value\n";
            var host = new GraphHost();
            if (!host.LoadFromText(text, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }
            if (host.Fire("Typo", Array.Empty<object>(), out _))
            {
                Console.WriteLine("  FAIL: firing an undeclared name reported success");
                return 1;
            }
            if (!host.Fire("Real", Array.Empty<object>(), out _))
            {
                Console.WriteLine("  FAIL: and the declared name stopped working after the refusal");
                return 1;
            }
            Console.WriteLine("  PASS: refused the typo, and the real name still fires afterwards");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: threw {ex}");
            return 1;
        }
    }
}
