// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The class level of the GDScript emitter: one partial C# class per script, fields, properties, signals, enums, constants, functions.

using System.Text;
using Rs = Microsoft.CodeAnalysis;

namespace Aver.Fabric.Convert.GdScript;

internal sealed class ParamInfo
{
    public Param? Decl;
    public string Gd = "";
    public string Cs = "";
    public GdType Type = GdType.Variant;
    public Expr? Default;
    public string? DefaultCode;           // a C# constant, or null when the default needs overloads
    public bool IsRest;
}

internal sealed partial class GdEmitter
{
    private readonly List<(MemberInfo Member, string Code)> _deferredInits = new();

    /// <summary>The C# file for a top-level script.</summary>
    public string EmitScript(ScriptInfo s)
    {
        string text = "";
        for (int round = 0; round < 6; round++)
        {
            _forceNew.Clear();
            ResetMemos();
            text = EmitScriptOnce(s);
            if (_forceNew.Count == 0) break;
            foreach (var kv in _forceNew)
                _forced[kv.Key] = _forced.TryGetValue(kv.Key, out GdType? old) && old.IsVariant ? old : kv.Value;
        }
        return text;
    }

    private void ResetMemos()
    {
        foreach (ScriptInfo si in _m.AllScripts())
            foreach (MemberInfo mi in si.Members)
            {
                mi.Type = null;
                mi.ParamTypes = new List<GdType?>();
                mi.Resolving = false;
                mi.CsConst = null;
                mi.InstanceDependent = false;
            }
    }

    private void CheckProvisional(Ex target, GdType newType)
    {
        if (target.Provisional == null || target.Type.IsVariant) return;
        GdType cur = target.Type;
        GdType? force = null;
        if (newType.IsVariant || newType.Kind == GK.Void) force = GdType.Variant;
        else if (newType.Kind == GK.Null) { if (!cur.IsReference) force = GdType.Variant; }
        else if (cur.IsNumeric && newType.IsNumeric) { if (newType.Kind == GK.Float && cur.Kind == GK.Int) force = GdType.Float; }
        else if (cur.Kind == GK.Enum && newType.Kind == GK.Int) { }
        else if (cur.Cs != newType.Cs && !cur.Accepts(newType)) force = GdType.Variant;
        if (force == null) return;
        if (_forceNew.TryGetValue(target.Provisional, out GdType? prior) && prior.IsVariant) return;
        _forceNew[target.Provisional] = force;
    }

    private string EmitScriptOnce(ScriptInfo s)
    {
        _script = s;
        _notes.Clear();
        _noteTag = 0;
        var body = new CodeWriter();
        _w = body;
        EmitClass(s, top: true);
        string code = body.ToString();

        var head = new StringBuilder();
        head.Append("// FABRIC: converted from ").Append(s.ResPath).Append(" by Aver.Fabric.Convert (GDScript to Aver.Fabric). Lines marked FABRIC need attention.\n");
        head.Append("#nullable disable\n");
        head.Append("using System;\n");
        if (code.Contains(".Select(") || code.Contains(".Any(") || code.Contains(".All(") || code.Contains(".ToList()") || code.Contains(".Sum(")) head.Append("using System.Linq;\n");
        if (code.Contains("Task")) head.Append("using System.Threading.Tasks;\n");
        head.Append("using Aver.Fabric;\n");
        if (code.Contains("Collections.")) head.Append("using Collections = Aver.Fabric.Collections;\n");
        head.Append('\n');
        return head.ToString() + code;
    }

