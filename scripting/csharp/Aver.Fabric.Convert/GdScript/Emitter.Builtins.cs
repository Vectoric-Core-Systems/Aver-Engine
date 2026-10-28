// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// GDScript's built-in functions (print, len, clamp, lerp, randf ...) and the methods of its built-in types (String, Array, Dictionary).

namespace Aver.Fabric.Convert.GdScript;

internal sealed partial class GdEmitter
{
    private static Ex R(string code, GdType type, int prec = P.Primary) => new(code, type, prec);

    private string PrintArg(Ex e) => e.Type.Kind == GK.Enum ? "(long)" + Wrap(e, P.Unary) : e.Code;

    private static readonly Dictionary<string, string> s_mathFloat = new()
    {
        ["sqrt"] = "Math.Sqrt", ["sin"] = "Math.Sin", ["cos"] = "Math.Cos", ["tan"] = "Math.Tan", ["asin"] = "Math.Asin", ["acos"] = "Math.Acos",
        ["atan"] = "Math.Atan", ["sinh"] = "Math.Sinh", ["cosh"] = "Math.Cosh", ["tanh"] = "Math.Tanh", ["exp"] = "Math.Exp", ["log"] = "Math.Log",
        ["floorf"] = "Math.Floor", ["ceilf"] = "Math.Ceiling", ["absf"] = "Math.Abs",
    };

    private static readonly Dictionary<string, string> s_mathfDouble = new()
    {
        ["lerp_angle"] = "Mathf.LerpAngle", ["inverse_lerp"] = "Mathf.InverseLerp", ["remap"] = "Mathf.Remap", ["move_toward"] = "Mathf.MoveToward",
        ["smoothstep"] = "Mathf.SmoothStep", ["deg_to_rad"] = "Mathf.DegToRad", ["rad_to_deg"] = "Mathf.RadToDeg", ["ease"] = "Mathf.Ease",
        ["cubic_interpolate"] = "Mathf.CubicInterpolate", ["bezier_interpolate"] = "Mathf.BezierInterpolate", ["angle_difference"] = "Mathf.AngleDifference",
        ["lerpf"] = "Mathf.Lerp", ["fposmod"] = "GDScript.FPosMod", ["wrapf"] = "GDScript.Wrap", ["pingpong"] = "GDScript.PingPong",
        ["linear_to_db"] = "GDScript.LinearToDb", ["db_to_linear"] = "GDScript.DbToLinear", ["snappedf"] = "Mathf.Snapped",
        ["atan2"] = "Math.Atan2", ["pow"] = "Math.Pow",
    };

    private Ex? _builtinResult;

    private bool TryBuiltin(string name, Call c, GdType? expected, out Ex? result)
    {
        _builtinResult = null;
        bool ok = TryBuiltinCore(name, c, expected);
        result = ok ? _builtinResult : null;
        return ok;
    }

