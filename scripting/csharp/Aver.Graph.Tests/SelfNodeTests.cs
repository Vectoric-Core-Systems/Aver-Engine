// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// THE `Self` NODE, and the reason it had to exist.
//
// Nearly every Scene, Character, Physics, Animation and Audio node takes an `entity` pin, and until
// this node the ONLY way to reach the graph's own handle was a top-level `PARAM entity int` record
// plus a Param node naming it. That is fine in hand-written text and impossible on the canvas: the
// editor's document model (aver::fmt::OcGraphData) has no parameters in it at all, so nothing the
// editor can write declares one. A graph authored entirely in the editor could therefore not name
// the entity it was running on, which is most of why every gameplay graph in this repo is text.
//
// Self is SUGAR, resolved at parse time by Graph.ResolveSelfNodes into exactly the `Param entity`
// it stands for. These tests are written against that claim rather than around it: each one checks
// an observable consequence of the rewrite (the parameter appears, the argument arrives, the same
// graph written the long way behaves identically), so if the desugar ever stops happening they fail
// rather than passing on a different mechanism.
using System;
using System.Linq;
using Aver.Graph;

// No namespace, matching every other file in this suite.
static class SelfNodeTests
{
    public static int RunAll()
    {
        int failures = 0;
        failures += SelfDeclaresTheParameterItNeeds();
        failures += SelfCarriesTheEntityArgumentThrough();
        failures += SelfAndAnExplicitParamAgreeOnOneParameter();
        failures += SelfIsIdenticalToWritingItTheLongWay();
        failures += SelfRefusesAMistypedEntityParam();
        failures += SelfInsideAFunctionIsRefused();
        failures += ParamInsideAFunctionIsRefused();
        return failures;
    }

    private static int Expect(string what, bool ok, string detail = "")
    {
        if (ok) { Console.WriteLine($"  PASS: {what}"); return 0; }
        Console.WriteLine($"  FAIL: {what}{(detail.Length > 0 ? " -- " + detail : "")}");
        return 1;
    }

    // The graph an editor would produce: a Self node wired to OUT, and NOT ONE top-level record
    // beyond the header. Nothing here declares a parameter, because nothing the editor writes can.
    private const string CanvasShaped =
        "OCGRAPH 1\n" +
        "NODE me Self\n" +
        "OUT me value\n";

    private static int SelfDeclaresTheParameterItNeeds()
    {
        Console.WriteLine("Test: a Self node declares the entity parameter the file never stated");
        int f = 0;
        if (!OcGraphParser.Parse(CanvasShaped, out var g, out var err))
            return Expect("the canvas-shaped graph parses", false, err ?? "");

        f += Expect("parsing added exactly one parameter", g.Parameters.Count == 1,
                    $"got {g.Parameters.Count}");
        var p = g.Parameters.FirstOrDefault();
        f += Expect("and it is 'entity', typed Int", p != null && p.Name == "entity" && p.Type == PinType.Int,
                    p == null ? "none" : $"{p.Name} {p.Type}");
        // The rewrite, observed directly: no Self node survives parsing.
        f += Expect("no Self node survives -- it became a Param node",
                    g.Nodes.Values.All(n => !n.Type.Equals("self", StringComparison.OrdinalIgnoreCase)) &&
                    g.Nodes["me"].Type.Equals("param", StringComparison.OrdinalIgnoreCase) &&
                    g.Nodes["me"].ParamName == "entity",
                    $"{g.Nodes["me"].Type} param={g.Nodes["me"].ParamName}");
        return f;
    }

    // THE ONE THAT MATTERS. Compiling proves the IL was accepted; invoking proves the handle
    // actually arrives. 16_777_216 is 2^24 -- the first integer a float cannot represent exactly --
    // chosen because entity handles start above it and a float-typed route would round here.
    private static int SelfCarriesTheEntityArgumentThrough()
    {
        Console.WriteLine("Test: the entity handed to the compiled graph is what Self reads back");
        int f = 0;
        if (!OcGraphParser.Parse(CanvasShaped, out var g, out var err))
            return Expect("parses", false, err ?? "");
        var compiled = new GraphCompiler(g).Compile(out err);
        if (compiled == null) return Expect("compiles", false, err ?? "");

        const int handle = 16_777_217;
        object? got = compiled.DynamicInvoke(handle);
        f += Expect("Self returns the exact handle it was invoked with",
                    got is int i && i == handle, $"got {got}");
        return f;
    }