    private void EmitClass(ScriptInfo s, bool top)
    {
        ScriptInfo savedScript = _script;
        _script = s;
        ClassDecl c = s.Decl;
        var onready = s.Members.Where(m => m.Kind == MK.Var && m.Var!.HasAnnotation("onready")).ToList();
        // An inner class has its own constructor assignments; the outer class's wait until it is done.
        var outerInits = _deferredInits.ToList();
        _deferredInits.Clear();

        if (top)
        {
            if (c.IsTool)
            {
                Note(c, "attribute", "Tool", Rating.Unsupported, "@tool: Aver has no editor scripts, the class runs in the game only");
                _w.Line("[Tool]");
            }
            if (c.Name != null) _w.Line("[GlobalClass]");
            if (c.Icon != null) _w.Line($"[Icon({Lit(c.Icon)})]");
        }

        string baseName = s.BaseScript != null ? QualifiedScript(s.BaseScript) : s.BaseApi != null ? s.BaseApi.ToDisplayString(s_nameFormat) : s.BaseUnresolved;
        _w.Open($"public partial class {s.CsName} : {baseName}");

        // The members, in source order. Group attributes stand for the next exported variable.
        var pendingAttrs = new List<string>();
        foreach (Member m in c.Members)
        {
            if (m is StandaloneAnnotation sa)
            {
                string? attr = StandaloneAttribute(sa.Annotation);
                if (attr != null) pendingAttrs.Add(attr);
                continue;
            }
            switch (m)
            {
                case SignalDecl sd: EmitSignal(s, sd); break;
                case EnumDecl ed: EmitEnum(s, ed); break;
                case ConstDecl cd: EmitConst(s, cd); break;
                case VarDecl vd:
                {
                    MemberInfo? mi = s.ByName.GetValueOrDefault(vd.Name);
                    if (mi != null && ReferenceEquals(mi.Decl, vd)) { EmitVar(s, mi, pendingAttrs); pendingAttrs.Clear(); }
                    break;
                }
                case FuncDecl:
                    break;   // functions follow the variables, below
                case ClassDecl cd2:
                {
                    MemberInfo? mi = s.ByName.GetValueOrDefault(cd2.Name ?? "");
                    if (mi?.Inner != null) { _w.Line(); EmitClass(mi.Inner, top: false); }
                    break;
                }
                case ErrorStmt es:
                    _w.Line($"// FABRIC: could not parse a declaration here (line {es.Start.Line})");
                    break;
            }
        }

        // Constructor: field initialisers that read instance state, then _init.
        EmitConstructor(s);

        // Functions in source order; _ready gets the @onready assignments.
        bool hasReady = false;
        foreach (Member m in c.Members)
        {
            if (m is not FuncDecl fd) continue;
            MemberInfo? mi = s.ByName.GetValueOrDefault(fd.Name);
            if (mi == null || !ReferenceEquals(mi.Decl, fd) || fd.Name == "_init") continue;
            _w.Line();
            if (fd.Name == "_ready") hasReady = true;
            EmitFunction(s, mi, onready);
        }
        if (!hasReady && onready.Count > 0)
        {
            _w.Line();
            EmitSyntheticReady(s, onready);
        }

        _w.Close();
        _script = savedScript;
        _deferredInits.Clear();
        _deferredInits.AddRange(outerInits);
    }

    private static readonly Rs.SymbolDisplayFormat s_nameFormat = new(
        typeQualificationStyle: Rs.SymbolDisplayTypeQualificationStyle.NameAndContainingTypes,
        genericsOptions: Rs.SymbolDisplayGenericsOptions.IncludeTypeParameters);

    // A name for a script class as the current class may write it.
    private string QualifiedScript(ScriptInfo target)
    {
        if (!target.IsInner) return target.CsName;
        for (ScriptInfo? o = _script; o != null; o = o.Outer)
            foreach (ScriptInfo chain in o.Chain())
                if (ReferenceEquals(chain, target.Outer) || ReferenceEquals(chain, target)) return target.CsName;
        return target.CsPath;
    }

    private string QualifiedEnum(EnumInfo e)
    {
        for (ScriptInfo? o = _script; o != null; o = o.Outer)
            foreach (ScriptInfo chain in o.Chain())
                if (ReferenceEquals(chain, e.Owner)) return Naming.TypeName(e.Name!);
        return e.CsPath;
    }

    // ---- attributes ----

    private string? StandaloneAttribute(Annotation a)
    {
        string Arg(int i) => i < a.Args.Count ? ConstText(a.Args[i]) : "";
        return a.Name switch
        {
            "export_group" => $"[ExportGroup({Lit(Arg(0))}{(a.Args.Count > 1 ? ", " + Lit(Arg(1)) : "")})]",
            "export_subgroup" => $"[ExportSubgroup({Lit(Arg(0))}{(a.Args.Count > 1 ? ", " + Lit(Arg(1)) : "")})]",
            "export_category" => $"[ExportCategory({Lit(Arg(0))})]",
            _ => null,
        };
    }

    /// <summary>A constant annotation argument as text (numbers as written, strings unquoted).</summary>
    private static string ConstText(Expr e) => e switch
    {
        Literal l when l.Kind is LitKind.String or LitKind.StringName or LitKind.NodePath => (string)l.Value!,
        Literal l => l.Text,
        Unary { Op: "-" } u => "-" + ConstText(u.Operand),
        Identifier i => i.Name,
        MemberAccess m => ConstText(m.Target) + "." + m.Name,
        _ => "",
    };