    private bool TryBuiltinCore(string name, Call c, GdType? expected)
    {
        var cache = new Ex?[c.Args.Count];
        Ex E(int i) => cache[i] ??= Expr(c.Args[i], null);
        string F(int i) => Adapt(E(i), GdType.Float);
        string I(int i) => Adapt(E(i), GdType.Int);
        int n = c.Args.Count;
        Ex? Done(Ex r) { _builtinResult = r; return r; }

        switch (name)
        {
            // ---- output ----
            case "print": case "print_debug": Done(R($"GD.Print({string.Join(", ", Enumerable.Range(0, n).Select(i => PrintArg(E(i))))})", GdType.Void)); return true;
            case "prints": Done(R($"GD.PrintS({string.Join(", ", Enumerable.Range(0, n).Select(i => PrintArg(E(i))))})", GdType.Void)); return true;
            case "printt": Done(R($"GD.PrintT({string.Join(", ", Enumerable.Range(0, n).Select(i => PrintArg(E(i))))})", GdType.Void)); return true;
            case "print_rich": Done(R($"GD.PrintRich({string.Join(", ", Enumerable.Range(0, n).Select(i => PrintArg(E(i))))})", GdType.Void)); return true;
            case "printraw": Done(R($"GD.PrintRaw({string.Join(", ", Enumerable.Range(0, n).Select(i => PrintArg(E(i))))})", GdType.Void)); return true;
            case "printerr": Done(R($"GD.PrintErr({string.Join(", ", Enumerable.Range(0, n).Select(i => PrintArg(E(i))))})", GdType.Void)); return true;
            case "push_error": Done(R($"GD.PushError({string.Join(", ", Enumerable.Range(0, n).Select(i => PrintArg(E(i))))})", GdType.Void)); return true;
            case "push_warning": Done(R($"GD.PushWarning({string.Join(", ", Enumerable.Range(0, n).Select(i => PrintArg(E(i))))})", GdType.Void)); return true;
            case "str" when n >= 1:
                if (n == 1 && E(0).Type.Kind == GK.String) { Done(E(0)); return true; }
                Done(R($"GD.Str({string.Join(", ", Enumerable.Range(0, n).Select(i => PrintArg(E(i))))})", GdType.String)); return true;
            case "var_to_str" when n == 1: Done(R($"GD.VarToStr({VariantFromTyped(E(0))})", GdType.String)); return true;
            case "str_to_var" when n == 1: Done(R($"VariantText.Parse({Adapt(E(0), GdType.String)})", GdType.Variant)); return true;

            // ---- sizes, ranges, types ----
            case "len" when n == 1:
            {
                Ex x = E(0);
                string xc = Wrap(x, P.Primary);
                Done(x.Type.Kind switch
                {
                    GK.String or GK.StringName => R($"{xc}.Length", GdType.Int32),
                    GK.Array or GK.Dict => R($"{xc}.Count", GdType.Int32),
                    GK.Packed => R($"{xc}.Length", GdType.Int32),
                    _ => R($"GDScript.Len({VariantFromTyped(x)})", GdType.Int),
                });
                return true;
            }
            case "range" when n is >= 1 and <= 3:
                Done(R($"new Collections.Array<long>(GDScript.Range({string.Join(", ", Enumerable.Range(0, n).Select(I))}))", GdType.ArrayOf(GdType.Int)));
                return true;
            case "typeof" when n == 1: Done(R($"GDScript.TypeOf({VariantFromTyped(E(0))})", GdType.Int)); return true;
            case "type_string" when n == 1: Done(R($"GDScript.TypeString({I(0)})", GdType.String)); return true;
            case "char" when n == 1: Done(R($"GDScript.Char({I(0)})", GdType.String)); return true;
            case "ord" when n == 1: Done(R($"GDScript.Ord({Adapt(E(0), GdType.String)})", GdType.Int)); return true;
            case "hash" when n == 1: Done(R($"GD.Hash({VariantFromTyped(E(0))})", GdType.Int32)); return true;
            case "is_instance_valid" when n == 1:
            {
                Ex x = E(0);
                string arg = x.Type.IsVariant ? x.Code + ".AsGodotObject()" : x.Code;
                Done(R($"IsInstanceValid({arg})", GdType.Bool)); return true;
            }
            case "instance_from_id" when n == 1: Done(R($"GodotObject.InstanceFromId((ulong){Wrap(new Ex(I(0), GdType.Int), P.Unary)})", Obj("GodotObject").WithWeak(true))); return true;
            case "weakref" when n == 1:
                Note(c, "function", "weakref", Rating.Approximated, "weakref() keeps a strong reference");
                Done(E(0)); return true;
            case "load" when n == 1:
            {
                if (c.Args[0] is Literal { Kind: LitKind.String } lit)
                {
                    GdType t = Obj(ResourceClassFor((string)lit.Value!));
                    Done(R($"GD.Load<{t.Cs}>({E(0).Code})", t));
                }
                else Done(R($"GD.Load<Resource>({Adapt(E(0), GdType.String)})", Obj("Resource")));
                return true;
            }

            // ---- conversions ----
            case "int" when n == 1: Done(ToIntEx(E(0))); return true;
            case "float" when n == 1: Done(ToFloatEx(E(0))); return true;
            case "bool" when n == 1: Done(R(Truth(E(0)), GdType.Bool)); return true;
            case "String" when n == 1: Done(R($"GD.Str({PrintArg(E(0))})", GdType.String)); return true;
            case "StringName" when n == 1: Done(R($"new StringName({Adapt(E(0), GdType.String)})", GdType.StringName)); return true;
            case "NodePath" when n == 1: Done(R($"new NodePath({Adapt(E(0), GdType.String)})", GdType.NodePath)); return true;
            case "Array" when n == 0: Done(R("new Collections.Array()", GdType.UntypedArray)); return true;
            case "Array" when n == 1:
                Done(R($"{Wrap(new Ex(VariantFromTyped(E(0)), GdType.Variant), P.Primary)}.AsGodotArray().Duplicate()", GdType.UntypedArray)); return true;
            case "Dictionary" when n == 0: Done(R("new Collections.Dictionary()", GdType.UntypedDict)); return true;
            case "Callable" when n == 2:
            {
                // Callable(self, "on_done") names one of our functions: use the generated MethodName constant.
                if (E(0).Code == "this" && c.Args[1] is Literal { Kind: LitKind.String } lit && FindScriptMember((string)lit.Value!).Member is { Kind: MK.Func } fm)
                { Done(R($"new Callable(this, MethodName.{fm.CsName})", GdType.Callable)); return true; }
                Done(R($"new Callable({E(0).Code}, {Adapt(E(1), GdType.String)})", GdType.Callable)); return true;
            }
            case "Signal" when n == 2: Done(R($"new Signal({E(0).Code}, {Adapt(E(1), GdType.String)})", GdType.Signal)); return true;

            // ---- randomness ----
            case "randf" when n == 0: Done(R("GD.Randf()", GdType.Float32)); return true;
            case "randi" when n == 0: Done(R("GD.Randi()", GdType.Int)); return true;
            case "randf_range" when n == 2: Done(R($"GD.RandRange({F(0)}, {F(1)})", GdType.Float)); return true;
            case "randi_range" when n == 2: Done(R($"GD.RandRange({Adapt(E(0), GdType.Int32)}, {Adapt(E(1), GdType.Int32)})", GdType.Int32)); return true;
            case "randfn" when n == 2: Done(R($"GD.Randfn({F(0)}, {F(1)})", GdType.Float)); return true;
            case "randomize": Done(R("GD.Randomize()", GdType.Void)); return true;
            case "seed" when n == 1: Done(R($"GD.Seed((ulong){Wrap(new Ex(I(0), GdType.Int), P.Unary)})", GdType.Void)); return true;

            // ---- numbers ----
            case "abs" when n == 1:
            {
                Ex x = E(0);
                if (x.Type.Kind == GK.Int) { Done(R($"Math.Abs({x.Code})", x.Type)); return true; }
                if (x.Type.Kind == GK.Float) { Done(R($"Math.Abs({x.Code})", x.Type)); return true; }
                if (x.Type.Kind == GK.Struct) { Done(R($"{Wrap(x, P.Primary)}.Abs()", x.Type)); return true; }
                Done(R($"GDScript.Abs({VariantFromTyped(x)})", GdType.Variant)); return true;
            }
            case "absi" when n == 1: Done(R($"Math.Abs({I(0)})", GdType.Int)); return true;
            case "sign" or "signf" or "signi" when n == 1:
            {
                Ex x = E(0);
                if (x.Type.Kind == GK.Struct) { Done(R($"{Wrap(x, P.Primary)}.Sign()", x.Type)); return true; }
                bool isInt = name == "signi" || (name == "sign" && x.Type.Kind == GK.Int);
                Done(R($"GDScript.Sign({(isInt ? I(0) : F(0))})", isInt ? GdType.Int : GdType.Float)); return true;
            }
            case "floor" or "ceil" or "round" when n == 1:
            {
                Ex x = E(0);
                if (x.Type.Kind == GK.Struct) { Done(R($"{Wrap(x, P.Primary)}.{Naming.Pascal(name)}()", x.Type)); return true; }
                Done(R(RoundCall(name, F(0)), GdType.Float)); return true;
            }
            case "roundf" or "round" when n == 1: Done(R(RoundCall("round", F(0)), GdType.Float)); return true;
            case "floori" when n == 1: Done(R($"(long)Math.Floor({F(0)})", GdType.Int, P.Unary)); return true;
            case "ceili" when n == 1: Done(R($"(long)Math.Ceiling({F(0)})", GdType.Int, P.Unary)); return true;
            case "roundi" when n == 1: Done(R($"(long){RoundCall("round", F(0))}", GdType.Int, P.Unary)); return true;
            case "posmod" when n == 2: Done(R($"GDScript.PosMod({I(0)}, {I(1)})", GdType.Int)); return true;
            case "wrapi" when n == 3: Done(R($"GDScript.Wrap({I(0)}, {I(1)}, {I(2)})", GdType.Int)); return true;
            case "snappedi" when n == 2: Done(R($"GDScript.Snapped({I(0)}, {I(1)})", GdType.Int)); return true;
            case "wrap" when n == 3:
            {
                bool ints = E(0).Type.Kind == GK.Int && E(1).Type.Kind == GK.Int && E(2).Type.Kind == GK.Int;
                Done(ints ? R($"GDScript.Wrap({I(0)}, {I(1)}, {I(2)})", GdType.Int) : R($"GDScript.Wrap({F(0)}, {F(1)}, {F(2)})", GdType.Float)); return true;
            }
            case "snapped" when n == 2:
            {
                bool ints = E(0).Type.Kind == GK.Int && E(1).Type.Kind == GK.Int;
                if (E(0).Type.Kind == GK.Struct) { Done(R($"{Wrap(E(0), P.Primary)}.Snapped({Adapt(E(1), E(0).Type)})", E(0).Type)); return true; }
                Done(ints ? R($"GDScript.Snapped({I(0)}, {I(1)})", GdType.Int) : R($"Mathf.Snapped({F(0)}, {F(1)})", GdType.Float)); return true;
            }
            case "min" or "max" or "mini" or "maxi" or "minf" or "maxf" when n >= 2:
            {
                string fn = name.StartsWith("min", StringComparison.Ordinal) ? "Min" : "Max";
                var all = Enumerable.Range(0, n).Select(E).ToList();
                bool ints = name.EndsWith("i", StringComparison.Ordinal) && name.Length == 4 || all.All(x => x.Type.Kind == GK.Int);
                bool floats = name.EndsWith("f", StringComparison.Ordinal) && name.Length == 4 || (!ints && all.All(x => x.Type.IsNumeric));
                if (ints || floats)
                {
                    GdType t = ints ? GdType.Int : GdType.Float;
                    string code = Adapt(all[0], t);
                    for (int i = 1; i < n; i++) code = $"Math.{fn}({code}, {Adapt(all[i], t)})";
                    Done(R(code, t)); return true;
                }
                string v = VariantFromTyped(all[0]);
                for (int i = 1; i < n; i++) v = $"GDScript.{fn}({v}, {VariantFromTyped(all[i])})";
                Done(R(v, GdType.Variant)); return true;
            }
            case "clamp" or "clampi" or "clampf" when n == 3:
            {
                var all = Enumerable.Range(0, 3).Select(E).ToList();
                bool ints = name == "clampi" || (name == "clamp" && all.All(x => x.Type.Kind == GK.Int));
                bool floats = name == "clampf" || (name == "clamp" && !ints && all.All(x => x.Type.IsNumeric));
                if (ints) { Done(R($"Math.Clamp({I(0)}, {I(1)}, {I(2)})", GdType.Int)); return true; }
                if (floats) { Done(R($"Math.Clamp({F(0)}, {F(1)}, {F(2)})", GdType.Float)); return true; }
                if (all[0].Type.Kind == GK.Struct) { Done(R($"{Wrap(all[0], P.Primary)}.Clamp({Adapt(all[1], all[0].Type)}, {Adapt(all[2], all[0].Type)})", all[0].Type)); return true; }
                Done(R($"GDScript.Min(GDScript.Max({VariantFromTyped(all[0])}, {VariantFromTyped(all[1])}), {VariantFromTyped(all[2])})", GdType.Variant)); return true;
            }
            case "lerp" when n == 3:
            {
                Ex a = E(0), b = E(1);
                if (a.Type.IsNumeric && b.Type.IsNumeric) { Done(R($"Mathf.Lerp({F(0)}, {F(1)}, {F(2)})", GdType.Float)); return true; }
                if (a.Type.Kind == GK.Struct) { Done(R($"{Wrap(a, P.Primary)}.Lerp({Adapt(b, a.Type)}, {Adapt(E(2), GdType.Float32)})", a.Type)); return true; }
                Done(R($"GDScript.Lerp({VariantFromTyped(a)}, {VariantFromTyped(b)}, {VariantFromTyped(E(2))})", GdType.Variant)); return true;
            }
            case "move_toward" when n == 3 && E(0).Type.Kind == GK.Struct:
                Done(R($"{Wrap(E(0), P.Primary)}.MoveToward({Adapt(E(1), E(0).Type)}, {Adapt(E(2), GdType.Float32)})", E(0).Type)); return true;
            case "fmod" when n == 2:
            {
                string fa = F(0), fb = F(1);
                Done(R($"{(E(0).Prec >= P.Mul && fa == E(0).Code ? fa : "(" + fa + ")")} % {(E(1).Prec > P.Mul && fb == E(1).Code ? fb : "(" + fb + ")")}", GdType.Float, P.Mul)); return true;
            }
            case "is_equal_approx" when n == 2:
                Done(R(E(0).Type.Kind == GK.Struct ? $"{Wrap(E(0), P.Primary)}.IsEqualApprox({Adapt(E(1), E(0).Type)})" : $"Mathf.IsEqualApprox({F(0)}, {F(1)})", GdType.Bool)); return true;
            case "is_zero_approx" when n == 1: Done(R($"Mathf.IsZeroApprox({F(0)})", GdType.Bool)); return true;
            case "is_nan" when n == 1: Done(R($"double.IsNaN({F(0)})", GdType.Bool)); return true;
            case "is_inf" when n == 1: Done(R($"double.IsInfinity({F(0)})", GdType.Bool)); return true;
            case "is_finite" when n == 1: Done(R($"double.IsFinite({F(0)})", GdType.Bool)); return true;
        }

        if (s_mathFloat.TryGetValue(name, out string? mf) && n == 1) { Done(R($"{mf}({F(0)})", GdType.Float)); return true; }
        if (s_mathfDouble.TryGetValue(name, out string? md) && n >= 1)
        {
            var args = Enumerable.Range(0, n).Select(F).ToList();
            Done(R($"{md}({string.Join(", ", args)})", name == "fmod" ? GdType.Float : GdType.Float)); return true;
        }
        return false;
    }

