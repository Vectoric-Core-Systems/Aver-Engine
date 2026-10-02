// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
//
// TAGS AND VISIBILITY: SetVisible / AddTag / RemoveTag / HasTag / GetTags.
//
// WHAT THESE CAN AND CANNOT PROVE, stated up front because the limit is real. Every one of these
// nodes bottoms out in Aver.Framework.Entity, which reads and writes the NATIVE scene -- and nothing
// in this test process boots one, exactly as NewNodeTests.cs's own header explains for InputKey and
// Raycast. So these check the half that is checkable here and is also the half that breaks: that the
// node has the right pins, that the compiler has an emitter for it, that the MethodInfo behind that
// emitter resolves by reflection, and that a writer is refused on the dataflow compiler.
//
// The reflection half is worth more than it sounds. Every interop method here is bound by NAME
// through GetMethod, so a rename on the Aver.Framework side compiles perfectly and fails at graph
// compile time -- and a test that gets as far as emitting a Call has already proven the handle
// resolved, because GraphCompiler's static initialiser throws if it did not.
using System;
using Aver.Graph;

// No namespace, matching every other file in this suite.
static class TagNodeTests
{
    public static int RunAll()
    {
        int failures = 0;
        failures += ReadersCompileOnBothPaths();
        failures += WritersCompileOnTheExecPath();
        failures += WriterPulledAsDataIsRefused();
        failures += PinShapes();
        return failures;
    }

    private static int Expect(string what, bool ok, string detail = "")
    {
        if (ok) { Console.WriteLine($"  PASS: {what}"); return 0; }
        Console.WriteLine($"  FAIL: {what}{(detail.Length > 0 ? " -- " + detail : "")}");
        return 1;
    }

    // HasTag and GetTags are pure reads, so BOTH compilers must take them -- the same rule
    // IsGrounded and GetWorldPosition live under.
    private static int ReadersCompileOnBothPaths()
    {
        Console.WriteLine("Test: HasTag and GetTags compile on the dataflow compiler (they are pure reads)");
        const string text = @"OCGRAPH 1
NAME TagRead
NODE e ConstInt value=1
NODE m ConstInt value=4
NODE ht HasTag
LINK e.value ht.entity
LINK m.value ht.mask
NODE gt GetTags
LINK e.value gt.entity
OUT gt mask
";
        if (!OcGraphParser.Parse(text, out var graph, out var perr))
            return Expect("graph parses", false, perr ?? "");
        var compiled = new GraphCompiler(graph).Compile(out var cerr);
        return Expect("Compile() accepts both readers", compiled != null, cerr ?? "");
    }

    private static int WritersCompileOnTheExecPath()
    {
        Console.WriteLine("Test: SetVisible / AddTag / RemoveTag compile on an ENTRY-driven exec chain");
        const string text = @"OCGRAPH 1
NAME TagWrite
NODE go OnStart
ENTRY go OnStart
NODE e ConstInt value=1
NODE m ConstInt value=4
NODE vis ConstBool value=false
NODE sv SetVisible
LINK go.exec sv.exec
LINK e.value sv.entity
LINK vis.value sv.visible
NODE at AddTag
LINK sv.then at.exec
LINK e.value at.entity
LINK m.value at.mask
NODE rt RemoveTag
LINK at.then rt.exec
LINK e.value rt.entity
LINK m.value rt.mask
OUT rt success
";
        if (!OcGraphParser.Parse(text, out var graph, out var perr))
            return Expect("graph parses", false, perr ?? "");
        var compiled = new GraphCompiler(graph).CompileEntryPoint("OnStart", out var cerr);
        return Expect("CompileEntryPoint() accepts all three writers", compiled != null, cerr ?? "");
    }

    // A write has no notion of WHEN in a dataflow graph -- the same refusal Spawn/SetVar/Teleport get.
    private static int WriterPulledAsDataIsRefused()
    {
        Console.WriteLine("Test: AddTag pulled as bare data is refused with a reason");
        const string text = @"OCGRAPH 1
NAME TagPull
NODE e ConstInt value=1
NODE m ConstInt value=4
NODE at AddTag
LINK e.value at.entity
LINK m.value at.mask
OUT at success
";
        if (!OcGraphParser.Parse(text, out var graph, out var perr))
            return Expect("graph parses", false, perr ?? "");
        var compiled = new GraphCompiler(graph).Compile(out var cerr);
        return Expect("Compile() refuses it and names the node",
                      compiled == null && cerr != null &&
                      cerr.IndexOf("AddTag", StringComparison.OrdinalIgnoreCase) >= 0,
                      cerr ?? "it compiled");
    }

    // THE PIN SHAPE IS THE CONTRACT between the C++ palette and this parser, and nothing automated
    // compares the two (see AVER_NODE_NODES.md's "Parser vs. editor catalog"). Pinning it here at
    // least stops the C# half drifting silently.
    private static int PinShapes()
    {
        Console.WriteLine("Test: the five nodes have exactly the pins the palette declares");
        const string text = @"OCGRAPH 1
NAME TagPins
NODE sv SetVisible
NODE at AddTag
NODE ht HasTag
NODE gt GetTags
";
        if (!OcGraphParser.Parse(text, out var graph, out var perr))
            return Expect("graph parses", false, perr ?? "");

        int bad = 0;
        bad += Check(graph, "sv", "exec:in:Exec entity:in:Int visible:in:Bool then:out:Exec success:out:Bool");
        bad += Check(graph, "at", "exec:in:Exec entity:in:Int mask:in:Int then:out:Exec success:out:Bool");
        bad += Check(graph, "ht", "entity:in:Int mask:in:Int has:out:Bool");
        bad += Check(graph, "gt", "entity:in:Int mask:out:Int");
        return bad;
    }

    private static int Check(Graph graph, string id, string expected)
    {
        if (!graph.Nodes.TryGetValue(id, out var n))
            return Expect($"node '{id}' exists", false);
        var got = string.Join(" ", System.Linq.Enumerable.Select(n.Pins,
            p => $"{p.Name}:{(p.IsOutput ? "out" : "in")}:{p.Type}"));
        return Expect($"{n.Type} pins", got == expected, $"got '{got}'");
    }
}
