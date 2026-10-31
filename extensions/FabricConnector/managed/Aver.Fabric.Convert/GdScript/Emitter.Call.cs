// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Calls: script functions, Aver.Fabric methods (overloads picked by the argument types), constructors, signals, lambdas, await.

using Rs = Microsoft.CodeAnalysis;

namespace Aver.Fabric.Convert.GdScript;

internal sealed partial class GdEmitter
{
    private Ex CallEx(Call c, GdType? expected)
    {
        switch (c.Callee)
        {
            case Identifier id: return CallIdentifier(id, c, expected);
            case MemberAccess m: return CallMember(m, c, expected);
            case SuperExpr:
            {
                // super(args): the same function of the base class.
                MemberInfo? self = _ctx.Func;
                if (self == null) return new Ex("base", GdType.Variant);
                if (self.Name == "_init")
                {
                    Note(c, "call", "super", Rating.Approximated, "super(...) in _init: a C# constructor cannot pass arguments on, the base constructor runs with none");
                    return new Ex("", GdType.Void);
                }
                MemberInfo? up = _script.BaseScript?.Find(self.Name);
                _ctx.UsedInstance = true;
                if (up != null && up.Kind == MK.Func) return CallScript(up, "base", c.Args);
                ApiMember? api = Api?.Lookup(_script.ApiRoot, self.Name);
                if (api != null && api.Kind == MemberKind.Method) return CallApi(api, "base", c.Args, expected, c);
                return MissingBase(self.Name, c);
            }
            case Subscript { Target: Identifier { Name: "Array" or "Dictionary" } } sub:
                return TypedCollectionCtor(sub, c);
            default:
            {
                Ex callee = Expr(c.Callee, null);
                return CallableCall(callee, "call", c.Args);
            }
        }
    }

    // super.name() where the base class has no such function: GDScript fails when it runs, so the C# reports the same at that point.
    private Ex MissingBase(string name, Call c)
    {
        Note(c, "call", name, Rating.Unsupported, $"the base class has no '{name}' to call; the converted call logs an error when it runs");
        return new Ex($"GD.PushError({Naming.Quote($"{_script.CsName}: the base class has no function {name}")})", GdType.Variant);
    }

    private Ex TypedCollectionCtor(Subscript sub, Call c)
    {
        var tr = new TypeRef { Start = sub.Start, End = sub.End };
        tr.Parts.Add(((Identifier)sub.Target).Name);
        if (sub.Index is Identifier ti) tr.Args.Add(new TypeRef { Parts = { ti.Name } });
        else if (sub.Index is MemberAccess ma) tr.Args.Add(new TypeRef { Parts = { FlattenName(ma) } });
        GdType t = ResolveType(tr);
        return new Ex($"new {t.Cs}()", t);
    }

    private static string FlattenName(Expr e) => e switch { Identifier i => i.Name, MemberAccess m => FlattenName(m.Target) + "." + m.Name, _ => "" };

    // ---- identifier calls ----

    private Ex CallIdentifier(Identifier id, Call c, GdType? expected)
    {
        string name = id.Name;
        if (FindLocal(name) is { } local && local.Type.Kind is GK.Callable or GK.Lambda or GK.Variant)
            return CallableCall(new Ex(local.Cs, local.Type), "call", c.Args);

        (MemberInfo? mi, ScriptInfo? _) = FindScriptMember(name);
        if (mi != null)
        {
            if (mi.Kind == MK.Func)
            {
                if (!mi.IsStatic) _ctx.UsedInstance = true;
                return CallScript(mi, null, c.Args);
            }
            if (mi.Kind == MK.Var || mi.Kind == MK.Const)
            {
                Ex v = MemberValue(mi, null, mi.Owner);
                if (v.Type.IsClassRef) return ConstructorCall(v, c.Args, expected, c);
                return CallableCall(v, "call", c.Args);
            }
        }

        if (name is "get_node" or "get_node_or_null" && TrySceneGetNode(name, null, c, expected, out Ex? sceneNode)) return sceneNode!;

        Rs.INamedTypeSymbol? root = _script.ApiRoot;
        ApiMember? api = IsApiTypeName(name) ? null : Api?.Lookup(root, name);
        if (api != null && api.Kind == MemberKind.Method) return CallApi(api, "", c.Args, expected, c);
        Ex? acc = Api != null && !IsApiTypeName(name) ? ApiAccessorCall(root, "", name, c.Args, c) : null;
        if (acc != null) return acc;

        if (TryBuiltin(name, c, expected, out Ex? built)) return built!;

        // Type constructors: Vector3(1, 2, 3), Color(...), Transform3D().
        Ex callee = IdentEx(id, null);
        if (callee.Type.IsClassRef) return ConstructorCall(callee, c.Args, expected, c);

        Note(c, "function", name, Rating.Unsupported, $"'{name}()' is not declared in this project or in Aver.Fabric: it is called by name on this object when the code runs");
        _ctx.UsedInstance = true;
        string args = string.Concat(c.Args.Select(a => ", " + VariantFromTyped(Expr(a, null))));
        return new Ex($"Call({Naming.Quote(name)}{args})", GdType.Variant) { UsesInstance = true };
    }

