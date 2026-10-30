// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// What GodotSharp's source generators add to a script class and Aver.Fabric has no generator for, written out as a partial class:
// the nested SignalName / MethodName / PropertyName name tables, a C# event per [Signal] delegate and an EmitSignalX helper.

using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace Aver.Fabric.Convert;

internal sealed class GeneratedPlan
{
    public SyntaxTree Tree = null!;
    public ClassDeclarationSyntax Anchor = null!;
    public string TypeName = "";
    public string Text = "";
    public int Signals;
    public int Methods;
    public int Properties;
}

internal static class GeneratedMembers
{
    private static readonly SymbolDisplayFormat s_full = SymbolDisplayFormat.FullyQualifiedFormat;

    private static string Id(string name) => SyntaxFacts.GetKeywordKind(name) != SyntaxKind.None ? "@" + name : name;

    private static bool InheritsGodotObject(INamedTypeSymbol type)
    {
        for (INamedTypeSymbol? b = type.BaseType; b != null; b = b.BaseType)
            if (b.Name == "GodotObject" && b.ContainingNamespace.ToDisplayString() == "Aver.Fabric") return true;
        return false;
    }

    private static bool InGenericType(INamedTypeSymbol type)
    {
        for (INamedTypeSymbol? t = type; t != null; t = t.ContainingType) if (t.IsGenericType) return true;
        return false;
    }

    // True when an ancestor's name table already has a member of that name, so ours must hide it.
    private static bool BaseHas(INamedTypeSymbol? baseType, string table, string member)
    {
        for (INamedTypeSymbol? t = baseType; t != null; t = t.BaseType)
        {
            foreach (INamedTypeSymbol nested in t.GetTypeMembers(table))
                for (INamedTypeSymbol? n = nested; n != null; n = n.BaseType)
                    if (n.GetMembers(member).Length > 0) return true;
        }
        return false;
    }

    private static bool IsSignalDelegate(INamedTypeSymbol t)
    {
        if (t.TypeKind != TypeKind.Delegate) return false;
        if (t.GetAttributes().Any(a => a.AttributeClass?.Name == "SignalAttribute")) return true;
        // An attribute the compiler could not bind (it has no class) is still the user's [Signal] by its spelling.
        foreach (SyntaxReference r in t.DeclaringSyntaxReferences)
            if (r.GetSyntax() is DelegateDeclarationSyntax d &&
                d.AttributeLists.SelectMany(l => l.Attributes).Any(a => GodotSharpRewriter.AttributeName(a.Name) == "Signal"))
                return true;
        return false;
    }

    /// <summary>One plan per Godot-derived class (first declaration), or none when the class needs nothing.</summary>
    public static List<GeneratedPlan> Plan(CSharpCompilation compilation, IReadOnlyList<SyntaxTree> trees, Func<SyntaxTree, string> newlineOf)
    {
        var plans = new List<GeneratedPlan>();
        var seen = new HashSet<INamedTypeSymbol>(SymbolEqualityComparer.Default);
        foreach (SyntaxTree tree in trees)
        {
            SemanticModel model = compilation.GetSemanticModel(tree);
            foreach (ClassDeclarationSyntax cls in tree.GetRoot().DescendantNodes().OfType<ClassDeclarationSyntax>())
            {
                if (model.GetDeclaredSymbol(cls) is not INamedTypeSymbol type) continue;
                if (type.IsStatic || InGenericType(type) || !InheritsGodotObject(type) || !seen.Add(type)) continue;
                var plan = Build(type, cls, tree, newlineOf(tree));
                if (plan != null) plans.Add(plan);
            }
        }
        return plans;
    }

