// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The GDScript to C# emitter: infrastructure shared by the class, statement, expression and call parts (docs/plans/fabric/G3_SCRIPTS.md).
// An expression is typed while it is written (Ex carries the C# text and the type), so inference and emission cannot disagree.

using System.Text;
using Rs = Microsoft.CodeAnalysis;

namespace Aver.Fabric.Convert.GdScript;

/// <summary>C# operator precedence levels, for parenthesising.</summary>
internal static class P
{
    public const int Assign = 0, Cond = 1, OrOr = 2, AndAnd = 3, BitOr = 4, BitXor = 5, BitAnd = 6, Eq = 7, Rel = 8, Shift = 9, Add = 10, Mul = 11, Unary = 12, Primary = 13;
}

/// <summary>An emitted expression: its C# text, its type and what else the next step needs to know.</summary>
internal sealed class Ex
{
    public string Code = "";
    public GdType Type = GdType.Variant;
    public int Prec = P.Primary;
    public object? Const;                  // long, double, string or bool when the expression is a literal
    public bool IsConstExpr;               // a C# compile-time constant
    public bool Addressable;               // a local, field or element: a struct member can be assigned in place
    public bool UsesInstance;              // reads instance state (this)
    public SignalRef? Signal;
    public MemberInfo? Func;               // a script function used as a value / being called
    public ApiMember? Api;                 // an API member group used as a value / being called
    public string? Receiver;               // for Func / Api groups and signals: the code of the object ("" for implicit this)
    public bool IsSuper;
    public string? SetTemplate;            // assignment is a call, not `target = v`: the template, \u0001 standing for the value
    public Ex? Parent;                     // for a field of a struct: the struct expression (assignment may need `P = P with { f = v }`)
    public string FieldCs = "";
    public bool IsAwaitable;               // the expression is a Task / SignalAwaiter the next await handles
    public Node? Provisional;              // the untyped variable (VarDecl or Param) whose type was inferred from its first value
    public bool IsDynamic;                 // a member read/written by name at run time
    public bool FromName;                  // a named constant: its Const value is for folding only, the code keeps the name

    public Ex() { }
    public Ex(string code, GdType type, int prec = P.Primary) { Code = code; Type = type; Prec = prec; }
    public override string ToString() => Code;
}

internal sealed class SignalRef
{
    public string Owner = "";            // code of the emitting object, "" for this
    public string NameCode = "";         // StringName expression
    public ScriptInfo? Script;           // the declaring script, when it is one of ours
    public SignalDecl? Decl;
    public string? EmitHelper;           // EmitSignalHit for our own signals
    public List<GdType> ParamTypes = new();
    public GdType OwnerType = GdType.Variant;
}

internal sealed class Local
{
    public string Gd = "";
    public string Cs = "";
    public GdType Type = GdType.Variant;
    public Node? Decl;
    public bool IsConstant;
    public bool Provisional;               // untyped, typed from its first value: a contradicting assignment widens or frees it
}

/// <summary>Indenting writer. A multi-line piece of code (a lambda body) is indented line by line.</summary>
internal sealed class CodeWriter
{
    private readonly StringBuilder _sb = new();
    public int Indent;
    public readonly List<string> Pending = new();

    public void Line(string text = "", string? trailing = null)
    {
        if (text.Length == 0 && trailing == null) { _sb.Append('\n'); return; }
        string[] parts = text.Split('\n');
        for (int i = 0; i < parts.Length; i++)
        {
            if (parts[i].Length > 0) _sb.Append(' ', Indent * 4).Append(parts[i]);
            if (i == parts.Length - 1)
            {
                if (trailing != null) _sb.Append(trailing);
                else if (Pending.Count > 0) { _sb.Append("  // ").Append(string.Join("; ", Pending)); Pending.Clear(); }
            }
            _sb.Append('\n');
        }
    }

    public void Open(string header = "")
    {
        if (header.Length > 0) Line(header);
        Line("{");
        Indent++;
    }

