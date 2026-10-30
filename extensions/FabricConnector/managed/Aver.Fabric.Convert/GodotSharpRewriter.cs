// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The syntactic half of the GodotSharp -> Aver.Fabric rewrite: namespaces, attributes, the registry name. It needs no references;
// what needs the compiler's view of the types (the generated SignalName / MethodName / PropertyName, events) is GeneratedMembers.cs.

using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace Aver.Fabric.Convert;

/// <summary>Something the rewrite wants the report to say, tied to a syntax node by an annotation so its line is known at the end.</summary>
internal sealed class Note
{
    public string Id = "";
    public string Type = "";
    public string Property = "";
    public Rating Rating;
    public string Reason = "";
    /// <summary>Also write a <c>// FABRIC:</c> comment at the end of the node's line.</summary>
    public bool Comment;
}

internal sealed class NoteBook
{
    public const string AnnotationKind = "fabric-note";
    public readonly Dictionary<string, Note> Notes = new();
    private int _next;

    public SyntaxAnnotation Add(Note note)
    {
        note.Id = "n" + (++_next);
        Notes[note.Id] = note;
        return new SyntaxAnnotation(AnnotationKind, note.Id);
    }
}

internal sealed class GodotSharpRewriter : CSharpSyntaxRewriter
{
    private readonly NoteBook _book;
    private readonly string _newline;
    private readonly string _primary;
    private readonly string _registry;
    private bool _primaryDone;

    /// <summary>References to the Godot namespace that became Aver.Fabric.</summary>
    public int NamespaceRewrites { get; private set; }
    /// <summary>[Export] members seen.</summary>
    public int Exports { get; private set; }

    public GodotSharpRewriter(NoteBook book, string newline, string primaryClass, string registryName)
    {
        _book = book;
        _newline = newline;
        _primary = primaryClass;
        _registry = registryName;
    }

    // ---- namespaces ----

    private static bool IsNamespaceRoot(IdentifierNameSyntax node) => node.Parent switch
    {
        UsingDirectiveSyntax u => u.NamespaceOrType == node,
        QualifiedNameSyntax q => q.Left == node,
        MemberAccessExpressionSyntax m => m.Expression == node,
        _ => false,
    };

    public override SyntaxNode? VisitIdentifierName(IdentifierNameSyntax node)
    {
        if (node.Identifier.ValueText == "Godot" && IsNamespaceRoot(node))
        {
            NamespaceRewrites++;
            return SyntaxFactory.ParseName("Aver.Fabric").WithTriviaFrom(node);
        }
        return base.VisitIdentifierName(node);
    }

    public override SyntaxNode? VisitAliasQualifiedName(AliasQualifiedNameSyntax node)
    {
        if (node.Alias.Identifier.ValueText == "global" && node.Name.Identifier.ValueText == "Godot")
        {
            NamespaceRewrites++;
            return SyntaxFactory.ParseName("global::Aver.Fabric").WithTriviaFrom(node);
        }
        return base.VisitAliasQualifiedName(node);
    }

    // ---- attributes ----

    internal static string AttributeName(NameSyntax name)
    {
        string n = name switch
        {
            QualifiedNameSyntax q => q.Right.Identifier.ValueText,
            AliasQualifiedNameSyntax a => a.Name.Identifier.ValueText,
            SimpleNameSyntax s => s.Identifier.ValueText,
            _ => name.ToString(),
        };
        return n.EndsWith("Attribute", StringComparison.Ordinal) && n.Length > "Attribute".Length ? n[..^"Attribute".Length] : n;
    }

    public override SyntaxNode? VisitAttribute(AttributeSyntax node)
    {
        string name = AttributeName(node.Name);
        var attr = (AttributeSyntax)base.VisitAttribute(node)!;
        switch (name)
        {
            case "Tool":
                return attr.WithAdditionalAnnotations(_book.Add(new Note
                {
                    Type = "attribute", Property = "Tool", Rating = Rating.Unsupported, Comment = true,
                    Reason = "[Tool] marks an editor tool script; Aver has no scripts running in the editor, so this class runs in the game only",
                }));
            case "Icon":
                return attr.WithAdditionalAnnotations(_book.Add(new Note
                {
                    Type = "attribute", Property = "Icon", Rating = Rating.Approximated, Comment = false,
                    Reason = "[Icon] is accepted and ignored: Aver does not show a per-class icon",
                }));
            case "GlobalClass":
                return attr.WithAdditionalAnnotations(_book.Add(new Note
                {
                    Type = "attribute", Property = "GlobalClass", Rating = Rating.Converted, Comment = false,
                    Reason = "the class is registered under its name (the framework class registry); there is no editor Create dialog",
                }));
            case "Export":
            case "ExportGroup":
            case "ExportSubgroup":
            case "ExportCategory":
                if (name == "Export") Exports++;
                return attr;
            case "Rpc":
                return RewriteRpc(attr);
        }
        return attr;
    }