    private static GeneratedPlan? Build(INamedTypeSymbol type, ClassDeclarationSyntax anchor, SyntaxTree tree, string nl)
    {
        string indent = GodotSharpRewriter.IndentOf(anchor.GetFirstToken().LeadingTrivia).ToString();
        string i1 = indent + "    ", i2 = i1 + "    ";
        string baseFq = type.BaseType!.ToDisplayString(s_full);

        var signals = type.GetTypeMembers().Where(IsSignalDelegate).ToList();
        var methods = type.GetMembers().OfType<IMethodSymbol>()
            .Where(m => m.MethodKind == MethodKind.Ordinary && !m.IsImplicitlyDeclared && m.ExplicitInterfaceImplementations.IsEmpty)
            .Select(m => m.Name).Distinct().ToList();
        var props = type.GetMembers().Where(m => !m.IsStatic && !m.IsImplicitlyDeclared &&
                ((m is IFieldSymbol f && !f.IsConst) || (m is IPropertySymbol p && !p.IsIndexer && p.ExplicitInterfaceImplementations.IsEmpty)))
            .Select(m => m.Name).Distinct().ToList();
        string[] tables = { "SignalName", "MethodName", "PropertyName" };
        // A hand-written table of the same name wins; and a name equal to a table's own name cannot be a field of it.
        bool wantSignals = !type.GetTypeMembers("SignalName").Any();
        bool wantMethods = !type.GetTypeMembers("MethodName").Any();
        bool wantProps = !type.GetTypeMembers("PropertyName").Any();
        methods.RemoveAll(n => tables.Contains(n));
        props.RemoveAll(n => tables.Contains(n));

        // Converting an already converted file must change nothing: members that exist are not written again.
        signals.RemoveAll(s => type.GetMembers(SignalOf(s)).Any(m => m is IEventSymbol) && type.GetMembers("EmitSignal" + SignalOf(s)).Any());
        if (signals.Count == 0 && !(wantMethods && methods.Count > 0) && !(wantProps && props.Count > 0)) return null;

        var sb = new StringBuilder();
        bool firstBlock = true;
        sb.Append(nl).Append(indent).Append("// FABRIC: generated. GodotSharp's source generators write these members for a script class; Aver.Fabric has none, so they are here.").Append(nl);
        sb.Append(indent).Append("partial class ").Append(Id(type.Name)).Append(nl).Append(indent).Append('{').Append(nl);

        void Table(string table, IEnumerable<string> names, Func<string, string> valueOf)
        {
            if (!firstBlock) sb.Append(nl);
            firstBlock = false;
            sb.Append(i1).Append("public new class ").Append(table).Append(" : ").Append(baseFq).Append('.').Append(table).Append(nl);
            sb.Append(i1).Append('{').Append(nl);
            foreach (string n in names)
            {
                string mod = BaseHas(type.BaseType, table, n) ? "new " : "";
                sb.Append(i2).Append("public ").Append(mod).Append("static readonly global::Aver.Fabric.StringName ").Append(Id(n)).Append(" = \"")
                  .Append(valueOf(n)).Append("\";").Append(nl);
            }
            sb.Append(i1).Append('}').Append(nl);
        }

        var signalNames = signals.Select(s => SignalOf(s)).ToList();
        if (wantSignals && signals.Count > 0) Table("SignalName", signalNames, n => n);
        if (wantMethods && methods.Count > 0) Table("MethodName", methods, n => n);
        if (wantProps && props.Count > 0) Table("PropertyName", props, n => n);

        foreach (INamedTypeSymbol s in signals)
        {
            string name = SignalOf(s);
            IMethodSymbol? invoke = s.DelegateInvokeMethod;
            string ps = invoke == null ? "" : string.Join(", ", invoke.Parameters.Select(p => p.Type.ToDisplayString(s_full) + " " + Id(p.Name)));
            string args = invoke == null || invoke.Parameters.Length == 0 ? "" : ", " + string.Join(", ", invoke.Parameters.Select(p => Id(p.Name)));
            if (!firstBlock) sb.Append(nl);
            firstBlock = false;
            sb.Append(i1).Append("public event ").Append(Id(s.Name)).Append(' ').Append(Id(name)).Append(" { add => ConnectEvent(SignalName.").Append(Id(name))
              .Append(", value); remove => DisconnectEvent(SignalName.").Append(Id(name)).Append(", value); }").Append(nl);
            sb.Append(i1).Append("protected void EmitSignal").Append(name).Append('(').Append(ps).Append(") => EmitSignal(SignalName.").Append(Id(name)).Append(args)
              .Append(");").Append(nl);
        }

        sb.Append(indent).Append('}').Append(nl);
        return new GeneratedPlan
        {
            Tree = tree, Anchor = anchor, TypeName = type.Name, Text = sb.ToString(),
            Signals = signals.Count, Methods = wantMethods ? methods.Count : 0, Properties = wantProps ? props.Count : 0,
        };
    }

    // [Signal] delegate void HitEventHandler(int damage) declares the signal "Hit".
    private static string SignalOf(INamedTypeSymbol d) =>
        d.Name.EndsWith("EventHandler", StringComparison.Ordinal) && d.Name.Length > "EventHandler".Length ? d.Name[..^"EventHandler".Length] : d.Name;

    /// <summary>Writes the plans of one tree into it: the class made partial where it was not, the generated declaration right after it.</summary>
    public static SyntaxNode Apply(SyntaxNode root, IReadOnlyList<GeneratedPlan> plans)
    {
        root = root.TrackNodes(plans.Select(p => p.Anchor));
        foreach (GeneratedPlan p in plans)
        {
            var current = (ClassDeclarationSyntax)root.GetCurrentNode(p.Anchor)!;
            if (!current.Modifiers.Any(m => m.IsKind(SyntaxKind.PartialKeyword)))
            {
                root = root.ReplaceNode(current, AddPartial(current));
                current = (ClassDeclarationSyntax)root.GetCurrentNode(p.Anchor)!;
            }
            MemberDeclarationSyntax gen = SyntaxFactory.ParseMemberDeclaration(p.Text)!;
            root = root.InsertNodesAfter(current, new[] { gen });
        }
        return root;
    }

    private static ClassDeclarationSyntax AddPartial(ClassDeclarationSyntax cls)
    {
        SyntaxToken partial = SyntaxFactory.Token(SyntaxKind.PartialKeyword).WithTrailingTrivia(SyntaxFactory.Space);
        if (cls.Modifiers.Count > 0) return cls.WithModifiers(cls.Modifiers.Add(partial));
        SyntaxToken kw = cls.Keyword;
        return cls.WithKeyword(kw.WithLeadingTrivia(SyntaxFactory.TriviaList()))
                  .WithModifiers(SyntaxFactory.TokenList(partial.WithLeadingTrivia(kw.LeadingTrivia)));
    }
}
