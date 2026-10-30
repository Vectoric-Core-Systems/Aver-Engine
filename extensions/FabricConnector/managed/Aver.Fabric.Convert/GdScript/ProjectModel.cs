// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The project-wide symbol table: every .gd file parsed first, so a script sees the classes, enums, signals and functions of the others.

using Rs = Microsoft.CodeAnalysis;

namespace Aver.Fabric.Convert.GdScript;

internal enum MK { Var, Const, Enum, Signal, Func, Class }

internal sealed class EnumInfo
{
    public string? Name;                       // null: anonymous (its values are constants of the class)
    public string CsPath = "";
    public EnumDecl Decl = null!;
    public ScriptInfo Owner = null!;
    public Dictionary<string, string> Values = new(); // gd name -> C# name
}

internal sealed class MemberInfo
{
    public MK Kind;
    public string Name = "";
    public string CsName = "";
    public Member Decl = null!;
    public ScriptInfo Owner = null!;
    public bool IsStatic;
    public GdType? Type;                       // var/const: its type; func: the return type; memoized
    public bool Resolving;
    public bool Async;                         // func containing await
    public bool AwaitedSomewhere;              // a call site does `await name(...)`: a void coroutine then returns Task
    public bool Overridden;                    // some descendant script defines the same function: virtual
    public bool IsOverride;                    // an ancestor (script or API) defines it: override
    public Rs.IMethodSymbol? ApiBase;          // the API virtual this overrides
    public EnumInfo? Enum;
    public ScriptInfo? Inner;
    public string Backing = "";                // var with accessors: the backing field name
    public bool HasAccessors => Decl is VarDecl v && (v.Setter != null || v.Getter != null);
    public bool Exported;
    public List<GdType?> ParamTypes = new();   // func: memoized parameter types
    public bool InstanceDependent;             // var initialiser reads instance state: assigned in the constructor
    public bool? CsConst;                      // const: emitted as a C# const (a compile-time constant of a primitive type)
    public string? ConstAlias;                 // const X = preload("res://y.gd") / = SomeClass: X is that class
    public ScriptInfo? AliasScript;

    public FuncDecl? Func => Decl as FuncDecl;
    public VarDecl? Var => Decl as VarDecl;
}

internal sealed class ScriptInfo
{
    public string ResPath = "";
    public string CsName = "";                 // the class name
    public string CsPath = "";                 // Outer.Inner
    public string Output = "";
    public ClassDecl Decl = null!;
    public ParseResult? Parse;
    public ScriptInfo? Outer;
    public ScriptInfo? BaseScript;
    public Rs.INamedTypeSymbol? BaseApi;
    public string BaseUnresolved = "";
    public string? DeclaredClassName;
    public List<MemberInfo> Members = new();
    public Dictionary<string, MemberInfo> ByName = new();
    public List<ScriptInfo> Inners = new();
    public List<EnumInfo> Enums = new();
    public bool IsInner => Outer != null;
    public string SourceText = "";

    public ScriptInfo Root => Outer == null ? this : Outer.Root;

    /// <summary>The API class at the bottom of the chain (CharacterBody3D ...), or null when it ends in an unknown class.</summary>
    public Rs.INamedTypeSymbol? ApiRoot
    {
        get
        {
            for (ScriptInfo? s = this; s != null; s = s.BaseScript) if (s.BaseScript == null) return s.BaseApi;
            return null;
        }
    }

    public IEnumerable<ScriptInfo> Chain() { for (ScriptInfo? s = this; s != null; s = s.BaseScript) yield return s; }

    public MemberInfo? Find(string name)
    {
        foreach (ScriptInfo s in Chain())
            if (s.ByName.TryGetValue(name, out MemberInfo? m)) return m;
        return null;
    }

    public bool DerivesFromScript(ScriptInfo other) => Chain().Any(s => ReferenceEquals(s, other));
}

/// <summary>An autoload from project.godot: a global name for a node under /root.</summary>
internal sealed class AutoloadInfo
{
    public string Name = "";
    public string Path = "";
    public bool IsSingleton;
}