    public void Close(string tail = "") { Indent--; Line("}" + tail); }

    public override string ToString() => _sb.ToString();
    public bool IsEmpty => _sb.Length == 0;
}

internal sealed class EmitNote
{
    public int Tag;
    public string Type = "";
    public string Property = "";
    public Rating Rating;
    public string Reason = "";
    public int GdLine;
}

/// <summary>What a function body needs while it is written.</summary>
internal sealed class Ctx
{
    public ScriptInfo Script = null!;
    public MemberInfo? Func;
    public bool IsStatic;
    public bool IsAsync;
    public GdType Return = GdType.Variant;
    public bool InferReturn;                      // the return type is being inferred: collect, don't adapt
    public List<GdType> ReturnTypes = new();
    public bool ReturnsWithoutValue;
    public readonly List<Dictionary<string, Local>> Scopes = new();
    public readonly HashSet<string> Used = new(StringComparer.Ordinal);
    public Dictionary<Node, string> DeclNames = new();
    public Block? BodyRoot;                       // the function body: where an untyped local's assignments are looked for
    public MemberInfo? InAccessor;                // inside this variable's own setter/getter: its name means the backing field
    public bool UsedInstance;                     // an instance member or this was referenced
    public bool IsInitializer;                    // writing a field initialiser: instance references force a constructor assignment
    public Dictionary<string, (string Pattern, GdType Type)> Narrow = new();
}

internal sealed partial class GdEmitter
{
    private readonly ProjectModel _m;
    private ApiModel? Api => _m.Api;
    private readonly List<EmitNote> _notes = new();
    private int _noteTag;
    private int _dry;
    // Untyped variables are typed from their first value; when an assignment says otherwise the script is written again with the
    // variable widened (int to float) or left a Variant. _forced holds those decisions, _forceNew the ones found in this round.
    private readonly Dictionary<Node, GdType> _forced = new();
    private readonly Dictionary<Node, GdType> _forceNew = new();
    private ScriptInfo _script = null!;
    private Ctx _ctx = new();
    private CodeWriter _w = new();

    public IReadOnlyList<EmitNote> Notes => _notes;
    public bool UsesCollectionsAlias { get; private set; }

    public GdEmitter(ProjectModel model)
    {
        _m = model;
        GdType.ScriptNamer = QualifiedScript;
        GdType.EnumNamer = QualifiedEnum;
    }

    // ---- notes ----

    private void Note(Node? at, string type, string property, Rating rating, string reason)
    {
        if (_dry > 0) return;
        var n = new EmitNote { Tag = ++_noteTag, Type = type, Property = property, Rating = rating, Reason = reason, GdLine = at?.Start.Line ?? 0 };
        _notes.Add(n);
        string text = reason.Replace("\r", " ").Replace("\n", " ");
        if (text.Length > 200) text = text[..197] + "...";
        _w.Pending.Add($"FABRIC[{n.Tag}]: {text}");
    }

    // ---- dry runs: type an expression without leaving traces ----

    private readonly struct DryScope : IDisposable
    {
        private readonly GdEmitter _e;
        private readonly HashSet<string> _used;
        private readonly int _scopes;
        private readonly CodeWriter _w;
        private readonly Ctx _ctx;
        private readonly bool _usedInstance;
        private readonly Dictionary<string, (string, GdType)> _narrow;

        public DryScope(GdEmitter e)
        {
            _e = e;
            _ctx = e._ctx;
            _used = new HashSet<string>(e._ctx.Used, StringComparer.Ordinal);
            _scopes = e._ctx.Scopes.Count;
            _w = e._w;
            _usedInstance = e._ctx.UsedInstance;
            _narrow = new Dictionary<string, (string, GdType)>(e._ctx.Narrow);
            e._dry++;
            e._w = new CodeWriter();
        }

        public void Dispose()
        {
            _e._dry--;
            _e._w = _w;
            _e._ctx = _ctx;
            _ctx.Used.Clear();
            foreach (string u in _used) _ctx.Used.Add(u);
            while (_ctx.Scopes.Count > _scopes) _ctx.Scopes.RemoveAt(_ctx.Scopes.Count - 1);
            _ctx.UsedInstance = _usedInstance;
            _ctx.Narrow = _narrow;
        }
    }