    // ---- script functions ----

    private string ArgsFor(IReadOnlyList<Expr> args, IReadOnlyList<GdType> types, bool lastIsRest = false)
    {
        var parts = new List<string>();
        for (int i = 0; i < args.Count; i++)
        {
            GdType? t = i < types.Count ? types[i] : (lastIsRest && types.Count > 0 ? GdType.Variant : null);
            Ex a = Expr(args[i], t != null && !t.IsVariant ? t : null);
            parts.Add(Adapt(a, t ?? GdType.Variant));
        }
        return string.Join(", ", parts);
    }

    private Ex CallScript(MemberInfo mi, string? receiver, IReadOnlyList<Expr> args)
    {
        FuncDecl fd = mi.Func!;
        List<GdType> pt = ParamTypes(mi);
        bool rest = fd.Params.Count > 0 && fd.Params[^1].IsRest;
        GdType ret = ReturnTypeOf(mi);
        if (_observed != null && _dry == 0) Observe(mi, args);
        string argText = ArgsFor(args, pt, rest);
        string cs = mi.CsName;
        string code;
        if (receiver != null) code = Wrap(new Ex(receiver, GdType.Variant), P.Primary) + "." + cs;
        else if (mi.IsStatic && !ScriptVisibleByName(mi.Owner)) code = mi.Owner.CsPath + "." + cs;
        else code = cs;
        var ex = new Ex($"{code}({argText})", ret.Kind == GK.Lambda ? GdType.Callable : ret) { Func = mi, UsesInstance = receiver == null && !mi.IsStatic };
        if (mi.Async) ex.IsAwaitable = true;
        return ex;
    }

    private string CallableOf(MemberInfo mi, string? receiver)
    {
        FuncDecl fd = mi.Func!;
        List<GdType> pt = ParamTypes(mi);
        GdType ret = ReturnTypeOf(mi);
        string target = receiver ?? "this";
        string method = (receiver == null ? "" : receiver + ".") + (mi.IsStatic && receiver == null && !ScriptVisibleByName(mi.Owner) ? mi.Owner.CsPath + "." : "") + mi.CsName;
        bool taskReturning = mi.Async && !UseAsyncVoid(mi);
        bool rest = fd.Params.Any(p => p.IsRest);
        bool optional = fd.Params.Any(p => p.Default != null);   // a delegate has all its parameters; a call by name takes the defaults
        if (rest || optional || taskReturning || pt.Count > 4 || (ret.Kind != GK.Void && pt.Count > 3))
        {
            // By name: the generated MethodName constant of the class (statics are not listed there).
            string byName = mi.IsStatic ? Naming.Quote(mi.CsName) : (receiver == null ? "" : QualifiedScript(mi.Owner) + ".") + "MethodName." + mi.CsName;
            return $"new Callable({target}, {byName})";
        }
        var targs = pt.Select(t => t.Cs).ToList();
        if (ret.Kind != GK.Void) targs.Add(ret.Cs);
        return targs.Count == 0 ? $"Callable.From({method})" : $"Callable.From<{string.Join(", ", targs)}>({method})";
    }

    // ---- constructors ----

    private Ex ConstructorCall(Ex typeRef, IReadOnlyList<Expr> args, GdType? expected, Node at)
    {
        GdType t = typeRef.Type;
        if (t.Kind == GK.ScriptClass)
        {
            MemberInfo? init = t.Script!.Find("_init");
            List<GdType> pt = init != null && init.Kind == MK.Func ? ParamTypes(init) : new List<GdType>();
            bool rest = init?.Func != null && init.Func.Params.Count > 0 && init.Func.Params[^1].IsRest;
            return new Ex($"new {typeRef.Code}({ArgsFor(args, pt, rest)})", GdType.OfScript(t.Script));
        }
        Rs.INamedTypeSymbol? sym = t.Sym;
        if (Api != null && sym != null)
        {
            List<Rs.IMethodSymbol> ctors = Api.Constructors(sym);
            if (ctors.Count > 0)
            {
                var argEx = args.Select(a => (Expr: a, Type: TypeOf(a, null))).ToList();
                (Rs.IMethodSymbol? m, _) = PickOverload(ctors, argEx.Select(x => x.Type).ToList(), null);
                if (m != null)
                    return new Ex($"new {typeRef.Code}({ArgsForMethod(m, args)})", GdType.Obj(sym));
                // A struct's default constructor, Vector3(): `new Vector3()` is zero; the shipped runtime names Identity where it matters.
                if (args.Count == 0) return new Ex($"new {typeRef.Code}()", GdType.Obj(sym));
                Note(at, "constructor", t.Cs, Rating.Unsupported, $"{t.Cs} has no constructor taking these {args.Count} argument(s)");
            }
        }
        return new Ex($"new {typeRef.Code}({string.Join(", ", args.Select(a => VariantFromTyped(Expr(a, null))))})", t.Kind == GK.Struct || t.IsGodotObject ? t : GdType.Variant);
    }