    private static string RoundCall(string fn, string x) => fn switch
    {
        "floor" => $"Math.Floor({x})",
        "ceil" => $"Math.Ceiling({x})",
        _ => $"Math.Round({x}, MidpointRounding.AwayFromZero)",
    };

    private Ex ToIntEx(Ex x) => x.Type.Kind switch
    {
        GK.Int => x,
        GK.Float => R($"(long){Wrap(x, P.Unary)}", GdType.Int, P.Unary),
        GK.Enum => R($"(long){Wrap(x, P.Unary)}", GdType.Int, P.Unary),
        GK.Bool => R($"GDScript.ToInt({x.Code})", GdType.Int),
        GK.String or GK.StringName => R($"{StringOperand(x, P.Primary)}.ToInt()", GdType.Int),
        _ => R($"{Wrap(x, P.Primary)}.AsInt64()", GdType.Int),
    };

    private Ex ToFloatEx(Ex x) => x.Type.Kind switch
    {
        GK.Float => x,
        GK.Int => R($"(double){Wrap(x, P.Unary)}", GdType.Float, P.Unary),
        GK.Enum => R($"(double){Wrap(x, P.Unary)}", GdType.Float, P.Unary),
        GK.Bool => R($"GDScript.ToFloat({x.Code})", GdType.Float),
        GK.String or GK.StringName => R($"{StringOperand(x, P.Primary)}.ToFloat()", GdType.Float),
        _ => R($"{Wrap(x, P.Primary)}.AsDouble()", GdType.Float),
    };