/// <summary>Everything the emitter knows about the project besides the one script it is writing.</summary>
internal sealed class ProjectModel
{
    public ApiModel? Api;
    public SceneIndex? Scenes;                                  // Fabric/nodes.json: the scenes' nodes, for typed node access
    public readonly List<ScriptInfo> Scripts = new();           // top-level scripts in manifest order
    public readonly Dictionary<string, ScriptInfo> ByPath = new(StringComparer.Ordinal);
    public readonly Dictionary<string, ScriptInfo> ByClassName = new(StringComparer.Ordinal);
    public readonly List<AutoloadInfo> Autoloads = new();
    public readonly List<string> Problems = new();

    public AutoloadInfo? FindAutoload(string name) => Autoloads.FirstOrDefault(a => a.Name == name);

    public IEnumerable<ScriptInfo> AllScripts()
    {
        foreach (ScriptInfo s in Scripts)
            foreach (ScriptInfo x in Flatten(s)) yield return x;
    }

    private static IEnumerable<ScriptInfo> Flatten(ScriptInfo s)
    {
        yield return s;
        foreach (ScriptInfo i in s.Inners)
            foreach (ScriptInfo x in Flatten(i)) yield return x;
    }

    /// <summary>Parses the sources and builds the table. Returns the scripts in the order given.</summary>
    public static ProjectModel Build(IReadOnlyList<GdSource> sources, ApiModel? api, string? godotProjectDir)
    {
        var model = new ProjectModel { Api = api };
        foreach (GdSource src in sources)
        {
            ParseResult parsed = GdScriptParser.Parse(src.Text);
            var info = new ScriptInfo
            {
                ResPath = src.ResPath, CsName = src.ClassName, CsPath = src.ClassName, Output = src.Output, Decl = parsed.Script, Parse = parsed,
                DeclaredClassName = parsed.Script.Name, SourceText = src.Text,
            };
            model.Scripts.Add(info);
            model.ByPath[src.ResPath] = info;
            if (!string.IsNullOrEmpty(parsed.Script.Name) && !model.ByClassName.ContainsKey(parsed.Script.Name!)) model.ByClassName[parsed.Script.Name!] = info;
        }
        if (godotProjectDir != null) model.ReadAutoloads(godotProjectDir);
        foreach (ScriptInfo s in model.Scripts) model.Populate(s);
        foreach (ScriptInfo s in model.Scripts) model.ResolveBases(s);
        model.MarkAwaited();
        foreach (ScriptInfo s in model.AllScripts().OrderBy(x => x.Chain().Count()).ToList()) model.MarkOverrides(s);
        return model;
    }

    private void ReadAutoloads(string dir)
    {
        string file = Path.Combine(dir, "project.godot");
        if (!File.Exists(file)) return;
        bool inSection = false;
        foreach (string raw in File.ReadLines(file))
        {
            string line = raw.Trim();
            if (line.StartsWith('[')) { inSection = line == "[autoload]"; continue; }
            if (!inSection || line.Length == 0 || line[0] == ';') continue;
            int eq = line.IndexOf('=');
            if (eq <= 0) continue;
            string name = line[..eq].Trim();
            string value = line[(eq + 1)..].Trim().Trim('"');
            bool singleton = value.StartsWith('*');
            Autoloads.Add(new AutoloadInfo { Name = name, Path = value.TrimStart('*'), IsSingleton = singleton });
        }
    }

    // ---- members ----

