// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
//
// THE ERROR MESSAGE A PUSH-ONLY NODE GIVES WHEN IT REACHES THE WRONG COMPILER.
//
// Four node types -- Spawn, CharacterMove, FireEvent, SetVar -- have their own hand-written case in
// EmitNode's switch, and each throws a sentence that names the node and points at CompileEntryPoint().
// Those four are tested elsewhere (SpawnNodeTests, CharacterMoveNodeTests, FireEventNodeTests,
// GraphVarTests) and are not the subject here.
//
// EVERY OTHER PUSH-ONLY NODE -- Jump, Print, PrintInt, SetVelocity, Teleport, Possess, Unpossess, the
// ten transform writers, the eleven physics writers and creators -- has NO case in that switch. They
// fell through to `default: throw new NotSupportedException($"Node type '{node.Type}' is not
// supported")`. That sentence is FALSE in the way that costs the most: the type IS supported, by the
// other compiler, and an author reading "Node type 'Jump' is not supported" concludes the engine has
// no Jump node and goes looking for a workaround for a problem that does not exist.
//
// These tests pin the corrected message. They deliberately assert on CONTENT, not on exception type:
// what matters is that the sentence names the node, says the type is supported, and names the thing
// to do about it. A future refactor is free to move which predicate catches it.
using System;
using Aver.Graph;

// No namespace, matching every other file in this suite.
static class PushOnlyNodeMessageTests
{
    public static int RunAll()
    {
        int failures = 0;
        // One per FAMILY of push-only node that reaches the default arm, rather than all thirty:
        // they share a single code path, and a test per node would pin the list rather than the
        // behaviour -- so adding a node would break tests that were not about it.
        failures += ExpectHelpfulRefusal("Jump", "NODE ch ConstInt value=1\nNODE j Jump\nLINK ch.value j.entity\nOUT j jumped\n");
        failures += ExpectHelpfulRefusal("PrintInt", "NODE v ConstInt value=7\nNODE p PrintInt\nLINK v.value p.value\nOUT v value\n");
        failures += ExpectHelpfulRefusal("SetGravity", "NODE z ConstFloat value=-980\nNODE g SetGravity\nLINK z.value g.z\nOUT z value\n");
        failures += ExpectHelpfulRefusal("Teleport", "NODE e ConstInt value=1\nNODE t Teleport\nLINK e.value t.entity\nOUT t success\n");
        failures += ExpectStillUnknownTypesAreStillUnknown();
        return failures;
    }

    // A graph containing `nodeType` and NO ENTRY record, so Compile() -- the pure-dataflow compiler --
    // is the one that meets it.
    private static int ExpectHelpfulRefusal(string nodeType, string body)
    {
        Console.WriteLine($"Test: a push-only {nodeType} met by Compile() is told the type is supported, not that it isn't");
        try
        {
            if (!OcGraphParser.Parse("OCGRAPH 1\n" + body, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiled = new GraphCompiler(graph).Compile(out var compileErr);
            if (compiled != null)
            {
                Console.WriteLine($"  FAIL: expected Compile() to refuse a {nodeType} with no exec chain, but it succeeded");
                return 1;
            }
            if (compileErr == null)
            {
                Console.WriteLine("  FAIL: refused with no error message at all");
                return 1;
            }
            // THE REGRESSION THIS EXISTS TO CATCH, stated as its own check so a failure says which
            // half broke: the old sentence, verbatim, must not come back.
            if (compileErr.IndexOf("is not supported", StringComparison.OrdinalIgnoreCase) >= 0)
            {
                Console.WriteLine($"  FAIL: still claims the node type is not supported: {compileErr}");
                return 1;
            }
            if (compileErr.IndexOf(nodeType, StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: the error does not name the node type '{nodeType}': {compileErr}");
                return 1;
            }
            if (compileErr.IndexOf("CompileEntryPoint", StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: the error does not say what to do instead: {compileErr}");
                return 1;
            }
            Console.WriteLine($"  PASS: {compileErr}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: unexpected exception: {ex.GetType().Name}: {ex.Message}");
            return 1;
        }
    }

    // The other half of the change, and the one a fix like this usually breaks: a genuinely unknown
    // node type must STILL say it is not supported. Widening the helpful message to everything would
    // tell someone who typed `NODE x Ad` (meaning `Add`) to go and build an exec chain for it.
    private static int ExpectStillUnknownTypesAreStillUnknown()
    {
        Console.WriteLine("Test: a genuinely unknown node type is still reported as unsupported");
        try
        {
            if (!OcGraphParser.Parse("OCGRAPH 1\nNODE x NoSuchNodeTypeAnywhere\nOUT x value\n", out var graph, out var err))
            {
                // Parsing may itself reject it, which is an equally correct place to say so -- but
                // then the message has to be about the unknown type, not about exec chains.
                if (err != null && err.IndexOf("CompileEntryPoint", StringComparison.OrdinalIgnoreCase) < 0)
                {
                    Console.WriteLine($"  PASS (refused at parse): {err}");
                    return 0;
                }
                Console.WriteLine($"  FAIL: parse refused it with an exec-chain message: {err}");
                return 1;
            }
            var compiled = new GraphCompiler(graph).Compile(out var compileErr);
            if (compiled != null)
            {
                Console.WriteLine("  FAIL: an unknown node type compiled successfully");
                return 1;
            }
            if (compileErr != null && compileErr.IndexOf("CompileEntryPoint", StringComparison.OrdinalIgnoreCase) >= 0)
            {
                Console.WriteLine($"  FAIL: an unknown type was told to build an exec chain: {compileErr}");
                return 1;
            }
            Console.WriteLine($"  PASS: {compileErr}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: unexpected exception: {ex.GetType().Name}: {ex.Message}");
            return 1;
        }
    }
}