    // ---- overload resolution against Aver.Fabric ----

    private static int Required(Rs.IMethodSymbol m) => m.Parameters.Count(p => !p.IsOptional && !p.IsParams);

    private (Rs.IMethodSymbol? Method, List<GdType> ParamTypes) PickOverload(IEnumerable<Rs.IMethodSymbol> cands, List<GdType> argTypes, GdType? expected)
    {
        Rs.IMethodSymbol? best = null;
        int bestScore = int.MinValue;
        List<GdType> bestTypes = new();
        foreach (Rs.IMethodSymbol m in cands)
        {
            if (m.IsGenericMethod && !(expected != null && expected.IsGodotObject)) continue;
            bool hasParams = m.Parameters.Length > 0 && m.Parameters[^1].IsParams;
            if (argTypes.Count < Required(m)) continue;
            if (!hasParams && argTypes.Count > m.Parameters.Length) continue;
            int score = 0;
            var types = new List<GdType>();
            bool ok = true;
            for (int i = 0; i < argTypes.Count && ok; i++)
            {
                Rs.IParameterSymbol p = i < m.Parameters.Length ? m.Parameters[i] : m.Parameters[^1];
                Rs.ITypeSymbol pty = p.IsParams && p.Type is Rs.IArrayTypeSymbol at ? at.ElementType : p.Type;
                GdType g = Api!.FromSymbol(pty);
                int s = ScoreArgFull(argTypes[i], g);
                if (s < 0) { ok = false; break; }
                score += s;
                types.Add(g);
            }
            if (!ok) continue;
            if (argTypes.Count == m.Parameters.Length) score += 1;
            if (m.IsGenericMethod) score += 2;
            if (m.Parameters.Length > argTypes.Count && !hasParams) score -= (m.Parameters.Length - argTypes.Count) * 0;
            if (score > bestScore) { best = m; bestScore = score; bestTypes = types; }
        }
        return (best, bestTypes);
    }

    private int ScoreArgFull(GdType from, GdType to)
    {
        if (from.Kind == GK.Null) return to.IsReference || to.IsVariant || to.Kind == GK.Callable ? 3 : -1;
        if (from.Kind == GK.Lambda) return to.Kind == GK.Callable ? 4 : to.IsVariant ? 1 : to.Kind == GK.Variant ? 1 : -1;
        if (from.Kind == GK.Callable && to.Kind == GK.Callable) return 4;
        if (from.Kind == GK.Enum && to.Kind == GK.Enum) return from.Cs == to.Cs ? 4 : -1;
        if (from.Kind == GK.Int && to.Kind == GK.Enum) return 1;
        if (from.IsGodotObject && to.IsGodotObject) return to.Accepts(from) ? (from.Cs == to.Cs ? 4 : 3) : 0;
        if (from.Kind == GK.Struct && to.Kind == GK.Struct) return from.Cs == to.Cs ? 4 : 1;
        if (from.Kind == GK.Float && to.Kind == GK.Int) return 0;
        if (from.Kind is GK.String or GK.StringName or GK.NodePath && to.Kind is GK.String or GK.StringName or GK.NodePath) return from.Kind == to.Kind ? 4 : 3;
        if (from.Kind == GK.Array && to.Kind == GK.Array) return 3;
        if (from.Kind == GK.Dict && to.Kind == GK.Dict) return 3;
        int s = ScoreArg(from, to);
        if (s >= 0) return s;
        // Anything converts to a Variant parameter; objects reach each other by a downcast.
        return to.IsVariant ? 1 : -1;
    }

    private string AdaptToParam(Ex a, Rs.IParameterSymbol p)
    {
        Rs.ITypeSymbol pty = p.IsParams && p.Type is Rs.IArrayTypeSymbol at ? at.ElementType : p.Type;
        GdType g = Api!.FromSymbol(pty);
        string code = Adapt(a, g);
        // uint, short, byte and friends have no implicit conversion from the long a GDScript int is.
        if (a.Type.Kind == GK.Int && pty.SpecialType is Rs.SpecialType.System_UInt32 or Rs.SpecialType.System_UInt64 or Rs.SpecialType.System_Int16
            or Rs.SpecialType.System_UInt16 or Rs.SpecialType.System_Byte or Rs.SpecialType.System_SByte && !(a.Const is long l && l >= 0 && l < 32768))
            return $"({pty.ToDisplayString(s_cs)}){Wrap(new Ex(code, g, a.Prec), P.Unary)}";
        if (a.Type.Kind == GK.Enum && pty.TypeKind != Rs.TypeKind.Enum && g.IsNumeric)
            return $"(long){Wrap(a, P.Unary)}";
        return code;
    }

