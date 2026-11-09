// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the data layer nodes (docs/LEVEL_STREAMING.md): the 2 rows in GraphDataLayerNodeTable.cs, every method on DataLayersGraph.
// Bare-process scope like TerrainNodeTests: the wiring the table owns (a real method behind every row, pins, valid IL, the layer= attribute)
// and the "never throw, answer the default" rule with no level loaded. What the streamer does with a state is tests/world's DataLayersHlodTest.
using System;
using System.Linq;
using Aver.Framework;
using Aver.Graph;
using Row = NodeRowKit.Row;

[AutoRun]
static class DataLayerNodeTests
{
    private static readonly Row[] Rows =
    {
        new("an_getdatalayerstate", GenericKind.Pure, new[] { "state:int:out" }),
        new("an_setdatalayerstate", GenericKind.Exec, new[] { "exec:exec:in", "state:int:in", "then:exec:out", "success:bool:out" }),
    };

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        failures += NodeRowKit.CheckRows("data layer", Rows);
        failures += TestRowSetIsTheDesignSet();
        failures += NodeRowKit.CheckDefaultPins("data layer", Rows);
        failures += NodeRowKit.CheckIlValid("data layer", types);
        failures += NodeRowKit.CheckCompilerSplit("data layer", types);
        failures += TestNothingThrowsWithoutALevel();
        failures += TestStateEnumMatchesTheAbi();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the data layer rows are exactly the designed set of 2: a pure read and an exec set");
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        var got = GameSystemNodes.All.Where(s => s.Owner.Name == "DataLayersGraph").Select(s => s.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int TestNothingThrowsWithoutALevel()
    {
        Console.WriteLine("Test: with no level the read answers -1, the set answers false, and the API reports no layers");
        if (DataLayersGraph.GetStateForGraph("Night") != -1) return Fail("an unknown layer should read -1");
        if (DataLayersGraph.GetStateForGraph("") != -1) return Fail("no layer= attribute should read -1");
        if (DataLayersGraph.SetStateForGraph("Night", 2)) return Fail("setting an unknown layer should be false");
        if (DataLayersGraph.SetStateForGraph("", 2)) return Fail("setting with no layer= attribute should be false");
        if (DataLayersGraph.SetStateForGraph("Night", 7)) return Fail("a state out of range should be false");
        if (DataLayers.Count != 0 || DataLayers.Names().Count != 0 || DataLayers.Name(0).Length != 0) return Fail("no layers expected");
        if (DataLayers.TryGetState("Night", out _)) return Fail("TryGetState on an unknown layer should be false");
        if (DataLayers.SetState("Night", DataLayerState.Activated) != DataLayerResult.Unknown) return Fail("SetState on an unknown layer should be Unknown");
        if (DataLayers.TryPollCompleted(out string name, out _) || name.Length != 0) return Fail("nothing should be pending");
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int TestStateEnumMatchesTheAbi()
    {
        Console.WriteLine("Test: DataLayerState values are the ABI's 0, 1, 2 and the results are 1, 0, -1, -2");
        if ((int)DataLayerState.Unloaded != 0 || (int)DataLayerState.Loaded != 1 || (int)DataLayerState.Activated != 2) return Fail("state values");
        if ((int)DataLayerResult.Ok != 1 || (int)DataLayerResult.Unknown != 0 || (int)DataLayerResult.NotRuntime != -1 || (int)DataLayerResult.BadState != -2) return Fail("result values");
        Console.WriteLine("  PASS");
        return 0;
    }
}
