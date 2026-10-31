// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Expressions other than calls: literals, names, members, indexing, operators (with GDScript's Variant rules), collections, lambdas.

using System.Globalization;
using Rs = Microsoft.CodeAnalysis;

namespace Aver.Fabric.Convert.GdScript;

internal sealed partial class GdEmitter
{
    private Ex Expr(Expr e, GdType? expected = null)
    {
        switch (e)
        {
            case Literal l: return LiteralEx(l, expected);
            case Identifier id: return IdentEx(id, expected);
            case SelfExpr: _ctx.UsedInstance = true; return new Ex("this", GdType.OfScript(_script)) { UsesInstance = true };
            case SuperExpr: return new Ex("base", GdType.OfScript(_script)) { IsSuper = true, UsesInstance = true };
            case Unary u: return UnaryEx(u, expected);
            case Binary b: return BinaryEx(b, expected);
            case Ternary t: return TernaryEx(t, expected);
            case Assign a:
            {
                // An assignment used as a value (inline lambda body): emitted as a statement by the caller; here its value.
                Ex v = Expr(a.Value, expected);
                return v;
            }
            case Call c: return CallEx(c, expected);
            case MemberAccess m: return MemberEx(m, expected);
            case Subscript s: return SubscriptEx(s, expected);
            case GetNodeExpr g: return GetNodeEx(g, expected);
            case ArrayLit a: return ArrayEx(a, expected);
            case DictLit d: return DictEx(d, expected);
            case LambdaExpr l: return LambdaEx(l, expected);
            case PreloadExpr p: return PreloadEx(p);
            case AwaitExpr aw: { string code = AwaitCode(aw, resultUsed: true, out GdType t); return new Ex("(" + code + ")", t); }
            case TypeTest tt: return TypeTestEx(tt);
            case Cast c: return CastEx(c);
        }
        return new Ex($"default /* {e.GetType().Name} */", GdType.Variant);
    }

    // ---- literals ----

    private Ex LiteralEx(Literal l, GdType? expected)
    {
        switch (l.Kind)
        {
            case LitKind.Int:
            {
                long v = (long)l.Value!;
                string code = v == long.MinValue ? "long.MinValue" : v.ToString(CultureInfo.InvariantCulture);
                return new Ex(code, GdType.Int, v < 0 ? P.Unary : P.Primary) { Const = v, IsConstExpr = true };
            }
            case LitKind.Float:
            {
                double d = (double)l.Value!;
                return new Ex(Naming.Num(d), GdType.Float, d < 0 ? P.Unary : P.Primary) { Const = d, IsConstExpr = true };
            }
            case LitKind.String: return new Ex(Naming.Quote((string)l.Value!), GdType.String) { Const = l.Value, IsConstExpr = true };
            case LitKind.StringName: return new Ex(Naming.Quote((string)l.Value!), GdType.StringName) { Const = l.Value, IsConstExpr = true };
            case LitKind.NodePath: return new Ex(Naming.Quote((string)l.Value!), GdType.NodePath) { Const = l.Value, IsConstExpr = true };
            case LitKind.Bool: return new Ex((bool)l.Value! ? "true" : "false", GdType.Bool) { Const = l.Value, IsConstExpr = true };
            default: return new Ex("null", GdType.Null) { IsConstExpr = true };
        }
    }

    // ---- identifiers ----

    private (MemberInfo? Member, ScriptInfo? Scope) FindScriptMember(string name)
    {
        for (ScriptInfo? o = _script; o != null; o = o.Outer)
        {
            MemberInfo? m = o.Find(name);
            if (m != null) return (m, o);
        }
        return (null, null);
    }

    private static Ex TypeRef(GdType t, string code) => new Ex(code, t.AsClassRef());

    private Ex IdentEx(Identifier id, GdType? expected)
    {
        string name = id.Name;
        if (_ctx.Narrow.TryGetValue(name, out var narrow)) return new Ex(narrow.Pattern, narrow.Type) { Addressable = true };
        Local? local = FindLocal(name);
        if (local != null) return new Ex(local.Cs, local.Type) { Addressable = true, Provisional = local.Provisional ? local.Decl : null };

        (MemberInfo? mi, ScriptInfo? scope) = FindScriptMember(name);
        if (mi != null) return MemberValue(mi, null, scope!);

        // The base class from Aver.Fabric: properties, fields, methods, signals of Node3D and so on.
        Rs.INamedTypeSymbol? root = _script.ApiRoot;
        ApiMember? api = IsApiTypeName(name) ? null : Api?.Lookup(root, name);
        if (api != null && api.Kind != MemberKind.NestedType) return ApiValue(api, "", root, expected);
        if (Api != null && root != null && !IsApiTypeName(name) && Api.IsSignal(root, name, out string sig))
            return new Ex("new Signal(this, " + root.Name + ".SignalName." + sig + ")", GdType.Signal)
            {
                Signal = new SignalRef { Owner = "", NameCode = root.Name + ".SignalName." + sig, OwnerType = GdType.OfScript(_script) }, UsesInstance = true,
            };

        // Globals.
        switch (name)
        {
            case "PI": return new Ex("Math.PI", GdType.Float) { IsConstExpr = true };
            case "TAU": return new Ex("Math.Tau", GdType.Float) { IsConstExpr = true };
            case "INF": return new Ex("double.PositiveInfinity", GdType.Float) { IsConstExpr = true };
            case "NAN": return new Ex("double.NaN", GdType.Float) { IsConstExpr = true };
            case "CONNECT_DEFERRED" or "CONNECT_PERSIST" or "CONNECT_ONE_SHOT" or "CONNECT_REFERENCE_COUNTED" when Api?.Fabric("GodotObject.ConnectFlags") is { } flags:
                return new Ex("GodotObject.ConnectFlags." + Naming.Pascal(name["CONNECT_".Length..]), GdType.Obj(flags)) { IsConstExpr = true };
        }
        AutoloadInfo? auto = _m.FindAutoload(name);
        if (auto != null) return AutoloadEx(auto);
        if (_m.ByClassName.TryGetValue(name, out ScriptInfo? cls)) return TypeRef(GdType.OfScript(cls), QualifiedScript(cls));
        if (Api != null)
        {
            Rs.INamedTypeSymbol? type = Api.Fabric(name);
            if (type != null) return TypeRef(GdType.Obj(type), type.ToDisplayString(s_nameFormat));
            if (Api.FindGlobalEnumConstant(name, out Rs.INamedTypeSymbol? en, out string member))
                return new Ex(en!.ToDisplayString(s_nameFormat) + "." + member, GdType.Obj(en), P.Primary) { IsConstExpr = true, Const = null };
        }
        if (name is "Object") return TypeRef(Obj("GodotObject"), "GodotObject");
        // Not declared anywhere the converter can see: GDScript looks such a name up on the object when it runs, so does the C#.
        Note(id, "identifier", name, Rating.Unsupported, $"'{name}' is not declared in this project or in Aver.Fabric: it is read by name on this object when the code runs");
        _ctx.UsedInstance = true;
        string q = Naming.Quote(name);
        return new Ex($"Get({q})", GdType.Variant) { SetTemplate = $"Set({q}, \u0001)", UsesInstance = true };
    }

