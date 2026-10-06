// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the table-driven game-systems nodes (timers and events, blackboard, streamed audio, anim
// state machines, game UI, prefabs, decals, crowds/hearing/cover).
//
// WHAT THESE CAN AND CANNOT PROVE. This bare process has no native engine DLLs, so nothing here
// asserts that a timer fires or a widget appears; each feature's native test does that. What these
// prove is the WIRING the table is responsible for: every row names a real method whose parameters
// match the pins feeding them, the parser gives every node its pins, an exec node is refused by the
// pure compiler, a pure read works under it, a missing attribute fails by name, and the emitted IL is
// VALID for every node -- invoking it either runs or fails inside the native call, never with an
// InvalidProgramException.
using System;
using System.Linq;
using System.Reflection;
using System.Text;
using Aver.Graph;

static class GameSystemNodeTests
{
    public static int RunAll()
    {
        int failures = 0;
        failures += TestTableMatchesMethods();
        failures += TestDefaultPinsAndValues();
        failures += TestEveryNodeEmitsValidIl();
        failures += TestExecNodeRefusedByPullCompiler();
        failures += TestPureNodeWorksOnBothCompilers();
        failures += TestMissingAttributeFailsByName();
        failures += TestOutputsReadDownstream();
        failures += TestOnEventIsAnEntryNode();
        return failures;
    }

    private static int Fail(string msg) { Console.WriteLine("  FAIL: " + msg); return 1; }

    private static int TestTableMatchesMethods()
    {
        Console.WriteLine("Test: every table row names a real method whose parameters match its pins");
        var problems = GameSystemNodes.Validate();
        foreach (var p in problems) Console.WriteLine("    " + p);
        if (problems.Count > 0) return Fail($"{problems.Count} problem(s)");
        Console.WriteLine($"  PASS: {GameSystemNodes.All.Count()} rows");
        return 0;
    }

    // One NODE line for a table node, with a dummy value for every attribute it takes.
    private static string NodeLine(GenericNodeSpec s, string id)
    {
        var sb = new StringBuilder($"NODE {id} {s.Type}");
        foreach (var a in s.Args.Where(x => x.StartsWith('@')))
        {
            string key = a[1..].TrimEnd('?').Split('|')[0];
            sb.Append($" {key}=dummy");
        }
        return sb.ToString();
    }

    private static int TestDefaultPinsAndValues()
    {
        Console.WriteLine("Test: a bare NODE line gets the table's pins, and its default pin values");
        const string text = "OCGRAPH 1\nNODE pm AN_PlayMusic sound=a.ocaudio\nNODE ev AN_OnEvent\nNODE bb GetBlackboardVec3 key=Aim\n";
        if (!OcGraphParser.Parse(text, out var g, out var err)) return Fail("parse: " + err);

        var pm = g.Nodes["pm"];
        bool pmOk = pm.Pins.Count == 8 &&
                    pm.Pins.Any(p => p.Name == "exec" && !p.IsOutput && p.Type == PinType.Exec) &&
                    pm.Pins.Any(p => p.Name == "then" && p.IsOutput && p.Type == PinType.Exec) &&
                    pm.Pins.Any(p => p.Name == "curve" && !p.IsOutput && p.Type == PinType.Int) &&
                    pm.Pins.Any(p => p.Name == "voice" && p.IsOutput && p.Type == PinType.Int);
        if (!pmOk) return Fail("AN_PlayMusic pins: " + string.Join(",", pm.Pins.Select(p => p.Name)));

        var vol = g.PinnedValues.FirstOrDefault(v => v.NodeId == "pm" && v.PinName == "volume");
        var looping = g.PinnedValues.FirstOrDefault(v => v.NodeId == "pm" && v.PinName == "looping");
        if (vol == null || vol.Value is not float f || Math.Abs(f - 1f) > 1e-6f) return Fail("volume default is not 1.0f");
        if (looping == null || looping.Value is not bool b || !b) return Fail("looping default is not true");

        var ev = g.Nodes["ev"];
        if (ev.Pins.Count != 1 || ev.Pins[0].Name != "exec" || !ev.Pins[0].IsOutput) return Fail("AN_OnEvent is not a bare exec output");

        var bb = g.Nodes["bb"];
        if (bb.Pins.Any(p => p.Type == PinType.Exec) || bb.Pins.Count != 5) return Fail("GetBlackboardVec3 should be pure with entity + x,y,z,success");
        if (bb.Attrs["key"] != "Aim") return Fail("key= did not reach Node.Attrs");

        Console.WriteLine("  PASS");
        return 0;
    }