    // Aver.Fabric's [Rpc] is GodotSharp's (G6 NET2): the RpcMode, CallLocal, TransferMode and TransferChannel are all read, so the attribute stays as written.
    private AttributeSyntax RewriteRpc(AttributeSyntax attr) =>
        attr.WithAdditionalAnnotations(_book.Add(new Note
        {
            Type = "attribute", Property = "Rpc", Rating = Rating.Converted, Comment = false,
            Reason = "[Rpc] is kept: SceneMultiplayer (G6 NET2) dispatches it with the mode, call-local, transfer mode and channel as written",
        }));

    // ---- the script class ----

    private static bool IsTopLevel(ClassDeclarationSyntax cls) => cls.Parent is CompilationUnitSyntax or BaseNamespaceDeclarationSyntax;

    public override SyntaxNode? VisitClassDeclaration(ClassDeclarationSyntax node)
    {
        var cls = (ClassDeclarationSyntax)base.VisitClassDeclaration(node)!;
        if (_primaryDone || !IsTopLevel(node) || node.Identifier.ValueText != _primary) return cls;
        _primaryDone = true;
        if (_registry.Length == 0 || _registry == _primary) return cls;
        // A file converted before already carries the attribute.
        if (cls.AttributeLists.SelectMany(l => l.Attributes).Any(a => AttributeName(a.Name) == "AverClass")) return cls;

        // The scenes place this script as _registry (the name scripts.json promised); the C# name is taken or reserved, so the attribute carries it.
        var list = SyntaxFactory.ParseCompilationUnit("[global::Aver.Framework.AverClass(\"" + _registry + "\")]" + _newline + "class X { }")
            .Members[0].AttributeLists[0];
        SyntaxToken first = cls.GetFirstToken();
        SyntaxTriviaList leading = first.LeadingTrivia;
        SyntaxTriviaList indent = IndentOf(leading);
        cls = cls.ReplaceToken(first, first.WithLeadingTrivia(indent));
        list = list.WithLeadingTrivia(leading).WithTrailingTrivia(SyntaxFactory.EndOfLine(_newline))
            .WithAdditionalAnnotations(_book.Add(new Note
            {
                Type = "class", Property = _primary, Rating = Rating.Approximated, Comment = false,
                Reason = $"registered as '{_registry}' (the scene converter's name for this script) because the C# name '{_primary}' is reserved or already taken",
            }));
        return cls.WithAttributeLists(cls.AttributeLists.Insert(0, list));
    }

    internal static SyntaxTriviaList IndentOf(SyntaxTriviaList leading)
    {
        if (leading.Count > 0 && leading[^1].IsKind(SyntaxKind.WhitespaceTrivia)) return SyntaxFactory.TriviaList(leading[^1]);
        return SyntaxFactory.TriviaList();
    }
}

internal static class SyntaxUtil
{
    /// <summary>The class a script file is about: the top-level class named like the file, else the first one (the scene converter's rule).</summary>
    public static ClassDeclarationSyntax? FindPrimaryClass(SyntaxNode root, string stem)
    {
        ClassDeclarationSyntax? first = null;
        foreach (ClassDeclarationSyntax cls in root.DescendantNodes(n => n is CompilationUnitSyntax or BaseNamespaceDeclarationSyntax).OfType<ClassDeclarationSyntax>())
        {
            first ??= cls;
            if (cls.Identifier.ValueText == stem) return cls;
        }
        return first;
    }

    public static string StemOf(string path)
    {
        string name = path.Replace('\\', '/');
        name = name[(name.LastIndexOf('/') + 1)..];
        int dot = name.LastIndexOf('.');
        return dot > 0 ? name[..dot] : name;
    }

    public static string NewlineOf(string text) => text.Contains("\r\n") ? "\r\n" : "\n";
}