    private string? ExportAttribute(VarDecl vd, MemberInfo mi)
    {
        foreach (Annotation a in vd.Annotations)
        {
            string args = string.Join(",", a.Args.Select(ConstText));
            string N(string hint, string text = "") => text.Length == 0 ? $"[Export(PropertyHint.{hint})]" : $"[Export(PropertyHint.{hint}, {Lit(text)})]";
            switch (a.Name)
            {
                case "export": case "export_storage": case "export_custom": return "[Export]";
                case "export_range": return N("Range", args);
                case "export_exp_easing": return N("Easing", args);
                case "export_enum": return N("Enum", args);
                case "export_flags": return N("Flags", args);
                case "export_file": return N("File", args);
                case "export_dir": return N("Dir");
                case "export_global_file": return N("GlobalFile", args);
                case "export_global_dir": return N("GlobalDir");
                case "export_multiline": return N("MultilineText");
                case "export_placeholder": return N("PlaceholderText", args);
                case "export_color_no_alpha": return N("ColorNoAlpha");
                case "export_node_path": return N("NodePathValidTypes", args);
                case "export_flags_3d_physics": return N("Layers3DPhysics");
                case "export_flags_3d_render": return N("Layers3DRender");
                case "export_flags_3d_navigation": return N("Layers3DNavigation");
                case "export_flags_2d_physics": return N("Layers2DPhysics");
                case "export_flags_2d_render": return N("Layers2DRender");
                case "export_flags_2d_navigation": return N("Layers2DNavigation");
            }
        }
        return null;
    }

    // ---- signals, enums, constants ----

    private void EmitSignal(ScriptInfo s, SignalDecl sd)
    {
        MemberInfo? mi = s.ByName.GetValueOrDefault(sd.Name);
        if (mi == null || !ReferenceEquals(mi.Decl, sd)) return;
        var ps = new List<string>();
        var names = new HashSet<string>();
        foreach (Param p in sd.Params)
        {
            GdType t = p.Type != null ? ResolveType(p.Type, s) : p.Default != null ? SignalParamFromDefault(p.Default, s) : GdType.Variant;
            string n = Naming.Safe(Naming.Camel(p.Name));
            while (!names.Add(n)) n += "2";
            ps.Add(t.Cs + " " + n);
        }
        _w.Line($"[Signal] public delegate void {mi.CsName}EventHandler({string.Join(", ", ps)});");
    }

    private GdType SignalParamFromDefault(Expr d, ScriptInfo s)
    {
        EnterMemberContext(s, isStatic: true);
        return TypeOf(d);
    }

    private void EmitEnum(ScriptInfo s, EnumDecl ed)
    {
        EnumInfo? info = s.Enums.FirstOrDefault(e => ReferenceEquals(e.Decl, ed));
        if (info == null) return;
        EnterMemberContext(s, isStatic: true);
        if (ed.Name != null)
        {
            var parts = new List<string>();
            foreach (EnumValue v in ed.Values)
            {
                string cs = info.Values[v.Name];
                if (v.Value == null) { parts.Add(cs); continue; }
                Ex ev = Expr(v.Value, GdType.Int);
                parts.Add($"{cs} = {EnumValueCode(ev)}");
            }
            if (parts.Count <= 4 && parts.Sum(p => p.Length) < 80) _w.Line($"public enum {Naming.TypeName(ed.Name)} {{ {string.Join(", ", parts)} }}");
            else
            {
                _w.Open($"public enum {Naming.TypeName(ed.Name)}");
                foreach (string p in parts) _w.Line(p + ",");
                _w.Close();
            }
            return;
        }
        // Anonymous enum: constants of the class, counting up from the last explicit value.
        long next = 0;
        foreach (EnumValue v in ed.Values)
        {
            string cs = info.Values[v.Name];
            if (v.Value != null)
            {
                Ex ev = Expr(v.Value, GdType.Int);
                if (ev.Const is long l) { next = l; _w.Line($"public const long {cs} = {ev.Code};"); next++; continue; }
                _w.Line($"public const long {cs} = {Adapt(ev, GdType.Int)};");
                continue;
            }
            _w.Line($"public const long {cs} = {next};");
            next++;
        }
    }