    // Parse + compile + invoke every node once. Invoking JIT-compiles the emitted IL, so an invalid
    // program throws InvalidProgramException before any native call; anything else (the missing DLL,
    // an entry point that does not exist) means the IL itself was fine.
    private static int TestEveryNodeEmitsValidIl()
    {
        Console.WriteLine("Test: the IL emitted for every table node is valid");
        int bad = 0, ran = 0;
        foreach (var s in GameSystemNodes.All.Where(x => x.Kind != GenericKind.Trigger))
        {
            string text;
            Func<GraphCompiler, Delegate?> compile;
            string? err = null;
            if (s.Kind == GenericKind.Exec)
            {
                text = "OCGRAPH 1\nNODE tick OnTick\n" + NodeLine(s, "n") + "\nLINK tick.exec n.exec\nENTRY tick OnTick\n";
                compile = c => c.CompileEntryPoint("OnTick", out err);
            }
            else
            {
                string outPin = s.Outputs[0].Name;
                text = "OCGRAPH 1\n" + NodeLine(s, "n") + $"\nOUT n {outPin}\n";
                compile = c => c.Compile(out err);
            }
            if (!OcGraphParser.Parse(text, out var g, out var perr)) { Console.WriteLine($"    {s.Type}: parse: {perr}"); ++bad; continue; }
            Delegate? fn;
            try { fn = compile(new GraphCompiler(g)); }
            catch (Exception e) { Console.WriteLine($"    {s.Type}: compile threw {e.GetType().Name}: {e.Message}"); ++bad; continue; }
            if (fn == null) { Console.WriteLine($"    {s.Type}: compile: {err}"); ++bad; continue; }
            try { fn.DynamicInvoke(); ++ran; }
            catch (TargetInvocationException e) when (e.InnerException is not InvalidProgramException) { ++ran; }
            catch (Exception e) { Console.WriteLine($"    {s.Type}: invoke: {e.GetType().Name}: {e.Message}"); ++bad; }
        }
        if (bad > 0) return Fail($"{bad} node(s) did not compile or emitted invalid IL");
        Console.WriteLine($"  PASS: {ran} nodes compiled and JIT'd");
        return 0;
    }

    private static int TestExecNodeRefusedByPullCompiler()
    {
        Console.WriteLine("Test: an exec node in a no-ENTRY graph is refused by Compile(), naming the node");
        const string text = "OCGRAPH 1\nNODE t AN_SetTimer event=Tick\nOUT t handle\n";
        if (!OcGraphParser.Parse(text, out var g, out var err)) return Fail("parse: " + err);
        if (new GraphCompiler(g).Compile(out var cerr) != null) return Fail("Compile() accepted an ungated AN_SetTimer");
        if (cerr == null || !cerr.Contains("AN_SetTimer", StringComparison.OrdinalIgnoreCase)) return Fail("error does not name the node: " + cerr);
        Console.WriteLine("  PASS: " + cerr);
        return 0;
    }