    // GDScript is case sensitive: Basis is the type and basis the property, but the case-blind member match would take Basis for the Node3D property
    // (and Mesh for MeshInstance3D.Mesh) and the constructor call would become a call by name. A PascalCase name that is a type of the API is the type.
    private bool IsApiTypeName(string name) =>
        Api != null && name.Length > 1 && char.IsUpper(name[0]) && name.Any(char.IsLower) && Api.Fabric(name) != null;

    private Ex AutoloadEx(AutoloadInfo a)
    {
        ScriptInfo? s = _m.ByPath.GetValueOrDefault(a.Path);
        GdType t = s != null ? GdType.OfScript(s) : Obj("Node").WithWeak(true);
        string tn = s != null ? QualifiedScript(s) : "Node";
        bool inNode = _script.ApiRoot is { } r && Api?.Fabric("Node") is { } node && GdType.Derives(r, node);
        string code = inNode ? $"GetNode<{tn}>(\"/root/{a.Name}\")" : $"SceneTree.Instance.Root.GetNode<{tn}>(\"{a.Name}\")";
        return new Ex(code, t) { UsesInstance = inNode };
    }

    private Ex MemberValue(MemberInfo mi, string? receiver, ScriptInfo scope)
    {
        switch (mi.Kind)
        {
            case MK.Var:
            {
                GdType t = VarType(mi);
                bool backing = ReferenceEquals(_ctx.InAccessor, mi);
                string cs = backing ? mi.Backing : mi.CsName;
                string code = receiver != null ? Wrap(new Ex(receiver, GdType.Variant), P.Primary) + "." + cs : cs;
                if (receiver == null && mi.IsStatic && !ScriptVisibleByName(mi.Owner)) code = mi.Owner.CsPath + "." + cs;
                if (!mi.IsStatic && receiver == null) _ctx.UsedInstance = true;
                var vdecl = (VarDecl)mi.Decl;
                bool prov = vdecl.Type == null && !vdecl.Inferred && vdecl.Init != null && !t.IsVariant;
                return new Ex(code, t)
                {
                    Addressable = !mi.HasAccessors || backing, UsesInstance = !mi.IsStatic && receiver == null, Provisional = prov ? vdecl : null,
                    SceneRes = t.Kind == GK.Object && t.Sym?.Name == "PackedScene" ? PackedSceneOf(mi) : null,
                };
            }
            case MK.Const:
            {
                if (mi.Enum != null && mi.Decl is EnumDecl)
                {
                    string c1 = receiver != null ? receiver + "." + mi.CsName : (ScriptVisibleByName(mi.Owner) ? mi.CsName : mi.Owner.CsPath + "." + mi.CsName);
                    return new Ex(c1, GdType.Int) { IsConstExpr = true };
                }
                if (mi.ConstAlias == null) ResolveConstAlias(mi);
                if (mi.AliasScript != null) return TypeRef(GdType.OfScript(mi.AliasScript), QualifiedScript(mi.AliasScript));
                GdType t = ConstType(mi);
                string code = receiver != null ? receiver + "." + mi.CsName : (ScriptVisibleByName(mi.Owner) ? mi.CsName : mi.Owner.CsPath + "." + mi.CsName);
                var ex = new Ex(code, t) { IsConstExpr = IsCsConst(mi), Const = ConstLiteralValue(mi), FromName = true, SceneRes = SceneOfConst(mi) };
                return ex;
            }
            case MK.Enum:
                return TypeRef(GdType.OfEnum(mi.Enum!), QualifiedEnum(mi.Enum!));
            case MK.Class:
                return TypeRef(GdType.OfScript(mi.Inner!), QualifiedScript(mi.Inner!));
            case MK.Signal:
            {
                var sd = (SignalDecl)mi.Decl;
                string owner = receiver ?? "";
                string nameCode = (receiver != null ? "" : "") + $"SignalName.{mi.CsName}";
                var sr = new SignalRef
                {
                    Owner = owner, NameCode = nameCode, Script = mi.Owner, Decl = sd, EmitHelper = "EmitSignal" + mi.CsName,
                    ParamTypes = sd.Params.Select(p => p.Type != null ? ResolveType(p.Type, mi.Owner) : GdType.Variant).ToList(),
                    OwnerType = GdType.OfScript(mi.Owner),
                };
                if (receiver != null) sr.NameCode = $"{QualifiedScript(mi.Owner)}.SignalName.{mi.CsName}";
                string recv = receiver ?? "this";
                if (receiver == null) _ctx.UsedInstance = true;
                return new Ex($"new Signal({recv}, {sr.NameCode})", GdType.Signal) { Signal = sr, UsesInstance = receiver == null };
            }
            case MK.Func:
            {
                if (mi.IsStatic) { }
                else if (receiver == null) _ctx.UsedInstance = true;
                string code = CallableOf(mi, receiver);
                return new Ex(code, GdType.Callable) { Func = mi, Receiver = receiver ?? "", UsesInstance = receiver == null && !mi.IsStatic };
            }
        }
        return new Ex(mi.CsName, GdType.Variant);
    }

    // A script class's own members are reached by simple name from itself, its descendants and its inner classes.
    private bool ScriptVisibleByName(ScriptInfo owner)
    {
        for (ScriptInfo? o = _script; o != null; o = o.Outer)
            foreach (ScriptInfo c in o.Chain())
                if (ReferenceEquals(c, owner)) return true;
        return false;
    }

