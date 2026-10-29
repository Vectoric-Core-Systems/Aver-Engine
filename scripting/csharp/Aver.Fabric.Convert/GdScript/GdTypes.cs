// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The converter's view of a GDScript type: what C# type it becomes and what is known about it.

using Rs = Microsoft.CodeAnalysis;

namespace Aver.Fabric.Convert.GdScript;

internal enum GK
{
    Variant, Void, Null, Bool, Int, Float, String, StringName, NodePath,
    Array, Dict, Packed, Callable, Signal, Object, Struct, Enum, ScriptClass, Lambda,
}

/// <summary>The C# numeric type a GDScript int or float lives in. GDScript's own are long and double; the API's are often int and float.</summary>
internal enum Num { Int, Long, Float, Double }

/// <summary>An immutable description of a type. <see cref="Cs"/> is its C# spelling.</summary>
internal sealed class GdType
{
    public GK Kind;
    public Num Number = Num.Long;          // Int and Float kinds
    public GdType? Elem;                   // Array element, Packed element
    public GdType? Key, Value;             // Dict
    public bool Typed;                     // Array[T] / Dictionary[K, V] written (the C# type is the generic one)
    public Rs.INamedTypeSymbol? Sym;       // Object / Struct / Enum: the Aver.Fabric (or other referenced) type
    public ScriptInfo? Script;             // ScriptClass: a converted script class; also an Object whose class is a script
    public EnumInfo? EnumDef;              // Enum of a script
    public string Name = "";               // the C# spelling for kinds that carry one (an unresolved class, a nested name)
    public bool Weak;                      // the declared type is a guess (GetNode with no type): member misses become dynamic access
    public bool IsClassRef;                // the value is the class itself (Foo.new(), Foo.CONSTANT), not an instance
    public bool FromScene;                 // the type came from the scenes the script is attached to (Fabric/nodes.json): a real type, but a member miss is a dynamic access

    public static readonly GdType Variant = new() { Kind = GK.Variant, Name = "Variant" };
    public static readonly GdType Void = new() { Kind = GK.Void, Name = "void" };
    public static readonly GdType Null = new() { Kind = GK.Null, Name = "null" };
    public static readonly GdType Bool = new() { Kind = GK.Bool, Name = "bool" };
    public static readonly GdType Int = new() { Kind = GK.Int, Number = Num.Long };
    public static readonly GdType Int32 = new() { Kind = GK.Int, Number = Num.Int };
    public static readonly GdType Float = new() { Kind = GK.Float, Number = Num.Double };
    public static readonly GdType Float32 = new() { Kind = GK.Float, Number = Num.Float };
    public static readonly GdType String = new() { Kind = GK.String, Name = "string" };
    public static readonly GdType StringName = new() { Kind = GK.StringName, Name = "StringName" };
    public static readonly GdType NodePath = new() { Kind = GK.NodePath, Name = "NodePath" };
    public static readonly GdType Callable = new() { Kind = GK.Callable, Name = "Callable" };
    public static readonly GdType Signal = new() { Kind = GK.Signal, Name = "Signal" };
    public static readonly GdType UntypedArray = new() { Kind = GK.Array, Elem = Variant };
    public static readonly GdType UntypedDict = new() { Kind = GK.Dict, Key = Variant, Value = Variant };

    /// <summary>How the emitter's current class names a script class or an enum (simple inside its own family, qualified elsewhere).</summary>
    public static Func<ScriptInfo, string>? ScriptNamer;
    public static Func<EnumInfo, string>? EnumNamer;

    public bool IsVariant => Kind == GK.Variant;
    public bool IsNumeric => Kind is GK.Int or GK.Float;
    public bool IsReference => Kind is GK.Object or GK.ScriptClass or GK.Array or GK.Dict or GK.String or GK.Null or GK.Packed;
    public bool IsGodotObject => Kind is GK.Object or GK.ScriptClass;

    public static GdType ArrayOf(GdType elem) => new() { Kind = GK.Array, Elem = elem, Typed = !elem.IsVariant };
    public static GdType DictOf(GdType k, GdType v) => new() { Kind = GK.Dict, Key = k, Value = v, Typed = !(k.IsVariant && v.IsVariant) };
    public static GdType PackedOf(GdType elem, string cs) => new() { Kind = GK.Packed, Elem = elem, Name = cs };
    public static GdType Obj(Rs.INamedTypeSymbol sym, bool weak = false) =>
        new() { Kind = sym.IsValueType && sym.TypeKind != Rs.TypeKind.Enum ? GK.Struct : sym.TypeKind == Rs.TypeKind.Enum ? GK.Enum : GK.Object, Sym = sym, Weak = weak, Name = sym.Name };
    public static GdType OfScript(ScriptInfo s) => new() { Kind = GK.ScriptClass, Script = s };
    public static GdType OfEnum(EnumInfo e) => new() { Kind = GK.Enum, EnumDef = e, Name = e.CsPath };
    public static GdType Unresolved(string name) => new() { Kind = GK.Object, Name = name };
    public static GdType Lam(GdType ret) => new() { Kind = GK.Lambda, Value = ret, Name = "Callable" };

    public GdType WithWeak(bool weak) => Weak == weak ? this : (GdType)Clone(weak);

