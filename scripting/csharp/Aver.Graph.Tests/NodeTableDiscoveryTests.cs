// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Node-table discovery: GameSystemNodes finds every table by reflection (a static zero-argument method returning
// GenericNodeSpec[]), tags each row with its family, tolerates a row repeated unchanged and reports a type that two
// tables define differently. Bare process, no native DLLs: this is registration, not behaviour.
using System;
using System.Collections.Generic;
using System.Linq;
using Aver.Graph;

[AutoRun]
static class NodeTableDiscoveryTests
{
    public static int RunAll()
    {
        int failures = 0;
        failures += TestEveryTableIsFound();
        failures += TestRowsAreTaggedWithTheirFamily();
        failures += TestLiveTablesHaveNoDuplicates();
        failures += TestIdenticalRepeatIsBenign();
        failures += TestConflictingDuplicateIsReported();
        failures += TestFamilyNames();
        return failures;
    }

    private static int Fail(string msg) { Console.WriteLine("  FAIL: " + msg); return 1; }

    // A synthetic row over a method that exists (System.Math.Abs), enough for the dictionary and the comparison.
    private static GenericNodeSpec Row(string type, string method = "Abs", string args = "p:a") => new()
    {
        Type = type, Kind = GenericKind.Pure, Owner = typeof(System.Math), MethodName = method,
        Inputs = new[] { ("a", PinType.Float, (object?)null) }, Outputs = new[] { ("result", PinType.Float) },
        Args = args.Split(','), Ret = "result",
    };

    private static int TestEveryTableIsFound()
    {
        Console.WriteLine("Test: every table is found by reflection and none of its rows is lost");
        var tables = GameSystemNodes.TableMethods().ToList();
        var families = tables.Select(t => t.Family).ToHashSet();
        foreach (var f in new[] { "GameSystem", "Gas", "AiQuery", "LevelInstance", "Terrain", "Ik", "MotionMatch", "Sequence" })
            if (!families.Contains(f)) return Fail($"the {f} table was not found (found: {string.Join(", ", families)})");

        int rows = tables.Sum(t => t.Rows.Length);
        int unique = tables.SelectMany(t => t.Rows).Select(r => r.Type.ToLowerInvariant()).Distinct().Count();
        int registered = GameSystemNodes.All.Count();
        if (registered != unique) return Fail($"{unique} distinct types across the tables, {registered} registered");
        if (registered < 189) return Fail($"only {registered} rows registered; 189 existed before discovery");
        Console.WriteLine($"  PASS: {tables.Count} tables, {rows} rows, {registered} types");
        return 0;
    }

    private static int TestRowsAreTaggedWithTheirFamily()
    {
        Console.WriteLine("Test: a row carries the family of the table it came from");
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var (family, rows) in GameSystemNodes.TableMethods())
            foreach (var r in rows)
                if (seen.Add(r.Type) && GameSystemNodes.TryGet(r.Type, out var s) && s.Family != family)
                    return Fail($"{r.Type} is in {family} but tagged {s.Family}");
        if (!GameSystemNodes.TryGet("an_settimer", out var timer) || timer.Family != "GameSystem")
            return Fail("an_settimer is not tagged GameSystem");
        if (!GameSystemNodes.TryGet("softbodydamage", out var soft) || soft.Family != "SoftBody")
            return Fail("softbodydamage is not tagged SoftBody");
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int TestLiveTablesHaveNoDuplicates()
    {
        Console.WriteLine("Test: no type is defined by two different rows in the shipped tables");
        var dups = GameSystemNodes.Duplicates;
        foreach (var d in dups) Console.WriteLine("    " + d);
        if (dups.Count > 0) return Fail($"{dups.Count} duplicate type(s)");
        if (GameSystemNodes.Validate().Any(p => p.Contains("defined by"))) return Fail("Validate() does not report them the same way");
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int TestIdenticalRepeatIsBenign()
    {
        Console.WriteLine("Test: a row repeated unchanged in a second table is kept once and not reported");
        var dups = new List<string>();
        var d = GameSystemNodes.Build(new[]
        {
            ("Alpha", new[] { Row("an_probe") }),
            ("Beta", new[] { Row("an_probe") }),
        }, dups);
        if (dups.Count != 0) return Fail("an identical repeat was reported: " + string.Join("; ", dups));
        if (d.Count != 1 || d["an_probe"].Family != "Alpha") return Fail("the first table's row should be the one kept");
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int TestConflictingDuplicateIsReported()
    {
        Console.WriteLine("Test: a type two tables define differently is reported by both family names");
        var dups = new List<string>();
        var d = GameSystemNodes.Build(new[]
        {
            ("Alpha", new[] { Row("an_probe", "Abs") }),
            ("Beta", new[] { Row("AN_Probe", "Sqrt") }),
        }, dups);
        if (dups.Count != 1) return Fail($"expected one report, got {dups.Count}");
        if (dups[0] != "AN_Probe: defined by Alpha and Beta") return Fail("report text was: " + dups[0]);
        if (d["an_probe"].MethodName != "Abs") return Fail("the first definition should win");
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int TestFamilyNames()
    {
        Console.WriteLine("Test: the family is the table method's name without Table");
        if (GameSystemNodes.FamilyOf("Table") != "GameSystem") return Fail("Table is not GameSystem");
        if (GameSystemNodes.FamilyOf("MathTable") != "Math") return Fail("MathTable is not Math");
        if (GameSystemNodes.FamilyOf("WorldFxTable") != "WorldFx") return Fail("WorldFxTable is not WorldFx");
        Console.WriteLine("  PASS");
        return 0;
    }
}