    private bool IsCsConst(MemberInfo mi)
    {
        if (mi.CsConst.HasValue) return mi.CsConst.Value;
        mi.CsConst = false;
        var cd = (ConstDecl)mi.Decl;
        if (cd.Value == null) return false;
        var saved = (_script, _ctx, _w);
        try
        {
            EnterMemberContext(mi.Owner, true);
            using var _ = new DryScope(this);
            Ex v = Expr(cd.Value, ConstType(mi));
            GdType t = ConstType(mi);
            mi.CsConst = v.IsConstExpr && t.Kind is GK.Int or GK.Float or GK.Bool or GK.String;
        }
        finally { (_script, _ctx, _w) = saved; }
        return mi.CsConst.Value;
    }

    private static object? ConstLiteralValue(MemberInfo mi) =>
        mi.Decl is ConstDecl { Value: Literal l } && l.Kind is LitKind.Int or LitKind.Float or LitKind.String or LitKind.Bool ? l.Value : null;

    // ---- API members used as values ----

    private Ex ApiValue(ApiMember api, string receiver, Rs.INamedTypeSymbol? ownerType, GdType? expected)
    {
        string prefix = receiver.Length > 0 ? Wrap(new Ex(receiver, GdType.Variant), P.Primary) + "." : "";
        if (!api.IsStatic && receiver.Length == 0) _ctx.UsedInstance = true;
        switch (api.Kind)
        {
            case MemberKind.Property:
            case MemberKind.Field:
            case MemberKind.Constant:
            {
                var value = new Ex(prefix + api.CsName, api.Type)
                {
                    Addressable = api.Kind == MemberKind.Field && api.CanWrite, UsesInstance = !api.IsStatic && receiver.Length == 0,
                    IsConstExpr = api.Kind == MemberKind.Constant, Api = api,
                };
                // A GDScript int is a long and C# has no implicit ulong to long.
                if (!api.CanWrite && (api.Symbol as Rs.IPropertySymbol)?.Type.SpecialType == Rs.SpecialType.System_UInt64) { value.Code = "(long)" + value.Code; value.Prec = P.Unary; }
                return value;
            }
            case MemberKind.Method:
            {
                // A method used as a value: a Callable of it.
                string target = receiver.Length > 0 ? receiver : "this";
                return new Ex($"new Callable({target}, {Naming.Quote(api.CsName)})", GdType.Callable) { Api = api, Receiver = receiver, UsesInstance = receiver.Length == 0 };
            }
        }
        return new Ex(prefix + api.CsName, api.Type) { Api = api };
    }

    // ---- members ----

    private Ex MemberEx(MemberAccess m, GdType? expected)
    {
        if (m.Target is SuperExpr)
        {
            _ctx.UsedInstance = true;
            MemberInfo? up = _script.BaseScript?.Find(m.Name);
            if (up != null) return MemberValue(up, "base", up.Owner);
            ApiMember? am = Api?.Lookup(_script.ApiRoot, m.Name);
            return am != null ? ApiValue(am, "base", _script.ApiRoot, expected) : new Ex("base." + Naming.Pascal(m.Name), GdType.Variant);
        }
        Ex target = Expr(m.Target, null);
        if (ReferenceEquals(_signalHintFor, m))
        {
            // x.name.connect(...) / await x.name: when the runtime has no such property, the name is a signal.
            _signalHintFor = null;
            bool dynamic;
            using (new DryScope(this)) dynamic = MemberOf(target, m.Name, m, expected).IsDynamic && !target.Type.IsClassRef;
            if (dynamic) return DynamicSignalEx(target, m.Name);
        }
        return MemberOf(target, m.Name, m, expected);
    }

    private MemberAccess? _signalHintFor;

    private Ex DynamicSignalEx(Ex target, string name)
    {
        string owner = target.Type.IsVariant ? Wrap(target, P.Primary) + ".AsGodotObject()" : target.Code == "this" ? "" : Wrap(target, P.Primary);
        string q = Naming.Quote(name);
        return new Ex($"new Signal({(owner.Length == 0 ? "this" : owner)}, {q})", GdType.Signal)
        { Signal = new SignalRef { Owner = owner, NameCode = q, OwnerType = target.Type } };
    }

    private Ex MemberOf(Ex target, string name, Node at, GdType? expected)
    {
        GdType tt = target.Type;
        string tcode = Wrap(target, P.Primary);

        // Members of a type name: statics, constants, enums, nested classes.
        if (tt.IsClassRef) return StaticMember(target, name, at);

        switch (tt.Kind)
        {
            case GK.ScriptClass:
            {
                ScriptInfo s = tt.Script!;
                MemberInfo? mi = s.Find(name);
                if (mi != null)
                {
                    Ex v = MemberValue(mi, target.Code == "this" ? null : tcode, mi.Owner);
                    if (target.Code == "this") v.Receiver = "";
                    return v;
                }
                Rs.INamedTypeSymbol? root = s.ApiRoot;
                ApiMember? api = Api?.Lookup(root, name);
                if (api != null && api.Kind != MemberKind.NestedType) return ApiValue(api, tcode, root, expected);
                if (Api != null && root != null && Api.IsSignal(root, name, out string sig))
                    return new Ex($"new Signal({tcode}, {root.Name}.SignalName.{sig})", GdType.Signal)
                    { Signal = new SignalRef { Owner = tcode, NameCode = root.Name + ".SignalName." + sig, OwnerType = tt } };
                return DynamicMember(target, name, at);
            }
            case GK.Object:
            case GK.Struct:
            {
                Rs.INamedTypeSymbol? sym = tt.Sym;
                ApiMember? api = Api?.Lookup(sym, name);
                if (api != null && api.Kind != MemberKind.NestedType)
                {
                    Ex v = ApiValue(api, tcode, sym, expected);
                    if (tt.Kind == GK.Struct && (api.Kind == MemberKind.Field || api.Kind == MemberKind.Property))
                    {
                        v.Parent = target;
                        v.FieldCs = api.CsName;
                        v.Addressable = target.Addressable && api.Kind == MemberKind.Field && api.CanWrite;
                    }
                    else if (api.Kind == MemberKind.Field) v.Addressable = true;
                    return v;
                }
                if (Api != null && sym != null && Api.IsSignal(sym, name, out string sig))
                    return new Ex($"new Signal({tcode}, {sym.ToDisplayString(s_nameFormat)}.SignalName.{sig})", GdType.Signal)
                    { Signal = new SignalRef { Owner = tcode, NameCode = sym.ToDisplayString(s_nameFormat) + ".SignalName." + sig, OwnerType = tt } };
                // A Godot snake_case accessor the C# API names as a property is handled by the call path; a plain miss on an
                // untyped (weak) receiver is a dynamic property read.
                if (SubtypeWith(tt, name, false) is { } sub) return MemberOf(AsScriptType(target, sub), name, at, expected);
                if (tt.Weak || tt.FromScene || sym == null || sym.Name is "Node" or "GodotObject") return DynamicMember(target, name, at);
                Note(at, "member", name, Rating.Unsupported, $"Aver.Fabric has no member '{name}' on {tt.Cs}");
                return new Ex(tcode + "." + Naming.Pascal(name), GdType.Variant);
            }
            case GK.Dict:
                // dict.key is dict["key"]
                return new Ex($"{tcode}[{Naming.Quote(name)}]", tt.Value ?? GdType.Variant, P.Primary) { Addressable = true };
            case GK.Variant:
                return DynamicMember(target, name, at);
            case GK.Callable or GK.Signal or GK.Array or GK.String or GK.Packed or GK.Int or GK.Float or GK.Bool or GK.StringName or GK.NodePath:
            default:
                Note(at, "member", name, Rating.Unsupported, $"'{name}' is not a property of {tt.Cs}");
                return new Ex(tcode + "." + Naming.Pascal(name), GdType.Variant);
        }
    }

