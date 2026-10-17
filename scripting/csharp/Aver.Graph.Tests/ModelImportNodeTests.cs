// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the model import nodes (docs/plans/wave9/FBX_IMPORT.md 2.10): Import Model (exec) and Is Model
// Import Available (pure) in GraphGameSystemNodeTable.cs, wrapping ModelImportGraph. Same bare-process scope
// as MontageNodeTests: the wiring the table owns, not what an editor does with a file.
using System;
using System.Linq;
using Aver.Framework;
using Aver.Graph;
using Row = NodeRowKit.Row;

static class ModelImportNodeTests
{
    private static readonly Row[] Rows =
    {
        new("importmodel", GenericKind.Exec, new[] { "exec:exec:in", "overwrite:bool:in", "scale:float:in", "then:exec:out", "success:bool:out", "meshes:int:out", "skeletons:int:out", "clips:int:out" }, new[] { "scale=100" }),
        new("ismodelimportavailable", GenericKind.Pure, new[] { "available:bool:out" }),
    };

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        failures += NodeRowKit.CheckRows("model import", Rows);
        failures += NodeRowKit.CheckDefaultPins("model import", Rows);
        failures += TestDocumentedDefaults();
        failures += NodeRowKit.CheckIlValid("model import", types);
        failures += NodeRowKit.CheckCompilerSplit("model import", types);
        failures += TestMissingAttributeFailsByName();
        failures += TestNoProviderAnswersUnavailable();
        failures += TestAvailabilityGatesImport();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    // The design's default: a scale of 100 (metres to centimetres).
    private static int TestDocumentedDefaults()
    {
        Console.WriteLine("Test: Import Model scale defaults to 100 and takes its source and destination as attributes");
        const string text = "OCGRAPH 1\nNODE i ImportModel source=Source/Hero.fbx dest=Meshes/Hero\n";
        if (!OcGraphParser.Parse(text, out var g, out var err)) return Fail("parse: " + err);
        var scale = g.PinnedValues.FirstOrDefault(v => v.NodeId == "i" && v.PinName == "scale")?.Value;
        if (scale is not float f || f != 100f) return Fail("scale default is missing or wrong");
        if (g.Nodes["i"].Attrs["source"] != "Source/Hero.fbx" || g.Nodes["i"].Attrs["dest"] != "Meshes/Hero")
            return Fail("source= / dest= did not reach Node.Attrs");
        Console.WriteLine("  PASS");
        return 0;
    }

    // Both attributes are required: a node missing one fails to compile and names it.
    private static int TestMissingAttributeFailsByName()
    {
        Console.WriteLine("Test: Import Model missing its source or dest fails to compile, naming it");
        int f = 0;
        f += NodeRowKit.CheckMissingAttribute("NODE n ImportModel dest=Meshes", "source=", exec: true);
        f += NodeRowKit.CheckMissingAttribute("NODE n ImportModel source=a.fbx", "dest=", exec: true);
        if (f == 0) Console.WriteLine("  PASS");
        return f == 0 ? 0 : 1;
    }

    // The method the row calls, directly: with no editor host (this process) it is unavailable, says so,
    // and never throws.
    private static int TestNoProviderAnswersUnavailable()
    {
        Console.WriteLine("Test: with no provider, ModelImportGraph answers unavailable and does not throw");
        try
        {
            if (ModelImportGraph.IsModelImportAvailableForGraph()) return Fail("a bare process reported import available");
            bool ok = ModelImportGraph.ImportModelForGraph("a.fbx", "Meshes", false, 100f, out int m, out int s, out int c);
            if (ok || m != 0 || s != 0 || c != 0) return Fail("ImportModelForGraph succeeded or counted something with no provider");
            var r = ModelImport.Import("a.fbx", "Meshes");
            if (r.Ok || r.Message.Length == 0) return Fail("ModelImport.Import gave no reason");
            if (ModelImport.Import("", "Meshes").Ok) return Fail("an empty source was accepted");
        }
        catch (Exception e) { return Fail("threw " + e.GetType().Name + ": " + e.Message); }
        Console.WriteLine("  PASS");
        return 0;
    }

    // The documented workflow: ask whether the host can import, then branch into the import.
    private static int TestAvailabilityGatesImport()
    {
        Console.WriteLine("Test: Is Model Import Available gates Import Model");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE q IsModelImportAvailable\nNODE b Branch\n" +
                            "NODE i ImportModel source=Source/Hero.fbx dest=Meshes/Hero\n" +
                            "LINK tick.exec b.exec\nLINK q.available b.cond\nLINK b.true i.exec\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }
}
