// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Functions: parameter and return types (declared, inherited from an override, or inferred), local-name uniqueness, signatures, bodies.

using Rs = Microsoft.CodeAnalysis;

namespace Aver.Fabric.Convert.GdScript;

internal sealed partial class GdEmitter
{
    private MemberInfo? AccessorOwner(MemberInfo fn) =>
        fn.Owner.Members.FirstOrDefault(v => v.Kind == MK.Var && v.Var is { } vd && (vd.Setter?.Delegate == fn.Name || vd.Getter?.Delegate == fn.Name));

    // ---- local names ----

    // Names of the members a function's locals must not take (C# would let the local hide a field the same function still reads).
    private static IReadOnlyCollection<string> MemberCsNames(ScriptInfo s)
    {
        var names = new HashSet<string>(StringComparer.Ordinal) { s.CsName };
        for (ScriptInfo? o = s; o != null; o = o.Outer)
            foreach (ScriptInfo c in o.Chain())
                foreach (MemberInfo m in c.Members)
                {
                    names.Add(m.CsName);
                    if (m.Backing.Length > 0) names.Add(m.Backing);
                }
        return names;
    }

    /// <summary>One C# name per declaration in a function: a name used again keeps itself only where the two scopes cannot see each other
    /// (C# forbids a nested declaration that shares a name with one in an enclosing scope, even a later one).</summary>
    private Dictionary<Node, string> ComputeNames(IEnumerable<Param> ps, Block body, IEnumerable<string> reserved)
    {
        var decls = new List<(Node Node, string Base, Block Scope)>();
        var parent = new Dictionary<Block, Block?>();

        void Walk(Node n, Block scope)
        {
            switch (n)
            {
                case Block b:
                    parent[b] = scope == b ? null : scope;
                    foreach (Node c in b.Children()) Walk(c, b);
                    return;
                case VarDecl v:
                    decls.Add((v, v.Name, scope));
                    break;
                case ConstDecl cd:
                    decls.Add((cd, cd.Name, scope));
                    break;
                case ForStmt f:
                    decls.Add((f, f.VarName, f.Body));
                    break;
                case MatchBranch mb:
                    foreach (Pattern p in mb.Patterns) CollectBinds(p, mb.Body, decls);
                    break;
                case LambdaExpr l:
                    foreach (Param p in l.Params) decls.Add((p, p.Name, l.Body));
                    break;
            }
            foreach (Node c in n.Children()) Walk(c, scope);
        }

        foreach (Param p in ps) decls.Add((p, p.Name, body));
        parent[body] = null;
        foreach (Node c in body.Children()) Walk(c, body);

        bool Related(Block a, Block b)
        {
            for (Block? x = a; x != null; x = parent.GetValueOrDefault(x)) if (ReferenceEquals(x, b)) return true;
            for (Block? x = b; x != null; x = parent.GetValueOrDefault(x)) if (ReferenceEquals(x, a)) return true;
            return false;
        }

        var result = new Dictionary<Node, string>();
        var taken = new List<(string Name, Block Scope)>();
        foreach (string r in reserved) taken.Add((r, body));
        foreach (var d in decls)
        {
            string name = Naming.Camel(d.Base.TrimStart('_').Length == 0 ? d.Base : d.Base);
            if (d.Base.StartsWith('_') && d.Base.TrimStart('_').Length > 0) name = Naming.Camel(d.Base.TrimStart('_'));
            string candidate = name;
            for (int i = 2; taken.Any(t => t.Name == candidate && Related(t.Scope, d.Scope)); i++) candidate = name + i;
            result[d.Node] = candidate;
            taken.Add((candidate, d.Scope));
        }
        return result;
    }

    private static void CollectBinds(Pattern p, Block scope, List<(Node, string, Block)> into)
    {
        switch (p)
        {
            case PatBind b: into.Add((b, b.Name, scope)); break;
            case PatArray a: foreach (Pattern i in a.Items) CollectBinds(i, scope, into); break;
            case PatDict d: foreach (PatDictEntry e in d.Entries) if (e.Value != null) CollectBinds(e.Value, scope, into); break;
        }
    }

    private string LocalName(Node decl, string gd)
    {
        if (_ctx.DeclNames.TryGetValue(decl, out string? n)) return Naming.Safe(n);
        return Fresh(Naming.Camel(gd));
    }

    // ---- parameter types ----