    private Ex DynamicMember(Ex target, string name, Node at)
    {
        string q = Naming.Quote(name);
        if (target.Type.IsVariant)
        {
            Note(at, "dynamic", name, Rating.Approximated, $"'{name}' is read by name at run time: the type of the receiver is not known");
            return new Ex($"GDScript.GetMember({target.Code}, {q})", GdType.Variant) { SetTemplate = $"GDScript.SetMember({target.Code}, {q}, \u0001)", IsDynamic = true };
        }
        Note(at, "dynamic", name, Rating.Approximated, $"'{name}' is read by name at run time: {target.Type.Cs} has no such member");
        string recv = Wrap(target, P.Primary);
        return new Ex($"{recv}.Get({q})", GdType.Variant) { SetTemplate = $"{recv}.Set({q}, \u0001)", IsDynamic = true };
    }

    private Ex StaticMember(Ex target, string name, Node at)
    {
        GdType tt = target.Type;
        switch (tt.Kind)
        {
            case GK.ScriptClass:
            {
                MemberInfo? mi = tt.Script!.Find(name);
                if (mi != null) return MemberValue(mi, target.Code, mi.Owner);
                break;
            }
            case GK.Enum when tt.EnumDef != null:
            {
                if (tt.EnumDef.Values.TryGetValue(name, out string? cs))
                    return new Ex(target.Code + "." + cs, tt.AsClassRefOff()) { IsConstExpr = true };
                break;
            }
        }
        Rs.INamedTypeSymbol? sym = tt.Kind == GK.ScriptClass ? tt.Script!.ApiRoot : tt.Sym;
        if (Api != null && sym != null)
        {
            if (sym.TypeKind == Rs.TypeKind.Enum)
            {
                Rs.IFieldSymbol? f = sym.GetMembers().OfType<Rs.IFieldSymbol>().FirstOrDefault(x => x.Name == Naming.Pascal(name));
                if (f != null) return new Ex(target.Code + "." + f.Name, GdType.Obj(sym)) { IsConstExpr = true };
            }
            ApiMember? api = Api.Lookup(sym, name);
            if (api != null)
            {
                if (api.Kind == MemberKind.NestedType) return TypeRef(api.Type, sym.ToDisplayString(s_nameFormat) + "." + api.CsName);
                return ApiValue(api, target.Code, sym, null);
            }
            if (Api.FindEnumConstant(sym, name, out Rs.INamedTypeSymbol? en, out string member))
                return new Ex(en!.ToDisplayString(s_nameFormat) + "." + member, GdType.Obj(en)) { IsConstExpr = true };
            // Color.RED is Colors.Red in the C# runtime.
            if (sym.Name == "Color" && Api.Fabric("Colors") is { } colors)
            {
                ApiMember? c = Api.Lookup(colors, name);
                if (c != null) return new Ex("Colors." + c.CsName, GdType.Obj(sym)) { Api = c };
            }
            if (Api.IsSignal(sym, name, out string sig))
            {
                string cls = sym.ToDisplayString(s_nameFormat);
                // A singleton that is a static class (RenderingServer) owns its signals through its Singleton object: the name is the signal itself.
                if (sym.IsStatic && Api.Lookup(sym, "singleton") != null)
                    return new Ex($"new Signal({cls}.Singleton, {cls}.SignalName.{sig})", GdType.Signal)
                    { Signal = new SignalRef { Owner = cls + ".Singleton", NameCode = cls + ".SignalName." + sig, OwnerType = tt } };
                return new Ex($"{cls}.SignalName.{sig}", GdType.StringName);
            }
        }
        Note(at, "member", name, Rating.Unsupported, $"Aver.Fabric has no member '{name}' on {tt.Cs}");
        return new Ex(target.Code + "." + Naming.Pascal(name), GdType.Variant);
    }

    // ---- subscript ----

    private Ex SubscriptEx(Subscript s, GdType? expected)
    {
        Ex t = Expr(s.Target, null);
        Ex i = Expr(s.Index, null);
        string tc = Wrap(t, P.Primary);
        switch (t.Type.Kind)
        {
            case GK.Array:
            case GK.Packed:
            {
                GdType elem = t.Type.Elem ?? GdType.Variant;
                string idx = i.Const is long l && l < 0 ? $"^{-l}" : Adapt(i, GdType.Int32);
                if (i.Type.IsVariant) idx = i.Code + ".AsInt32()";
                return new Ex($"{tc}[{idx}]", elem) { Addressable = true };
            }
            case GK.Dict:
            {
                GdType kt = t.Type.Typed && t.Type.Key != null ? t.Type.Key : GdType.Variant;
                return new Ex($"{tc}[{Adapt(i, kt)}]", t.Type.Value ?? GdType.Variant) { Addressable = true };
            }
            case GK.String:
                return new Ex($"{tc}[{Adapt(i, GdType.Int32)}].ToString()", GdType.String);
            case GK.Struct when Api?.IndexerType(t.Type.Sym) is { } element:
                return new Ex($"{tc}[{Adapt(i, GdType.Int32)}]", element) { Addressable = false };
            case GK.Variant:
            default:
                return new Ex($"GDScript.Index({VariantFromTyped(t)}, {VariantFromTyped(i)})", GdType.Variant)
                { SetTemplate = $"GDScript.SetIndex({VariantFromTyped(t)}, {VariantFromTyped(i)}, \u0001)" };
        }
    }

