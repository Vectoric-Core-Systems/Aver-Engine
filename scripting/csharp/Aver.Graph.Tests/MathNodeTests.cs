// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the Math family (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md 2.2 and 5): the 55 rows in
// GraphMathNodeTable.cs, every method on MathGraph. Unlike the wiring-only families these are pure CPU maths, so
// the numbers are checked: golden values against System.Math, the zero-divisor and empty-range edge cases, integer
// wrap and sign rules, random range and seeding, and the rotator conversions against an independent quaternion rotation.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Reflection;
using Aver.Graph;
using AverMath = Aver.Framework.AverMath;
using AverRandom = Aver.Framework.AverRandom;
using Quat = Aver.Scene.Quat;
using Rot = Aver.Scene.Rot;
using Vec3 = Aver.Scene.Vec3;
using Row = NodeRowKit.Row;

/// <summary>Calls the method behind a table row the way the generic emitter would, with pin values by name, so a test
/// exercises the row's argument mapping and the owner method together. Shared by the four wave-9 FAMILIES_A test files.</summary>
internal static class FamilyAEval
{
    /// <summary>Alternating pin name and value, as a dictionary.</summary>
    public static Dictionary<string, object> In(params object[] kv)
    {
        var d = new Dictionary<string, object>();
        for (int i = 0; i + 1 < kv.Length; i += 2) d[(string)kv[i]] = kv[i + 1];
        return d;
    }

    /// <summary>Invokes the row's method. Missing inputs take the row's default or zero; attributes come from
    /// <paramref name="attrs"/> or are empty. Returns every output pin by name. A throw inside the method surfaces unwrapped.</summary>
    public static Dictionary<string, object> Call(string type, Dictionary<string, object>? ins = null, Dictionary<string, string>? attrs = null)
    {
        if (!GameSystemNodes.TryGet(type, out var s)) throw new InvalidOperationException($"{type}: no row in the node table");
        MethodInfo m = s.Method;
        var args = new object?[s.Args.Length];
        for (int i = 0; i < args.Length; i++)
        {
            string a = s.Args[i];
            if (a.StartsWith("p:", StringComparison.Ordinal))
            {
                string pin = a[2..];
                var inp = s.Inputs.First(x => x.Name == pin);
                object? v = ins != null && ins.TryGetValue(pin, out var given) ? given : inp.Default;
                args[i] = Coerce(inp.Type, v);
            }
            else if (a.StartsWith('@'))
            {
                string key = a[1..].TrimEnd('?').Split('|')[0];
                args[i] = attrs != null && attrs.TryGetValue(key, out var av) ? av : "";
            }
            else if (a.StartsWith('=')) args[i] = a[1..];
            else if (a.StartsWith("i:", StringComparison.Ordinal)) args[i] = int.Parse(a[2..], CultureInfo.InvariantCulture);
            else if (a.StartsWith("f:", StringComparison.Ordinal)) args[i] = float.Parse(a[2..], CultureInfo.InvariantCulture);
            else if (a.StartsWith("b:", StringComparison.Ordinal)) args[i] = a[2..] != "0";
            // an out parameter stays null; Invoke writes it back
        }
        object? ret;
        try { ret = m.Invoke(null, args); }
        catch (TargetInvocationException e) when (e.InnerException != null)
        {
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(e.InnerException).Throw();
            throw;
        }
        var res = new Dictionary<string, object>();
        if (s.Ret != null && ret != null) res[s.Ret] = ret;
        for (int i = 0; i < args.Length; i++)
            if (s.Args[i].StartsWith('>') && s.Args[i] != ">_" && args[i] != null) res[s.Args[i][1..]] = args[i]!;
        return res;
    }

    private static object Coerce(PinType t, object? v) => t switch
    {
        PinType.Float => v == null ? 0f : Convert.ToSingle(v, CultureInfo.InvariantCulture),
        PinType.Int => v == null ? 0 : Convert.ToInt32(v, CultureInfo.InvariantCulture),
        _ => v == null ? false : Convert.ToBoolean(v, CultureInfo.InvariantCulture),
    };

    /// <summary>One float output of a row, inputs as alternating name and value.</summary>
    public static float F(string type, string pin, params object[] kv) => (float)Call(type, In(kv))[pin];
    /// <summary>One int output of a row.</summary>
    public static int I(string type, string pin, params object[] kv) => (int)Call(type, In(kv))[pin];
    /// <summary>One bool output of a row.</summary>
    public static bool B(string type, string pin, params object[] kv) => (bool)Call(type, In(kv))[pin];
}

/// <summary>Collects a test's failures so a loop reports every bad case, then prints one verdict.</summary>
internal sealed class FamilyAChecks
{
    public int Bad;

    public void Near(string what, double got, double want, double tol = 2e-6)
    {
        if (!(Math.Abs(got - want) <= tol * Math.Max(1.0, Math.Abs(want))))
        {
            Console.WriteLine($"    {what}: got {got:R}, expected {want:R}");
            Bad++;
        }
    }

    public void Eq<T>(string what, T got, T want)
    {
        if (!EqualityComparer<T>.Default.Equals(got, want))
        {
            Console.WriteLine($"    {what}: got {got}, expected {want}");
            Bad++;
        }
    }

    public void True(string what, bool cond)
    {
        if (!cond) { Console.WriteLine($"    {what}: expected true"); Bad++; }
    }

    public int Done()
    {
        if (Bad > 0) return NodeRowKit.Fail($"{Bad} problem(s)");
        Console.WriteLine("  PASS");
        return 0;
    }
}