    private List<GdType> ParamTypes(MemberInfo mi)
    {
        FuncDecl fd = mi.Func!;
        if (mi.ParamTypes.Count == fd.Params.Count && mi.ParamTypes.All(t => t != null)) return mi.ParamTypes.Select(t => t!).ToList();
        var result = new List<GdType>();
        // An override takes the signature of what it overrides, so C# accepts it.
        List<GdType>? inherited = null;
        if (mi.IsOverride)
        {
            if (mi.ApiBase != null) inherited = mi.ApiBase.Parameters.Select(p => Api!.FromSymbol(p.Type)).ToList();
            else
            {
                MemberInfo? up = mi.Owner.BaseScript?.Find(mi.Name);
                if (up != null && up.Kind == MK.Func) inherited = ParamTypes(up);
            }
        }
        var saved = (_script, _ctx, _w);
        try
        {
            EnterMemberContext(mi.Owner, isStatic: true);
            for (int i = 0; i < fd.Params.Count; i++)
            {
                Param p = fd.Params[i];
                if (p.IsRest) { result.Add(GdType.Variant); continue; }
                if (inherited != null && i < inherited.Count) { result.Add(inherited[i]); continue; }
                if (p.Type != null) result.Add(ResolveType(p.Type, mi.Owner));
                else if (p.Default != null)
                {
                    GdType t = CleanInferred(TypeOf(p.Default));
                    result.Add(p.Inferred ? t : DefaultLiteralType(p.Default, t));
                }
                else result.Add(GdType.Variant);
            }
        }
        finally { (_script, _ctx, _w) = saved; }
        mi.ParamTypes = result.Select(t => (GdType?)t).ToList();
        return result;
    }

    // An untyped parameter with a plain default (`count = 1`) takes the default's type; a null default or a computed one stays a Variant.
    private static GdType DefaultLiteralType(Expr d, GdType t) =>
        LiteralType(d) is { } lt && lt.Kind is GK.Int or GK.Float or GK.Bool or GK.String ? (lt.Kind == GK.Int ? GdType.Int : lt) : GdType.Variant;

    // ---- return types ----

    private GdType ReturnTypeOf(MemberInfo mi)
    {
        if (mi.Type != null) return mi.Type;
        FuncDecl fd = mi.Func!;
        // An override of a runtime virtual returns what the virtual returns (a `-> float` over a C# float virtual is a float, not a double).
        if (mi.IsOverride && mi.ApiBase != null && fd.ReturnType != null) return mi.Type = Api!.FromSymbol(mi.ApiBase.ReturnType);
        if (fd.ReturnType != null) return mi.Type = ResolveType(fd.ReturnType, mi.Owner);
        if (mi.IsOverride)
        {
            if (mi.ApiBase != null) return mi.Type = Api!.FromSymbol(mi.ApiBase.ReturnType);
            MemberInfo? up = mi.Owner.BaseScript?.Find(mi.Name);
            if (up != null && up.Kind == MK.Func) return mi.Type = ReturnTypeOf(up);
        }
        if (mi.Resolving) return GdType.Variant;
        mi.Resolving = true;
        try { mi.Type = InferReturnType(mi); }
        finally { mi.Resolving = false; }
        return mi.Type!;
    }

    private GdType InferReturnType(MemberInfo mi)
    {
        FuncDecl fd = mi.Func!;
        var saved = (_script, _ctx, _w);
        _dry++;
        Ctx ctx;
        try
        {
            _script = mi.Owner;
            ctx = NewFuncCtx(mi, fd, GdType.Variant, infer: true, out _);
            _ctx = ctx;
            _w = new CodeWriter();
            EmitBlockBody(fd.Body);
        }
        finally { _dry--; (_script, _ctx, _w) = saved; }

        GdType? u = null;
        bool anyNull = false;
        foreach (GdType t in ctx.ReturnTypes)
        {
            if (t.Kind == GK.Null) { anyNull = true; continue; }
            if (t.Kind == GK.Void) continue;
            u = u == null ? t : JoinTypes(u, t);
        }
        if (u == null) return anyNull ? GdType.Variant : GdType.Void;
        bool falls = ctx.ReturnsWithoutValue || !AlwaysReturns(fd.Body);
        if (falls || anyNull)
        {
            bool nullable = u.IsReference && u.Kind != GK.String;
            if (!nullable) return GdType.Variant;
        }
        if (mi.Async && u.Kind == GK.Void) return GdType.Void;
        // A return type is GDScript's: int is 64-bit and float a double, whatever the API call it came from returned.
        if (u.Kind == GK.Int) return GdType.Int;
        if (u.Kind == GK.Float) return GdType.Float;
        return u.WithWeak(false);
    }

    private static GdType JoinTypes(GdType a, GdType b)
    {
        if (a.Cs == b.Cs) return a;
        if (a.IsNumeric && b.IsNumeric) return a.Kind == GK.Float || b.Kind == GK.Float ? GdType.Float : GdType.Int;
        if (a.IsGodotObject && b.IsGodotObject)
        {
            if (a.Accepts(b)) return a;
            if (b.Accepts(a)) return b;
        }
        return GdType.Variant;
    }

