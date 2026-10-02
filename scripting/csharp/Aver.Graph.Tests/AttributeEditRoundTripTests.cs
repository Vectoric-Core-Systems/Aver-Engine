// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
// The Gap B cross-language proof: tests/editor/src/GraphEditorLoadSaveTest.cpp's
// testAttributeEditRoundTrip() loads attribute_edit_test.ocgraph (checked in, right beside this file),
// edits the 'sp' node through GraphEditor's REAL public setAttribute()/save() path -- the exact same
// two calls the ImGui details panel in sandbox/src/GraphEditor.cpp's draw() makes -- and writes the
// result to attribute_edit_test.output.ocgraph, next to it.
//
// This file reads THAT generated output (not the checked-in "before" fixture, and not a hand-typed
// transcription of it) and proves, with the real C# runtime, that what the C++ editor wrote is still
// valid input: it parses, the attribute the C++ editor added is readable as Node.ClassName exactly the
// way OcGraphParser.cs's own "class=" case expects, the hand-authored attribute the editor's catalog
// does not recognise survived untouched, and the whole graph still COMPILES and reaches a real native
// call when invoked -- not merely "the file still has the right shape".
//
// RUN GraphEditorLoadSaveTest.exe BEFORE this suite, at least once, or TestAttributeEditOutputExists
// below fails loudly with instructions rather than this file silently skipping.
using System;
using System.IO;
using Aver.Graph;

static class AttributeEditRoundTripTests
{
    private const string OutputFile = "attribute_edit_test.output.ocgraph";

    public static int RunAll()
    {
        int failures = 0;
        failures += TestAttributeEditOutputExists();
        failures += TestClassAttributeTheCppEditorAddedIsReadable();
        failures += TestUnrecognisedAttributeSurvivedTheCppEdit();
        failures += TestEditedGraphStillCompilesAndEmitsARealNativeCall();
        return failures;
    }

    private static int TestAttributeEditOutputExists()
    {
        Console.WriteLine("Test: the C++ editor's output file is present (run GraphEditorLoadSaveTest.exe first)");
        if (!File.Exists(OutputFile))
        {
            Console.WriteLine($"  FAIL: '{OutputFile}' not found in {Directory.GetCurrentDirectory()}. " +
                               "Run build/bin/GraphEditorLoadSaveTest.exe first -- its testAttributeEditRoundTrip() " +
                               "writes this file from a real GraphEditor::setAttribute()/save() call.");
            return 1;
        }
        Console.WriteLine("  PASS: found");
        return 0;
    }

    private static int TestClassAttributeTheCppEditorAddedIsReadable()
    {
        Console.WriteLine("Test: 'sp'.class=, added by the C++ editor's setAttribute(), parses as Node.ClassName");
        try
        {
            if (!File.Exists(OutputFile)) { Console.WriteLine("  FAIL: output file missing (see previous test)"); return 1; }
            string text = File.ReadAllText(OutputFile);
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            if (!graph.Nodes.TryGetValue("sp", out var sp))
            {
                Console.WriteLine("  FAIL: node 'sp' not found in the parsed graph");
                return 1;
            }
            if (sp.ClassName != "SomeSpawnedClass")
            {
                Console.WriteLine($"  FAIL: expected ClassName 'SomeSpawnedClass' (set by GraphEditor::setAttribute), got '{sp.ClassName}'");
                return 1;
            }
            Console.WriteLine("  PASS: Node.ClassName == 'SomeSpawnedClass' -- the C++ editor's edit round-trips into the real C# reader");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestUnrecognisedAttributeSurvivedTheCppEdit()
    {
        Console.WriteLine("Test: 'debugLabel=spawn_test_marker' (C++ editor does not recognise it) survived the class= edit, verbatim");
        try
        {
            if (!File.Exists(OutputFile)) { Console.WriteLine("  FAIL: output file missing (see previous test)"); return 1; }
            string text = File.ReadAllText(OutputFile);
            // Byte-level check, deliberately not routed through OcGraphParser: OcGraphParser.cs has no
            // 'debugLabel' case at all (grep confirms), so this token would be silently ignored by the
            // C# parser either way -- the property this test actually needs proven is that the C++
            // WRITER put the exact bytes back, which only a text check can show.
            if (!text.Contains("debugLabel=spawn_test_marker"))
            {
                Console.WriteLine("  FAIL: 'debugLabel=spawn_test_marker' is gone from the C++ editor's output -- an edit to 'class' corrupted an unrelated attribute");
                return 1;
            }
            if (!text.Contains("NODE sp Spawn 400 100 debugLabel=spawn_test_marker class=SomeSpawnedClass"))
            {
                Console.WriteLine("  FAIL: the two tokens are present but not in the expected NODE line / order");
                return 1;
            }
            Console.WriteLine("  PASS: the hand-authored, uncatalogued attribute survived the C++ editor's class= edit, in its original position");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // The strongest form of "not corrupted for the C# reader": not just parses, actually COMPILES and
    // reaches a real native call on invoke -- same evidentiary shape SpawnNodeTests.cs's own
    // TestSpawnEmitsRealNativeCallOnExecChain uses (this graph is that exact shape, with 'debugLabel='
    // added and 'class=Widget' replaced by the C++ editor's 'class=SomeSpawnedClass').
    private static int TestEditedGraphStillCompilesAndEmitsARealNativeCall()
    {
        Console.WriteLine("Test: the C++-edited graph still compiles (CompileEntryPoint) and reaches a real native call on invoke");
        try
        {
            if (!File.Exists(OutputFile)) { Console.WriteLine("  FAIL: output file missing (see previous test)"); return 1; }
            string text = File.ReadAllText(OutputFile);
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnTick", out var compileErr);
            if (compiled is not Func<int> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                int unused = fn();
                Console.WriteLine($"  FAIL: expected invoking this to throw (see SpawnNodeTests.cs's own header comment) but it returned {unused}");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_fw_class_find"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_fw_class_find: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: compiled from the C++ editor's own output and reached the real 'aver_fw_class_find' call on invoke: {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }
}