    private void Populate(ScriptInfo s)
    {
        var used = new HashSet<string>(StringComparer.Ordinal);
        // Types keep their names (other scripts refer to them); a variable of the same name is the one that moves.
        foreach (Member m in s.Decl.Members)
        {
            if (m is ClassDecl { Name: not null } cd0) used.Add(Naming.TypeName(cd0.Name));
            else if (m is EnumDecl { Name: not null } ed0) used.Add(Naming.TypeName(ed0.Name));
        }
        foreach (Member m in s.Decl.Members)
        {
            switch (m)
            {
                case ClassDecl c when c.Name != null:
                {
                    var inner = new ScriptInfo
                    {
                        ResPath = s.ResPath, CsName = Naming.TypeName(c.Name), Decl = c, Outer = s, Output = s.Output, SourceText = s.SourceText, Parse = s.Parse,
                    };
                    inner.CsPath = s.CsPath + "." + inner.CsName;
                    s.Inners.Add(inner);
                    var mi = new MemberInfo { Kind = MK.Class, Name = c.Name, CsName = inner.CsName, Decl = c, Owner = s, Inner = inner };
                    Add(s, mi, used);
                    Populate(inner);
                    break;
                }
                case SignalDecl sd:
                    Add(s, new MemberInfo { Kind = MK.Signal, Name = sd.Name, CsName = Naming.Pascal(sd.Name), Decl = sd, Owner = s }, used);
                    break;
                case EnumDecl ed:
                {
                    var info = new EnumInfo { Name = ed.Name, Decl = ed, Owner = s, CsPath = ed.Name != null ? s.CsPath + "." + Naming.TypeName(ed.Name) : "" };
                    foreach (EnumValue v in ed.Values) info.Values[v.Name] = Naming.Pascal(v.Name);
                    s.Enums.Add(info);
                    if (ed.Name != null)
                        Add(s, new MemberInfo { Kind = MK.Enum, Name = ed.Name, CsName = Naming.TypeName(ed.Name), Decl = ed, Owner = s, Enum = info }, used);
                    else
                        foreach (EnumValue v in ed.Values)
                            Add(s, new MemberInfo { Kind = MK.Const, Name = v.Name, CsName = Naming.Pascal(v.Name), Decl = ed, Owner = s, IsStatic = true, Enum = info, Type = GdType.Int }, used);
                    break;
                }
                case ConstDecl cd:
                    Add(s, new MemberInfo { Kind = MK.Const, Name = cd.Name, CsName = Naming.Pascal(cd.Name), Decl = cd, Owner = s, IsStatic = true }, used);
                    break;
                case VarDecl vd:
                {
                    var mi = new MemberInfo { Kind = MK.Var, Name = vd.Name, CsName = Naming.FieldName(vd.Name), Decl = vd, Owner = s, IsStatic = vd.IsStatic };
                    mi.Exported = vd.Annotations.Any(a => a.Name.StartsWith("export", StringComparison.Ordinal) && !a.Name.StartsWith("export_group") && !a.Name.StartsWith("export_sub") && !a.Name.StartsWith("export_cat"));
                    Add(s, mi, used);
                    if (vd.Setter != null || vd.Getter != null) mi.Backing = "_" + Naming.Camel(vd.Name.TrimStart('_'));
                    break;
                }
                case FuncDecl fd:
                {
                    var mi = new MemberInfo { Kind = MK.Func, Name = fd.Name, Decl = fd, Owner = s, IsStatic = fd.IsStatic, Async = ContainsAwait(fd.Body) };
                    mi.CsName = Naming.MethodName(fd.Name);
                    if (mi.CsName == s.CsName) mi.CsName += "_";   // a C# member cannot be named like its class
                    if (!s.ByName.ContainsKey(fd.Name)) Add(s, mi, used, rename: false);
                    break;
                }
            }
        }
    }

    // Two members must not share a C# name (nor with a nested type): a variable called state beside enum State becomes state.
    // A member also must not be named like a class the script uses: a const ENEMY next to class Enemy, a var timer next to Timer.
    private bool IsTypeName(string cs) =>
        Api?.Fabric(cs) != null || ByClassName.ContainsKey(cs) || Scripts.Any(x => x.CsName == cs);

    private void Add(ScriptInfo s, MemberInfo mi, HashSet<string> used, bool rename = true)
    {
        if (s.ByName.ContainsKey(mi.Name)) return;
        if (mi.Kind == MK.Func && IsTypeName(mi.CsName) && mi.CsName != s.CsName) mi.CsName += "_";
        if (rename && mi.Kind is not (MK.Class or MK.Enum))
        {
            string cs = mi.CsName;
            if (used.Contains(cs) || cs == s.CsName || (mi.Kind != MK.Signal && IsTypeName(cs)))
            {
                string alt = Naming.Camel(mi.Name.TrimStart('_'));
                if (mi.Kind == MK.Var && !used.Contains(alt) && alt != cs && !Naming.IsKeyword(alt)) cs = alt;
                else { cs += "_"; while (used.Contains(cs)) cs += "_"; }
            }
            mi.CsName = cs;
        }
        used.Add(mi.CsName);
        if (mi.Kind == MK.Signal) { used.Add(mi.CsName + "EventHandler"); }
        s.Members.Add(mi);
        s.ByName[mi.Name] = mi;
    }

