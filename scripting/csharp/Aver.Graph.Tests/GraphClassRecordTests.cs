// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
// Tests for the CLASS record -- GRAPH-AS-CLASS's format half: a .ocgraph declaring itself a
// spawnable actor class. See OcGraphParser.cs's "CLASS" case and Graph.cs's ClassName/ClassParent/
// ClassMesh/ClassMaterial fields for what these tests are proving parses correctly. Registration
// (aver_fw_class_declare) and per-instance binding are Aver.Scripting.Bridge's job (HostBridge.cs's
// "graph classes" region) and are NOT reachable from this bare test process -- see
// CharacterMoveNodeTests.cs's own header comment for the general "no native scripting host in this
// process" limitation this suite already lives with. These tests are parse-level only, by design.
using System;
using Aver.Graph;

static class GraphClassRecordTests
{
    public static int RunAll()
    {
        int failures = 0;

        failures += TestClassRecordDefaultsParentToActor();
        failures += TestClassRecordWithExplicitParent();
        failures += TestClassRecordWithMeshAndMaterial();
        failures += TestClassRecordWithView();
        failures += TestClassRecordWithoutViewLeavesClassViewNull();
        failures += TestClassRecordWithPawnAndController();
        failures += TestClassRecordWithoutPawnLeavesBothNull();
        failures += TestGraphWithNoClassRecordHasNullClassName();
        failures += TestDuplicateClassRecordIsRejected();
        failures += TestClassRecordWithNoNameIsRejected();

        return failures;
    }

    // pawn=/controller= are what let a GRAPH GameMode be possessed at all. Parse-level only, like every
    // other test here -- what HostBridge does with them (resolve by name in a second pass, refuse a
    // non-GameMode, warn when a pawn is named with no controller) needs the framework ABI, which this
    // bare process has no host for. See Graph.cs's ClassPawn/ClassController doc comments.
    //
    // THE PAIR IS TESTED TOGETHER because they are only useful together: aver_fw_begin_play possesses
    // when it has BOTH, and the built-in PlayerController is abstract and unspawnable, so a pawn named
    // alone is spawned and never possessed.
    private static int TestClassRecordWithPawnAndController()
    {
        Console.WriteLine("Test: CLASS pawn=/controller= parse together on a GameMode record");
        try
        {
            var text = "OCGRAPH 1\nCLASS AN_Rules GameMode pawn=AN_Hero controller=AN_Ctrl\nNODE k ConstFloat value=1.0\nOUT k value\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            bool ok = graph.ClassName == "AN_Rules"
                   && graph.ClassParent == "GameMode"
                   && graph.ClassPawn == "AN_Hero"
                   && graph.ClassController == "AN_Ctrl";
            if (!ok)
            {
                Console.WriteLine($"  FAIL: name='{graph.ClassName}' parent='{graph.ClassParent}' "
                                + $"pawn='{graph.ClassPawn}' controller='{graph.ClassController}'");
                return 1;
            }
            Console.WriteLine("  PASS: both parsed, and neither displaced the parent token");
            return 0;
        }
        catch (Exception ex) { Console.WriteLine($"  FAIL: {ex.Message}"); return 1; }
    }

    private static int TestClassRecordWithoutPawnLeavesBothNull()
    {
        Console.WriteLine("Test: a CLASS record with no pawn=/controller= leaves both null");
        try
        {
            var text = "OCGRAPH 1\nCLASS AN_Rules GameMode\nNODE k ConstFloat value=1.0\nOUT k value\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            if (graph.ClassPawn != null || graph.ClassController != null)
            {
                Console.WriteLine($"  FAIL: expected both null, got pawn='{graph.ClassPawn}' "
                                + $"controller='{graph.ClassController}'");
                return 1;
            }
            Console.WriteLine("  PASS: both null, so HostBridge's second pass skips this class entirely");
            return 0;
        }
        catch (Exception ex) { Console.WriteLine($"  FAIL: {ex.Message}"); return 1; }
    }

    private static int TestClassRecordDefaultsParentToActor()
    {
        Console.WriteLine("Test: CLASS with no parent token defaults ClassParent to 'Actor'");
        try
        {
            var text = "OCGRAPH 1\nCLASS AN_Fixture\nNODE seed OnStart\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            if (graph.ClassName != "AN_Fixture" || graph.ClassParent != "Actor")
            {
                Console.WriteLine($"  FAIL: ClassName='{graph.ClassName}' ClassParent='{graph.ClassParent}'");
                return 1;
            }
            Console.WriteLine("  PASS: ClassName='AN_Fixture', ClassParent defaulted to 'Actor'");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: threw {ex}");
            return 1;
        }
    }

    private static int TestClassRecordWithExplicitParent()
    {
        Console.WriteLine("Test: CLASS <name> <parent> carries the explicit parent through");
        try
        {
            var text = "OCGRAPH 1\nCLASS AN_Guard Pawn\nNODE seed OnStart\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            if (graph.ClassName != "AN_Guard" || graph.ClassParent != "Pawn")
            {
                Console.WriteLine($"  FAIL: ClassName='{graph.ClassName}' ClassParent='{graph.ClassParent}'");
                return 1;
            }
            Console.WriteLine("  PASS: ClassName='AN_Guard', ClassParent='Pawn'");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: threw {ex}");
            return 1;
        }
    }

