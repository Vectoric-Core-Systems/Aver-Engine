// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the GameFlow family (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md 2.2): the 20 rows in
// GraphGameFlowNodeTable.cs, every method on GraphGameFlow. Bare-process scope like MontageNodeTests: the wiring the
// table owns (pins, defaults, the method each row names, required attributes, valid IL, the compiler split) plus the
// rule that a wrapper never throws when the native service is missing. What the clock, the pause and the settings
// store do once the engine runs is checked where the engine is.
using System;
using System.Collections.Generic;
using System.Linq;
using Aver.Graph;
using Row = NodeRowKit.Row;

[AutoRun]
static class GameFlowNodeTests
{
    private static string[] Pins(bool exec, string ins, string outs)
    {
        var l = new List<string>();
        if (exec) l.Add("exec:exec:in");
        foreach (var p in ins.Split(' ', StringSplitOptions.RemoveEmptyEntries)) l.Add(p + ":in");
        if (exec) l.Add("then:exec:out");
        foreach (var p in outs.Split(' ', StringSplitOptions.RemoveEmptyEntries)) l.Add(p + ":out");
        return l.ToArray();
    }

    private static Row P(string type, string ins, string outs, params string[] defaults) =>
        new(type, GenericKind.Pure, Pins(false, ins, outs), defaults.Length == 0 ? null : defaults);

    private static Row X(string type, string ins, string outs, params string[] defaults) =>
        new(type, GenericKind.Exec, Pins(true, ins, outs), defaults.Length == 0 ? null : defaults);

    private static readonly Row[] Rows =
    {
        P("an_gettime", "", "seconds:float"),
        P("an_getrealtime", "", "seconds:float"),
        P("an_gettimedilation", "", "value:float"),
        X("an_settimedilation", "value:float", "success:bool", "value=1"),
        P("an_isgamepaused", "", "paused:bool"),
        X("an_setgamepaused", "paused:bool", "success:bool"),
        X("an_quitgame", "", ""),
        P("an_timerremaining", "handle:int", "seconds:float"),
        P("an_istimeractive", "handle:int", "active:bool"),
        X("an_pausetimer", "handle:int paused:bool", "success:bool"),
        X("an_resettimer", "handle:int delay:float", "success:bool", "delay=-1"),
        P("an_gettimerfirecount", "handle:int", "count:int"),
        P("an_getsettingfloat", "fallback:float", "value:float"),
        P("an_getsettingint", "fallback:int", "value:int"),
        P("an_getsettingbool", "fallback:bool", "value:bool"),
        P("an_hassetting", "", "has:bool"),
        X("an_setsettingfloat", "value:float", "success:bool"),
        X("an_setsettingint", "value:int", "success:bool"),
        X("an_setsettingbool", "value:bool", "success:bool"),
        X("an_flushsettings", "", "success:bool"),
    };

    // Rows that read or write a settings key and so need key=.
    private static readonly string[] SettingsPure = { "an_getsettingfloat", "an_getsettingint", "an_getsettingbool", "an_hassetting" };
    private static readonly string[] SettingsExec = { "an_setsettingfloat", "an_setsettingint", "an_setsettingbool" };

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        // First, before anything calls these wrappers: the failure of an unavailable native service is logged once.
        failures += TestFailureLogsOnce();
        failures += NodeRowKit.CheckRows("game flow", Rows);
        failures += TestRowSetIsTheDesignSet();
        failures += NodeRowKit.CheckDefaultPins("game flow", Rows);
        failures += NodeRowKit.CheckIlValid("game flow", types);
        failures += NodeRowKit.CheckCompilerSplit("game flow", types.Where(t => t != "an_quitgame"));   // no output pin to read back
        failures += TestSettingsKeyIsRequired();
        failures += TestWrappersNeverThrow();
        failures += TestSettingsRoundTripWhenAvailable();
        failures += TestPublicPauseSurface();
        failures += TestTimerAndSettingsChain();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the game-flow rows are exactly the designed set of 20, 11 pure reads and 9 exec writes");
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        var got = GameSystemNodes.All.Where(s => s.Owner.Name == "GraphGameFlow").Select(s => s.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        if (Rows.Length != 20) return Fail($"expected 20 rows, test lists {Rows.Length}");
        if (Rows.Count(r => r.Kind == GenericKind.Pure) != 11) return Fail("expected 11 pure reads");
        Console.WriteLine("  PASS");
        return 0;
    }

