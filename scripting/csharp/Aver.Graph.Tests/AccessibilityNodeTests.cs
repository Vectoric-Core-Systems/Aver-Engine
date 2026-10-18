// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the Accessibility nodes (docs/plans/wave9/ACCESSIBILITY.md section 2.10): the 11 rows in
// GraphAccessibilityNodeTable.cs, every method on AccessibilityGraphInterop. Bare-process scope like SequenceNodeTests:
// the wiring the table owns, not what the engine does when called (the native ABI is covered by
// FrameworkAccessAbiTest; managed tests cannot see it).
using System;
using System.Linq;
using Aver.Graph;
using Row = NodeRowKit.Row;

static class AccessibilityNodeTests
{
    private static readonly Row[] Rows =
    {
        new("setaccessibilityoption", GenericKind.Exec, new[] { "exec:exec:in", "value:float:in", "then:exec:out", "success:bool:out" }),
        new("getaccessibilityoption", GenericKind.Pure, new[] { "value:float:out", "found:bool:out" }),
        new("showcaption", GenericKind.Exec, new[] { "exec:exec:in", "x:float:in", "y:float:in", "z:float:in", "positional:bool:in", "then:exec:out", "success:bool:out" }),
        new("showcaptiontext", GenericKind.Exec, new[] { "exec:exec:in", "seconds:float:in", "kind:int:in", "then:exec:out", "success:bool:out" }, new[] { "seconds=3" }),
        new("stopcaption", GenericKind.Exec, new[] { "exec:exec:in", "then:exec:out", "success:bool:out" }),
        new("announce", GenericKind.Exec, new[] { "exec:exec:in", "interrupt:bool:in", "then:exec:out", "success:bool:out" }),
        new("stopspeaking", GenericKind.Exec, new[] { "exec:exec:in", "then:exec:out", "success:bool:out" }),
        new("isspeaking", GenericKind.Pure, new[] { "result:bool:out" }),
        new("addcamerashake", GenericKind.Exec, new[] { "exec:exec:in", "trauma:float:in", "then:exec:out", "success:bool:out" }, new[] { "trauma=0.5" }),
        new("setcamerabob", GenericKind.Exec, new[] { "exec:exec:in", "amount:float:in", "speed:float:in", "then:exec:out", "success:bool:out" }),
        new("setactiontoggleallowed", GenericKind.Exec, new[] { "exec:exec:in", "allowed:bool:in", "then:exec:out", "success:bool:out" }, new[] { "allowed=true" }),
    };

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        failures += NodeRowKit.CheckRows("accessibility", Rows);
        failures += TestRowSetIsTheDesignSet();
        failures += NodeRowKit.CheckDefaultPins("accessibility", Rows);
        failures += NodeRowKit.CheckIlValid("accessibility", types);
        failures += NodeRowKit.CheckCompilerSplit("accessibility", types);
        failures += TestMissingAttributeFailsByName();
        failures += TestOptionChain();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the accessibility rows are exactly the designed set of 11, 2 pure reads and 9 exec writes");
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        var got = GameSystemNodes.All.Where(s => s.Owner.Name == "AccessibilityGraphInterop").Select(s => s.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        if (Rows.Length != 11) return Fail($"expected 11 rows, test lists {Rows.Length}");
        if (Rows.Count(r => r.Kind == GenericKind.Pure) != 2) return Fail("only Get Accessibility Option and Is Speaking are reads");
        Console.WriteLine("  PASS");
        return 0;
    }

    // Keys, cues, texts and actions are attributes; speaker and cue-to-stop are optional.
    private static int TestMissingAttributeFailsByName()
    {
        Console.WriteLine("Test: accessibility nodes missing a required attribute fail to compile, naming it; the optional ones compile bare");
        int f = 0;
        f += NodeRowKit.CheckMissingAttribute("NODE n SetAccessibilityOption", "key=", exec: true);
        f += NodeRowKit.CheckMissingAttribute("NODE n GetAccessibilityOption", "key=", exec: false);
        f += NodeRowKit.CheckMissingAttribute("NODE n ShowCaption", "cue=", exec: true);
        f += NodeRowKit.CheckMissingAttribute("NODE n ShowCaptionText speaker=Guard", "text=", exec: true);
        f += NodeRowKit.CheckMissingAttribute("NODE n Announce", "text=", exec: true);
        f += NodeRowKit.CheckMissingAttribute("NODE n SetActionToggleAllowed", "action=", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n ShowCaptionText text=Hello", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n StopCaption", exec: true);
        if (f == 0) Console.WriteLine("  PASS");
        return f == 0 ? 0 : 1;
    }

    // A cutscene beat: read an option, set another, caption a cue, announce, shake the camera, and stop it all.
    private static int TestOptionChain()
    {
        Console.WriteLine("Test: option, caption, speech and camera nodes chain; reads feed later nodes");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE go GetAccessibilityOption key=access.ui.textScale\nNODE so SetAccessibilityOption key=access.ui.textScale\n" +
                            "NODE sc ShowCaption cue=intro_01\nNODE st ShowCaptionText speaker=Guard text=Halt\nNODE an Announce text=Hello\nNODE sh AddCameraShake\n" +
                            "NODE bb SetCameraBob\nNODE ip IsSpeaking\nNODE br Branch\nNODE ss StopSpeaking\nNODE sp StopCaption\nNODE tg SetActionToggleAllowed action=Pause\n" +
                            "LINK tick.exec so.exec\nLINK go.value so.value\nLINK so.then sc.exec\nLINK sc.then st.exec\nLINK st.then an.exec\nLINK an.then sh.exec\n" +
                            "LINK sh.then bb.exec\nLINK bb.then br.exec\nLINK ip.result br.cond\nLINK br.true ss.exec\nLINK ss.then sp.exec\nLINK sp.then tg.exec\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }
}
