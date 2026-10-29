// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The Aver.Fabric API as the converter sees it: Roslyn symbols read from the reference folder's assemblies, so type inference
// follows whatever runtime is staged (no table to keep in step) and a missing class or member shows up as a compile error.

using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

namespace Aver.Fabric.Convert.GdScript;

internal enum MemberKind { Property, Field, Method, Signal, Constant, EnumValue, NestedType }

/// <summary>One thing a GDScript name resolved to in the API.</summary>
internal sealed class ApiMember
{
    public MemberKind Kind;
    public string CsName = "";
    public GdType Type = GdType.Variant;
    public List<IMethodSymbol> Methods = new();
    public bool CanWrite;
    public bool IsStatic;
    public INamedTypeSymbol? Owner;
    public ISymbol? Symbol;
}

internal sealed class ApiModel
{
    private readonly CSharpCompilation _comp;
    private readonly Dictionary<string, INamedTypeSymbol?> _types = new();

    private ApiModel(CSharpCompilation comp) => _comp = comp;

    public static ApiModel Create(IEnumerable<MetadataReference> refs, IReadOnlyList<(string Path, string Text)>? extra = null)
    {
        var trees = (extra ?? Array.Empty<(string, string)>()).Select(e => CSharpSyntaxTree.ParseText(e.Text, path: e.Path)).ToList();
        return new ApiModel(CSharpCompilation.Create("FabricApi", trees, refs, new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary)));
    }

    // ---- types ----

    /// <summary>Aver.Fabric.&lt;name&gt; (nested names with a dot), else a System type used by name.</summary>
    public INamedTypeSymbol? Fabric(string name)
    {
        if (_types.TryGetValue(name, out INamedTypeSymbol? hit)) return hit;
        // GDScript spells the integer vectors and rectangle Vector2i / Vector3i / Vector4i / Rect2i; GodotSharp (and so Aver.Fabric) spells them with a capital I.
        string lookup = name is "Vector2i" or "Vector3i" or "Vector4i" or "Rect2i" ? name[..^1] + "I" : name;
        INamedTypeSymbol? t = _comp.GetTypeByMetadataName("Aver.Fabric." + lookup.Replace('.', '+'));
        _types[name] = t;
        return t;
    }

    public INamedTypeSymbol? Collections(string name) => _comp.GetTypeByMetadataName("Aver.Fabric.Collections." + name);
    public INamedTypeSymbol? Meta(string metadataName) => _comp.GetTypeByMetadataName(metadataName);

    private INamedTypeSymbol? _godotObject;
    public INamedTypeSymbol? GodotObject => _godotObject ??= Fabric("GodotObject");

    /// <summary>Maps a C# type of the API to the converter's type.</summary>
    public GdType FromSymbol(ITypeSymbol? t)
    {
        if (t == null) return GdType.Variant;
        switch (t.SpecialType)
        {
            case SpecialType.System_Void: return GdType.Void;
            case SpecialType.System_Boolean: return GdType.Bool;
            case SpecialType.System_SByte: case SpecialType.System_Byte: case SpecialType.System_Int16: case SpecialType.System_UInt16:
            case SpecialType.System_Int32: return GdType.Int32;
            case SpecialType.System_UInt32: case SpecialType.System_Int64: case SpecialType.System_UInt64: return GdType.Int;
            case SpecialType.System_Single: return GdType.Float32;
            case SpecialType.System_Double: return GdType.Float;
            case SpecialType.System_String: return GdType.String;
            case SpecialType.System_Object: return GdType.Variant;
        }
        if (t is IArrayTypeSymbol arr) return GdType.PackedOf(FromSymbol(arr.ElementType), t.ToDisplayString(s_cs));
        if (t is not INamedTypeSymbol n) return GdType.Variant;
        if (n.TypeKind == TypeKind.Enum) return GdType.Obj(n);
        string full = n.OriginalDefinition.ToDisplayString();
        switch (full)
        {
            case "Aver.Fabric.Variant": return GdType.Variant;
            case "Aver.Fabric.StringName": return GdType.StringName;
            case "Aver.Fabric.NodePath": return GdType.NodePath;
            case "Aver.Fabric.Callable": return GdType.Callable;
            case "Aver.Fabric.Signal": return GdType.Signal;
            case "Aver.Fabric.Collections.Array": return GdType.UntypedArray;
            case "Aver.Fabric.Collections.Array<T>": return GdType.ArrayOf(FromSymbol(n.TypeArguments[0]));
            case "Aver.Fabric.Collections.Dictionary": return GdType.UntypedDict;
            case "Aver.Fabric.Collections.Dictionary<TKey, TValue>": return GdType.DictOf(FromSymbol(n.TypeArguments[0]), FromSymbol(n.TypeArguments[1]));
        }
        if (n.IsGenericType && n.TypeArguments.Any(a => a.TypeKind == TypeKind.TypeParameter)) return GdType.Variant;
        if (n.TypeKind is TypeKind.Class or TypeKind.Struct or TypeKind.Interface) return GdType.Obj(n);
        return GdType.Variant;
    }

    private static readonly SymbolDisplayFormat s_cs = new(
        typeQualificationStyle: SymbolDisplayTypeQualificationStyle.NameAndContainingTypes,
        genericsOptions: SymbolDisplayGenericsOptions.IncludeTypeParameters,
        miscellaneousOptions: SymbolDisplayMiscellaneousOptions.UseSpecialTypes);

    public static bool IsGodotDerived(INamedTypeSymbol? t)
    {
        for (; t != null; t = t.BaseType) if (t.Name == "GodotObject" && t.ContainingNamespace?.ToDisplayString() == "Aver.Fabric") return true;
        return false;
    }

    // ---- members ----

    private static IEnumerable<INamedTypeSymbol> Chain(INamedTypeSymbol? t)
    {
        for (; t != null; t = t.BaseType) yield return t;
    }

    private static bool Visible(ISymbol s) => s.DeclaredAccessibility is Accessibility.Public or Accessibility.Protected or Accessibility.ProtectedOrInternal;

    private static string Fold(string s) => s.Replace("_", "").ToLowerInvariant();

    /// <summary>The candidate C# names for a GDScript identifier: Godot's snake_case turned Pascal, then a case- and underscore-blind match.</summary>
    private static IEnumerable<string> Candidates(string gd)
    {
        yield return Naming.Pascal(gd);
        if (gd.StartsWith('_')) yield return "_" + Naming.Pascal(gd);
    }

    // Pascal(snake_case) first; a case- and underscore-blind match only between names that are both private-looking or both not
    // (so the identifier Input is the class, not the virtual _Input).
    private static bool NameMatches(ISymbol s, string gd, string pascal) =>
        s.Name == pascal || (s.Name.Length == pascal.Length && string.Equals(s.Name, pascal, StringComparison.OrdinalIgnoreCase))
        || (s.Name.StartsWith('_') == gd.StartsWith('_') && Fold(s.Name) == Fold(gd));

    /// <summary>Looks a GDScript member name up on a type: property, field, method group, signal, constant. Null when absent.</summary>
    public ApiMember? Lookup(INamedTypeSymbol? type, string gd, bool wantStatic = false)
    {
        if (type == null) return null;
        string pascal = Naming.Pascal(gd);
        var methods = new List<IMethodSymbol>();
        foreach (INamedTypeSymbol t in Chain(type))
        {
            foreach (ISymbol m in t.GetMembers())
            {
                if (!Visible(m) || !NameMatches(m, gd, pascal)) continue;
                switch (m)
                {
                    case IPropertySymbol p when !p.IsIndexer:
                        return new ApiMember
                        {
                            Kind = MemberKind.Property, CsName = p.Name, Type = FromSymbol(p.Type), CanWrite = p.SetMethod != null && Visible(p.SetMethod),
                            IsStatic = p.IsStatic, Owner = t, Symbol = p,
                        };
                    case IFieldSymbol f when f.IsConst:
                        return new ApiMember { Kind = MemberKind.Constant, CsName = f.Name, Type = FromSymbol(f.Type), IsStatic = true, Owner = t, Symbol = f };
                    case IFieldSymbol f:
                        return new ApiMember { Kind = MemberKind.Field, CsName = f.Name, Type = FromSymbol(f.Type), CanWrite = !f.IsReadOnly, IsStatic = f.IsStatic, Owner = t, Symbol = f };
                    case IMethodSymbol ms when ms.MethodKind == MethodKind.Ordinary:
                        if (!methods.Any(x => x.Name == ms.Name && SameSignature(x, ms))) methods.Add(ms);
                        break;
                    case INamedTypeSymbol nested when nested.TypeKind is TypeKind.Enum or TypeKind.Class or TypeKind.Struct:
                        if (methods.Count == 0) return new ApiMember { Kind = MemberKind.NestedType, CsName = nested.Name, Owner = t, Symbol = nested, Type = GdType.Obj(nested), IsStatic = true };
                        break;
                }
            }
        }
        if (methods.Count > 0)
            return new ApiMember { Kind = MemberKind.Method, CsName = methods[0].Name, Methods = methods, IsStatic = methods.All(x => x.IsStatic), Owner = methods[0].ContainingType, Symbol = methods[0] };
        return null;
    }

    private static bool SameSignature(IMethodSymbol a, IMethodSymbol b)
    {
        if (a.Parameters.Length != b.Parameters.Length || a.TypeParameters.Length != b.TypeParameters.Length) return false;
        for (int i = 0; i < a.Parameters.Length; i++)
            if (!SymbolEqualityComparer.Default.Equals(a.Parameters[i].Type, b.Parameters[i].Type)) return false;
        return true;
    }

    /// <summary>A signal of the type (or a base): the nested SignalName class declares a field for it.</summary>
    public bool IsSignal(INamedTypeSymbol? type, string gd, out string csName)
    {
        csName = Naming.Pascal(gd);
        foreach (INamedTypeSymbol t in Chain(type))
            foreach (INamedTypeSymbol n in t.GetTypeMembers("SignalName"))
                foreach (IFieldSymbol f in n.GetMembers().OfType<IFieldSymbol>())
                    if (f.Name == csName || Fold(f.Name) == Fold(gd)) { csName = f.Name; return true; }
        return false;
    }

    /// <summary>A constant written SCREAMING_SNAKE in GDScript (Input.MOUSE_MODE_CAPTURED, KEY_SPACE) found in a nested enum of the type
    /// or its bases, or among the global enums: the enum and its member.</summary>
    public bool FindEnumConstant(INamedTypeSymbol? type, string gd, out INamedTypeSymbol? enumType, out string member)
    {
        enumType = null;
        member = "";
        string pascal = Naming.Pascal(gd);
        foreach (INamedTypeSymbol t in Chain(type))
            foreach (INamedTypeSymbol e in t.GetTypeMembers().Where(x => x.TypeKind == TypeKind.Enum))
                if (MatchEnum(e, pascal, out member)) { enumType = e; return true; }
        return false;
    }

    private static readonly string[] s_globalEnums = { "Key", "MouseButton", "JoyButton", "JoyAxis", "Error", "Side", "Corner", "Orientation", "HorizontalAlignment", "VerticalAlignment", "KeyModifierMask", "MouseButtonMask", "PropertyHint" };

    public bool FindGlobalEnumConstant(string gd, out INamedTypeSymbol? enumType, out string member)
    {
        enumType = null;
        member = "";
        if (gd == "OK") return Fixed("Error", "Ok", out enumType, out member);
        if (gd == "FAILED") return Fixed("Error", "Failed", out enumType, out member);
        string pascal = Naming.Pascal(gd);
        foreach (string name in s_globalEnums)
        {
            INamedTypeSymbol? e = Fabric(name);
            if (e != null && e.TypeKind == TypeKind.Enum && MatchEnum(e, pascal, out member, stripPrefix: true)) { enumType = e; return true; }
        }
        if (gd.StartsWith("TYPE_", StringComparison.Ordinal) && Fabric("Variant.Type") is { } vt && MatchEnum(vt, "Type" + Naming.Pascal(gd[5..]), out member)) { enumType = vt; return true; }
        return false;
    }

    private bool Fixed(string enumName, string member, out INamedTypeSymbol? enumType, out string m)
    {
        enumType = Fabric(enumName);
        m = member;
        return enumType != null;
    }

    private static bool MatchEnum(INamedTypeSymbol e, string pascal, out string member, bool stripPrefix = true)
    {
        member = "";
        string prefix = e.Name.EndsWith("Enum", StringComparison.Ordinal) ? e.Name[..^4] : e.Name;
        if (stripPrefix && pascal.StartsWith(prefix, StringComparison.Ordinal) && pascal.Length > prefix.Length)
        {
            string rest = pascal[prefix.Length..];
            IFieldSymbol? f = e.GetMembers().OfType<IFieldSymbol>().FirstOrDefault(x => x.Name == rest);
            if (f != null) { member = f.Name; return true; }
        }
        IFieldSymbol? exact = e.GetMembers().OfType<IFieldSymbol>().FirstOrDefault(x => x.Name == pascal && x.HasConstantValue);
        if (exact != null && !stripPrefix) { member = exact.Name; return true; }
        return false;
    }

    /// <summary>The virtual a GDScript function of that name overrides in the API base chain: the C# name and the method.</summary>
    public IMethodSymbol? FindVirtual(INamedTypeSymbol? baseType, string gd)
    {
        string name = gd.StartsWith('_') ? Naming.VirtualName(gd) : Naming.MethodName(gd);
        foreach (INamedTypeSymbol t in Chain(baseType))
            foreach (ISymbol m in t.GetMembers(name))
                if (m is IMethodSymbol ms && (ms.IsVirtual || ms.IsAbstract || ms.IsOverride) && Visible(ms)) return ms;
        return null;
    }

    public List<IMethodSymbol> Constructors(INamedTypeSymbol? t) =>
        t == null ? new() : t.Constructors.Where(c => !c.IsStatic && c.DeclaredAccessibility == Accessibility.Public).ToList();

    /// <summary>Operator methods (op_Multiply ...) of the types, for typed arithmetic on structs.</summary>
    public IEnumerable<IMethodSymbol> Operators(string opName, params INamedTypeSymbol?[] types)
    {
        foreach (INamedTypeSymbol? t in types)
            if (t != null)
                foreach (IMethodSymbol m in t.GetMembers(opName).OfType<IMethodSymbol>()) yield return m;
    }

    public bool HasImplicitConversion(INamedTypeSymbol? from, string toFabricName) =>
        from != null && from.GetMembers("op_Implicit").OfType<IMethodSymbol>().Any(m => m.ReturnType.Name == toFabricName);

    /// <summary>Methods and properties named like <paramref name="csName"/> found in the nested type chain (for SignalName lookups).</summary>
    public bool HasMember(INamedTypeSymbol? type, string csName) => Chain(type).Any(t => t.GetMembers(csName).Any());
}