    // ---- String ----

    private Ex StringMethod(Ex t, string name, Call c)
    {
        string s = StringOperand(t, P.Primary);
        int n = c.Args.Count;
        var cache = new Ex?[n];
        Ex E(int i) => cache[i] ??= Expr(c.Args[i], null);
        string S(int i) => Adapt(E(i), GdType.String);
        string L(int i) => Adapt(E(i), GdType.Int);
        string Opt(int from, params int[] idx) => string.Concat(idx.Where(i => i < n).Select(i => ", " + (i == from ? "" : "") + L(i)));

        switch (name)
        {
            case "length": return R($"{s}.Length", GdType.Int32);
            case "is_empty": return R($"string.IsNullOrEmpty({s})", GdType.Bool);
            case "to_upper": return R($"{s}.ToUpperInvariant()", GdType.String);
            case "to_lower": return R($"{s}.ToLowerInvariant()", GdType.String);
            case "begins_with": return R($"{s}.StartsWith({S(0)}, StringComparison.Ordinal)", GdType.Bool);
            case "ends_with": return R($"{s}.EndsWith({S(0)}, StringComparison.Ordinal)", GdType.Bool);
            case "contains": return R($"{s}.Contains({S(0)})", GdType.Bool);
            case "find": return R($"{s}.Find({S(0)}{Opt(1, 1)})", GdType.Int);
            case "rfind": return R($"{s}.Rfind({S(0)}{Opt(1, 1)})", GdType.Int);
            case "replace": return R($"{s}.Replace({S(0)}, {S(1)})", GdType.String);
            case "strip_edges": return R(n == 0 ? $"{s}.StripEdges()" : $"{s}.StripEdges({Adapt(E(0), GdType.Bool)}{(n > 1 ? ", " + Adapt(E(1), GdType.Bool) : "")})", GdType.String);
            case "split":
                return R($"{s}.Split({S(0)}{(n > 1 ? ", " + Adapt(E(1), GdType.Bool) : ", true")}{(n > 2 ? ", " + L(2) : "")})", GdType.PackedOf(GdType.String, "string[]"));
            case "substr": return R($"{s}.Substr({L(0)}{Opt(1, 1)})", GdType.String);
            case "left": return R($"{s}.Left({L(0)})", GdType.String);
            case "right": return R($"{s}.Right({L(0)})", GdType.String);
            case "to_int": return R($"{s}.ToInt()", GdType.Int);
            case "to_float": return R($"{s}.ToFloat()", GdType.Float);
            case "is_valid_int": return R($"{s}.IsValidInt()", GdType.Bool);
            case "is_valid_float": return R($"{s}.IsValidFloat()", GdType.Bool);
            case "capitalize": return R($"{s}.Capitalize()", GdType.String);
            case "pad_zeros": return R($"{s}.PadZeros({L(0)})", GdType.String);
            case "pad_decimals": return R($"{s}.PadDecimals({L(0)})", GdType.String);
            case "repeat": return R($"{s}.Repeat({L(0)})", GdType.String);
            case "count": return R($"{s}.Count({S(0)})", GdType.Int);
            case "insert": return R($"{s}.Insert({L(0)}, {S(1)})", GdType.String);
            case "erase": return R($"{s}.Erase({L(0)}{Opt(1, 1)})", GdType.String);
            case "format": return R($"{s}.Format({VariantFromTyped(E(0))})", GdType.String);
            case "join":
            {
                Ex a = E(0);
                return R($"{s}.Join({a.Code})", GdType.String);
            }
            case "get_file": return R($"{s}.GetFile()", GdType.String);
            case "get_base_dir": return R($"{s}.GetBaseDir()", GdType.String);
            case "get_extension": return R($"{s}.GetExtension()", GdType.String);
            case "to_snake_case": return R($"{s}.ToSnakeCase()", GdType.String);
            case "to_pascal_case": return R($"{s}.ToPascalCase()", GdType.String);
            case "to_string": return R(s, GdType.String);
            case "hash": return R($"GD.Hash({s})", GdType.Int32);
            case "unicode_at": return R($"(long){s}[{Adapt(E(0), GdType.Int32)}]", GdType.Int, P.Unary);
        }
        Note(c, "call", name, Rating.Unsupported, $"String.{name}() is not converted");
        return R($"{s}.{Naming.Pascal(name)}({string.Join(", ", c.Args.Select(a => VariantFromTyped(Expr(a, null))))})", GdType.Variant);
    }