    private GdType TypeOf(Expr e, GdType? expected = null)
    {
        using var _ = new DryScope(this);
        return Expr(e, expected).Type;
    }

    // ---- names ----

    private string Fresh(string preferred)
    {
        string bare = preferred;
        for (int i = 2; _ctx.Used.Contains(bare); i++) bare = preferred + i;
        _ctx.Used.Add(bare);
        return Naming.Safe(bare);
    }

    private Local? FindLocal(string gd)
    {
        for (int i = _ctx.Scopes.Count - 1; i >= 0; i--)
            if (_ctx.Scopes[i].TryGetValue(gd, out Local? l)) return l;
        return null;
    }

    private Local Declare(string gd, string cs, GdType type, Node? decl = null, bool constant = false)
    {
        var l = new Local { Gd = gd, Cs = cs, Type = type, Decl = decl, IsConstant = constant };
        _ctx.Scopes[^1][gd] = l;
        return l;
    }

    private void PushScope() => _ctx.Scopes.Add(new Dictionary<string, Local>());
    private void PopScope() => _ctx.Scopes.RemoveAt(_ctx.Scopes.Count - 1);

    // ---- type references ----

    private static readonly Dictionary<string, GdType> s_builtinTypes = new()
    {
        ["int"] = GdType.Int, ["float"] = GdType.Float, ["bool"] = GdType.Bool, ["String"] = GdType.String, ["StringName"] = GdType.StringName,
        ["NodePath"] = GdType.NodePath, ["Variant"] = GdType.Variant, ["void"] = GdType.Void, ["Callable"] = GdType.Callable, ["Signal"] = GdType.Signal,
        ["PackedStringArray"] = GdType.PackedOf(GdType.String, "string[]"), ["PackedInt32Array"] = GdType.PackedOf(GdType.Int32, "int[]"),
        ["PackedInt64Array"] = GdType.PackedOf(GdType.Int, "long[]"), ["PackedFloat32Array"] = GdType.PackedOf(GdType.Float32, "float[]"),
        ["PackedFloat64Array"] = GdType.PackedOf(GdType.Float, "double[]"), ["PackedByteArray"] = GdType.PackedOf(GdType.Int32, "byte[]"),
    };

    private GdType ResolveType(TypeRef? t, ScriptInfo? from = null)
    {
        if (t == null) return GdType.Variant;
        from ??= _script;
        if (t.Parts.Count == 1)
        {
            string n = t.Parts[0];
            if (s_builtinTypes.TryGetValue(n, out GdType? b)) return b;
            if (n == "Array") return t.Args.Count == 1 ? GdType.ArrayOf(ResolveType(t.Args[0], from)) : GdType.UntypedArray;
            if (n == "Dictionary") return t.Args.Count == 2 ? GdType.DictOf(ResolveType(t.Args[0], from), ResolveType(t.Args[1], from)) : GdType.UntypedDict;
            if (n == "Object") return Obj("GodotObject");
            if (n == "PackedVector3Array") return GdType.PackedOf(Obj("Vector3"), "Vector3[]");
            if (n == "PackedVector2Array") return GdType.PackedOf(Obj("Vector2"), "Vector2[]");
            if (n == "PackedColorArray") return GdType.PackedOf(Obj("Color"), "Color[]");
            if (n == "RID") return Obj("Rid");
        }
        // Script classes, inner classes and enums: the dotted path.
        GdType? script = ResolveScriptPath(t.Parts, from);
        if (script != null) return script;
        string full = string.Join(".", t.Parts);
        return Obj(full);
    }