    // A settings node with no key= could never find anything, so it fails the compile naming the attribute.
    private static int TestSettingsKeyIsRequired()
    {
        Console.WriteLine("Test: a settings node with no key= fails to compile, naming key");
        foreach (string t in SettingsPure)
            if (NodeRowKit.CheckMissingAttribute($"NODE n {t}", "key", exec: false) != 0) return 1;
        foreach (string t in SettingsExec)
            if (NodeRowKit.CheckMissingAttribute($"NODE n {t}", "key", exec: true) != 0) return 1;
        Console.WriteLine("  PASS");
        return 0;
    }

    // Reads and writes return their neutral value rather than throw, whether or not the native services are loaded.
    private static int TestWrappersNeverThrow()
    {
        Console.WriteLine("Test: no game-flow wrapper throws, with or without the native services");
        var c = new FamilyAChecks();
        var key = new Dictionary<string, string> { ["key"] = "aver.test.gameflow.never" };
        foreach (var r in Rows)
        {
            try
            {
                var ins = FamilyAEval.In("handle", 0, "fallback", 1);
                if (r.Type == "an_quitgame") continue;   // would request a real quit when the engine is loaded
                FamilyAEval.Call(r.Type, ins, key);
            }
            catch (Exception e) { Console.WriteLine($"    {r.Type} threw {e.GetType().Name}: {e.Message}"); c.Bad++; }
        }
        try { Aver.Framework.Settings.Remove(key["key"]); } catch (Exception) { /* nothing to leave behind when the store is absent */ }
        // the fallbacks come back untouched for a key nobody set, whichever way the store is unavailable
        var unset = new Dictionary<string, string> { ["key"] = "aver.test.gameflow.unset" };
        c.Eq("getsettingint falls back", (int)FamilyAEval.Call("an_getsettingint", FamilyAEval.In("fallback", 41), unset)["value"], 41);
        c.Near("getsettingfloat falls back", (float)FamilyAEval.Call("an_getsettingfloat", FamilyAEval.In("fallback", 2.5f), unset)["value"], 2.5);
        c.True("getsettingbool falls back to true", (bool)FamilyAEval.Call("an_getsettingbool", FamilyAEval.In("fallback", true), unset)["value"]);
        c.True("hassetting is false for a key nobody set", !(bool)FamilyAEval.Call("an_hassetting", null, unset)["has"]);
        // an invalid timer handle: not active, negative remaining, -1 fire count
        c.True("istimeractive(0) is false", !FamilyAEval.B("an_istimeractive", "active", "handle", 0));
        c.True("timerremaining(0) is negative", FamilyAEval.F("an_timerremaining", "seconds", "handle", 0) < 0f);
        c.Eq("gettimerfirecount(0) is -1", FamilyAEval.I("an_gettimerfirecount", "count", "handle", 0), -1);
        c.True("pausetimer(0) fails", !FamilyAEval.B("an_pausetimer", "success", "handle", 0, "paused", true));
        c.True("resettimer(0) fails", !FamilyAEval.B("an_resettimer", "success", "handle", 0));
        c.True("settimedilation(NaN) is refused", !FamilyAEval.B("an_settimedilation", "success", "value", float.NaN));
        return c.Done();
    }

    // The failure of a missing native service is reported once per wrapper, not once per call, so a graph on OnTick does
    // not flood the log. Skipped (passes) when the native service is present or the wrapper already reported.
    private static int TestFailureLogsOnce()
    {
        Console.WriteLine("Test: a wrapper whose native call fails logs once and keeps returning its neutral value");
        var lines = new List<string>();
        Aver.Scripting.Log.SetSink((level, message) => lines.Add(message));
        try
        {
            for (int i = 0; i < 5; i++) FamilyAEval.Call("an_getrealtime");
            int mine = lines.Count(l => l.Contains("GetRealTime"));
            if (mine > 1) return Fail($"GetRealTime logged {mine} times for 5 failing calls");
            Console.WriteLine(mine == 1 ? "  PASS: one log line for five failing calls" : "  PASS: nothing to log (native service present)");
            return 0;
        }
        finally { Aver.Scripting.Log.SetSink(null); }
    }