    // An enum member's value must be an int constant; a long constant is fine as long as it fits.
    private static string EnumValueCode(Ex e) =>
        e.Const is long l && l >= int.MinValue && l <= int.MaxValue ? l.ToString(System.Globalization.CultureInfo.InvariantCulture) : e.Code;

    private void EnterMemberContext(ScriptInfo s, bool isStatic)
    {
        _script = s;
        _ctx = new Ctx { Script = s, IsStatic = isStatic, IsInitializer = true };
        _ctx.Scopes.Add(new Dictionary<string, Local>());
        foreach (string n in MemberCsNames(s)) _ctx.Used.Add(n);
    }

    private void EmitConst(ScriptInfo s, ConstDecl cd)
    {
        MemberInfo? mi = s.ByName.GetValueOrDefault(cd.Name);
        if (mi == null || !ReferenceEquals(mi.Decl, cd)) return;
        EnterMemberContext(s, isStatic: true);
        if (mi.ConstAlias == null) ResolveConstAlias(mi);
        if (mi.AliasScript != null)
        {
            _w.Line($"// {cd.Name} names {mi.AliasScript.CsPath}: uses below say {QualifiedScript(mi.AliasScript)} directly");
            return;
        }
        GdType t = ConstType(mi);
        Ex init = cd.Value != null ? Expr(cd.Value, t) : new Ex("default", t);
        string value = Adapt(init, t);
        bool canConst = init.IsConstExpr && t.Kind is GK.Int or GK.Float or GK.Bool or GK.String;
        if (canConst) _w.Line($"public const {t.Cs} {mi.CsName} = {value};");
        else _w.Line($"public static readonly {t.Cs} {mi.CsName} = {value};");
    }

    private void ResolveConstAlias(MemberInfo mi)
    {
        mi.ConstAlias = "";
        var cd = (ConstDecl)mi.Decl;
        if (cd.Value is PreloadExpr p && p.Path is Literal { Kind: LitKind.String } lit && lit.Value is string path &&
            path.EndsWith(".gd", StringComparison.Ordinal) && _m.ByPath.TryGetValue(path, out ScriptInfo? target))
            mi.AliasScript = target;
        else if (cd.Value is Identifier id && _m.ByClassName.TryGetValue(id.Name, out ScriptInfo? byName) && mi.Owner.Find(id.Name) == null)
            mi.AliasScript = byName;
    }

    // ---- variables ----

    private GdType ConstType(MemberInfo mi)
    {
        if (mi.Type != null) return mi.Type;
        if (mi.Resolving) return GdType.Variant;
        mi.Resolving = true;
        try
        {
            if (mi.Enum != null && mi.Decl is EnumDecl) { mi.Type = GdType.Int; return mi.Type; }
            var cd = (ConstDecl)mi.Decl;
            ScriptInfo savedScript = _script; Ctx savedCtx = _ctx; CodeWriter savedW = _w;
            try
            {
                EnterMemberContext(mi.Owner, isStatic: true);
                mi.Type = cd.Type != null ? ResolveType(cd.Type, mi.Owner) : cd.Value != null ? CleanInferred(TypeOf(cd.Value)) : GdType.Variant;
            }
            finally { _script = savedScript; _ctx = savedCtx; _w = savedW; }
            return mi.Type!;
        }
        finally { mi.Resolving = false; }
    }

    /// <summary>A member or local type inferred from an initialiser: a bare null or void carries no type.</summary>
    private static GdType CleanInferred(GdType t) => t.Kind is GK.Null or GK.Void or GK.Lambda && t.Kind != GK.Lambda ? GdType.Variant : t.Kind == GK.Lambda ? GdType.Callable : t.WithWeak(false);

    private GdType VarType(MemberInfo mi)
    {
        if (mi.Type != null) return mi.Type;
        if (mi.Kind == MK.Const) return ConstType(mi);
        if (mi.Resolving) return GdType.Variant;
        mi.Resolving = true;
        try
        {
            var vd = (VarDecl)mi.Decl;
            ScriptInfo savedScript = _script; Ctx savedCtx = _ctx; CodeWriter savedW = _w;
            try
            {
                EnterMemberContext(mi.Owner, mi.IsStatic);
                if (vd.Type != null) mi.Type = ResolveType(vd.Type, mi.Owner);
                else if (vd.Init != null)
                {
                    GdType t = GuardScene(CleanInferred(TypeOf(vd.Init)), vd.Name, mi.Owner.Decl);
                    mi.Type = vd.Inferred ? t : Unify(t, vd.Name, mi.Owner.Decl);
                    if (!vd.Inferred && _forced.TryGetValue(vd, out GdType? forced)) mi.Type = forced;
                }
                else mi.Type = GdType.Variant;
            }
            finally { _script = savedScript; _ctx = savedCtx; _w = savedW; }
            return mi.Type!;
        }
        finally { mi.Resolving = false; }
    }