    private static int TestPureNodeWorksOnBothCompilers()
    {
        Console.WriteLine("Test: a pure read compiles under BOTH compilers and its outputs can be pulled by pin");
        const string pull = "OCGRAPH 1\nNODE e ConstInt value=1\nNODE g GetBlackboardVec3 key=Aim\nLINK e.value g.entity\nOUT g y\nOUT g success\n";
        if (!OcGraphParser.Parse(pull, out var g1, out var e1)) return Fail("pull parse: " + e1);
        if (new GraphCompiler(g1).Compile(out var c1) == null) return Fail("Compile() refused a pure read: " + c1);

        const string push = "OCGRAPH 1\nNODE tick OnTick\nNODE e ConstInt value=1\nNODE g GetBlackboardFloat key=Hp\n"
                          + "NODE ps AN_SetTimer event=Beat\nLINK e.value g.entity\nLINK tick.exec ps.exec\n"
                          + "LINK g.value ps.delay\nENTRY tick OnTick\nOUT ps handle\n";
        if (!OcGraphParser.Parse(push, out var g2, out var e2)) return Fail("push parse: " + e2);
        if (new GraphCompiler(g2).CompileEntryPoint("OnTick", out var c2) == null) return Fail("CompileEntryPoint refused a pure read feeding an exec node: " + c2);
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int TestMissingAttributeFailsByName()
    {
        Console.WriteLine("Test: a node missing its required attribute fails to compile, naming the attribute");
        string[] cases =
        {
            "NODE n AN_SetTimer",        // event=
            "NODE n SetBlackboardFloat", // key=
            "NODE n AN_PlayStream",      // sound=
            "NODE n AN_SpawnPrefab",     // prefab=
        };
        string[] wants = { "event=", "key=", "sound=", "prefab=" };
        for (int i = 0; i < cases.Length; ++i)
        {
            string text = "OCGRAPH 1\nNODE tick OnTick\n" + cases[i] + "\nLINK tick.exec n.exec\nENTRY tick OnTick\n";
            if (!OcGraphParser.Parse(text, out var g, out var err)) return Fail("parse: " + err);
            if (new GraphCompiler(g).CompileEntryPoint("OnTick", out var cerr) != null) return Fail($"{cases[i]}: compiled with no {wants[i]}");
            if (cerr == null || !cerr.Contains(wants[i])) return Fail($"{cases[i]}: error does not mention {wants[i]}: {cerr}");
        }
        // The optional ones do not need it.
        const string ok = "OCGRAPH 1\nNODE tick OnTick\nNODE n AN_FindPrefabNode\nLINK tick.exec n.exec\nENTRY tick OnTick\n";
        if (!OcGraphParser.Parse(ok, out var g2, out var e2)) return Fail("parse: " + e2);
        if (new GraphCompiler(g2).CompileEntryPoint("OnTick", out var c2) == null) return Fail("AN_FindPrefabNode (node= optional) refused: " + c2);
        Console.WriteLine("  PASS");
        return 0;
    }

    // An exec node's outputs are read by later nodes through its exec locals.
    private static int TestOutputsReadDownstream()
    {
        Console.WriteLine("Test: an exec node's outputs feed later nodes (SetTimer.handle -> ClearTimer.handle)");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE st AN_SetTimer event=Beat\nNODE ct AN_ClearTimer\n"
                          + "LINK tick.exec st.exec\nLINK st.then ct.exec\nLINK st.handle ct.handle\nENTRY tick OnTick\nOUT ct cleared\n";
        if (!OcGraphParser.Parse(text, out var g, out var err)) return Fail("parse: " + err);
        var fn = new GraphCompiler(g).CompileEntryPoint("OnTick", out var cerr);
        if (fn == null) return Fail("compile: " + cerr);
        try { fn.DynamicInvoke(); }
        catch (TargetInvocationException e) when (e.InnerException is not InvalidProgramException) { }
        catch (Exception e) { return Fail(e.GetType().Name + ": " + e.Message); }
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int TestOnEventIsAnEntryNode()
    {
        Console.WriteLine("Test: AN_OnEvent starts a chain, like OnHit");
        const string text = "OCGRAPH 1\nNODE e AN_OnEvent\nNODE p AN_EventPayload\nNODE pr PrintInt\nLINK e.exec p.exec\n"
                          + "LINK p.i pr.value\nLINK p.then pr.exec\nENTRY e Damaged\n";
        if (!OcGraphParser.Parse(text, out var g, out var err)) return Fail("parse: " + err);
        var fn = new GraphCompiler(g).CompileEntryPoint("Damaged", out var cerr);
        if (fn == null) return Fail("compile: " + cerr);
        var plain = new GraphCompiler(g).Compile(out var pe);
        if (plain != null || pe == null) { /* an exec-only graph is fine to Compile() as nothing; just must not throw */ }
        Console.WriteLine("  PASS");
        return 0;
    }
}