    // ---- node paths, collections, preload ----

    private Ex GetNodeEx(GetNodeExpr g, GdType? expected)
    {
        _ctx.UsedInstance = true;
        string path = Naming.Quote(g.Path);
        if (expected != null && expected.IsGodotObject && expected.Kind != GK.Null && !expected.Weak && !(expected.Sym?.Name == "GodotObject"))
            return new Ex($"GetNode<{expected.Cs}>({path})", expected.WithWeak(false)) { UsesInstance = true };
        // No type was asked for: the scenes this script is attached to say what is there.
        if (ScriptIsNode() && SceneNodeEx(g.Path, false, SelfPlaces(), "") is { } typed) return typed;
        if (CastPathEx(g.Path, false) is { } cast) return cast;
        return new Ex($"GetNode({path})", Obj("Node").WithWeak(true)) { UsesInstance = true };
    }

    private Ex ArrayEx(ArrayLit a, GdType? expected)
    {
        // A packed array (PackedStringArray, PackedVector3Array ...) is a C# array.
        if (expected is { Kind: GK.Packed } pk)
        {
            GdType pe = pk.Elem ?? GdType.Variant;
            var parts = a.Items.Select(x => Adapt(Expr(x, pe.IsVariant ? null : pe), pe)).ToList();
            string arrType = pk.Name.EndsWith("[]", StringComparison.Ordinal) ? pk.Name : pk.Name + "[]";
            return new Ex(parts.Count == 0 ? $"new {arrType.Replace("[]", "[0]")}" : $"new {arrType} {{ {string.Join(", ", parts)} }}", pk);
        }
        GdType t = expected is { Kind: GK.Array } ? expected : GdType.UntypedArray;
        GdType elem = t.Typed && t.Elem != null ? t.Elem : GdType.Variant;
        if (a.Items.Count == 0) return new Ex($"new {t.Cs}()", t);
        var items = a.Items.Select(x => Adapt(Expr(x, elem.IsVariant ? null : elem), elem)).ToList();
        string body = string.Join(", ", items);
        string code = items.Sum(x => x.Length) > 90 || items.Any(x => x.Contains('\n'))
            ? $"new {t.Cs}\n{{\n    {string.Join(",\n    ", items.Select(x => x.Replace("\n", "\n    ")))}\n}}"
            : $"new {t.Cs} {{ {body} }}";
        return new Ex(code, t);
    }

    private Ex DictEx(DictLit d, GdType? expected)
    {
        GdType t = expected is { Kind: GK.Dict } ? expected : GdType.UntypedDict;
        GdType kt = t.Typed && t.Key != null ? t.Key : GdType.Variant;
        GdType vt = t.Typed && t.Value != null ? t.Value : GdType.Variant;
        if (d.Entries.Count == 0) return new Ex($"new {t.Cs}()", t);
        var entries = d.Entries.Select(e =>
        {
            string k = Adapt(Expr(e.Key, kt.IsVariant ? null : kt), kt);
            string v = Adapt(Expr(e.Value, vt.IsVariant ? null : vt), vt);
            return $"{{ {k}, {v} }}";
        }).ToList();
        string code = entries.Sum(x => x.Length) > 80 || entries.Any(x => x.Contains('\n'))
            ? $"new {t.Cs}\n{{\n    {string.Join(",\n    ", entries.Select(x => x.Replace("\n", "\n    ")))}\n}}"
            : $"new {t.Cs} {{ {string.Join(", ", entries)} }}";
        return new Ex(code, t);
    }

    private static string ResourceClassFor(string path)
    {
        string ext = Path.GetExtension(path).ToLowerInvariant();
        return ext switch
        {
            ".tscn" or ".scn" => "PackedScene",
            ".png" or ".jpg" or ".jpeg" or ".webp" or ".svg" or ".bmp" or ".tga" or ".exr" or ".hdr" => "Texture2D",
            ".ogg" or ".wav" or ".mp3" => "AudioStream",
            ".mesh" or ".ocmesh" or ".obj" => "Mesh",
            ".material" or ".ocmat" => "Material",
            ".gdshader" or ".shader" => "Shader",
            ".ttf" or ".otf" or ".woff" or ".woff2" => "Font",
            _ => "Resource",
        };
    }

    private Ex PreloadEx(PreloadExpr p)
    {
        if (p.Path is Literal { Kind: LitKind.String } lit)
        {
            string path = (string)lit.Value!;
            if (path.EndsWith(".gd", StringComparison.Ordinal) && _m.ByPath.TryGetValue(path, out ScriptInfo? s))
                return TypeRef(GdType.OfScript(s), QualifiedScript(s));
            GdType t = Obj(ResourceClassFor(path));
            return new Ex($"GD.Preload<{t.Cs}>({Naming.Quote(path)})", t) { SceneRes = path.EndsWith(".tscn", StringComparison.Ordinal) ? path : null };
        }
        Ex pe = Expr(p.Path, GdType.String);
        return new Ex($"GD.Preload<Resource>({Adapt(pe, GdType.String)})", Obj("Resource"));
    }

    // ---- type tests and casts ----

    private Ex TypeTestEx(TypeTest tt)
    {
        Ex v = Expr(tt.Value, null);
        GdType t = ResolveType(tt.Type);
        if (v.Type.IsVariant)
        {
            if (t.Kind is GK.Object or GK.ScriptClass) return new Ex($"{Wrap(v, P.Primary)}.AsGodotObject() is {t.Cs}", GdType.Bool, P.Rel);
            string vt = t.Kind switch
            {
                GK.Int => "Int", GK.Float => "Float", GK.Bool => "Bool", GK.String => "String", GK.StringName => "StringName", GK.NodePath => "NodePath",
                GK.Array => "Array", GK.Dict => "Dictionary", GK.Callable => "Callable", GK.Signal => "Signal",
                GK.Struct => t.Sym?.Name ?? "Nil", _ => "Nil",
            };
            return new Ex($"{Wrap(v, P.Primary)}.VariantType == Variant.Type.{vt}", GdType.Bool, P.Eq);
        }
        return new Ex($"{Wrap(v, P.Rel + 1)} is {t.Cs}", GdType.Bool, P.Rel);
    }