    /// <summary>An untyped <c>var x = init</c> keeps the initialiser's type unless an assignment in <paramref name="scope"/> says otherwise
    /// (GDScript's x is a Variant; a C# variable that takes only one kind of value reads better and means the same).</summary>
    private GdType Unify(GdType init, string name, Node scope)
    {
        if (init.Kind is GK.Variant or GK.Null or GK.Void or GK.Lambda || init.Weak) return GdType.Variant;
        // A type read off a scene is what the node is where it was found; a variable that is assigned again may hold another kind of node.
        if (init.FromScene && AssignsTo(scope, name)) return GdType.Variant;
        if (init.Kind is GK.Object or GK.ScriptClass) return init;
        GdType cur = init;
        foreach (Assign a in AssignsIn(scope))
        {
            if (!IsNamed(a.Target, name)) continue;
            GdType? rhs = LiteralType(a.Value);
            if (rhs == null)
            {
                // Not a literal: names and calls could be anything; arithmetic on a number stays a number.
                if (a.Op != "=" && cur.IsNumeric) continue;
                if (a.Op != "=" && cur.Kind == GK.String) continue;
                if (a.Op != "=" && cur.Kind == GK.Struct) continue;
                continue;
            }
            if (a.Op != "=" && cur.IsNumeric && rhs.IsNumeric) { if (rhs.Kind == GK.Float) cur = GdType.Float; continue; }
            if (a.Op != "=" && cur.Kind == GK.String && rhs.Kind == GK.String) continue;
            if (cur.IsNumeric && rhs.IsNumeric) { if (rhs.Kind == GK.Float) cur = GdType.Float; continue; }
            if (rhs.Kind == GK.Null) { if (cur.IsReference) continue; return GdType.Variant; }
            if (cur.Kind != rhs.Kind) return GdType.Variant;
        }
        return cur;
    }

    // Every assignment below a node, found once (the tree walk sorts children, so it is not cheap).
    private readonly Dictionary<Node, List<Assign>> _assigns = new();

    private List<Assign> AssignsIn(Node scope)
    {
        if (!_assigns.TryGetValue(scope, out List<Assign>? list))
            _assigns[scope] = list = scope.Descendants().OfType<Assign>().ToList();
        return list;
    }

    private static bool IsNamed(Expr target, string name) =>
        target is Identifier i && i.Name == name || target is MemberAccess { Target: SelfExpr } m && m.Name == name;

    private static GdType? LiteralType(Expr e) => e switch
    {
        Literal l => l.Kind switch
        {
            LitKind.Int => GdType.Int, LitKind.Float => GdType.Float, LitKind.Bool => GdType.Bool, LitKind.String => GdType.String,
            LitKind.StringName => GdType.StringName, LitKind.NodePath => GdType.NodePath, LitKind.Null => GdType.Null, _ => null,
        },
        Unary { Op: "-" or "+" } u => LiteralType(u.Operand) is { IsNumeric: true } n ? n : null,
        _ => null,
    };

    private void EmitVar(ScriptInfo s, MemberInfo mi, List<string> pendingAttrs)
    {
        var vd = (VarDecl)mi.Decl;
        GdType t = VarType(mi);
        EnterMemberContext(s, mi.IsStatic);
        _ctx.InAccessor = null;
        bool onready = vd.HasAnnotation("onready");

        string? export = ExportAttribute(vd, mi);
        string mods = "public " + (mi.IsStatic ? "static " : "");
        void Attributes()
        {
            foreach (string a in pendingAttrs) _w.Line(a);
            if (export != null) _w.Line(export);
        }

        // The initial value: an expression when it does not read instance state, else the constructor assigns it.
        string? initCode = null;
        if (vd.Init != null && !onready)
        {
            _ctx.UsedInstance = false;
            Ex init = Expr(vd.Init, t);
            initCode = Adapt(init, t);
            if (_ctx.UsedInstance && !mi.IsStatic)
            {
                mi.InstanceDependent = true;
                _deferredInits.Add((mi, initCode));
                initCode = null;
            }
        }

        if (!mi.HasAccessors)
        {
            Attributes();
            if (initCode != null) _w.Line($"{mods}{t.Cs} {mi.CsName} = {initCode};");
            else _w.Line($"{mods}{t.Cs} {mi.CsName}{DefaultInit(t, vd, onready)};");
            return;
        }

        // A variable with a setter or getter: a property over a backing field (the field is what the accessors' own references mean).
        _w.Line($"private {(mi.IsStatic ? "static " : "")}{t.Cs} {mi.Backing}{(initCode != null ? " = " + initCode : DefaultInit(t, vd, onready))};");
        Attributes();
        _w.Open($"{mods}{t.Cs} {mi.CsName}");
        PropertyAccessor? getter = vd.Getter, setter = vd.Setter;
        if (getter == null) _w.Line($"get => {mi.Backing};");
        else EmitAccessor(s, mi, getter, isSetter: false, t);
        if (setter == null) _w.Line($"set => {mi.Backing} = value;");
        else EmitAccessor(s, mi, setter, isSetter: true, t);
        _w.Close();
    }