    // With the native settings store reachable the nodes behave as Settings does. Without it there is nothing to check.
    private static int TestSettingsRoundTripWhenAvailable()
    {
        Console.WriteLine("Test: setting nodes round trip through the settings store when it is reachable");
        var key = new Dictionary<string, string> { ["key"] = "aver.test.gameflow.roundtrip" };
        bool wrote;
        try { wrote = FamilyAEval.Call("an_setsettingint", FamilyAEval.In("value", 5), key).TryGetValue("success", out var ok) && (bool)ok; }
        catch (Exception e) { return Fail($"setsettingint threw {e.GetType().Name}"); }
        if (!wrote) { Console.WriteLine("  PASS: settings store not reachable here"); return 0; }

        var c = new FamilyAChecks();
        c.Eq("int read back", (int)FamilyAEval.Call("an_getsettingint", FamilyAEval.In("fallback", -1), key)["value"], 5);
        c.True("hassetting after a write", (bool)FamilyAEval.Call("an_hassetting", null, key)["has"]);
        c.True("setsettingfloat", (bool)FamilyAEval.Call("an_setsettingfloat", FamilyAEval.In("value", 1.5f), key)["success"]);
        c.Near("float read back", (float)FamilyAEval.Call("an_getsettingfloat", FamilyAEval.In("fallback", 0f), key)["value"], 1.5);
        c.True("setsettingbool", (bool)FamilyAEval.Call("an_setsettingbool", FamilyAEval.In("value", true), key)["success"]);
        c.True("bool read back", (bool)FamilyAEval.Call("an_getsettingbool", FamilyAEval.In("fallback", false), key)["value"]);
        try { Aver.Framework.Settings.Remove(key["key"]); } catch (Exception) { /* leave nothing behind when we can */ }
        return c.Done();
    }

    // Game.SetPaused/Pause/Resume are public C#; without a running session they report false (or the native DLL is absent).
    private static int TestPublicPauseSurface()
    {
        Console.WriteLine("Test: Game.SetPaused, Pause and Resume exist, and a pause with no session reports false");
        try
        {
            bool paused = Aver.Framework.Game.SetPaused(true);
            bool resumed = Aver.Framework.Game.Resume();
            bool again = Aver.Framework.Game.Pause();
            Aver.Framework.Game.Resume();
            if (Aver.Framework.Game.State == Aver.Framework.PlayState.Editor && (paused || resumed || again))
                return Fail("a pause request succeeded with no play session");
        }
        catch (Exception e) when (NodeRowKit.NativeMissing(e)) { }
        var node = FamilyAEval.Call("an_setgamepaused", FamilyAEval.In("paused", true));
        if ((bool)node["success"] && Aver.Framework.Game.State == Aver.Framework.PlayState.Editor)
            return Fail("the node reported success with no play session");
        Console.WriteLine("  PASS");
        return 0;
    }

    // Timer control and settings nodes chain on an exec path, and a pure read feeds a branch.
    private static int TestTimerAndSettingsChain()
    {
        Console.WriteLine("Test: timer, pause and settings nodes chain exec to exec and their reads feed later nodes");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE st AN_SetTimer event=Ping\nNODE pt an_pausetimer\nNODE rt an_resettimer\nNODE td an_settimedilation\n" +
                            "NODE ss an_setsettingint key=graph.test\nNODE fl an_flushsettings\nNODE gp an_setgamepaused\nNODE pi PrintInt\nNODE pf Print\nNODE br Branch\n" +
                            "NODE rem an_timerremaining\nNODE gs an_getsettingint key=graph.test\nNODE gt an_gettime\nNODE paused an_isgamepaused\n" +
                            "LINK tick.exec st.exec\nLINK st.then pt.exec\nLINK st.handle pt.handle\nLINK pt.then rt.exec\nLINK st.handle rt.handle\nLINK rt.then td.exec\n" +
                            "LINK td.then ss.exec\nLINK ss.then fl.exec\nLINK fl.then gp.exec\nLINK gp.then br.exec\nLINK paused.paused br.cond\nLINK br.true pf.exec\n" +
                            "LINK rem.seconds pf.value\nLINK st.handle rem.handle\nLINK pf.then pi.exec\nLINK gs.value pi.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        const string pull = "OCGRAPH 1\nNODE gt an_gettime\nOUT gt seconds\n";
        string? perr = NodeRowKit.CompileError(pull, null);
        if (perr != null) return Fail("gettime did not compile as a pure read: " + perr);
        Console.WriteLine("  PASS");
        return 0;
    }
}