    private Ex CastEx(Cast c)
    {
        GdType t = ResolveType(c.Type);
        Ex v = Expr(c.Value, t);
        if (t.IsGodotObject)
        {
            if (v.Type.IsVariant) return new Ex($"{Wrap(v, P.Primary)}.As<{t.Cs}>()", t);
            if (v.Type.IsGodotObject && t.Accepts(v.Type)) return new Ex(v.Code, t) { Prec = v.Prec };
            if (v.Type.Kind == GK.Null) return new Ex(v.Code, t);
            return new Ex($"{Wrap(v, P.Rel + 1)} as {t.Cs}", t, P.Rel);
        }
        return new Ex(Adapt(v, t), t, v.Type.Cs == t.Cs || v.Type.IsVariant ? (v.Type.IsVariant ? P.Primary : v.Prec) : P.Unary);
    }

    // ---- ternary, unary ----

    private Ex TernaryEx(Ternary t, GdType? expected)
    {
        string cond = Truth(Expr(t.Cond, GdType.Bool));
        Ex a = Expr(t.Then, expected);
        Ex b = Expr(t.Else, expected);
        GdType rt;
        if (a.Type.Kind == GK.Null && b.Type.Kind != GK.Null) rt = b.Type.IsReference || b.Type.IsVariant ? b.Type : GdType.Variant;
        else if (b.Type.Kind == GK.Null && a.Type.Kind != GK.Null) rt = a.Type.IsReference || a.Type.IsVariant ? a.Type : GdType.Variant;
        else if (a.Type.Kind == GK.Null && b.Type.Kind == GK.Null) rt = GdType.Variant;
        else rt = JoinTypes(a.Type, b.Type);
        if (rt.Kind is GK.Lambda) rt = GdType.Callable;
        string ac = a.Type.Kind == GK.Null ? "null" : Adapt(a, rt);
        string bc = b.Type.Kind == GK.Null ? "null" : Adapt(b, rt);
        if (rt.IsVariant)
        {
            if (a.Type.Kind == GK.Null) ac = "default(Variant)";
            if (b.Type.Kind == GK.Null) bc = "default(Variant)";
        }
        return new Ex($"{Wrap(new Ex(cond, GdType.Bool, cond.Contains(' ') ? P.Eq : P.Primary), P.OrOr)} ? {WrapCond(ac)} : {WrapCond(bc)}", rt, P.Cond);
    }

    private static string WrapCond(string code) => code.Contains(" ? ") && !code.StartsWith('(') ? "(" + code + ")" : code;

    private Ex UnaryEx(Unary u, GdType? expected)
    {
        if (u.Op == "not")
        {
            string cond = Truth(Expr(u.Operand, GdType.Bool));
            bool simple = cond.All(c => char.IsLetterOrDigit(c) || c == '_' || c == '.' || c == '@') || cond.EndsWith(")") && !cond.Contains(' ') && !cond.Contains("&&") && !cond.Contains("||");
            return new Ex("!" + (simple ? cond : "(" + cond + ")"), GdType.Bool, P.Unary);
        }
        Ex v = Expr(u.Operand, expected);
        switch (u.Op)
        {
            case "-" when v.Type.IsNumeric:
                if (v.Const is long l) return new Ex((-l).ToString(CultureInfo.InvariantCulture), v.Type, P.Unary) { Const = -l, IsConstExpr = true };
                if (v.Const is double d) return new Ex(Naming.Num(-d), v.Type, P.Unary) { Const = -d, IsConstExpr = true };
                return new Ex("-" + Wrap(v, P.Unary), v.Type, P.Unary) { IsConstExpr = v.IsConstExpr };
            case "-" when v.Type.Kind == GK.Struct:
                return new Ex("-" + Wrap(v, P.Unary), v.Type, P.Unary);
            case "-" when v.Type.Kind == GK.Enum:
                return new Ex("-(long)" + Wrap(v, P.Unary), GdType.Int, P.Unary);
            case "+" when v.Type.IsNumeric || v.Type.Kind == GK.Struct:
                return v;
            case "~" when v.Type.Kind == GK.Int:
                return new Ex("~" + Wrap(v, P.Unary), v.Type, P.Unary) { IsConstExpr = v.IsConstExpr };
            case "-": return new Ex($"GDScript.Neg({VariantFromTyped(v)})", GdType.Variant);
            case "+": return new Ex($"GDScript.Plus({VariantFromTyped(v)})", GdType.Variant);
            default: return new Ex($"GDScript.BitNot({VariantFromTyped(v)})", GdType.Variant);
        }
    }

    // ---- binary ----

    private Ex BinaryEx(Binary b, GdType? expected)
    {
        if (b.Op == "and" || b.Op == "or")
        {
            string l = CondCode(b.Left, narrow: false);
            string r = CondCode(b.Right, narrow: false);
            string op = b.Op == "and" ? "&&" : "||";
            int prec = b.Op == "and" ? P.AndAnd : P.OrOr;
            // && inside || reads without parentheses; || inside && needs them.
            if (b.Op == "and" && b.Left is Binary { Op: "or" }) l = "(" + l + ")";
            if (b.Op == "and" && b.Right is Binary { Op: "or" }) r = "(" + r + ")";
            if (b.Left is Ternary) l = "(" + l + ")";
            if (b.Right is Ternary) r = "(" + r + ")";
            return new Ex($"{l} {op} {r}", GdType.Bool, prec);
        }
        // The operand types decide what the other side is expected to be (so `x * 2` leaves the literal an int and `v * 2.0` a float).
        Ex left = Expr(b.Left, null);
        GdType? hint = left.Type.IsNumeric || left.Type.Kind == GK.Struct ? null : left.Type.Kind == GK.Enum ? null : left.Type;
        Ex right = Expr(b.Right, b.Op is "in" or "not in" ? null : hint);
        return BinaryOp(b.Op, left, right, b);
    }

    private static Num NumResult(GdType a, GdType b)
    {
        if (a.Number == Num.Double && a.Kind == GK.Float || b.Number == Num.Double && b.Kind == GK.Float) return Num.Double;
        if (a.Kind == GK.Float || b.Kind == GK.Float)
        {
            // float with a long becomes float in C#; with an int, float.
            if ((a.Kind == GK.Float && a.Number == Num.Float) || (b.Kind == GK.Float && b.Number == Num.Float)) return Num.Float;
            return Num.Double;
        }
        return a.Number == Num.Long || b.Number == Num.Long ? Num.Long : Num.Int;
    }

    private static GdType NumType(Num n, bool isFloat) =>
        isFloat ? (n == Num.Float ? GdType.Float32 : GdType.Float) : (n == Num.Int ? GdType.Int32 : GdType.Int);