    private static readonly Rs.SymbolDisplayFormat s_cs = new(
        typeQualificationStyle: Rs.SymbolDisplayTypeQualificationStyle.NameAndContainingTypes,
        miscellaneousOptions: Rs.SymbolDisplayMiscellaneousOptions.UseSpecialTypes);

    private string ArgsForMethod(Rs.IMethodSymbol m, IReadOnlyList<Expr> args)
    {
        var parts = new List<string>();
        for (int i = 0; i < args.Count; i++)
        {
            Rs.IParameterSymbol p = i < m.Parameters.Length ? m.Parameters[i] : m.Parameters[^1];
            Rs.ITypeSymbol pty = p.IsParams && p.Type is Rs.IArrayTypeSymbol at ? at.ElementType : p.Type;
            GdType g = Api!.FromSymbol(pty);
            Ex a = Expr(args[i], g.IsVariant || g.Kind == GK.Void ? null : g);
            parts.Add(AdaptToParam(a, p));
        }
        return string.Join(", ", parts);
    }

    /// <summary>A call of an Aver.Fabric method (<paramref name="receiver"/> empty: on this).</summary>
    private Ex CallApi(ApiMember api, string receiver, IReadOnlyList<Expr> args, GdType? expected, Node at)
    {
        var argTypes = args.Select(a => TypeOf(a, null)).ToList();
        (Rs.IMethodSymbol? m, _) = PickOverload(api.Methods, argTypes, expected);
        string prefix = receiver.Length > 0 ? Wrap(new Ex(receiver, GdType.Variant), P.Primary) + "." : "";
        if (!api.IsStatic && receiver.Length == 0) _ctx.UsedInstance = true;
        if (m == null)
        {
            // GDScript's get_x() / set_x(v) for a property the C# API has as one.
            Ex? acc = ApiAccessorCall(api.Owner, receiver, api.CsName, args, at);
            if (acc != null) return acc;
            Note(at, "call", api.CsName, Rating.Unsupported, $"Aver.Fabric has no {api.CsName} taking {args.Count} argument(s) of these types");
            return new Ex($"{prefix}{api.CsName}({string.Join(", ", args.Select(a => VariantFromTyped(Expr(a, null))))})", GdType.Variant);
        }
        string typeArgs = "";
        GdType ret = Api!.FromSymbol(m.ReturnType);
        if (m.IsGenericMethod)
        {
            typeArgs = $"<{expected!.Cs}>";
            ret = expected;
        }
        string argText = ArgsForMethod(m, args);
        // A method that returns the base Node/object is a guess about what the node really is.
        bool weak = !m.IsGenericMethod && m.ReturnType is Rs.INamedTypeSymbol rn && (rn.Name == "Node" || rn.Name == "GodotObject");
        string typePrefix = m.IsStatic && receiver.Length == 0 ? m.ContainingType.ToDisplayString(s_nameFormat) + "." : prefix;
        var ex = new Ex($"{typePrefix}{m.Name}{typeArgs}({argText})", weak ? ret.WithWeak(true) : ret) { Api = api, UsesInstance = !m.IsStatic && receiver.Length == 0 };
        // A GDScript int is a long and C# has no implicit ulong to long (an object id, a Rid's number).
        if (m.ReturnType.SpecialType == Rs.SpecialType.System_UInt64) { ex.Code = "(long)" + ex.Code; ex.Prec = P.Unary; }
        return ex;
    }

    // get_global_transform(), set_visible(false), is_visible(): the property of the same name.
    private Ex? ApiAccessorCall(Rs.INamedTypeSymbol? type, string receiver, string name, IReadOnlyList<Expr> args, Node at)
    {
        if (Api == null || type == null) return null;
        string gd = name;
        string? prop = null;
        bool setter = false;
        foreach (string pre in new[] { "get_", "set_", "is_", "has_" })
        {
            if (!gd.StartsWith(pre, StringComparison.Ordinal) && !Naming.Pascal(gd).StartsWith(Naming.Pascal(pre), StringComparison.Ordinal)) continue;
            string rest = gd.StartsWith(pre, StringComparison.Ordinal) ? gd[pre.Length..] : Naming.Pascal(gd)[Naming.Pascal(pre).Length..];
            if (rest.Length == 0) continue;
            ApiMember? p = Api.Lookup(type, rest);
            if (p == null || p.Kind is not (MemberKind.Property or MemberKind.Field)) continue;
            if (pre == "set_" && args.Count == 1) { prop = p.CsName; setter = true; break; }
            if (pre != "set_" && args.Count == 0) { prop = p.CsName; break; }
        }
        if (prop == null)
        {
            // Names that arrive already Pascal (the call path passes api.CsName): GetPosition, SetVisible.
            foreach (string pre in new[] { "Get", "Set", "Is", "Has" })
            {
                if (!name.StartsWith(pre, StringComparison.Ordinal) || name.Length <= pre.Length || !char.IsUpper(name[pre.Length])) continue;
                ApiMember? p = Api.Lookup(type, name[pre.Length..]);
                if (p == null || p.Kind is not (MemberKind.Property or MemberKind.Field)) continue;
                if (pre == "Set" && args.Count == 1) { prop = p.CsName; setter = true; break; }
                if (pre != "Set" && args.Count == 0) { prop = p.CsName; break; }
            }
        }
        if (prop == null) return null;
        ApiMember member = Api.Lookup(type, prop)!;
        string prefix = receiver.Length > 0 ? Wrap(new Ex(receiver, GdType.Variant), P.Primary) + "." : "";
        if (receiver.Length == 0 && !member.IsStatic) _ctx.UsedInstance = true;
        if (setter)
        {
            Ex v = Expr(args[0], member.Type);
            return new Ex($"{prefix}{prop} = {Adapt(v, member.Type)}", GdType.Void, P.Assign);
        }
        return new Ex($"{prefix}{prop}", member.Type);
    }