[AutoRun]
static class MathNodeTests
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

    private static Row U(string type) => P(type, "a:float", "result:float");
    private static Row II(string type) => P(type, "a:int b:int", "result:int");
    private const string XYZ = "x:float y:float z:float";
    private const string AB6 = "ax:float ay:float az:float bx:float by:float bz:float";

    private static readonly Row[] Rows =
    {
        U("an_tan"), U("an_asin"), U("an_acos"), U("an_atan"),
        P("an_atan2", "y:float x:float", "result:float"),
        U("an_sign"), U("an_frac"), U("an_truncate"), U("an_degtorad"), U("an_radtodeg"), U("an_exp"), U("an_log"),
        P("an_smoothstep", "edge0:float edge1:float x:float", "result:float"),
        P("an_inverselerp", "a:float b:float value:float", "result:float"),
        P("an_maprange", "value:float inMin:float inMax:float outMin:float outMax:float clamp:bool", "result:float", "inMin=0", "inMax=1", "outMin=0", "outMax=1", "clamp=true"),
        P("an_wrap", "value:float min:float max:float", "result:float"),
        P("an_interpto", "current:float target:float deltaTime:float speed:float", "result:float"),
        P("an_deltaangle", "a:float b:float", "result:float"),
        P("an_lerpangle", "a:float b:float t:float", "result:float"),
        P("an_nearlyequal", "a:float b:float tolerance:float", "result:bool", "tolerance=0.0001"),
        P("an_isfinite", "a:float", "result:bool"),
        II("an_intadd"), II("an_intsubtract"), II("an_intmultiply"), II("an_intmin"), II("an_intmax"), II("an_intdivide"), II("an_intmodulo"),
        P("an_intclamp", "a:int min:int max:int", "result:int"),
        P("an_intabs", "a:int", "result:int"), P("an_intsign", "a:int", "result:int"),
        P("an_intcompare", "a:int b:int", "less:bool equal:bool greater:bool"),
        II("an_bitand"), II("an_bitor"), II("an_bitxor"),
        P("an_bitshiftleft", "a:int count:int", "result:int"), P("an_bitshiftright", "a:int count:int", "result:int"),
        P("an_bittest", "value:int bit:int", "result:bool"),
        X("an_randomfloat", "min:float max:float", "value:float", "min=0", "max=1"),
        X("an_randomint", "min:int max:int", "value:int", "min=0", "max=1"),
        X("an_randombool", "chance:float", "value:bool", "chance=0.5"),
        X("an_randomunitvector", "", XYZ),
        X("an_setrandomseed", "seed:int", ""),
        P("an_vecnegate", "ax:float ay:float az:float", XYZ),
        P("an_vecmultiply", AB6, XYZ),
        P("an_veclength2d", "ax:float ay:float", "result:float"),
        P("an_vecdistance2d", "ax:float ay:float bx:float by:float", "result:float"),
        P("an_vecreflect", "vx:float vy:float vz:float nx:float ny:float nz:float", XYZ),
        P("an_vecproject", AB6, XYZ),
        P("an_vecclamplength", "vx:float vy:float vz:float maxLength:float", XYZ),
        P("an_vecangle", AB6, "result:float"),
        P("an_rotatortoforward", "yaw:float pitch:float", XYZ),
        P("an_forwardtorotator", XYZ, "yaw:float pitch:float"),
        P("an_rotatevector", "vx:float vy:float vz:float yaw:float pitch:float roll:float", XYZ),
        P("an_lookatrotation", "fromX:float fromY:float fromZ:float toX:float toY:float toZ:float", "yaw:float pitch:float"),
    };

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        failures += NodeRowKit.CheckRows("math", Rows);
        failures += TestRowSetIsTheDesignSet();
        failures += NodeRowKit.CheckDefaultPins("math", Rows);
        failures += NodeRowKit.CheckIlValid("math", types);
        failures += NodeRowKit.CheckCompilerSplit("math", types.Where(t => t != "an_setrandomseed"));   // no output pin to read back
        failures += TestScalarGolden();
        failures += TestScalarEdgeCases();
        failures += TestIntegerRows();
        failures += TestRandomRows();
        failures += TestVectorRows();
        failures += TestRotatorRows();
        failures += TestPublicSurface();
        failures += TestGraphsEndToEnd();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the math rows are exactly the designed set of 55, the 5 random rows exec and all others pure");
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        var got = GameSystemNodes.All.Where(s => s.Owner.Name == "MathGraph").Select(s => s.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        if (Rows.Length != 55) return Fail($"expected 55 rows, test lists {Rows.Length}");
        var exec = Rows.Where(r => r.Kind == GenericKind.Exec).Select(r => r.Type).OrderBy(x => x).ToArray();
        var wantExec = new[] { "an_randombool", "an_randomfloat", "an_randomint", "an_randomunitvector", "an_setrandomseed" };
        if (!exec.SequenceEqual(wantExec)) return Fail("only the random rows may be exec");
        Console.WriteLine("  PASS");
        return 0;
    }

    // ---- scalar --------------------------------------------------------------------------------------------

    private static int TestScalarGolden()
    {
        Console.WriteLine("Test: float rows match System.Math");
        var c = new FamilyAChecks();
        foreach (float x in new[] { -3.2f, -1f, -0.25f, 0f, 0.5f, 1.3f, 4f })
        {
            c.Near($"tan({x})", FamilyAEval.F("an_tan", "result", "a", x), Math.Tan(x));
            c.Near($"atan({x})", FamilyAEval.F("an_atan", "result", "a", x), Math.Atan(x));
            c.Near($"exp({x})", FamilyAEval.F("an_exp", "result", "a", x), Math.Exp(x));
            c.Near($"truncate({x})", FamilyAEval.F("an_truncate", "result", "a", x), Math.Truncate(x));
            c.Near($"frac({x})", FamilyAEval.F("an_frac", "result", "a", x), x - Math.Floor(x));
            c.Near($"sign({x})", FamilyAEval.F("an_sign", "result", "a", x), Math.Sign(x));
            c.Near($"degtorad({x})", FamilyAEval.F("an_degtorad", "result", "a", x), x * Math.PI / 180.0);
            c.Near($"radtodeg({x})", FamilyAEval.F("an_radtodeg", "result", "a", x), x * 180.0 / Math.PI);
        }
        foreach (float x in new[] { -1f, -0.75f, -0.25f, 0f, 0.5f, 0.99f, 1f })
        {
            c.Near($"asin({x})", FamilyAEval.F("an_asin", "result", "a", x), Math.Asin(x));
            c.Near($"acos({x})", FamilyAEval.F("an_acos", "result", "a", x), Math.Acos(x));
        }
        foreach (float x in new[] { 0.001f, 0.5f, 1f, 2.718281828f, 10f, 12345f })
            c.Near($"log({x})", FamilyAEval.F("an_log", "result", "a", x), Math.Log(x));
        foreach (var (y, x) in new[] { (1f, -2f), (-1f, -2f), (0.5f, 0.5f), (-3f, 0f), (0f, -1f) })
            c.Near($"atan2({y},{x})", FamilyAEval.F("an_atan2", "result", "y", y, "x", x), Math.Atan2(y, x));
        c.Near("degtorad(180)", FamilyAEval.F("an_degtorad", "result", "a", 180f), Math.PI);
        c.Near("radtodeg(pi)", FamilyAEval.F("an_radtodeg", "result", "a", (float)Math.PI), 180.0);
        c.Near("frac(-1.25)", FamilyAEval.F("an_frac", "result", "a", -1.25f), 0.75);
        c.Near("smoothstep(0,10,2.5)", FamilyAEval.F("an_smoothstep", "result", "edge0", 0f, "edge1", 10f, "x", 2.5f), 0.15625);
        c.Near("inverselerp(2,4,3)", FamilyAEval.F("an_inverselerp", "result", "a", 2f, "b", 4f, "value", 3f), 0.5);
        c.Near("maprange(5,0,10,0,100)", FamilyAEval.F("an_maprange", "result", "value", 5f, "inMin", 0f, "inMax", 10f, "outMin", 0f, "outMax", 100f), 50.0);
        c.Near("wrap(5,0,3)", FamilyAEval.F("an_wrap", "result", "value", 5f, "min", 0f, "max", 3f), 2.0);
        c.Near("interpto half step", FamilyAEval.F("an_interpto", "result", "current", 0f, "target", 10f, "deltaTime", 0.1f, "speed", 5f), 5.0);
        c.Near("lerpangle(350,10,0.5)", FamilyAEval.F("an_lerpangle", "result", "a", 350f, "b", 10f, "t", 0.5f), 360.0);
        return c.Done();
    }

    private static int TestScalarEdgeCases()
    {
        Console.WriteLine("Test: scalar edge cases give the documented neutral values, never NaN");
        var c = new FamilyAChecks();
        float F(string t, params object[] kv) => FamilyAEval.F(t, "result", kv);

        // asin/acos clamp; sign of zero and NaN
        c.Near("asin(2)", F("an_asin", "a", 2f), Math.PI / 2);
        c.Near("asin(-9)", F("an_asin", "a", -9f), -Math.PI / 2);
        c.Near("acos(2)", F("an_acos", "a", 2f), 0.0);
        c.Near("acos(-2)", F("an_acos", "a", -2f), Math.PI);
        c.Near("sign(0)", F("an_sign", "a", 0f), 0.0);
        c.Near("sign(NaN)", F("an_sign", "a", float.NaN), 0.0);
        // log of zero or less is 0, the Divide convention
        c.Near("log(0)", F("an_log", "a", 0f), 0.0);
        c.Near("log(-4)", F("an_log", "a", -4f), 0.0);
        c.Near("log(NaN)", F("an_log", "a", float.NaN), 0.0);
        // smoothstep: ramps, clamps, and equal edges give a step at the edge
        c.Near("smoothstep below", F("an_smoothstep", "edge0", 0f, "edge1", 1f, "x", -1f), 0.0);
        c.Near("smoothstep above", F("an_smoothstep", "edge0", 0f, "edge1", 1f, "x", 2f), 1.0);
        c.Near("smoothstep middle", F("an_smoothstep", "edge0", 0f, "edge1", 1f, "x", 0.5f), 0.5);
        c.Near("smoothstep equal edges before", F("an_smoothstep", "edge0", 2f, "edge1", 2f, "x", 1.9f), 0.0);
        c.Near("smoothstep equal edges at", F("an_smoothstep", "edge0", 2f, "edge1", 2f, "x", 2f), 1.0);
        c.Near("smoothstep equal edges after", F("an_smoothstep", "edge0", 2f, "edge1", 2f, "x", 3f), 1.0);
        // inverselerp: equal ends give 0, never clamped
        c.Near("inverselerp equal ends", F("an_inverselerp", "a", 5f, "b", 5f, "value", 9f), 0.0);
        c.Near("inverselerp extrapolates", F("an_inverselerp", "a", 2f, "b", 4f, "value", 8f), 3.0);
        c.Near("inverselerp below", F("an_inverselerp", "a", 2f, "b", 4f, "value", 0f), -1.0);
        // maprange: clamps by default, an empty input range gives outMin
        c.Near("maprange clamps high", F("an_maprange", "value", 15f, "inMin", 0f, "inMax", 10f, "outMin", 0f, "outMax", 100f), 100.0);
        c.Near("maprange clamps low", F("an_maprange", "value", -5f, "inMin", 0f, "inMax", 10f, "outMin", 0f, "outMax", 100f), 0.0);
        c.Near("maprange unclamped", F("an_maprange", "value", 15f, "inMin", 0f, "inMax", 10f, "outMin", 0f, "outMax", 100f, "clamp", false), 150.0);
        c.Near("maprange equal input bounds", F("an_maprange", "value", 3f, "inMin", 2f, "inMax", 2f, "outMin", 7f, "outMax", 9f), 7.0);
        c.Near("maprange reversed output", F("an_maprange", "value", 0.25f, "inMin", 0f, "inMax", 1f, "outMin", 10f, "outMax", 0f), 7.5);
        // wrap: [min, max), empty or reversed range gives min
        c.Near("wrap negative", F("an_wrap", "value", -1f, "min", 0f, "max", 3f), 2.0);
        c.Near("wrap at max", F("an_wrap", "value", 3f, "min", 0f, "max", 3f), 0.0);
        c.Near("wrap big", F("an_wrap", "value", 370f, "min", 0f, "max", 360f), 10.0);
        c.Near("wrap offset range", F("an_wrap", "value", -0.5f, "min", -1f, "max", 1f), -0.5);
        c.Near("wrap empty range", F("an_wrap", "value", 7f, "min", 5f, "max", 5f), 5.0);
        c.Near("wrap reversed range", F("an_wrap", "value", 7f, "min", 5f, "max", 4f), 5.0);
        // interpto: never overshoots; speed <= 0 snaps; dt <= 0 stays
        c.Near("interpto overshoot guard", F("an_interpto", "current", 0f, "target", 10f, "deltaTime", 1f, "speed", 5f), 10.0);
        c.Near("interpto zero speed", F("an_interpto", "current", 0f, "target", 10f, "deltaTime", 0.1f, "speed", 0f), 10.0);
        c.Near("interpto negative speed", F("an_interpto", "current", 3f, "target", -4f, "deltaTime", 0.1f, "speed", -2f), -4.0);
        c.Near("interpto zero dt", F("an_interpto", "current", 0f, "target", 10f, "deltaTime", 0f, "speed", 5f), 0.0);
        c.Near("interpto downwards", F("an_interpto", "current", 10f, "target", 0f, "deltaTime", 0.1f, "speed", 5f), 5.0);
        float v = 0f; bool monotone = true;
        for (int i = 0; i < 200; i++)
        {
            float next = F("an_interpto", "current", v, "target", 10f, "deltaTime", 0.016f, "speed", 3f);
            if (next < v || next > 10f) monotone = false;
            v = next;
        }
        c.True("interpto 200 steps stay monotone and at or below the target", monotone);
        c.True("interpto converges", v > 9.9f);
        // deltaangle: shortest signed way, in (-180, 180]
        c.Near("deltaangle(350,10)", F("an_deltaangle", "a", 350f, "b", 10f), 20.0);
        c.Near("deltaangle(10,350)", F("an_deltaangle", "a", 10f, "b", 350f), -20.0);
        c.Near("deltaangle(0,180)", F("an_deltaangle", "a", 0f, "b", 180f), 180.0);
        c.Near("deltaangle(0,720)", F("an_deltaangle", "a", 0f, "b", 720f), 0.0);
        c.Near("deltaangle(-170,170)", F("an_deltaangle", "a", -170f, "b", 170f), -20.0);
        c.Near("lerpangle short way", F("an_lerpangle", "a", 350f, "b", 10f, "t", 0.25f), 355.0);
        c.Near("lerpangle end", F("an_lerpangle", "a", 10f, "b", 350f, "t", 1f), -10.0);
        // nearlyequal / isfinite
        c.True("nearlyequal within default tolerance", FamilyAEval.B("an_nearlyequal", "result", "a", 1f, "b", 1.00005f));
        c.True("nearlyequal outside default tolerance", !FamilyAEval.B("an_nearlyequal", "result", "a", 1f, "b", 1.001f));
        c.True("nearlyequal explicit tolerance", FamilyAEval.B("an_nearlyequal", "result", "a", 1f, "b", 1.001f, "tolerance", 0.01f));
        c.True("nearlyequal NaN", !FamilyAEval.B("an_nearlyequal", "result", "a", float.NaN, "b", float.NaN));
        c.True("isfinite(1)", FamilyAEval.B("an_isfinite", "result", "a", 1f));
        c.True("isfinite(NaN)", !FamilyAEval.B("an_isfinite", "result", "a", float.NaN));
        c.True("isfinite(+inf)", !FamilyAEval.B("an_isfinite", "result", "a", float.PositiveInfinity));
        c.True("isfinite(-inf)", !FamilyAEval.B("an_isfinite", "result", "a", float.NegativeInfinity));
        return c.Done();
    }

    // ---- integer -------------------------------------------------------------------------------------------

    private static int TestIntegerRows()
    {
        Console.WriteLine("Test: integer rows wrap, never throw on a zero divisor, mask shifts and compare exactly");
        var c = new FamilyAChecks();
        int I(string t, params object[] kv) => FamilyAEval.I(t, "result", kv);
        c.Eq("intadd wraps", I("an_intadd", "a", int.MaxValue, "b", 1), int.MinValue);
        c.Eq("intsubtract wraps", I("an_intsubtract", "a", int.MinValue, "b", 1), int.MaxValue);
        c.Eq("intmultiply wraps", I("an_intmultiply", "a", 65536, "b", 65536), 0);
        c.Eq("intmultiply", I("an_intmultiply", "a", -6, "b", 7), -42);
        c.Eq("intmin", I("an_intmin", "a", -3, "b", 4), -3);
        c.Eq("intmax", I("an_intmax", "a", -3, "b", 4), 4);
        c.Eq("intdivide 7/2", I("an_intdivide", "a", 7, "b", 2), 3);
        c.Eq("intdivide -7/2 truncates toward zero", I("an_intdivide", "a", -7, "b", 2), -3);
        c.Eq("intdivide by zero", I("an_intdivide", "a", 7, "b", 0), 0);
        c.Eq("intdivide min/-1 does not throw", I("an_intdivide", "a", int.MinValue, "b", -1), int.MinValue);
        c.Eq("intmodulo -7%2 follows the dividend", I("an_intmodulo", "a", -7, "b", 2), -1);
        c.Eq("intmodulo 7%-2", I("an_intmodulo", "a", 7, "b", -2), 1);
        c.Eq("intmodulo by zero", I("an_intmodulo", "a", 7, "b", 0), 0);
        c.Eq("intmodulo min%-1 does not throw", I("an_intmodulo", "a", int.MinValue, "b", -1), 0);
        c.Eq("intclamp high", I("an_intclamp", "a", 5, "min", 0, "max", 3), 3);
        c.Eq("intclamp low", I("an_intclamp", "a", -5, "min", 0, "max", 3), 0);
        c.Eq("intclamp inside", I("an_intclamp", "a", 2, "min", 0, "max", 3), 2);
        c.Eq("intabs", I("an_intabs", "a", -4), 4);
        c.Eq("intabs min stays", I("an_intabs", "a", int.MinValue), int.MinValue);
        c.Eq("intsign -", I("an_intsign", "a", -9), -1);
        c.Eq("intsign 0", I("an_intsign", "a", 0), 0);
        c.Eq("intsign +", I("an_intsign", "a", 5), 1);

        // compare is exact above 2^24, where a float cannot tell the two apart
        var cmp = FamilyAEval.Call("an_intcompare", FamilyAEval.In("a", 16777217, "b", 16777216));
        c.True("intcompare 2^24+1 vs 2^24 is greater", (bool)cmp["greater"] && !(bool)cmp["less"] && !(bool)cmp["equal"]);
        c.True("the same two as floats are equal", (float)16777217 == (float)16777216);
        cmp = FamilyAEval.Call("an_intcompare", FamilyAEval.In("a", -2, "b", 3));
        c.True("intcompare less", (bool)cmp["less"] && !(bool)cmp["equal"] && !(bool)cmp["greater"]);
        cmp = FamilyAEval.Call("an_intcompare", FamilyAEval.In("a", 8, "b", 8));
        c.True("intcompare equal", !(bool)cmp["less"] && (bool)cmp["equal"] && !(bool)cmp["greater"]);

        c.Eq("bitand", I("an_bitand", "a", 0b1100, "b", 0b1010), 0b1000);
        c.Eq("bitor", I("an_bitor", "a", 0b1100, "b", 0b1010), 0b1110);
        c.Eq("bitxor", I("an_bitxor", "a", 0b1100, "b", 0b1010), 0b0110);
        c.Eq("shiftleft 1<<4", I("an_bitshiftleft", "a", 1, "count", 4), 16);
        c.Eq("shiftleft count masked (33 is 1)", I("an_bitshiftleft", "a", 1, "count", 33), 2);
        c.Eq("shiftleft 1<<31", I("an_bitshiftleft", "a", 1, "count", 31), int.MinValue);
        c.Eq("shiftleft negative count masked", I("an_bitshiftleft", "a", 1, "count", -1), int.MinValue);
        c.Eq("shiftright 16>>2", I("an_bitshiftright", "a", 16, "count", 2), 4);
        c.Eq("shiftright count masked (33 is 1)", I("an_bitshiftright", "a", 16, "count", 33), 8);
        c.Eq("shiftright fills zeros", I("an_bitshiftright", "a", -8, "count", 1), 0x7FFFFFFC);
        c.Eq("shiftright of a top-bit mask by 31", I("an_bitshiftright", "a", int.MinValue, "count", 31), 1);
        c.True("bittest bit 0", FamilyAEval.B("an_bittest", "result", "value", 5, "bit", 0));
        c.True("bittest bit 1", !FamilyAEval.B("an_bittest", "result", "value", 5, "bit", 1));
        c.True("bittest bit 2", FamilyAEval.B("an_bittest", "result", "value", 5, "bit", 2));
        c.True("bittest bit 31", FamilyAEval.B("an_bittest", "result", "value", int.MinValue, "bit", 31));
        c.True("bittest bit 32 is false", !FamilyAEval.B("an_bittest", "result", "value", -1, "bit", 32));
        c.True("bittest negative bit is false", !FamilyAEval.B("an_bittest", "result", "value", -1, "bit", -1));
        return c.Done();
    }

    // ---- random --------------------------------------------------------------------------------------------

    private static int TestRandomRows()
    {
        Console.WriteLine("Test: random rows stay in range, int is inclusive at both ends, a seed repeats the sequence");
        var c = new FamilyAChecks();
        FamilyAEval.Call("an_setrandomseed", FamilyAEval.In("seed", 12345));

        double sum = 0; bool inRange = true;
        for (int i = 0; i < 10000; i++)
        {
            float v = FamilyAEval.F("an_randomfloat", "value", "min", -2f, "max", 3f);
            if (v < -2f || v >= 3f) inRange = false;
            sum += v;
        }
        c.True("randomfloat in [-2, 3) over 10000 draws", inRange);
        c.Near("randomfloat mean near 0.5", sum / 10000, 0.5, 0.1);
        float swapped = FamilyAEval.F("an_randomfloat", "value", "min", 5f, "max", 1f);
        c.True("randomfloat with reversed bounds still lands between them", swapped >= 1f && swapped <= 5f);
        c.Near("randomfloat with equal bounds", FamilyAEval.F("an_randomfloat", "value", "min", 4f, "max", 4f), 4.0);

        bool lo = false, hi = false; inRange = true;
        for (int i = 0; i < 10000; i++)
        {
            int v = FamilyAEval.I("an_randomint", "value", "min", 3, "max", 7);
            if (v < 3 || v > 7) inRange = false;
            if (v == 3) lo = true;
            if (v == 7) hi = true;
        }
        c.True("randomint in [3, 7]", inRange);
        c.True("randomint reaches both ends", lo && hi);
        int rev = FamilyAEval.I("an_randomint", "value", "min", 7, "max", 3);
        c.True("randomint with reversed bounds", rev >= 3 && rev <= 7);
        c.Eq("randomint with equal bounds", FamilyAEval.I("an_randomint", "value", "min", 5, "max", 5), 5);
        FamilyAEval.I("an_randomint", "value", "min", int.MinValue, "max", int.MaxValue);   // full range must not throw

        int never = 0, always = 0, half = 0;
        for (int i = 0; i < 1000; i++)
        {
            if (FamilyAEval.B("an_randombool", "value", "chance", 0f)) never++;
            if (!FamilyAEval.B("an_randombool", "value", "chance", 1f)) always++;
            if (FamilyAEval.B("an_randombool", "value", "chance", 0.5f)) half++;
        }
        c.Eq("randombool(0) never true", never, 0);
        c.Eq("randombool(1) always true", always, 0);
        c.True($"randombool(0.5) about half ({half}/1000)", half > 420 && half < 580);

        double mx = 0, my = 0, mz = 0; bool unit = true;
        for (int i = 0; i < 2000; i++)
        {
            var r = FamilyAEval.Call("an_randomunitvector");
            float x = (float)r["x"], y = (float)r["y"], z = (float)r["z"];
            if (Math.Abs(Math.Sqrt(x * x + y * y + z * z) - 1.0) > 1e-4) unit = false;
            mx += x; my += y; mz += z;
        }
        c.True("randomunitvector has unit length", unit);
        c.True("randomunitvector is centred on the origin", Math.Abs(mx / 2000) < 0.08 && Math.Abs(my / 2000) < 0.08 && Math.Abs(mz / 2000) < 0.08);

        (List<float>, List<int>) Draw(int seed)
        {
            FamilyAEval.Call("an_setrandomseed", FamilyAEval.In("seed", seed));
            var fl = new List<float>(); var il = new List<int>();
            for (int i = 0; i < 8; i++) { fl.Add(FamilyAEval.F("an_randomfloat", "value")); il.Add(FamilyAEval.I("an_randomint", "value", "min", 0, "max", 1000)); }
            return (fl, il);
        }
        var (f1, i1) = Draw(42);
        var (f2, i2) = Draw(42);
        var (f3, i3) = Draw(43);
        c.True("the same seed repeats the same floats and ints", f1.SequenceEqual(f2) && i1.SequenceEqual(i2));
        c.True("a different seed gives a different sequence", !f1.SequenceEqual(f3) || !i1.SequenceEqual(i3));
        return c.Done();
    }

    // ---- vectors -------------------------------------------------------------------------------------------

    private static int TestVectorRows()
    {
        Console.WriteLine("Test: vector rows");
        var c = new FamilyAChecks();
        void V(string what, Dictionary<string, object> r, double ex, double ey, double ez)
        {
            c.Near(what + ".x", (float)r["x"], ex, 1e-5);
            c.Near(what + ".y", (float)r["y"], ey, 1e-5);
            c.Near(what + ".z", (float)r["z"], ez, 1e-5);
        }
        V("negate", FamilyAEval.Call("an_vecnegate", FamilyAEval.In("ax", 1f, "ay", -2f, "az", 3f)), -1, 2, -3);
        V("multiply", FamilyAEval.Call("an_vecmultiply", FamilyAEval.In("ax", 1f, "ay", 2f, "az", 3f, "bx", 4f, "by", 5f, "bz", 6f)), 4, 10, 18);
        c.Near("length2d ignores z", FamilyAEval.F("an_veclength2d", "result", "ax", 3f, "ay", 4f), 5.0);
        c.Near("distance2d", FamilyAEval.F("an_vecdistance2d", "result", "ax", 1f, "ay", 1f, "bx", 4f, "by", 5f), 5.0);

        // reflect off a non-unit normal; a zero normal returns the vector
        V("reflect", FamilyAEval.Call("an_vecreflect", FamilyAEval.In("vx", 1f, "vy", 0f, "vz", -1f, "nx", 0f, "ny", 0f, "nz", 2f)), 1, 0, 1);
        V("reflect off a slanted wall", FamilyAEval.Call("an_vecreflect", FamilyAEval.In("vx", 1f, "vy", 0f, "vz", 0f, "nx", -1f, "ny", 1f, "nz", 0f)), 0, 1, 0);
        V("reflect zero normal", FamilyAEval.Call("an_vecreflect", FamilyAEval.In("vx", 1f, "vy", 2f, "vz", 3f)), 1, 2, 3);

        V("project onto x", FamilyAEval.Call("an_vecproject", FamilyAEval.In("ax", 3f, "ay", 4f, "az", 0f, "bx", 1f, "by", 0f, "bz", 0f)), 3, 0, 0);
        V("project onto scaled z", FamilyAEval.Call("an_vecproject", FamilyAEval.In("ax", 1f, "ay", 1f, "az", 1f, "bx", 0f, "by", 0f, "bz", 2f)), 0, 0, 1);
        V("project onto zero", FamilyAEval.Call("an_vecproject", FamilyAEval.In("ax", 1f, "ay", 1f, "az", 1f)), 0, 0, 0);

        V("clamp long", FamilyAEval.Call("an_vecclamplength", FamilyAEval.In("vx", 3f, "vy", 4f, "vz", 0f, "maxLength", 2.5f)), 1.5, 2, 0);
        V("clamp short", FamilyAEval.Call("an_vecclamplength", FamilyAEval.In("vx", 3f, "vy", 4f, "vz", 0f, "maxLength", 9f)), 3, 4, 0);
        V("clamp to zero length", FamilyAEval.Call("an_vecclamplength", FamilyAEval.In("vx", 3f, "vy", 4f, "vz", 0f, "maxLength", 0f)), 0, 0, 0);
        V("clamp to negative length", FamilyAEval.Call("an_vecclamplength", FamilyAEval.In("vx", 3f, "vy", 4f, "vz", 0f, "maxLength", -1f)), 0, 0, 0);

        c.Near("angle 90", FamilyAEval.F("an_vecangle", "result", "ax", 1f, "ay", 0f, "az", 0f, "bx", 0f, "by", 1f, "bz", 0f), 90.0, 1e-4);
        c.Near("angle 45", FamilyAEval.F("an_vecangle", "result", "ax", 1f, "ay", 0f, "az", 0f, "bx", 1f, "by", 1f, "bz", 0f), 45.0, 1e-4);
        c.Near("angle same", FamilyAEval.F("an_vecangle", "result", "ax", 2f, "ay", 0f, "az", 0f, "bx", 5f, "by", 0f, "bz", 0f), 0.0, 1e-4);
        c.Near("angle opposite", FamilyAEval.F("an_vecangle", "result", "ax", 1f, "ay", 0f, "az", 0f, "bx", -3f, "by", 0f, "bz", 0f), 180.0, 1e-3);
        c.Near("angle with a zero vector", FamilyAEval.F("an_vecangle", "result", "ax", 0f, "ay", 0f, "az", 0f, "bx", 1f, "by", 0f, "bz", 0f), 0.0);
        return c.Done();
    }

    // ---- rotators ------------------------------------------------------------------------------------------

    // v' = q v q*, written with the Hamilton product so it shares nothing with AverMath.Rotate.
    private static Vec3 QRot(Quat q, Vec3 v)
    {
        var r = q * new Quat(v.X, v.Y, v.Z, 0f) * new Quat(-q.X, -q.Y, -q.Z, q.W);
        return new Vec3(r.X, r.Y, r.Z);
    }

    private static int TestRotatorRows()
    {
        Console.WriteLine("Test: rotator rows agree with Rot.ToQuat applied by an independent quaternion rotation");
        var c = new FamilyAChecks();
        var yaws = new[] { -170f, -90f, -33f, 0f, 45f, 90f, 150f };
        var pitches = new[] { -80f, -45f, 0f, 30f, 60f, 85f };

        foreach (float y in yaws)
            foreach (float p in pitches)
            {
                var r = FamilyAEval.Call("an_rotatortoforward", FamilyAEval.In("yaw", y, "pitch", p));
                Vec3 want = QRot(new Rot(y, p, 0f).ToQuat(), Vec3.Forward);
                c.Near($"forward({y},{p}).x", (float)r["x"], want.X, 1e-5);
                c.Near($"forward({y},{p}).y", (float)r["y"], want.Y, 1e-5);
                c.Near($"forward({y},{p}).z", (float)r["z"], want.Z, 1e-5);

                // forwardtorotator undoes it
                var back = FamilyAEval.Call("an_forwardtorotator", FamilyAEval.In("x", (float)r["x"], "y", (float)r["y"], "z", (float)r["z"]));
                c.Near($"round trip yaw({y},{p})", (float)back["yaw"], y, 1e-3);
                c.Near($"round trip pitch({y},{p})", (float)back["pitch"], p, 1e-3);
            }

        var f = FamilyAEval.Call("an_rotatortoforward", FamilyAEval.In("yaw", 0f, "pitch", 0f));
        c.Near("forward(0,0)", (float)f["x"], 1.0, 1e-6);
        f = FamilyAEval.Call("an_rotatortoforward", FamilyAEval.In("yaw", 90f, "pitch", 0f));
        c.Near("forward(90,0).y", (float)f["y"], 1.0, 1e-5);
        f = FamilyAEval.Call("an_rotatortoforward", FamilyAEval.In("yaw", 0f, "pitch", 90f));
        c.Near("positive pitch tips the nose down (-z)", (float)f["z"], -1.0, 1e-5);

        // directions with no rotator of their own
        var up = FamilyAEval.Call("an_forwardtorotator", FamilyAEval.In("x", 0f, "y", 0f, "z", 1f));
        c.Near("straight up pitch", (float)up["pitch"], -90.0, 1e-3);
        c.Near("straight up yaw", (float)up["yaw"], 0.0, 1e-3);
        var down = FamilyAEval.Call("an_forwardtorotator", FamilyAEval.In("x", 0f, "y", 0f, "z", -3f));
        c.Near("straight down pitch", (float)down["pitch"], 90.0, 1e-3);
        var east = FamilyAEval.Call("an_forwardtorotator", FamilyAEval.In("x", 0f, "y", 2f, "z", 0f));
        c.Near("non-unit +y yaw", (float)east["yaw"], 90.0, 1e-3);
        var none = FamilyAEval.Call("an_forwardtorotator");
        c.Near("zero direction yaw", (float)none["yaw"], 0.0);
        c.Near("zero direction pitch", (float)none["pitch"], 0.0);

        // rotatevector against the quaternion, with roll
        foreach (var v in new[] { new Vec3(1, 2, 3), new Vec3(0, -4, 1.5f), new Vec3(-2, 0.5f, 0) })
            foreach (float y in new[] { -120f, 0f, 75f })
                foreach (float p in new[] { -50f, 0f, 40f })
                    foreach (float rl in new[] { -90f, 0f, 33f, 170f })
                    {
                        var r = FamilyAEval.Call("an_rotatevector", FamilyAEval.In("vx", v.X, "vy", v.Y, "vz", v.Z, "yaw", y, "pitch", p, "roll", rl));
                        Vec3 want = QRot(new Rot(y, p, rl).ToQuat(), v);
                        double tol = 1e-5 * Math.Max(1.0, v.Length);
                        c.Near($"rotatevector {v} ({y},{p},{rl}).x", (float)r["x"], want.X, tol);
                        c.Near($"rotatevector {v} ({y},{p},{rl}).y", (float)r["y"], want.Y, tol);
                        c.Near($"rotatevector {v} ({y},{p},{rl}).z", (float)r["z"], want.Z, tol);
                    }
        var yawed = FamilyAEval.Call("an_rotatevector", FamilyAEval.In("vx", 1f, "vy", 0f, "vz", 0f, "yaw", 90f));
        c.Near("yaw 90 turns +x into +y", (float)yawed["y"], 1.0, 1e-5);
        var rolled = FamilyAEval.Call("an_rotatevector", FamilyAEval.In("vx", 0f, "vy", 1f, "vz", 0f, "roll", 90f));
        c.Near("roll 90 turns +y into +z", (float)rolled["z"], 1.0, 1e-5);

        // lookatrotation: the rotator's forward points at the target, same sign as LookAtForGraph
        var l = FamilyAEval.Call("an_lookatrotation", FamilyAEval.In("toX", 0f, "toY", 10f, "toZ", 0f));
        c.Near("look at +y yaw", (float)l["yaw"], 90.0, 1e-3);
        l = FamilyAEval.Call("an_lookatrotation", FamilyAEval.In("toX", 10f, "toY", 0f, "toZ", 10f));
        c.Near("look at 45 degrees up pitch is -45 in Rot's sign", (float)l["pitch"], -45.0, 1e-3);
        l = FamilyAEval.Call("an_lookatrotation", FamilyAEval.In("fromX", 5f, "fromY", 5f, "fromZ", 5f, "toX", 5f, "toY", 5f, "toZ", 5f));
        c.Near("look at self yaw", (float)l["yaw"], 0.0);
        c.Near("look at self pitch", (float)l["pitch"], 0.0);
        foreach (var (src, dst) in new[] { (new Vec3(0, 0, 0), new Vec3(30, -40, 12)), (new Vec3(100, 20, -5), new Vec3(-60, 90, 300)), (new Vec3(1, 1, 1), new Vec3(1, 1, -50)) })
        {
            var look = FamilyAEval.Call("an_lookatrotation", FamilyAEval.In("fromX", src.X, "fromY", src.Y, "fromZ", src.Z, "toX", dst.X, "toY", dst.Y, "toZ", dst.Z));
            Vec3 fwd = QRot(new Rot((float)look["yaw"], (float)look["pitch"], 0f).ToQuat(), Vec3.Forward);
            Vec3 dir = (dst - src).Normalized;
            c.Near($"look {src}->{dst} aims at the target", Vec3.Dot(fwd, dir), 1.0, 1e-5);
        }
        return c.Done();
    }

    // ---- the C# surface the nodes wrap ---------------------------------------------------------------------

    private static int TestPublicSurface()
    {
        Console.WriteLine("Test: AverMath and AverRandom give C# callers the same answers as the nodes");
        var c = new FamilyAChecks();
        c.Near("DeltaAngle", AverMath.DeltaAngle(350f, 10f), 20.0);
        c.Near("Wrap", AverMath.Wrap(-1f, 0f, 3f), 2.0);
        c.Near("MapRange", AverMath.MapRange(15f, 0f, 10f, 0f, 100f), 100.0);
        c.Eq("IntDivide by zero", AverMath.IntDivide(5, 0), 0);
        c.Eq("BitShiftLeft masks the count", AverMath.BitShiftLeft(1, 33), 2);
        c.True("BitTest", AverMath.BitTest(5, 2));
        var refl = AverMath.Reflect(new Vec3(1, 0, -1), new Vec3(0, 0, 2));
        c.True("Reflect", Math.Abs(refl.X - 1) < 1e-6 && Math.Abs(refl.Y) < 1e-6 && Math.Abs(refl.Z - 1) < 1e-6);
        var fwd = AverMath.ForwardOf(new Rot(90f, 0f, 0f));
        c.True("ForwardOf yaw 90", Math.Abs(fwd.X) < 1e-6 && Math.Abs(fwd.Y - 1) < 1e-6);
        var rot = AverMath.LookAt(new Vec3(0, 0, 0), new Vec3(0, 10, 0));
        c.Near("LookAt yaw", rot.Yaw, 90.0, 1e-4);
        var step = AverMath.InterpTo(new Vec3(0, 0, 0), new Vec3(10, -10, 4), 0.1f, 5f);
        c.True("InterpTo(Vec3) moves each component half way", Math.Abs(step.X - 5) < 1e-5 && Math.Abs(step.Y + 5) < 1e-5 && Math.Abs(step.Z - 2) < 1e-5);

        AverRandom.Seed(9);
        float a = AverRandom.NextFloat(); int b = AverRandom.NextInt(0, 100);
        AverRandom.Seed(9);
        c.True("AverRandom.Seed repeats", a == AverRandom.NextFloat() && b == AverRandom.NextInt(0, 100));
        // the nodes draw from the same stream
        AverRandom.Seed(9);
        AverRandom.NextFloat();
        float viaNode = FamilyAEval.F("an_randomfloat", "value", "min", 0f, "max", 1f);
        AverRandom.Seed(9);
        AverRandom.NextFloat();
        c.True("a node draw is the next draw of the shared generator", viaNode == AverRandom.NextFloat());
        return c.Done();
    }

    // ---- through the compiler ------------------------------------------------------------------------------

    private static int TestGraphsEndToEnd()
    {
        Console.WriteLine("Test: math nodes compile and run inside graphs, pull and push");
        var c = new FamilyAChecks();

        float RunFloat(string text)
        {
            if (!OcGraphParser.Parse(text, out var g, out var err)) { Console.WriteLine("    parse: " + err); c.Bad++; return float.NaN; }
            if (new GraphCompiler(g).Compile(out var cerr) is not Func<float> fn) { Console.WriteLine("    compile: " + cerr); c.Bad++; return float.NaN; }
            return fn();
        }
        int RunInt(string text)
        {
            if (!OcGraphParser.Parse(text, out var g, out var err)) { Console.WriteLine("    parse: " + err); c.Bad++; return int.MinValue; }
            if (new GraphCompiler(g).Compile(out var cerr) is not Func<int> fn) { Console.WriteLine("    compile: " + cerr); c.Bad++; return int.MinValue; }
            return fn();
        }

        c.Near("maprange defaults clamp 15 to 1",
               RunFloat("OCGRAPH 1\nNODE c ConstFloat value=15\nNODE m an_maprange\nLINK c.value m.value\nOUT m result\n"), 1.0);
        c.Near("maprange from pinned values",
               RunFloat("OCGRAPH 1\nNODE c ConstFloat value=5\nNODE m an_maprange\nPINVAL m inMax 10\nPINVAL m outMax 200\nLINK c.value m.value\nOUT m result\n"), 100.0);
        c.Eq("intdivide by zero in a graph",
             RunInt("OCGRAPH 1\nNODE a ConstInt value=7\nNODE z ConstInt value=0\nNODE d an_intdivide\nLINK a.value d.a\nLINK z.value d.b\nOUT d result\n"), 0);
        c.Eq("intmodulo sign in a graph",
             RunInt("OCGRAPH 1\nNODE a ConstInt value=-7\nNODE b ConstInt value=2\nNODE d an_intmodulo\nLINK a.value d.a\nLINK b.value d.b\nOUT d result\n"), -1);
        c.Near("one output of a multi-output node: forwardtorotator yaw",
               RunFloat("OCGRAPH 1\nNODE x ConstFloat value=0\nNODE y ConstFloat value=1\nNODE z ConstFloat value=0\nNODE r an_forwardtorotator\nLINK x.value r.x\nLINK y.value r.y\nLINK z.value r.z\nOUT r yaw\n"), 90.0);
        c.Near("two nodes chained: deltaangle then maprange",
               RunFloat("OCGRAPH 1\nNODE a ConstFloat value=350\nNODE b ConstFloat value=10\nNODE d an_deltaangle\nNODE m an_maprange\nPINVAL m inMax 40\nLINK a.value d.a\nLINK b.value d.b\nLINK d.result m.value\nOUT m result\n"), 0.5);

        // a seeded random draw feeds a later node, on an exec chain
        const string chain = "OCGRAPH 1\nNODE tick OnTick\nNODE s an_setrandomseed\nPINVAL s seed 7\nNODE r an_randomint\nPINVAL r min 3\nPINVAL r max 9\nNODE u an_randomunitvector\n" +
                             "NODE f an_randomfloat\nNODE b an_randombool\nNODE p PrintInt\nNODE pf Print\nNODE br Branch\n" +
                             "LINK tick.exec s.exec\nLINK s.then r.exec\nLINK r.then u.exec\nLINK u.then f.exec\nLINK f.then b.exec\nLINK b.then p.exec\nLINK r.value p.value\n" +
                             "LINK p.then pf.exec\nLINK u.x pf.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(chain, "OnTick");
        if (bad != null) { Console.WriteLine("    " + bad); c.Bad++; }
        return c.Done();
    }
}