    private static readonly Dictionary<string, (string Name, int Prec)> s_binaryOps = new()
    {
        ["+"] = ("op_Addition", P.Add), ["-"] = ("op_Subtraction", P.Add), ["*"] = ("op_Multiply", P.Mul), ["/"] = ("op_Division", P.Mul),
        ["%"] = ("op_Modulus", P.Mul), ["=="] = ("op_Equality", P.Eq), ["!="] = ("op_Inequality", P.Eq), ["<"] = ("op_LessThan", P.Rel),
        [">"] = ("op_GreaterThan", P.Rel), ["<="] = ("op_LessThanOrEqual", P.Rel), [">="] = ("op_GreaterThanOrEqual", P.Rel),
        ["&"] = ("op_BitwiseAnd", P.BitAnd), ["|"] = ("op_BitwiseOr", P.BitOr), ["^"] = ("op_ExclusiveOr", P.BitXor),
        ["<<"] = ("op_LeftShift", P.Shift), [">>"] = ("op_RightShift", P.Shift),
    };

    private static bool IsComparison(string op) => op is "==" or "!=" or "<" or ">" or "<=" or ">=";

    /// <summary>One binary operator on two typed operands, by GDScript's rules.</summary>
    private Ex BinaryOp(string op, Ex l, Ex r, Node at)
    {
        // An enum is an int to GDScript.
        if (l.Type.Kind == GK.Enum && !(r.Type.Kind == GK.Enum && l.Type.Cs == r.Type.Cs && IsComparison(op)) && op != "in" && op != "not in")
            l = new Ex("(long)" + Wrap(l, P.Unary), GdType.Int, P.Unary);
        if (r.Type.Kind == GK.Enum && !(l.Type.Kind == GK.Enum && IsComparison(op)) && op != "in" && op != "not in" && l.Type.Kind != GK.Enum)
            r = new Ex("(long)" + Wrap(r, P.Unary), GdType.Int, P.Unary);

        if (op == "in") return InOp(l, r, negate: false);
        if (op == "not in") return InOp(l, r, negate: true);
        if (op == "**") return PowOp(l, r);

        GdType lt = l.Type, rt = r.Type;
        int prec = s_binaryOps.TryGetValue(op, out var info) ? info.Prec : P.Add;

        // Numbers.
        if (lt.IsNumeric && rt.IsNumeric)
        {
            bool isFloat = lt.Kind == GK.Float || rt.Kind == GK.Float;
            if (IsComparison(op))
                return new Ex($"{Wrap(l, prec)} {op} {WrapRight(r, prec)}", GdType.Bool, prec);
            if (op is "<<" or ">>")
            {
                // 1 << 40 is a 64-bit shift in GDScript; the literal must be a long for C# to agree.
                string left = l.Const is long ? l.Code + "L" : Wrap(l, prec);
                var sh = new Ex($"{left} {op} {Adapt(r, GdType.Int32)}", NumType(lt.Number, false), prec) { IsConstExpr = l.IsConstExpr && r.IsConstExpr };
                if (l.Const is long sl && r.Const is long sr && sr is >= 0 and < 64) sh.Const = op == "<<" ? sl << (int)sr : sl >> (int)sr;
                return sh;
            }
            if (op is "&" or "|" or "^")
                return new Ex($"{Wrap(l, prec)} {op} {WrapRight(r, prec)}", NumType(NumResult(lt, rt), false), prec) { IsConstExpr = l.IsConstExpr && r.IsConstExpr };
            Num n = NumResult(lt, rt);
            Ex res = new Ex($"{Wrap(l, prec)} {op} {WrapRight(r, prec)}", NumType(n, isFloat), prec) { IsConstExpr = l.IsConstExpr && r.IsConstExpr };
            if (l.Const is long a && r.Const is long b2 && !isFloat)
            {
                long? v = op switch { "+" => a + b2, "-" => a - b2, "*" => a * b2, "/" when b2 != 0 => a / b2, "%" when b2 != 0 => a % b2, _ => null };
                if (v.HasValue)
                {
                    res.Const = v.Value;
                    // Two int literals whose result does not fit C#'s int: write the 64-bit result (C# would wrap or refuse).
                    if (v.Value < int.MinValue || v.Value > int.MaxValue) { res.Code = v.Value.ToString(CultureInfo.InvariantCulture) + "L"; res.Prec = v.Value < 0 ? P.Unary : P.Primary; }
                }
            }
            return res;
        }

        // Strings.
        if (lt.Kind is GK.String or GK.StringName or GK.NodePath && rt.Kind is GK.String or GK.StringName or GK.NodePath)
        {
            if (op == "+")
                return new Ex($"{StringOperand(l)} + {StringOperand(r, P.Add + 1)}", GdType.String, P.Add) { IsConstExpr = l.IsConstExpr && r.IsConstExpr };
            if (op is "==" or "!=")
                return new Ex($"{StringOperand(l, P.Eq + 1)} {op} {StringOperand(r, P.Eq + 1)}", GdType.Bool, P.Eq);
            if (IsComparison(op))
                return new Ex($"string.CompareOrdinal({StringOperand(l, P.Primary)}, {StringOperand(r, P.Primary)}) {op} 0", GdType.Bool, P.Rel);
        }
        if (op == "%" && lt.Kind is GK.String or GK.StringName)
        {
            string arg = r.Type.Kind == GK.Array && r.Type.Cs == "Collections.Array" ? r.Code : VariantFromTyped(r);
            return new Ex($"GDScript.Format({StringOperand(l, P.Primary)}, {arg})", GdType.String);
        }

        // Booleans.
        if (lt.Kind == GK.Bool && rt.Kind == GK.Bool && op is "==" or "!=" or "&" or "|" or "^")
            return new Ex($"{Wrap(l, prec)} {op} {WrapRight(r, prec)}", GdType.Bool, prec);

        // Equality of everything else: references, null, structs of one type, enums of one type, callables.
        if (op is "==" or "!=")
        {
            if (lt.Kind == GK.Null || rt.Kind == GK.Null)
            {
                Ex other = lt.Kind == GK.Null ? r : l;
                if (other.Type.IsVariant) return new Ex($"{Wrap(other, P.Eq + 1)}.IsNil".Replace("..", "."), GdType.Bool, P.Primary) { Code = (op == "==" ? "" : "!") + Wrap(other, P.Primary) + ".IsNil" };
                if (other.Type.IsReference || other.Type.Kind == GK.Callable) return new Ex($"{Wrap(l, P.Eq + 1)} {op} {Wrap(r, P.Eq + 1)}", GdType.Bool, P.Eq);
                return new Ex(op == "==" ? "false" : "true", GdType.Bool);
            }
            if (!lt.IsVariant && !rt.IsVariant && lt.Kind == rt.Kind || lt.IsGodotObject && rt.IsGodotObject)
                return new Ex($"{Wrap(l, P.Eq + 1)} {op} {Wrap(r, P.Eq + 1)}", GdType.Bool, P.Eq);
            if (lt.IsVariant || rt.IsVariant)
                return new Ex($"{Wrap(l, P.Eq + 1)} {op} {Wrap(r, P.Eq + 1)}", GdType.Bool, P.Eq);
            return new Ex($"{VariantFromTyped(l)} {op} {VariantFromTyped(r)}", GdType.Bool, P.Eq);
        }

        // Math types: the operator the C# type declares.
        if ((lt.Kind == GK.Struct || rt.Kind == GK.Struct) && s_binaryOps.TryGetValue(op, out var opi) && Api != null)
        {
            Rs.IMethodSymbol? best = null;
            int bestScore = -1;
            foreach (Rs.IMethodSymbol m in Api.Operators(opi.Name, lt.Sym, rt.Sym))
            {
                if (m.Parameters.Length != 2) continue;
                int s1 = ScoreArg(lt, Api.FromSymbol(m.Parameters[0].Type)), s2 = ScoreArg(rt, Api.FromSymbol(m.Parameters[1].Type));
                if (s1 < 0 || s2 < 0) continue;
                if (s1 + s2 > bestScore) { best = m; bestScore = s1 + s2; }
            }
            if (best != null)
            {
                GdType p0 = Api.FromSymbol(best.Parameters[0].Type), p1 = Api.FromSymbol(best.Parameters[1].Type);
                GdType result = Api.FromSymbol(best.ReturnType);
                string lc = Adapt(l, p0), rc = Adapt(r, p1);
                string lcode = lc == l.Code ? Wrap(l, prec) : (lc.StartsWith('(') && !lc.StartsWith("((") && lc.IndexOf(')') < lc.Length - 1 ? lc : Wrap(new Ex(lc, p0, l.Prec), prec));
                string rcode = rc == r.Code ? WrapRight(r, prec) : WrapRight(new Ex(rc, p1, rc == r.Code ? r.Prec : P.Unary), prec);
                return new Ex($"{lcode} {op} {rcode}", result, prec);
            }
        }

        // Anything else: the Variant rules, at run time.
        string fn = op switch
        {
            "+" => "Add", "-" => "Sub", "*" => "Mul", "/" => "Div", "%" => "Mod", "&" => "BitAnd", "|" => "BitOr", "^" => "BitXor",
            "<<" => "Shl", ">>" => "Shr", "<" => "Lt", "<=" => "Le", ">" => "Gt", ">=" => "Ge", _ => "",
        };
        if (fn.Length == 0) return new Ex($"{Wrap(l, prec)} {op} {WrapRight(r, prec)}", GdType.Variant, prec);
        bool boolResult = IsComparison(op);
        if (!lt.IsVariant && !rt.IsVariant && !boolResult)
            Note(at, "variant", op, Rating.Approximated, $"'{op}' on {lt.Cs} and {rt.Cs} is evaluated by GDScript's Variant rules at run time");
        return new Ex($"GDScript.{fn}({VariantFromTyped(l)}, {VariantFromTyped(r)})", boolResult ? GdType.Bool : GdType.Variant);
    }

