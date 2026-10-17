// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the Game UI text nodes (docs/GAME_UI_TEXT.md section 2.13): the 4 rows in GraphUiTextNodeTable.cs, every
// method on UiTextGraphInterop. Bare-process scope like MotionMatchNodeTests: the wiring the table owns, not what
// the text engine draws (tests/ui does that).
using System;
using System.Linq;
using System.Reflection;
using Aver.Graph;
using Row = NodeRowKit.Row;

static class UiTextNodeTests
{
    private static readonly Row[] Rows =
    {
        new("an_setuifont", GenericKind.Exec, new[] { "exec:exec:in", "widget:int:in", "size:float:in", "bold:bool:in", "italic:bool:in", "then:exec:out", "success:bool:out" }),
        new("an_setuitextstyle", GenericKind.Exec, new[] { "exec:exec:in", "widget:int:in", "align:int:in", "dir:int:in", "wrap:bool:in", "ellipsis:bool:in", "maxLines:int:in", "lineSpacing:float:in", "letterSpacing:float:in", "rich:bool:in", "then:exec:out", "success:bool:out" }, new[] { "lineSpacing=1" }),
        new("an_setuitextcolor", GenericKind.Exec, new[] { "exec:exec:in", "widget:int:in", "r:float:in", "g:float:in", "b:float:in", "a:float:in", "then:exec:out", "success:bool:out" }, new[] { "a=1" }),
        new("an_getuitextsize", GenericKind.Pure, new[] { "widget:int:in", "found:bool:out", "width:float:out", "height:float:out", "lines:int:out" }),
    };

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        failures += NodeRowKit.CheckRows("game UI text", Rows);
        failures += TestRowSetIsTheDesignSet();
        failures += NodeRowKit.CheckDefaultPins("game UI text", Rows);
        failures += TestFamilyIsAnAttribute();
        failures += NodeRowKit.CheckIlValid("game UI text", types);
        failures += NodeRowKit.CheckCompilerSplit("game UI text", types);
        failures += TestFamilyIsOptional();
        failures += TestUnknownWidgetReturnsFalse();
        failures += TestStyleChain();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the game UI text rows are exactly the designed set of 4, one pure read and three exec writes, owned by UiTextGraphInterop");
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        var got = GameSystemNodes.All.Where(s => s.Owner.Name == "UiTextGraphInterop").Select(s => s.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        if (Rows.Length != 4) return Fail($"expected 4 rows, test lists {Rows.Length}");
        if (Rows.Count(r => r.Kind == GenericKind.Pure) != 1 || Rows.First(r => r.Kind == GenericKind.Pure).Type != "an_getuitextsize")
            return Fail("only AN_GetUiTextSize is a read");
        Console.WriteLine("  PASS");
        return 0;
    }

    // The family rides a NODE attribute (strings are not pins) and reaches the parsed node without becoming a pin.
    private static int TestFamilyIsAnAttribute()
    {
        Console.WriteLine("Test: family= reaches the node and leaves no pin behind");
        const string text = "OCGRAPH 1\nNODE f AN_SetUiFont family=Heading\n";
        if (!OcGraphParser.Parse(text, out var g, out var err)) return Fail("parse: " + err);
        if (g.Nodes["f"].Attrs["family"] != "Heading") return Fail("family= did not reach Node.Attrs");
        if (g.Nodes["f"].Pins.Any(p => p.Name == "family")) return Fail("the family must not be a pin");
        Console.WriteLine("  PASS");
        return 0;
    }

    // An empty or missing family keeps the widget's own, so the attribute is optional on the font node and the other three have none.
    private static int TestFamilyIsOptional()
    {
        Console.WriteLine("Test: Set UI Font compiles without family=; the other three compile bare");
        int f = 0;
        f += NodeRowKit.CheckOptionalAttribute("NODE n AN_SetUiFont", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n AN_SetUiTextStyle", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n AN_SetUiTextColor", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n AN_GetUiTextSize", exec: false);
        if (f == 0) Console.WriteLine("  PASS");
        return f == 0 ? 0 : 1;
    }

    // A widget id that is not a live widget answers false and writes zeroes, whether or not the UI library is loadable here.
    private static int TestUnknownWidgetReturnsFalse()
    {
        Console.WriteLine("Test: every method answers false for an unknown widget id");
        const BindingFlags any = BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic;
        object?[][] calls =
        {
            new object?[] { 987654, 20f, false, false, "Heading" },
            new object?[] { 987654, 0, 0, false, false, 0, 1f, 0f, false },
            new object?[] { 987654, 1f, 1f, 1f, 1f },
        };
        string[] names = { "an_setuifont", "an_setuitextstyle", "an_setuitextcolor" };
        for (int i = 0; i < names.Length; i++)
        {
            if (!GameSystemNodes.TryGet(names[i], out var s)) return Fail($"{names[i]}: no row");
            MethodInfo? m = s.Owner.GetMethod(s.MethodName, any);
            if (m == null) return Fail($"{names[i]}: no method {s.MethodName}");
            object? r;
            try { r = m.Invoke(null, calls[i]); }
            catch (Exception e) { return Fail($"{names[i]}: threw {e.GetType().Name}: {e.InnerException?.Message ?? e.Message}"); }
            if (r is not false) return Fail($"{names[i]}: returned {r}, expected false for an unknown widget");
        }
        if (!GameSystemNodes.TryGet("an_getuitextsize", out var g)) return Fail("an_getuitextsize: no row");
        MethodInfo? gm = g.Owner.GetMethod(g.MethodName, any);
        if (gm == null) return Fail("an_getuitextsize: no method");
        object?[] args = { 987654, 9f, 9f, 9 };
        object? found;
        try { found = gm.Invoke(null, args); }
        catch (Exception e) { return Fail($"an_getuitextsize: threw {e.GetType().Name}: {e.InnerException?.Message ?? e.Message}"); }
        if (found is not false) return Fail("an_getuitextsize: found an unknown widget");
        if ((float)args[1]! != 0f || (float)args[2]! != 0f || (int)args[3]! != 0) return Fail("an_getuitextsize: the out values were not zeroed");
        Console.WriteLine("  PASS");
        return 0;
    }

    // A tick that styles a label and reads its size back: the three writes chain, the read feeds later nodes.
    private static int TestStyleChain()
    {
        Console.WriteLine("Test: Set UI Font, Text Style and Text Color chain; Get UI Text Size feeds later nodes");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE sf AN_SetUiFont family=Heading\nNODE ss AN_SetUiTextStyle\nNODE sc AN_SetUiTextColor\n" +
                            "NODE gs AN_GetUiTextSize\nNODE pw Print\nNODE pl PrintInt\n" +
                            "LINK tick.exec sf.exec\nLINK sf.then ss.exec\nLINK ss.then sc.exec\nLINK sc.then pw.exec\nLINK gs.width pw.value\n" +
                            "LINK pw.then pl.exec\nLINK gs.lines pl.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }
}