    // ---- calls on members ----

    private Ex CallMember(MemberAccess m, Call c, GdType? expected)
    {
        string name = m.Name;
        if (m.Target is SuperExpr)
        {
            _ctx.UsedInstance = true;
            MemberInfo? up = _script.BaseScript?.Find(name);
            if (up != null && up.Kind == MK.Func) return CallScript(up, "base", c.Args);
            ApiMember? api = Api?.Lookup(_script.ApiRoot, name);
            if (api != null && api.Kind == MemberKind.Method) return CallApi(api, "base", c.Args, expected, c);
            return MissingBase(name, c);
        }

        if (name is "connect" or "disconnect" or "emit" or "is_connected" && m.Target is MemberAccess sigAccess) _signalHintFor = sigAccess;
        Ex t = Expr(m.Target, null);
        _signalHintFor = null;
        string rc = t.Code == "this" ? "" : Wrap(t, P.Primary);   // the receiver as a call prefix; "" is this

        if (t.Signal != null) return SignalCall(t, name, c, expected);

        if (t.Type.IsClassRef)
        {
            if (name == "new") return ConstructorCall(new Ex(t.Code, t.Type.AsClassRefOff()), c.Args, expected, c);
            return StaticCall(t, name, c, expected);
        }

        GdType tt = t.Type;
        if (name is "get_node" or "get_node_or_null" && tt.IsGodotObject && TrySceneGetNode(name, t, c, expected, out Ex? sceneNode)) return sceneNode!;
        if (name == "instantiate" && tt.Kind == GK.Object && TryInstantiate(t, c, expected, out Ex? instance)) return instance!;
        switch (tt.Kind)
        {
            case GK.String or GK.StringName or GK.NodePath:
                return StringMethod(t, name, c);
            case GK.Array:
                return ArrayMethod(t, name, c);
            case GK.Dict:
                return DictMethod(t, name, c);
            case GK.Packed:
                return PackedMethod(t, name, c);
            case GK.Callable or GK.Lambda:
                return CallableCall(t, name, c.Args);
            case GK.Signal:
                break;
            case GK.ScriptClass:
            {
                MemberInfo? mi = tt.Script!.Find(name);
                if (mi != null && mi.Kind == MK.Func) return CallScript(mi, rc.Length == 0 ? null : rc, c.Args);
                if (mi != null && (mi.Kind == MK.Var || mi.Kind == MK.Const))
                {
                    Ex v = MemberValue(mi, Wrap(t, P.Primary), mi.Owner);
                    return CallableCall(v, name, c.Args);
                }
                ApiMember? api = Api?.Lookup(tt.Script.ApiRoot, name);
                if (api != null && api.Kind == MemberKind.Method) return CallApi(api, rc, c.Args, expected, c);
                Ex? acc = ApiAccessorCall(tt.Script.ApiRoot, rc, name, c.Args, c);
                if (acc != null) return acc;
                return DynamicCall(t, name, c);
            }
            case GK.Object or GK.Struct:
            {
                Rs.INamedTypeSymbol? sym = tt.Sym;
                ApiMember? api = Api?.Lookup(sym, name);
                if (api != null && api.Kind == MemberKind.Method)
                {
                    Ex r = CallApi(api, rc, c.Args, expected, c);
                    return r;
                }
                Ex? acc = ApiAccessorCall(sym, rc, name, c.Args, c);
                if (acc != null) return acc;
                if (SubtypeWith(tt, name, true) is { } sub && sub.Find(name) is { } subFunc) return CallScript(subFunc, Wrap(AsScriptType(t, sub), P.Primary), c.Args);
                if (tt.Weak || tt.FromScene || sym == null || sym.Name is "Node" or "GodotObject") return DynamicCall(t, name, c);
                // A member the runtime lacks: keep the name, let the compiler say so.
                string args = string.Join(", ", c.Args.Select(a => VariantFromTyped(Expr(a, null))));
                Note(c, "call", name, Rating.Unsupported, $"Aver.Fabric has no method '{name}' on {tt.Cs}");
                return new Ex($"{Wrap(t, P.Primary)}.{Naming.Pascal(name)}({args})", GdType.Variant);
            }
        }
        return DynamicCall(t, name, c);
    }