    private static int TestClassRecordWithMeshAndMaterial()
    {
        Console.WriteLine("Test: CLASS mesh=/material= attributes parse independently of the parent slot");
        try
        {
            var text = "OCGRAPH 1\nCLASS AN_Prop Actor mesh=Content/Meshes/Prop.ocmesh material=M_Prop\n" +
                       "NODE seed OnStart\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            bool ok = graph.ClassName == "AN_Prop" && graph.ClassParent == "Actor" &&
                      graph.ClassMesh == "Content/Meshes/Prop.ocmesh" && graph.ClassMaterial == "M_Prop";
            if (!ok)
            {
                Console.WriteLine($"  FAIL: ClassName='{graph.ClassName}' ClassParent='{graph.ClassParent}' " +
                                   $"ClassMesh='{graph.ClassMesh}' ClassMaterial='{graph.ClassMaterial}'");
                return 1;
            }
            Console.WriteLine("  PASS: mesh and material both parsed");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: threw {ex}");
            return 1;
        }
    }

    // view= is what the First Person template (templates/FirstPerson) added: the CLASS record's only
    // attribute that targets one specific ancestor type (AverCharacter) rather than every actor alike
    // -- see Graph.cs's ClassView doc comment for why it exists at all. Parse-level only, same
    // limitation this whole file's header comment already states: HostBridge's DispBind (the code
    // that actually reads ginfo.View and sets CameraViewMode) needs a live native+managed bridge this
    // process does not have.
    private static int TestClassRecordWithView()
    {
        Console.WriteLine("Test: CLASS view=firstperson parses independently of mesh=/material=/parent");
        try
        {
            var text = "OCGRAPH 1\nCLASS AN_Hero Character mesh=Meshes/x.ocmesh view=firstperson\n" +
                       "NODE seed OnStart\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            bool ok = graph.ClassParent == "Character" && graph.ClassMesh == "Meshes/x.ocmesh" &&
                      graph.ClassView == "firstperson";
            if (!ok)
            {
                Console.WriteLine($"  FAIL: ClassParent='{graph.ClassParent}' ClassMesh='{graph.ClassMesh}' " +
                                   $"ClassView='{graph.ClassView}'");
                return 1;
            }
            Console.WriteLine("  PASS: view= parsed alongside mesh= and an explicit parent");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: threw {ex}");
            return 1;
        }
    }

    private static int TestClassRecordWithoutViewLeavesClassViewNull()
    {
        Console.WriteLine("Test: a CLASS record with no view= leaves ClassView null (the overwhelmingly common case)");
        try
        {
            var text = "OCGRAPH 1\nCLASS AN_Guard Pawn\nNODE seed OnStart\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            if (graph.ClassView != null)
            {
                Console.WriteLine($"  FAIL: ClassView='{graph.ClassView}' (expected null)");
                return 1;
            }
            Console.WriteLine("  PASS: ClassView stayed null");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: threw {ex}");
            return 1;
        }
    }

    // BACKWARD COMPATIBILITY: this is the overwhelmingly common case -- every graph that predates
    // CLASS, including the checked-in cross-implementation fixture -- and it must stay a plain
    // project-utility/dataflow graph, not silently become a class.
    private static int TestGraphWithNoClassRecordHasNullClassName()
    {
        Console.WriteLine("Test: a graph with no CLASS record leaves ClassName/ClassParent null");
        try
        {
            var text = "OCGRAPH 1\nNAME Plain\nPARAM entity int\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            if (graph.ClassName != null || graph.ClassParent != null)
            {
                Console.WriteLine($"  FAIL: ClassName='{graph.ClassName}' ClassParent='{graph.ClassParent}' (expected both null)");
                return 1;
            }
            Console.WriteLine("  PASS: ClassName and ClassParent are both null");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: threw {ex}");
            return 1;
        }
    }

    private static int TestDuplicateClassRecordIsRejected()
    {
        Console.WriteLine("Test: a second CLASS record in one file is rejected, naming the reason");
        try
        {
            var text = "OCGRAPH 1\nCLASS AN_First\nCLASS AN_Second\nNODE seed OnStart\n";
            if (OcGraphParser.Parse(text, out _, out var err))
            {
                Console.WriteLine("  FAIL: expected a parse failure for a duplicate CLASS record");
                return 1;
            }
            if (err == null || !err.Contains("duplicate CLASS", StringComparison.OrdinalIgnoreCase))
            {
                Console.WriteLine($"  FAIL: error message does not name the duplicate-CLASS reason: '{err}'");
                return 1;
            }
            Console.WriteLine($"  PASS: {err}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: threw {ex}");
            return 1;
        }
    }

    private static int TestClassRecordWithNoNameIsRejected()
    {
        Console.WriteLine("Test: a bare 'CLASS' line with no name is rejected, not silently ignored");
        try
        {
            var text = "OCGRAPH 1\nCLASS\nNODE seed OnStart\n";
            if (OcGraphParser.Parse(text, out _, out var err))
            {
                Console.WriteLine("  FAIL: expected a parse failure for CLASS with no name");
                return 1;
            }
            Console.WriteLine($"  PASS: {err}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: threw {ex}");
            return 1;
        }
    }
}