    // ---- Array ----

    private Ex ArrayMethod(Ex t, string name, Call c)
    {
        string a = Wrap(t, P.Primary);
        GdType at = t.Type;
        bool typed = at.Typed && at.Elem != null;
        GdType elem = typed ? at.Elem! : GdType.Variant;
        int n = c.Args.Count;
        var cache = new Ex?[n];
        Ex E(int i) => cache[i] ??= Expr(c.Args[i], null);
        string El(int i) => Adapt(E(i), elem);
        string L(int i) => Adapt(E(i), GdType.Int32);

        switch (name)
        {
            case "size": return R($"{a}.Count", GdType.Int32);
            case "is_empty": return R($"{a}.Count == 0", GdType.Bool, P.Eq);
            case "append": case "push_back": return R($"{a}.Add({El(0)})", GdType.Void);
            case "push_front": return R($"{a}.PushFront({El(0)})", GdType.Void);
            case "append_array": return R($"{a}.AppendArray({E(0).Code})", GdType.Void);
            case "pop_back": return R(typed ? $"{a}.PopBack()" : $"{a}.Pop()", elem);
            case "pop_front": return R($"{a}.PopFront()", elem);
            case "has": return R($"{a}.Contains({El(0)})", GdType.Bool);
            case "find": return R($"{a}.Find({El(0)}{(n > 1 ? ", " + Adapt(E(1), GdType.Int) : "")})", GdType.Int);
            case "erase": return R($"{a}.Remove({El(0)})", GdType.Bool);
            case "remove_at": return R($"{a}.RemoveAt({L(0)})", GdType.Void);
            case "insert": return R($"{a}.Insert({L(0)}, {Adapt(E(1), elem)})", GdType.Void);
            case "clear": return R($"{a}.Clear()", GdType.Void);
            case "resize": return R($"{a}.Resize({(typed ? Adapt(E(0), GdType.Int) : L(0))})", typed ? GdType.Void : Obj("Error"));
            case "front": return R($"{a}.Front()", elem);
            case "back": return R($"{a}.Back()", elem);
            case "reverse": return R($"{a}.Reverse()", GdType.Void);
            case "shuffle": return R($"{a}.Shuffle()", GdType.Void);
            case "sort": return R($"{a}.Sort()", GdType.Void);
            case "pick_random": return R($"{a}.PickRandom()", elem);
            case "duplicate": return R($"{a}.Duplicate({(n > 0 ? Adapt(E(0), GdType.Bool) : "")})", at);
            case "slice" when !typed: return R($"{a}.Slice({Adapt(E(0), GdType.Int)}{(n > 1 ? ", " + Adapt(E(1), GdType.Int) : "")})", GdType.UntypedArray);
            case "min": return R($"{a}.Min()", elem);
            case "max": return R($"{a}.Max()", elem);
            case "count": return R($"{a}.Count({El(0)})", GdType.Int);
            case "fill": return R($"{a}.Fill({El(0)})", GdType.Void);
            case "any": case "all": return LambdaArrayCall(a, name == "any" ? "Any" : "All", c, elem, typed, GdType.Bool, 1, null);
            case "filter": return LambdaArrayCall(a, "Filter", c, elem, typed, at, 1, null);
            case "map": return LambdaArrayCall(a, "Map", c, elem, typed, null, 1, null);
            case "sort_custom": return LambdaArrayCall(a, "SortCustom", c, elem, typed, GdType.Void, 2, null);
            case "reduce": return LambdaArrayCall(a, "Reduce", c, elem, typed, null, 2, n > 1 ? E(1) : null);
        }
        Note(c, "call", name, Rating.Unsupported, $"Array.{name}() is not converted");
        return R($"{a}.{Naming.Pascal(name)}({string.Join(", ", c.Args.Select(x => VariantFromTyped(Expr(x, null))))})", GdType.Variant);
    }

