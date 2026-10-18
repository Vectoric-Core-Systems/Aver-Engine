// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the generic game-UI nodes (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md section 2.2): the 6 rows in
// GraphUiExtNodeTable.cs, every method on UiExtGraph. Bare-process scope like MorphNodeTests: the wiring the table owns (pins,
// defaults, required and optional attributes, valid IL, the compiler split), not what the UI library does with a call.
using System;
using System.Linq;
using Aver.Graph;
using Row = NodeRowKit.Row;

[AutoRun]
static class UiExtNodeTests
{
    private static readonly Row[] Rows =
    {
        new("an_setuiproperty", GenericKind.Exec, new[] { "exec:exec:in", "widget:int:in", "value:float:in", "then:exec:out", "success:bool:out" }),
        new("an_setuipropertytext", GenericKind.Exec, new[] { "exec:exec:in", "widget:int:in", "then:exec:out", "success:bool:out" }),
        new("an_setuiitems", GenericKind.Exec, new[] { "exec:exec:in", "widget:int:in", "then:exec:out", "success:bool:out" }),
        new("an_setuiselected", GenericKind.Exec, new[] { "exec:exec:in", "widget:int:in", "index:int:in", "then:exec:out", "success:bool:out" }),
        new("an_setuitheme", GenericKind.Exec, new[] { "exec:exec:in", "then:exec:out", "success:bool:out" }),
        new("an_getuiinputstate", GenericKind.Pure, new[] { "pointer:bool:out", "keyboard:bool:out", "pausesGame:bool:out", "showCursor:bool:out" }),
    };

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        failures += NodeRowKit.CheckRows("UI extras", Rows);
        failures += TestRowSetIsTheDesignSet();
        failures += NodeRowKit.CheckDefaultPins("UI extras", Rows);
        failures += NodeRowKit.CheckIlValid("UI extras", types);
        failures += NodeRowKit.CheckCompilerSplit("UI extras", types);
        failures += TestAttributes();
        failures += TestMenuChain();
        failures += TestInputStateGatesABranch();
        failures += TestNoUiScaleNode();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the UI-extra rows are exactly the designed set of 6, 1 pure read and 5 exec writes");
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        var got = GameSystemNodes.All.Where(s => s.Owner.Name == "UiExtGraph").Select(s => s.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        if (Rows.Length != 6) return Fail($"expected 6 rows, test lists {Rows.Length}");
        if (Rows.Count(r => r.Kind == GenericKind.Pure) != 1) return Fail("only Get UI Input State is a read");
        Console.WriteLine("  PASS");
        return 0;
    }

    // A property and a theme name are required; the value text and the item list may be empty (that clears them).
    private static int TestAttributes()
    {
        Console.WriteLine("Test: property= and theme= are required and named in the error; text= and items= may be left empty");
        int f = 0;
        f += NodeRowKit.CheckMissingAttribute("NODE n AN_SetUiProperty", "property=", exec: true);
        f += NodeRowKit.CheckMissingAttribute("NODE n AN_SetUiPropertyText", "property=", exec: true);
        f += NodeRowKit.CheckMissingAttribute("NODE n AN_SetUiTheme", "theme=", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n AN_SetUiPropertyText property=textAlign", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n AN_SetUiItems", exec: true);
        if (f == 0) Console.WriteLine("  PASS");
        return f == 0 ? 0 : 1;
    }

    // Open a layout, find a choice, fill it, select, restyle a label and switch theme.
    private static int TestMenuChain()
    {
        Console.WriteLine("Test: Open Layout -> Find Widget -> Set Items, Selected, Property, Property Text and Theme chain and compile");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE ol AN_OpenUiLayout path=ui/menu.ocui\nNODE fw AN_FindUiWidget name=quality\nNODE si AN_SetUiItems items=Low|Medium|High\n" +
                            "NODE ss AN_SetUiSelected\nNODE sp AN_SetUiProperty property=fontSize\nNODE st AN_SetUiPropertyText property=textAlign text=center\nNODE th AN_SetUiTheme theme=light\n" +
                            "LINK tick.exec ol.exec\nLINK ol.then fw.exec\nLINK ol.layout fw.layout\nLINK fw.then si.exec\nLINK fw.widget si.widget\nLINK si.then ss.exec\nLINK fw.widget ss.widget\n" +
                            "LINK ss.then sp.exec\nLINK fw.widget sp.widget\nLINK sp.then st.exec\nLINK fw.widget st.widget\nLINK st.then th.exec\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }

    // The four flags are the gate a game pauses its own input on.
    private static int TestInputStateGatesABranch()
    {
        Console.WriteLine("Test: Get UI Input State's flags feed Branch nodes without being in the exec chain");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE gs AN_GetUiInputState\nNODE br Branch\nNODE bq Branch\nNODE pa Print\n" +
                            "LINK tick.exec br.exec\nLINK gs.pointer br.cond\nLINK br.false bq.exec\nLINK gs.keyboard bq.cond\nLINK bq.false pa.exec\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }

    // UI scale belongs to the accessibility option graphics.uiScale; no row here may take it.
    private static int TestNoUiScaleNode()
    {
        Console.WriteLine("Test: no UI-extra row is a UI scale node (Accessibility owns graphics.uiScale)");
        foreach (var s in GameSystemNodes.All.Where(x => x.Owner.Name == "UiExtGraph"))
            if (s.Type.Contains("scale", StringComparison.OrdinalIgnoreCase)) return Fail($"{s.Type} duplicates the accessibility UI scale option");
        Console.WriteLine("  PASS");
        return 0;
    }
}