    private GdType Clone(bool weak)
    {
        var c = (GdType)MemberwiseClone();
        c.Weak = weak;
        return c;
    }

    public GdType AsScene()
    {
        if (FromScene) return this;
        var c = (GdType)MemberwiseClone();
        c.FromScene = true;
        return c;
    }

    public GdType AsClassRef()
    {
        var c = (GdType)MemberwiseClone();
        c.IsClassRef = true;
        return c;
    }

    public GdType AsClassRefOff()
    {
        if (!IsClassRef) return this;
        var c = (GdType)MemberwiseClone();
        c.IsClassRef = false;
        return c;
    }

    /// <summary>The C# spelling.</summary>
    public string Cs => Kind switch
    {
        GK.Variant => "Variant",
        GK.Void => "void",
        GK.Null => "object",
        GK.Bool => "bool",
        GK.Int => Number == Num.Int ? "int" : "long",
        GK.Float => Number == Num.Float ? "float" : "double",
        GK.String => "string",
        GK.StringName => "StringName",
        GK.NodePath => "NodePath",
        GK.Callable or GK.Lambda => "Callable",
        GK.Signal => "Signal",
        GK.Array => Typed && Elem != null ? "Collections.Array<" + Elem.Cs + ">" : "Collections.Array",
        GK.Dict => Typed && Key != null && Value != null ? "Collections.Dictionary<" + Key.Cs + ", " + Value.Cs + ">" : "Collections.Dictionary",
        GK.Packed => Name,
        GK.ScriptClass => ScriptNamer?.Invoke(Script!) ?? Script!.CsPath,
        GK.Enum => EnumDef != null ? (EnumNamer?.Invoke(EnumDef) ?? EnumDef.CsPath) : Sym != null ? Sym.ToDisplayString(CsFormat) : Name,
        GK.Object or GK.Struct => Sym != null ? Sym.ToDisplayString(CsFormat) : Name,
        _ => "Variant",
    };

    private static readonly Rs.SymbolDisplayFormat CsFormat = new(
        typeQualificationStyle: Rs.SymbolDisplayTypeQualificationStyle.NameAndContainingTypes,
        genericsOptions: Rs.SymbolDisplayGenericsOptions.IncludeTypeParameters,
        miscellaneousOptions: Rs.SymbolDisplayMiscellaneousOptions.UseSpecialTypes);

    public override string ToString() => Cs;

    /// <summary>True when a value of <paramref name="from"/> can be used where this type is expected without a conversion.</summary>
    public bool Accepts(GdType from)
    {
        if (ReferenceEquals(this, from)) return true;
        if (IsVariant) return true;
        if (from.IsVariant) return false;
        switch (Kind)
        {
            case GK.Int: return from.Kind == GK.Int && Widens(from.Number, Number);
            case GK.Float: return (from.Kind is GK.Int or GK.Float) && Widens(from.Number, Number);
            case GK.Bool: return from.Kind == GK.Bool;
            case GK.String: return from.Kind is GK.String or GK.Null or GK.StringName or GK.NodePath;
            case GK.StringName: return from.Kind is GK.StringName or GK.String or GK.Null;
            case GK.NodePath: return from.Kind is GK.NodePath or GK.String or GK.Null;
            case GK.Callable: return from.Kind is GK.Callable or GK.Lambda;
            case GK.Signal: return from.Kind == GK.Signal;
            case GK.Array: return from.Kind == GK.Null || (from.Kind == GK.Array && Cs == from.Cs);
            case GK.Dict: return from.Kind == GK.Null || (from.Kind == GK.Dict && Cs == from.Cs);
            case GK.Packed: return from.Kind == GK.Null || (from.Kind == GK.Packed && Cs == from.Cs);
            case GK.Enum: return from.Kind == GK.Enum && Cs == from.Cs;
            case GK.Struct: return from.Kind == GK.Struct && Cs == from.Cs;
            case GK.Object:
            case GK.ScriptClass:
                if (from.Kind == GK.Null) return true;
                if (!from.IsGodotObject) return false;
                return IsAssignableObject(from);
            default: return false;
        }
    }

    private bool IsAssignableObject(GdType from)
    {
        if (Cs == from.Cs) return true;
        // Script classes: walk the script chain, then the API base.
        if (from.Kind == GK.ScriptClass)
        {
            for (ScriptInfo? s = from.Script; s != null; s = s.BaseScript)
            {
                if (Kind == GK.ScriptClass && ReferenceEquals(Script, s)) return true;
                if (s.BaseScript == null && s.BaseApi != null && Sym != null && Derives(s.BaseApi, Sym)) return true;
            }
            return false;
        }
        return Sym != null && from.Sym != null && Derives(from.Sym, Sym);
    }

    internal static bool Derives(Rs.INamedTypeSymbol type, Rs.INamedTypeSymbol baseType)
    {
        for (Rs.INamedTypeSymbol? t = type; t != null; t = t.BaseType)
            if (Rs.SymbolEqualityComparer.Default.Equals(t, baseType)) return true;
        return false;
    }

    public static bool Widens(Num from, Num to) => from == to || (from, to) switch
    {
        (Num.Int, Num.Long or Num.Float or Num.Double) => true,
        (Num.Long, Num.Float or Num.Double) => true,
        (Num.Float, Num.Double) => true,
        _ => false,
    };
}