    // map/filter/reduce/any/all/sort_custom: a typed array takes the lambda as a C# delegate, the untyped one as a Callable.
    private Ex LambdaArrayCall(string recv, string method, Call c, GdType elem, bool typed, GdType? fixedResult, int lambdaParams, Ex? initial)
    {
        Expr fn = c.Args[0];
        if (typed && fn is LambdaExpr lam)
        {
            var ptypes = new List<GdType>();
            if (method == "Reduce") { ptypes.Add(initial?.Type ?? GdType.Variant); ptypes.Add(elem); }
            else for (int i = 0; i < lambdaParams; i++) ptypes.Add(elem);
            (string text, GdType ret, bool _) = BuildLambda(lam, ptypes);
            string init = method == "Reduce" && initial != null ? ", " + Adapt(initial, initial.Type) : "";
            GdType result = fixedResult ?? (method == "Map" ? GdType.ArrayOf(ret) : method == "Reduce" ? (initial?.Type ?? ret) : GdType.Void);
            string call = method == "SortCustom" ? $"{recv}.SortCustom({text})" : $"{recv}.{method}({text}{init})";
            return R(call, result);
        }
        Ex f = Expr(fn, GdType.Callable);
        string init2 = method == "Reduce" && initial != null ? ", " + VariantFromTyped(initial) : "";
        if (typed)
        {
            // A callable (not a lambda) over a typed array: call it through the Variant.
            string conv = method switch
            {
                "Any" or "All" or "Filter" => $"x => {Wrap(f, P.Primary)}.Call({VariantFromTyped(new Ex("x", elem))}).AsBool()",
                "SortCustom" => $"(x, y) => {Wrap(f, P.Primary)}.Call({VariantFromTyped(new Ex("x", elem))}, {VariantFromTyped(new Ex("y", elem))}).AsBool()",
                "Map" => $"x => {Wrap(f, P.Primary)}.Call({VariantFromTyped(new Ex("x", elem))})",
                _ => $"(acc, x) => {Wrap(f, P.Primary)}.Call(acc, {VariantFromTyped(new Ex("x", elem))})",
            };
            GdType result = fixedResult ?? (method == "Map" ? GdType.ArrayOf(GdType.Variant) : GdType.Variant);
            return R($"{recv}.{method}({conv}{init2})", result);
        }
        GdType res = fixedResult ?? (method == "Map" ? GdType.UntypedArray : GdType.Variant);
        if (method == "Filter") res = GdType.UntypedArray;
        return R($"{recv}.{method}({f.Code}{init2})", res);
    }