    private Ex DynamicCall(Ex t, string name, Call c)
    {
        string q = Naming.Quote(name);
        var args = c.Args.Select(a => VariantFromTyped(Expr(a, null))).ToList();
        string argText = args.Count == 0 ? "" : ", " + string.Join(", ", args);
        if (t.Type.IsVariant)
        {
            Note(c, "dynamic", name, Rating.Approximated, $"'{name}()' is called by name at run time: the type of the receiver is not known");
            return new Ex($"GDScript.CallMethod({t.Code}, {q}{argText})", GdType.Variant);
        }
        Note(c, "dynamic", name, Rating.Approximated, $"'{name}()' is called by name at run time: {t.Type.Cs} has no such method");
        return new Ex($"{Wrap(t, P.Primary)}.Call({q}{argText})", GdType.Variant);
    }

    private Ex StaticCall(Ex t, string name, Call c, GdType? expected)
    {
        GdType tt = t.Type;
        // A script enum is a dictionary in GDScript: State.keys() are the names as written, State.values() the numbers.
        if (tt.Kind == GK.Enum && tt.EnumDef != null && c.Args.Count == 0)
        {
            if (name == "keys")
                return new Ex("new[] { " + string.Join(", ", tt.EnumDef.Values.Keys.Select(Naming.Quote)) + " }", GdType.PackedOf(GdType.String, "string[]"));
            if (name == "values")
                return new Ex($"Enum.GetValues<{tt.Cs}>().Select(v => (long)v).ToArray()", GdType.PackedOf(GdType.Int, "long[]"));
        }
        // The number of names, which duplicate values do not change (the C# enum has one member per name).
        if (tt.Kind == GK.Enum && tt.EnumDef != null && c.Args.Count == 0 && name == "size")
        {
            long n = tt.EnumDef.Values.Count;
            return new Ex(n.ToString(System.Globalization.CultureInfo.InvariantCulture), GdType.Int) { Const = n, IsConstExpr = true };
        }
        if (tt.Kind == GK.ScriptClass)
        {
            MemberInfo? mi = tt.Script!.Find(name);
            if (mi != null && mi.Kind == MK.Func) return CallScript(mi, t.Code, c.Args);
        }
        Rs.INamedTypeSymbol? sym = tt.Kind == GK.ScriptClass ? tt.Script!.ApiRoot : tt.Sym;
        if (Api != null && sym != null)
        {
            ApiMember? api = Api.Lookup(sym, name);
            if (api != null && api.Kind == MemberKind.Method) return CallApi(api, t.Code, c.Args, expected, c);
            if (sym.Name == "Color" && Api.Fabric("Colors") is { } colors)
            {
                ApiMember? fromHtml = Api.Lookup(sym, "from_" + name) ?? Api.Lookup(sym, name == "html" ? "from_html" : name);
                if (fromHtml != null && fromHtml.Kind == MemberKind.Method) return CallApi(fromHtml, t.Code, c.Args, expected, c);
            }
        }
        string args = string.Join(", ", c.Args.Select(a => VariantFromTyped(Expr(a, null))));
        Note(c, "call", name, Rating.Unsupported, $"Aver.Fabric has no static '{name}' on {tt.Cs}");
        return new Ex($"{t.Code}.{Naming.Pascal(name)}({args})", GdType.Variant);
    }

    // ---- signals ----

    private Ex SignalCall(Ex t, string name, Call c, GdType? expected)
    {
        SignalRef sr = t.Signal!;
        string owner = sr.Owner;
        string recv = owner.Length > 0 ? Wrap(new Ex(owner, GdType.Variant), P.Primary) + "." : "";
        switch (name)
        {
            case "connect":
            {
                string callable = Adapt(Expr(c.Args[0], GdType.Callable), GdType.Callable);
                string flags = c.Args.Count > 1 ? ", " + FlagsArg(c.Args[1]) : "";
                if (owner.Length == 0) _ctx.UsedInstance = true;
                return new Ex($"{recv}Connect({sr.NameCode}, {callable}{flags})", Obj("Error"));
            }
            case "disconnect":
                if (owner.Length == 0) _ctx.UsedInstance = true;
                return new Ex($"{recv}Disconnect({sr.NameCode}, {Adapt(Expr(c.Args[0], GdType.Callable), GdType.Callable)})", GdType.Void);
            case "is_connected":
                if (owner.Length == 0) _ctx.UsedInstance = true;
                return new Ex($"{recv}IsConnected({sr.NameCode}, {Adapt(Expr(c.Args[0], GdType.Callable), GdType.Callable)})", GdType.Bool);
            case "emit":
            {
                if (owner.Length == 0) _ctx.UsedInstance = true;
                // Our own signals have an EmitSignalX helper with the declared parameter types.
                if (owner.Length == 0 && sr.EmitHelper != null && sr.Script != null && ReferenceEquals(sr.Script, _script) || (owner.Length == 0 && sr.EmitHelper != null))
                    return new Ex($"{sr.EmitHelper}({ArgsFor(c.Args, sr.ParamTypes)})", GdType.Void);
                var args = c.Args.Select(a => VariantFromTyped(Expr(a, null))).ToList();
                return new Ex($"{recv}EmitSignal({sr.NameCode}{(args.Count > 0 ? ", " + string.Join(", ", args) : "")})", Obj("Error"));
            }
            case "get_name": return new Ex(sr.NameCode, GdType.StringName);
            case "has_connections": return new Ex($"{recv}HasConnections({sr.NameCode})", GdType.Bool);
        }
        Note(c, "call", name, Rating.Unsupported, $"signal.{name}() is not converted");
        return new Ex($"default(Variant) /* signal.{name} */", GdType.Variant);
    }