    // A hand-written PARAM and a canvas-placed Self in the same file is an ordinary thing to end up
    // with once the editor can save these. It must reuse the declaration, not add a second one --
    // two parameters would change the compiled method's arity and break every caller.
    private static int SelfAndAnExplicitParamAgreeOnOneParameter()
    {
        Console.WriteLine("Test: Self reuses an entity PARAM the file already declares");
        int f = 0;
        const string text =
            "OCGRAPH 1\n" +
            "PARAM entity int\n" +
            "NODE me Self\n" +
            "OUT me value\n";
        if (!OcGraphParser.Parse(text, out var g, out var err))
            return Expect("parses", false, err ?? "");
        f += Expect("still exactly one parameter", g.Parameters.Count == 1, $"got {g.Parameters.Count}");

        var compiled = new GraphCompiler(g).Compile(out err);
        if (compiled == null) return f + Expect("compiles", false, err ?? "");
        f += Expect("and it still carries the handle", compiled.DynamicInvoke(4242) is int v && v == 4242);
        return f;
    }

    // The equivalence the sugar claims, checked against the long form rather than asserted. Both
    // graphs are invoked with the same argument and must agree.
    private static int SelfIsIdenticalToWritingItTheLongWay()
    {
        Console.WriteLine("Test: Self and 'PARAM entity int' + a Param node compile to the same behaviour");
        int f = 0;
        const string longForm =
            "OCGRAPH 1\n" +
            "PARAM entity int\n" +
            "NODE me Param param=entity\n" +
            "OUT me value\n";

        if (!OcGraphParser.Parse(CanvasShaped, out var gs, out var e1)) return Expect("sugar parses", false, e1 ?? "");
        if (!OcGraphParser.Parse(longForm, out var gl, out var e2)) return Expect("long form parses", false, e2 ?? "");
        var cs = new GraphCompiler(gs).Compile(out e1);
        var cl = new GraphCompiler(gl).Compile(out e2);
        if (cs == null || cl == null) return Expect("both compile", false, (e1 ?? "") + (e2 ?? ""));

        const int handle = 9_000_001;
        object? a = cs.DynamicInvoke(handle);
        object? b = cl.DynamicInvoke(handle);
        f += Expect("both return the same value", a is int x && b is int y && x == y && x == handle,
                    $"sugar={a} long={b}");
        return f;
    }

    // A float cannot hold an entity handle: above 2^24 the mantissa has run out of integers, so the
    // value rounds to a NEIGHBOURING handle -- a real entity, just not this one. Every call
    // downstream then succeeds on the wrong object, which reads as a framework fault rather than a
    // typo. Refused at parse time instead.
    private static int SelfRefusesAMistypedEntityParam()
    {
        Console.WriteLine("Test: Self refuses a graph that declares 'entity' as a float");
        const string text =
            "OCGRAPH 1\n" +
            "PARAM entity float\n" +
            "NODE me Self\n" +
            "OUT me value\n";
        bool parsed = OcGraphParser.Parse(text, out _, out var err);
        int f = Expect("the parse is refused", !parsed, "it was accepted");
        f += Expect("and the message says an entity handle does not fit",
                    !parsed && err != null && err.Contains("does not fit"), err ?? "no message");
        return f;
    }

    // A function's arguments are its OWN declared inputs, so a Param (or Self) node inside one would
    // emit an Ldarg into the function's frame and read whichever input sits at that index -- a
    // different value, silently. Both spellings are refused; the Self case is checked separately
    // because the desugar runs before Validate and could have hidden it.
    private static int SelfInsideAFunctionIsRefused()
    {
        Console.WriteLine("Test: a Self node inside a function body is refused, not silently misread");
        const string text =
            "OCGRAPH 1\n" +
            "FUNC Helper pure\n" +
            "FUNCOUT Helper out int\n" +
            "NODE fe FuncEntry func=Helper\n" +
            "NODE me Self func=Helper\n" +
            "NODE fr FuncReturn func=Helper\n" +
            "LINK me.value fr.out\n";
        bool parsed = OcGraphParser.Parse(text, out _, out var err);
        int f = Expect("the parse is refused", !parsed, "it was accepted");
        f += Expect("and the message names the function and offers the fix",
                    !parsed && err != null && err.Contains("Helper") && err.Contains("inputs"),
                    err ?? "no message");
        return f;
    }

    private static int ParamInsideAFunctionIsRefused()
    {
        Console.WriteLine("Test: a Param node inside a function body is refused too");
        const string text =
            "OCGRAPH 1\n" +
            "PARAM entity int\n" +
            "FUNC Helper pure\n" +
            "FUNCOUT Helper out int\n" +
            "NODE fe FuncEntry func=Helper\n" +
            "NODE me Param param=entity func=Helper\n" +
            "NODE fr FuncReturn func=Helper\n" +
            "LINK me.value fr.out\n";
        bool parsed = OcGraphParser.Parse(text, out _, out var err);
        int f = Expect("the parse is refused", !parsed, "it was accepted");
        f += Expect("and the message names the function",
                    !parsed && err != null && err.Contains("Helper"), err ?? "no message");
        return f;
    }
}