    private GdType Obj(string apiName)
    {
        Rs.INamedTypeSymbol? sym = Api?.Fabric(apiName);
        return sym != null ? GdType.Obj(sym) : GdType.Unresolved(apiName);
    }

    private GdType? ResolveScriptPath(IReadOnlyList<string> parts, ScriptInfo from)
    {
        ScriptInfo? cur = null;
        int i = 0;
        // The first name: a member of this script or an enclosing one (inner class, enum), else a global class_name.
        for (ScriptInfo? o = from; o != null && cur == null; o = o.Outer)
        {
            foreach (ScriptInfo chain in o.Chain())
            {
                if (chain.ByName.TryGetValue(parts[0], out MemberInfo? mi))
                {
                    if (mi.Kind == MK.Class) { cur = mi.Inner; break; }
                    if (mi.Kind == MK.Enum && parts.Count == 1) return GdType.OfEnum(mi.Enum!);
                }
            }
            if (cur == null && o.Decl.Name == parts[0] && !o.IsInner) cur = o;
        }
        if (cur == null && _m.ByClassName.TryGetValue(parts[0], out ScriptInfo? g)) cur = g;
        if (cur == null) return null;
        for (i = 1; i < parts.Count; i++)
        {
            MemberInfo? mi = cur.Find(parts[i]);
            if (mi == null) return null;
            if (mi.Kind == MK.Class) cur = mi.Inner!;
            else if (mi.Kind == MK.Enum && i == parts.Count - 1) return GdType.OfEnum(mi.Enum!);
            else return null;
        }
        return GdType.OfScript(cur);
    }

    // ---- default values ----

    private string DefaultValue(GdType t) => t.Kind switch
    {
        GK.Int => "0",
        GK.Float => t.Number == Num.Float ? "0f" : "0.0",
        GK.Bool => "false",
        GK.String => "\"\"",
        GK.Array or GK.Dict => "new()",
        GK.Struct => t.Sym?.Name switch
        {
            "Quaternion" => "Quaternion.Identity", "Basis" => "Basis.Identity", "Transform3D" => "Transform3D.Identity",
            "Transform2D" => "Transform2D.Identity", "Projection" => "Projection.Identity", "Color" => "new Color(0, 0, 0, 1)", _ => "default",
        },
        _ => "default",
    };

    // ---- adapting an expression to a wanted type ----

    private static string Wrap(Ex e, int min) => e.Prec >= min ? e.Code : "(" + e.Code + ")";

    private string VariantFromTyped(Ex e)
    {
        switch (e.Type.Kind)
        {
            case GK.Enum: return "(long)" + Wrap(e, P.Unary);
            case GK.Array when e.Type.Typed:
            case GK.Dict when e.Type.Typed:
                return "Variant.From(" + e.Code + ")";
            case GK.Lambda: return e.Code;
            case GK.Null: return "default";
            default: return e.Code;
        }
    }

    private static readonly Dictionary<string, string> s_asMethods = new()
    {
        ["Vector2"] = "AsVector2", ["Vector2I"] = "AsVector2I", ["Rect2"] = "AsRect2", ["Rect2I"] = "AsRect2I", ["Vector3"] = "AsVector3",
        ["Vector3I"] = "AsVector3I", ["Transform2D"] = "AsTransform2D", ["Vector4"] = "AsVector4", ["Vector4I"] = "AsVector4I", ["Plane"] = "AsPlane",
        ["Quaternion"] = "AsQuaternion", ["Aabb"] = "AsAabb", ["Basis"] = "AsBasis", ["Transform3D"] = "AsTransform3D", ["Projection"] = "AsProjection",
        ["Color"] = "AsColor", ["Rid"] = "AsRid",
    };