    private string FlagsArg(Expr e)
    {
        Ex f = Expr(e, null);
        return f.Type.Kind == GK.Enum ? $"(uint){Wrap(f, P.Unary)}" : $"(uint){Wrap(f, P.Unary)}";
    }

    // ---- callables ----

    private Ex CallableCall(Ex callee, string name, IReadOnlyList<Expr> args)
    {
        string recv = Wrap(callee, P.Primary);
        if (callee.Type.IsVariant) recv += ".AsCallable()";
        var va = args.Select(a => VariantFromTyped(Expr(a, null))).ToList();
        string list = string.Join(", ", va);
        switch (name)
        {
            case "call": return new Ex($"{recv}.Call({list})", GdType.Variant);
            case "callv": return new Ex($"{recv}.Callv({list})", GdType.Variant);
            case "call_deferred": return new Ex($"{recv}.CallDeferred({list})", GdType.Void);
            case "bind": return new Ex($"{recv}.Bind({list})", GdType.Callable);
            case "unbind": return new Ex($"{recv}.Unbind({Adapt(Expr(args[0], GdType.Int32), GdType.Int32)})", GdType.Callable);
            case "rpc": return new Ex($"(int){recv}.Rpc({list})", GdType.Int32);
            case "rpc_id" when args.Count > 0:
                return new Ex($"(int){recv}.RpcId({Adapt(Expr(args[0], GdType.Int), GdType.Int)}{(args.Count > 1 ? ", " + string.Join(", ", va.Skip(1)) : "")})", GdType.Int32);
            case "is_valid": return new Ex($"{recv}.IsValid()", GdType.Bool);
            case "is_null": return new Ex($"{recv}.IsNull", GdType.Bool);
            case "get_method": return new Ex($"{recv}.Method", GdType.StringName);
            case "get_object": return new Ex($"{recv}.Target", Obj("GodotObject"));
        }
        Note(args.Count > 0 ? args[0] : null, "call", name, Rating.Unsupported, $"Callable.{name}() is not converted");
        return new Ex($"{recv}.{Naming.Pascal(name)}({list})", GdType.Variant);
    }

    // ---- lambdas ----

    private Ex LambdaEx(LambdaExpr l, GdType? expected)
    {
        string? self = null;
        if (l.Name != null)
        {
            // func fact(n): ... fact(n - 1): the name inside the body is the lambda itself.
            PushScope();
            self = LocalName(l, l.Name);
            Declare(l.Name, self + "()", GdType.Callable, l);
        }
        (string text, GdType ret, bool isAsync) = BuildLambda(l, null);
        string code = isAsync ? $"Callable.From((Action)({text}))" : $"Callable.From({text})";
        if (self != null)
        {
            PopScope();
            code = $"GDScript.Named({self} => {code})";
        }
        return new Ex(code, GdType.Lam(ret));
    }