    // ---- Dictionary ----

    private Ex DictMethod(Ex t, string name, Call c)
    {
        string d = Wrap(t, P.Primary);
        GdType dt = t.Type;
        bool typed = dt.Typed && dt.Key != null && dt.Value != null;
        GdType kt = typed ? dt.Key! : GdType.Variant, vt = typed ? dt.Value! : GdType.Variant;
        int n = c.Args.Count;
        var cache = new Ex?[n];
        Ex E(int i) => cache[i] ??= Expr(c.Args[i], null);
        switch (name)
        {
            case "size": return R($"{d}.Count", GdType.Int32);
            case "is_empty": return R($"{d}.Count == 0", GdType.Bool, P.Eq);
            case "has": return R($"{d}.ContainsKey({Adapt(E(0), kt)})", GdType.Bool);
            case "has_all": return R($"{E(0).Code}.All(k => {d}.ContainsKey(k))", GdType.Bool);
            case "get": return R($"{d}.Get({Adapt(E(0), kt)}{(n > 1 ? ", " + Adapt(E(1), vt) : "")})", vt);
            case "erase": return R($"{d}.Remove({Adapt(E(0), kt)})", GdType.Bool);
            case "clear": return R($"{d}.Clear()", GdType.Void);
            case "keys": return R($"{d}.KeysArray()", typed ? GdType.ArrayOf(kt) : GdType.UntypedArray);
            case "values": return R($"{d}.ValuesArray()", typed ? GdType.ArrayOf(vt) : GdType.UntypedArray);
            case "merge": return R($"{d}.Merge({E(0).Code}{(n > 1 ? ", " + Adapt(E(1), GdType.Bool) : "")})", GdType.Void);
            case "duplicate": return R($"{d}.Duplicate({(n > 0 ? Adapt(E(0), GdType.Bool) : "")})", dt);
        }
        Note(c, "call", name, Rating.Unsupported, $"Dictionary.{name}() is not converted");
        return R($"{d}.{Naming.Pascal(name)}({string.Join(", ", c.Args.Select(x => VariantFromTyped(Expr(x, null))))})", GdType.Variant);
    }

    // ---- Packed arrays (C# arrays) ----

    private Ex PackedMethod(Ex t, string name, Call c)
    {
        string a = Wrap(t, P.Primary);
        GdType elem = t.Type.Elem ?? GdType.Variant;
        switch (name)
        {
            case "size": return R($"{a}.Length", GdType.Int32);
            case "is_empty": return R($"{a}.Length == 0", GdType.Bool, P.Eq);
            case "has": return R($"{a}.Contains({Adapt(Expr(c.Args[0], elem), elem)})", GdType.Bool);
        }
        Note(c, "call", name, Rating.Unsupported, $"Packed array .{name}() is not converted");
        return R($"{a}.{Naming.Pascal(name)}({string.Join(", ", c.Args.Select(x => VariantFromTyped(Expr(x, null))))})", GdType.Variant);
    }
}