    /// <summary>Code that reads a Variant as the given type.</summary>
    private string FromVariant(string code, GdType to)
    {
        string v = code;
        switch (to.Kind)
        {
            case GK.Variant: return code;
            case GK.Int: return to.Number == Num.Int ? v + ".AsInt32()" : v + ".AsInt64()";
            case GK.Float: return to.Number == Num.Float ? v + ".AsSingle()" : v + ".AsDouble()";
            case GK.Bool: return v + ".AsBool()";
            case GK.String: return v + ".AsString()";
            case GK.StringName: return v + ".AsStringName()";
            case GK.NodePath: return v + ".AsNodePath()";
            case GK.Callable or GK.Lambda: return v + ".AsCallable()";
            case GK.Signal: return v + ".AsSignal()";
            case GK.Array: return to.Typed ? v + ".As<" + to.Cs + ">()" : v + ".AsGodotArray()";
            case GK.Dict: return to.Typed ? v + ".As<" + to.Cs + ">()" : v + ".AsGodotDictionary()";
            case GK.Struct: return s_asMethods.TryGetValue(to.Sym?.Name ?? "", out string? m) ? v + "." + m + "()" : v + ".As<" + to.Cs + ">()";
            case GK.Enum: return "(" + to.Cs + ")" + v + ".AsInt64()";
            case GK.Packed:
                return to.Name switch
                {
                    "string[]" => v + ".AsStringArray()", "int[]" => v + ".AsInt32Array()", "long[]" => v + ".AsInt64Array()", "float[]" => v + ".AsFloat32Array()",
                    "double[]" => v + ".AsFloat64Array()", "byte[]" => v + ".AsByteArray()", "Vector3[]" => v + ".AsVector3Array()", "Vector2[]" => v + ".AsVector2Array()",
                    "Color[]" => v + ".AsColorArray()", _ => v + ".As<" + to.Name + ">()",
                };
            default: return v + ".As<" + to.Cs + ">()";
        }
    }

    /// <summary>The expression's code, converted so a variable or parameter of <paramref name="to"/> accepts it.</summary>
    private string Adapt(Ex e, GdType? to)
    {
        if (to == null || to.Kind == GK.Void) return e.Code;
        GdType from = e.Type;
        if (to.IsVariant) return from.IsVariant ? e.Code : VariantFromTyped(e);
        if (from.IsVariant) return FromVariant(Wrap(e, P.Primary), to);

        if (to.Kind == GK.Float && from.Kind is GK.Int or GK.Float or GK.Enum && e.Const != null && e.Const is not string && !e.FromName)
            return ConstNumber(e, to);
        if (to.Kind == GK.Int && from.Kind == GK.Int && e.Const is long cl && !e.FromName &&
            (to.Number == Num.Long || (to.Number == Num.Int && cl >= int.MinValue && cl <= int.MaxValue))) return e.Code;

        if (to.IsNumeric && (from.IsNumeric || from.Kind == GK.Enum))
        {
            if (from.Kind == GK.Enum) return to.Kind == GK.Float ? "(double)" + Wrap(e, P.Unary) : "(" + to.Cs + ")" + Wrap(e, P.Unary);
            if (to.Kind == GK.Int && from.Kind == GK.Float) return "(" + to.Cs + ")" + Wrap(e, P.Unary);
            if (to.Accepts(from)) return e.Code;
            return "(" + to.Cs + ")" + Wrap(e, P.Unary);
        }
        if (to.Kind == GK.Enum && from.Kind == GK.Int) return e.Const is long ? "(" + to.Cs + ")" + e.Code : "(" + to.Cs + ")" + Wrap(e, P.Unary);
        if (to.Kind == GK.Bool && from.Kind != GK.Bool) return Truth(e);
        if (to.Kind == GK.String && from.Kind == GK.NodePath) return e.Code;
        if (to.Kind == GK.StringName && from.Kind == GK.NodePath) return "(string)" + Wrap(e, P.Unary);
        if (to.IsGodotObject && from.IsGodotObject && !to.Accepts(from) && to.Cs != "GodotObject")
            return "(" + to.Cs + ")" + Wrap(e, P.Unary);
        return e.Code;
    }