    private string DefaultInit(GdType t, VarDecl vd, bool onready)
    {
        if (onready) return "";
        // GDScript variables start as the type's default: 0, "", an empty collection, null.
        return t.Kind switch
        {
            GK.Array or GK.Dict => " = new()",
            GK.String => " = \"\"",
            GK.Struct when DefaultValue(t) != "default" => " = " + DefaultValue(t),
            _ => "",
        };
    }

    private void EmitAccessor(ScriptInfo s, MemberInfo mi, PropertyAccessor acc, bool isSetter, GdType t)
    {
        string keyword = isSetter ? "set" : "get";
        if (acc.Delegate != null)
        {
            MemberInfo? fn = s.Find(acc.Delegate);
            string call = (fn?.CsName ?? Naming.MethodName(acc.Delegate));
            _w.Line(isSetter ? $"set => {call}(value);" : $"get => {call}();");
            if (fn != null && fn.Kind == MK.Func) _accessorFuncs[fn] = mi;
            return;
        }
        if (acc.Body == null) { _w.Line(isSetter ? $"set => {mi.Backing} = value;" : $"get => {mi.Backing};"); return; }

        _w.Open(keyword);
        var saved = (_ctx, _w);
        var ctx = new Ctx { Script = s, IsStatic = mi.IsStatic, InAccessor = mi, Return = isSetter ? GdType.Void : t };
        ctx.Scopes.Add(new Dictionary<string, Local>());
        _ctx = ctx;
        foreach (string n in MemberCsNames(s)) ctx.Used.Add(n);
        string valueName = acc.ParamName != null ? Naming.Safe(Naming.Camel(acc.ParamName)) : "value";
        var ps = new List<Param>();
        if (isSetter)
        {
            ctx.Used.Add(valueName);
            Declare(acc.ParamName ?? "value", valueName, t);
        }
        ctx.DeclNames = ComputeNames(ps, acc.Body, isSetter ? new[] { valueName } : Array.Empty<string>());
        foreach (string u in ctx.DeclNames.Values) ctx.Used.Add(u);
        // A setter written set(v): names its parameter v, which C# calls value: alias it.
        if (isSetter && acc.ParamName != null && acc.ParamName != "value") ctx.Scopes[^1][acc.ParamName] = new Local { Gd = acc.ParamName, Cs = "value", Type = t };
        ctx.Used.Add("value");
        EmitBlockBody(acc.Body);
        _w.Close();
        (_ctx, _w) = saved;
    }

    private readonly Dictionary<MemberInfo, MemberInfo> _accessorFuncs = new();

    // ---- constructor ----

    private void EmitConstructor(ScriptInfo s)
    {
        MemberInfo? init = s.ByName.GetValueOrDefault("_init");
        var deferred = _deferredInits.Where(d => ReferenceEquals(d.Member.Owner, s)).ToList();
        if (init == null || init.Kind != MK.Func)
        {
            if (deferred.Count == 0) return;
            _w.Line();
            _w.Open($"public {s.CsName}()");
            foreach ((MemberInfo m, string code) in deferred) _w.Line($"{FieldTarget(m)} = {code};");
            _w.Close();
            return;
        }
        _w.Line();
        EmitFunction(s, init, new List<MemberInfo>(), ctorInits: deferred);
    }

    private static string FieldTarget(MemberInfo m) => m.HasAccessors ? m.Backing : m.CsName;
}