    internal static bool ContainsAwait(Node n)
    {
        foreach (Node c in n.Children())
        {
            if (c is AwaitExpr) return true;
            if (c is LambdaExpr) continue;
            if (ContainsAwait(c)) return true;
        }
        return false;
    }

    // ---- bases ----

    private void ResolveBases(ScriptInfo s)
    {
        ResolveBase(s);
        foreach (ScriptInfo i in s.Inners) ResolveBases(i);
    }

    private void ResolveBase(ScriptInfo s)
    {
        ExtendsClause? e = s.Decl.Extends;
        if (e == null)
        {
            s.BaseApi = Api?.Fabric("RefCounted");
            s.BaseUnresolved = s.BaseApi == null ? "RefCounted" : "";
            return;
        }
        if (e.ResPath != null)
        {
            string path = e.ResPath;
            if (ByPath.TryGetValue(path, out ScriptInfo? target))
            {
                ScriptInfo? t = target;
                foreach (string part in e.Path) t = t?.Inners.FirstOrDefault(i => i.Decl.Name == part);
                if (t != null && !ReferenceEquals(t, s) && !t.DerivesFromScript(s)) { s.BaseScript = t; return; }
            }
            s.BaseUnresolved = "GodotObject";
            s.BaseApi = Api?.GodotObject;
            return;
        }
        string first = e.Path.Count > 0 ? e.Path[0] : "";
        ScriptInfo? script = null;
        // An inner class of this or an enclosing script, else a global class_name, else an API class.
        for (ScriptInfo? o = s.Outer ?? s; o != null && script == null; o = o.Outer)
            script = o.Inners.FirstOrDefault(i => i.Decl.Name == first && !ReferenceEquals(i, s));
        if (script == null && ByClassName.TryGetValue(first, out ScriptInfo? g) && !ReferenceEquals(g, s)) script = g;
        if (script != null)
        {
            ScriptInfo? t = script;
            foreach (string part in e.Path.Skip(1)) t = t?.Inners.FirstOrDefault(i => i.Decl.Name == part);
            if (t != null && !t.DerivesFromScript(s)) { s.BaseScript = t; return; }
        }
        string api = string.Join(".", e.Path);
        if (api == "Object") api = "GodotObject";
        s.BaseApi = Api?.Fabric(api);
        if (s.BaseApi == null) s.BaseUnresolved = api;
    }

    // A void coroutine somebody awaits must return a Task; one nobody awaits is async void (a signal handler, a virtual).
    private void MarkAwaited()
    {
        var awaited = new HashSet<string>(StringComparer.Ordinal);
        foreach (ScriptInfo s in Scripts)
            foreach (Node n in s.Decl.Descendants())
                if (n is AwaitExpr { Operand: Call c })
                {
                    if (c.Callee is Identifier id) awaited.Add(id.Name);
                    else if (c.Callee is MemberAccess ma) awaited.Add(ma.Name);
                }
        foreach (ScriptInfo s in AllScripts())
            foreach (MemberInfo m in s.Members)
                if (m.Kind == MK.Func && m.Async && awaited.Contains(m.Name)) m.AwaitedSomewhere = true;
    }

    private void MarkOverrides(ScriptInfo s)
    {
        foreach (MemberInfo m in s.Members.Where(m => m.Kind == MK.Func))
        {
            for (ScriptInfo? b = s.BaseScript; b != null; b = b.BaseScript)
            {
                if (b.ByName.TryGetValue(m.Name, out MemberInfo? bm) && bm.Kind == MK.Func)
                {
                    m.IsOverride = true;
                    m.CsName = bm.CsName;
                    bm.Overridden = true;
                    break;
                }
            }
            if (!m.IsOverride && Api != null)
            {
                Rs.IMethodSymbol? v = Api.FindVirtual(s.ApiRoot, m.Name);
                if (v != null)
                {
                    m.IsOverride = true;
                    m.ApiBase = v;
                    m.CsName = v.Name;
                }
            }
        }
    }
}

/// <summary>A .gd file to convert.</summary>
public sealed class GdSource
{
    public string ResPath { get; init; } = "";
    public string Text { get; init; } = "";
    /// <summary>The C# class name (Fabric/scripts.json className).</summary>
    public string ClassName { get; init; } = "";
    public string Output { get; init; } = "";
}