    private string ConstNumber(Ex e, GdType to)
    {
        double d = e.Const switch { long l => l, double x => x, _ => 0 };
        if (to.Number == Num.Float) return e.Const is long ? d.ToString("R", System.Globalization.CultureInfo.InvariantCulture) + "f" : FloatSuffix(Naming.Num(d));
        return e.Const is long ? Naming.Num(d) : e.Code;
    }

    private static string FloatSuffix(string num) =>
        num.StartsWith("double.", StringComparison.Ordinal) ? "float." + num[7..] : num + "f";

    /// <summary>A bool from any value, by GDScript's truthiness.</summary>
    private string Truth(Ex e)
    {
        GdType t = e.Type;
        switch (t.Kind)
        {
            case GK.Bool: return e.Code;
            case GK.Int: return Wrap(e, P.Eq + 1) + " != 0";
            case GK.Float: return Wrap(e, P.Eq + 1) + " != 0.0";
            case GK.String: return "!string.IsNullOrEmpty(" + e.Code + ")";
            case GK.StringName: return "!string.IsNullOrEmpty(" + e.Code + ")";
            case GK.NodePath: return "!string.IsNullOrEmpty((string)" + Wrap(e, P.Unary) + ")";
            case GK.Variant: return Wrap(e, P.Primary) + ".Booleanize()";
            case GK.Array or GK.Dict: return Wrap(e, P.Primary) + ".Count > 0";
            case GK.Packed: return Wrap(e, P.Primary) + ".Length > 0";
            case GK.Struct:
                string n = t.Sym?.Name ?? "";
                return n switch
                {
                    "Vector2" or "Vector3" or "Vector4" or "Vector2I" or "Vector3I" or "Vector4I" => Wrap(e, P.Eq + 1) + " != " + n + ".Zero",
                    "Quaternion" or "Basis" or "Transform3D" or "Transform2D" or "Projection" => Wrap(e, P.Eq + 1) + " != " + n + ".Identity",
                    _ => "GDScript.Truthy(" + VariantFromTyped(e) + ")",
                };
            case GK.Object or GK.ScriptClass:
                return t.IsGodotObject && IsNodeLike(t) ? "IsInstanceValid(" + e.Code + ")" : Wrap(e, P.Eq + 1) + " != null";
            case GK.Callable or GK.Lambda: return "!" + Wrap(e, P.Primary) + ".IsNull";
            case GK.Signal: return "!" + Wrap(e, P.Primary) + ".IsNull";
            case GK.Null: return "false";
            case GK.Enum: return "(long)" + Wrap(e, P.Unary) + " != 0";
            default: return "GDScript.Truthy(" + VariantFromTyped(e) + ")";
        }
    }

    // A freed Node is falsy in GDScript; IsInstanceValid is a static of GodotObject, in scope inside a script class.
    private bool IsNodeLike(GdType t)
    {
        Rs.INamedTypeSymbol? sym = t.Kind == GK.ScriptClass ? t.Script?.ApiRoot : t.Sym;
        Rs.INamedTypeSymbol? node = Api?.Fabric("Node");
        if (sym == null || node == null) return !_ctx.IsStatic;
        return GdType.Derives(sym, node) && _ctx.Script != null && !_ctx.IsStatic;
    }

    private static string Lit(string s) => Naming.Quote(s);

    /// <summary>Removes one pair of parentheses that wraps the whole expression (for places that need no precedence: a return, an initialiser).</summary>
    private static string Unparen(string code)
    {
        if (code.Length < 2 || code[0] != '(' || code[^1] != ')') return code;
        int depth = 0;
        bool inString = false;
        for (int i = 0; i < code.Length; i++)
        {
            char c = code[i];
            if (inString) { if (c == '\\') i++; else if (c == '"') inString = false; continue; }
            if (c == '"') inString = true;
            else if (c == '(') depth++;
            else if (c == ')' && --depth == 0) return i == code.Length - 1 ? code[1..^1] : code;
        }
        return code;
    }
}