    private static bool AlwaysReturns(Block b)
    {
        if (b.Statements.Count == 0) return false;
        Stmt last = b.Statements[^1];
        return last switch
        {
            ReturnStmt => true,
            IfStmt i => i.Else != null && i.Branches.All(br => AlwaysReturns(br.Body)) && AlwaysReturns(i.Else),
            MatchStmt m => m.Branches.Count > 0 && m.Branches.Any(br => br.Patterns.Any(p => p is PatWildcard or PatBind) && br.Guard == null) && m.Branches.All(br => AlwaysReturns(br.Body)),
            WhileStmt w => w.Cond is Literal { Kind: LitKind.Bool, Value: true },
            _ => false,
        };
    }

    // ---- the context of one function ----

    private Ctx NewFuncCtx(MemberInfo mi, FuncDecl fd, GdType ret, bool infer, out List<ParamInfo> ps)
    {
        var ctx = new Ctx
        {
            Script = mi.Owner, Func = mi, IsStatic = mi.IsStatic, IsAsync = mi.Async, Return = ret, InferReturn = infer, BodyRoot = fd.Body,
            InAccessor = AccessorOwner(mi),
        };
        ctx.Scopes.Add(new Dictionary<string, Local>());
        IReadOnlyCollection<string> members = MemberCsNames(mi.Owner);
        ctx.DeclNames = ComputeNames(fd.Params, fd.Body, members);
        foreach (string n in ctx.DeclNames.Values) ctx.Used.Add(n);
        foreach (string n in members) ctx.Used.Add(n);

        List<GdType> types = ParamTypes(mi);
        ps = new List<ParamInfo>();
        for (int i = 0; i < fd.Params.Count; i++)
        {
            Param p = fd.Params[i];
            string cs = Naming.Safe(ctx.DeclNames[p]);
            var info = new ParamInfo { Decl = p, Gd = p.Name, Cs = cs, Type = types[i], Default = p.Default, IsRest = p.IsRest };
            ps.Add(info);
            if (p.IsRest)
            {
                // The C# params array is Variant[]; the body sees GDScript's Array.
                info.Cs = cs + "Args";
                ctx.Scopes[0][p.Name] = new Local { Gd = p.Name, Cs = cs, Type = GdType.UntypedArray, Decl = p };
            }
            else ctx.Scopes[0][p.Name] = new Local { Gd = p.Name, Cs = cs, Type = types[i], Decl = p };
        }
        return ctx;
    }

    // ---- emitting a function ----

    private void EmitSyntheticReady(ScriptInfo s, List<MemberInfo> onready)
    {
        _w.Open("public override void _Ready()");
        var saved = (_script, _ctx);
        _script = s;
        _ctx = new Ctx { Script = s, IsStatic = false, Return = GdType.Void, BodyRoot = new Block() };
        _ctx.Scopes.Add(new Dictionary<string, Local>());
        EmitOnready(onready);
        if (s.BaseScript != null && s.BaseScript.Chain().Any(b => b.ByName.ContainsKey("_ready"))) _w.Line("base._Ready();");
        (_script, _ctx) = saved;
        _w.Close();
    }

    private void EmitOnready(List<MemberInfo> onready)
    {
        foreach (MemberInfo m in onready)
        {
            var vd = (VarDecl)m.Decl;
            if (vd.Init == null) continue;
            GdType t = VarType(m);
            Ex init = Expr(vd.Init, t);
            _w.Line($"{m.CsName} = {Adapt(init, t)};");
        }
    }