    private static string WrapRight(Ex e, int prec) => e.Prec > prec ? e.Code : "(" + e.Code + ")";

    private static string StringOperand(Ex e, int min = P.Add) =>
        e.Type.Kind == GK.String ? Wrap(e, min) : e.Type.Kind == GK.NodePath ? "(string)" + Wrap(e, P.Unary) : "(string)" + Wrap(e, P.Unary);

    private int ScoreArg(GdType from, GdType to)
    {
        if (to.IsVariant) return from.IsVariant ? 3 : 1;
        if (from.IsVariant) return 1;
        if (from.Cs == to.Cs) return 4;
        if (to.Accepts(from)) return 3;
        if (from.IsNumeric && to.IsNumeric) return 2;
        if (from.Kind == GK.Enum && to.IsNumeric) return 1;
        return -1;
    }

    private Ex PowOp(Ex l, Ex r)
    {
        if (l.Type.Kind == GK.Int && r.Type.Kind == GK.Int)
            return new Ex($"GDScript.IntPow({Adapt(l, GdType.Int)}, {Adapt(r, GdType.Int)})", GdType.Int);
        if (l.Type.IsNumeric && r.Type.IsNumeric)
            return new Ex($"Math.Pow({Adapt(l, GdType.Float)}, {Adapt(r, GdType.Float)})", GdType.Float);
        return new Ex($"GDScript.Pow({VariantFromTyped(l)}, {VariantFromTyped(r)})", GdType.Variant);
    }

    private Ex InOp(Ex item, Ex container, bool negate)
    {
        string not = negate ? "!" : "";
        GdType ct = container.Type;
        string c = Wrap(container, P.Primary);
        switch (ct.Kind)
        {
            case GK.Array when ct.Typed && ct.Elem != null:
                return new Ex($"{not}{c}.Contains({Adapt(item, ct.Elem)})", GdType.Bool, negate ? P.Unary : P.Primary);
            case GK.Array:
                return new Ex($"{not}{c}.Contains({VariantFromTyped(item)})", GdType.Bool, negate ? P.Unary : P.Primary);
            case GK.Dict when ct.Typed && ct.Key != null:
                return new Ex($"{not}{c}.ContainsKey({Adapt(item, ct.Key)})", GdType.Bool, negate ? P.Unary : P.Primary);
            case GK.Dict:
                return new Ex($"{not}{c}.ContainsKey({VariantFromTyped(item)})", GdType.Bool, negate ? P.Unary : P.Primary);
            case GK.String when item.Type.Kind is GK.String or GK.StringName:
                return new Ex($"{not}{c}.Contains({StringOperand(item, P.Primary)})", GdType.Bool, negate ? P.Unary : P.Primary);
            case GK.Packed:
                return new Ex($"{not}{c}.Contains({Adapt(item, ct.Elem)})", GdType.Bool, negate ? P.Unary : P.Primary);
        }
        return new Ex($"{not}GDScript.In({VariantFromTyped(item)}, {VariantFromTyped(container)})", GdType.Bool, negate ? P.Unary : P.Primary);
    }
}