    /// <summary>The C# lambda for a GDScript one. <paramref name="paramTypes"/> overrides untyped parameters (an array method knows its elements).</summary>
    private (string Text, GdType Return, bool IsAsync) BuildLambda(LambdaExpr l, List<GdType>? paramTypes)
    {
        bool isAsync = PContainsAwaitLambda(l);
        // Pass 1 (dry): what the body returns.
        GdType ret = LambdaReturnType(l, paramTypes);

        var saved = (_ctx.Return, _ctx.InferReturn, _ctx.ReturnTypes, _ctx.ReturnsWithoutValue, _w);
        _ctx.Return = ret;
        _ctx.InferReturn = false;
        _ctx.ReturnTypes = new List<GdType>();
        PushScope();
        var ps = new List<string>();
        bool hasDefaults = false;
        for (int i = 0; i < l.Params.Count; i++)
        {
            Param p = l.Params[i];
            GdType pt = p.Type != null ? ResolveType(p.Type) : paramTypes != null && i < paramTypes.Count ? paramTypes[i] : (p.Default != null ? DefaultLiteralType(p.Default, GdType.Variant) : GdType.Variant);
            string cs = LocalName(p, p.Name);
            string def = "";
            if (p.Default != null)
            {
                Ex dv = Expr(p.Default, pt);
                def = dv.Type.Kind == GK.Null ? " = default" : dv.IsConstExpr || dv.Const != null ? " = " + Adapt(dv, pt) : "";
                hasDefaults = true;
            }
            Declare(p.Name, cs, pt, p);
            ps.Add($"{pt.Cs} {cs}{def}");
        }
        string header = $"({string.Join(", ", ps)}) =>";
        string text;

        // A body that is one return or one call is an expression body.
        Stmt? single = l.Body.Statements.Count == 1 ? l.Body.Statements[0] : null;
        if (single is ReturnStmt { Value: not null } rs && !isAsync)
        {
            Ex v = Expr(rs.Value!, ret.IsVariant || ret.Kind == GK.Void ? null : ret);
            text = $"{header} {Unparen(Adapt(v, ret))}";
        }
        else if (single is ExprStmt { Expr: Call or MemberAccess } es && !isAsync && ret.Kind == GK.Void)
        {
            Ex v = Expr(es.Expr, null);
            text = AsStatement(v.Code).StartsWith("_ = ", StringComparison.Ordinal) ? BlockLambda(header, l.Body) : $"{header} {v.Code}";
        }
        else text = BlockLambda(header, l.Body);
        PopScope();
        (_ctx.Return, _ctx.InferReturn, _ctx.ReturnTypes, _ctx.ReturnsWithoutValue, _w) = saved;
        // A lambda with default parameters keeps its own delegate type, so the Callable can fill the defaults in.
        if (hasDefaults) text = "(Delegate)(" + text + ")";
        return (text, ret, isAsync);
    }

    private string BlockLambda(string header, Block body)
    {
        var inner = new CodeWriter();
        var keep = _w;
        _w = inner;
        inner.Indent = 1;
        EmitBlockBody(body);
        _w = keep;
        return header + "\n{\n" + inner.ToString().TrimEnd('\n') + "\n}";
    }

    private static bool PContainsAwaitLambda(LambdaExpr l) => ProjectModel.ContainsAwait(l.Body);

    private GdType LambdaReturnType(LambdaExpr l, List<GdType>? paramTypes)
    {
        if (l.ReturnType != null) return ResolveType(l.ReturnType);
        var saved = (_ctx.Return, _ctx.InferReturn, _ctx.ReturnTypes, _ctx.ReturnsWithoutValue);
        _ctx.InferReturn = true;
        _ctx.ReturnTypes = new List<GdType>();
        _ctx.ReturnsWithoutValue = false;
        List<GdType> types;
        bool bare;
        using (var _ = new DryScope(this))
        {
            PushScope();
            for (int i = 0; i < l.Params.Count; i++)
            {
                Param p = l.Params[i];
                GdType pt = p.Type != null ? ResolveType(p.Type) : paramTypes != null && i < paramTypes.Count ? paramTypes[i] : GdType.Variant;
                Declare(p.Name, LocalName(p, p.Name), pt, p);
            }
            EmitBlockBody(l.Body);
            types = _ctx.ReturnTypes;
            bare = _ctx.ReturnsWithoutValue;
        }
        (_ctx.Return, _ctx.InferReturn, _ctx.ReturnTypes, _ctx.ReturnsWithoutValue) = saved;
        GdType? u = null;
        foreach (GdType t in types)
        {
            if (t.Kind is GK.Null or GK.Void) { if (u == null) u = null; continue; }
            u = u == null ? t : JoinTypes(u, t);
        }
        if (u == null) return types.Any(t => t.Kind == GK.Null) ? GdType.Variant : GdType.Void;
        if (bare) return GdType.Variant;
        return u.WithWeak(false);
    }

    // ---- await ----

    private string AwaitCode(AwaitExpr aw, bool resultUsed, out GdType type)
    {
        if (aw.Operand is MemberAccess ama) _signalHintFor = ama;
        Ex op = Expr(aw.Operand, null);
        _signalHintFor = null;
        _ctx.UsedInstance = true;
        if (op.Signal != null)
        {
            string owner = op.Signal.Owner.Length > 0 ? op.Signal.Owner : "this";
            string awaitCode = $"await ToSignal({owner}, {op.Signal.NameCode})";
            if (resultUsed) { type = GdType.Variant; return $"GDScript.AwaitResult({awaitCode})"; }
            type = GdType.Void;
            return awaitCode;
        }
        if (op.IsAwaitable && op.Func != null)
        {
            GdType ret = ReturnTypeOf(op.Func);
            type = ret;
            return "await " + op.Code;
        }
        if (op.Type.Kind == GK.Signal || op.Type.IsVariant)
        {
            string awaitCode = $"await GDScript.ToSignal({VariantFromTyped(op)})";
            type = resultUsed ? GdType.Variant : GdType.Void;
            return resultUsed ? $"GDScript.AwaitResult({awaitCode})" : awaitCode;
        }
        // Awaiting a plain value resumes at once.
        Note(aw, "await", "", Rating.Approximated, "await on a value that is neither a signal nor a coroutine of this project: it does not wait");
        type = op.Type;
        return op.Code;
    }
}