    private void EmitFunction(ScriptInfo s, MemberInfo mi, List<MemberInfo> onready, List<(MemberInfo Member, string Code)>? ctorInits = null)
    {
        FuncDecl fd = mi.Func!;
        bool isCtor = fd.Name == "_init";
        bool isReady = fd.Name == "_ready";
        GdType ret = isCtor ? GdType.Void : ReturnTypeOf(mi);

        _script = s;
        Ctx ctx = NewFuncCtx(mi, fd, ret, infer: false, out List<ParamInfo> ps);
        _ctx = ctx;

        // Defaults that C# cannot write as constants become forwarding overloads.
        bool needOverloads = false;
        foreach (ParamInfo p in ps)
        {
            if (p.Default == null || p.IsRest) continue;
            Ex d = Expr(p.Default, p.Type);
            if (d.Type.Kind == GK.Null) p.DefaultCode = p.Type.IsReference || p.Type.Kind == GK.Object ? "null" : "default";
            else if (d.IsConstExpr || (d.Const != null)) p.DefaultCode = Adapt(d, p.Type);
            else needOverloads = true;
        }
        // A constructor with defaults also gets the parameterless one the runtime needs to make the node.
        if (isCtor && ps.Any(p => p.Default != null && !p.IsRest)) needOverloads = true;
        if (needOverloads) foreach (ParamInfo p in ps) p.DefaultCode = null;

        string RetCs() => !mi.Async || isCtor ? ret.Cs : (ret.Kind == GK.Void ? (UseAsyncVoid(mi) ? "void" : "Task") : "Task<" + ret.Cs + ">");
        string mods = "public ";
        if (!isCtor)
        {
            if (mi.IsStatic) mods += "static ";
            else if (mi.IsOverride) mods += "override ";
            else if (mi.Overridden) mods += "virtual ";
            if (mi.Async) mods += "async ";
        }
        string name = isCtor ? s.CsName : mi.CsName;
        string sig = string.Join(", ", ps.Select(p =>
            p.IsRest ? $"params Variant[] {p.Cs}"
            : $"{p.Type.Cs} {p.Cs}{(p.DefaultCode != null && !needOverloads ? " = " + p.DefaultCode : p.Default != null && !needOverloads ? " = default" : "")}"));
        string header = isCtor ? $"{mods}{name}({sig})" : $"{mods}{RetCs()} {name}({sig})";

        if (fd.GetAnnotation("rpc") is { } rpc)
        {
            string mode = rpc.Args.Count > 0 && rpc.Args[0] is Literal { Kind: LitKind.String } ml && (string)ml.Value! == "any_peer" ? "AnyPeer" : "Authority";
            Note(fd, "attribute", "Rpc", Rating.Approximated, "@rpc is recorded as [Rpc]; nothing is networked yet");
            _w.Line($"[Rpc({Lit(mode)})]");
        }
        _w.Open(header);
        if (isCtor && ctorInits != null)
            foreach ((MemberInfo m, string code) in ctorInits) _w.Line($"{FieldTarget(m)} = {code};");
        foreach (ParamInfo p in ps.Where(p => p.IsRest))
            _w.Line($"var {Naming.Safe(ctx.DeclNames[p.Decl!])} = new Collections.Array({p.Cs});");
        if (isReady) EmitOnready(onready);
        EmitBlockBody(fd.Body);
        if (ret.Kind != GK.Void && !isCtor && !AlwaysReturns(fd.Body))
            _w.Line("return " + (ret.IsReference || ret.IsVariant || ret.Kind == GK.Null ? (ret.IsVariant ? "default" : "null") : DefaultValue(ret) is var dv && dv != "new()" ? dv : "default") + ";");
        _w.Close();

        if (needOverloads) EmitOverloads(mi, ps, name, isCtor, ret, mods);
    }

    private bool UseAsyncVoid(MemberInfo mi) => mi.IsOverride || !mi.AwaitedSomewhere;

    // name(a, b, c = expr): name(a), name(a, b) forward with the default expressions written out.
    private void EmitOverloads(MemberInfo mi, List<ParamInfo> ps, string name, bool isCtor, GdType ret, string mods)
    {
        int firstOptional = ps.FindIndex(p => p.Default != null);
        if (firstOptional < 0) return;
        string baseMods = mods.Replace("override ", "").Replace("virtual ", "").Replace("async ", "");
        for (int k = firstOptional; k < ps.Count; k++)
        {
            if (ps[k].IsRest) break;
            var args = new List<string>();
            for (int i = 0; i < ps.Count; i++)
            {
                if (ps[i].IsRest) break;
                if (i < k) args.Add(ps[i].Cs);
                else
                {
                    var d = ps[i].Default;
                    if (d == null) { args.Add(ps[i].Cs); continue; }
                    EnterMemberContext(mi.Owner, isStatic: mi.IsStatic);
                    Ex e = Expr(d, ps[i].Type);
                    args.Add(Adapt(e, ps[i].Type));
                }
            }
            string sig = string.Join(", ", ps.Take(k).Select(p => p.Type.Cs + " " + p.Cs));
            _w.Line();
            if (isCtor) _w.Line($"{baseMods}{name}({sig}) : this({string.Join(", ", args)}) {{ }}");
            else
            {
                bool task = mi.Async;
                string call = $"{name}({string.Join(", ", args)})";
                string r = task ? (ret.Kind == GK.Void ? (UseAsyncVoid(mi) ? "void" : "Task") : "Task<" + ret.Cs + ">") : ret.Cs;
                if (r == "void") _w.Line($"{baseMods}void {name}({sig}) => {call};");
                else _w.Line($"{baseMods}{r} {name}({sig}) => {call};");
            }
        }
    }
}
