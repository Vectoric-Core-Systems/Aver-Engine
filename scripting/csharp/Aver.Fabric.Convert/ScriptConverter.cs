// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The conversion pipeline: rewrite each GodotSharp script, compile them together against Aver.Fabric (in memory), write the members a
// source generator would have written, resolve the names the implicit usings make ambiguous, then report what still does not compile.

using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;
using Aver.Fabric.Convert.GdScript;

namespace Aver.Fabric.Convert;

public static class ScriptConverter
{
    // What the Godot editor defines when it builds a project; the converted file defines them too so its #if GODOT blocks stay live.
    private static readonly string[] s_godotSymbols =
        { "GODOT", "GODOT4", "GODOT4_1_OR_GREATER", "GODOT4_2_OR_GREATER", "GODOT4_3_OR_GREATER", "GODOT4_4_OR_GREATER", "GODOT_PC" };

    private static readonly CSharpParseOptions s_parse =
        new(LanguageVersion.Latest, DocumentationMode.None, SourceCodeKind.Regular, s_godotSymbols.Concat(new[] { "DEBUG", "TRACE" }).ToArray());

    // Microsoft.NET.Sdk's ImplicitUsings, which every Aver Scripts.csproj turns on and a Godot project does not.
    private const string ImplicitUsings =
        "global using global::System;\nglobal using global::System.Collections.Generic;\nglobal using global::System.IO;\nglobal using global::System.Linq;\n" +
        "global using global::System.Net.Http;\nglobal using global::System.Threading;\nglobal using global::System.Threading.Tasks;\n";

    private static int s_compilations;

    // The errors that mean "this name is not in Aver.Fabric": the report and the comment name the member.
    private static readonly HashSet<string> s_missingMember = new() { "CS0103", "CS0117", "CS0234", "CS0246", "CS0426", "CS1061" };

    private sealed class FileState
    {
        public ScriptSource Source = null!;
        public SyntaxTree Tree = null!;
        public string Newline = "\n";
        public string Registry = "";
        public string Declared = "";
        public int NamespaceRewrites;
        public int Exports;
        public int Signals, MethodNames, PropertyNames;
        public List<string> Aliases = new();
        public GdConvertedFile? Gd;
    }

    /// <summary>Converts the scripts as one set (they see each other's classes) and compiles them when <see cref="ConvertSettings.RefsDir"/> is given.</summary>
    public static ConversionResult Convert(IReadOnlyList<ScriptSource> sources, ConvertSettings settings)
    {
        var result = new ConversionResult();
        var book = new NoteBook();
        var files = new List<FileState>();

        // The reference folder is read once: the GDScript converter types against it, then the compile checks the output against it.
        List<MetadataReference>? refs = null;
        string? refsProblem = null;
        if (settings.RefsDir != null) refs = References.Build(settings.RefsDir, out refsProblem);

        // GDScript first: it writes C# (against the same runtime), which then goes through the steps below like any other script.
        var gd = new Dictionary<string, GdConvertedFile>(StringComparer.Ordinal);
        List<ScriptSource> gdSources = sources.Where(s => s.Kind == "gdscript").ToList();
        if (gdSources.Count > 0)
        {
            ApiModel? api = refs != null ? ApiModel.Create(refs, settings.ExtraSources) : null;
            var gdInput = gdSources.Select(s => new GdSource { ResPath = s.ResPath, Text = s.Text, ClassName = s.ClassName.Length > 0 ? s.ClassName : GdScript.Naming.Pascal(SyntaxUtil.StemOf(s.ResPath)), Output = s.Output }).ToList();
            foreach (GdConvertedFile f in GdScriptConverter.Convert(gdInput, api, settings.ProjectDir)) gd[f.ResPath] = f;
        }

        foreach (ScriptSource src0 in sources)
        {
            if (src0.Kind == "gdscript")
            {
                GdConvertedFile g = gd[src0.ResPath];
                var gsrc = new ScriptSource { ResPath = src0.ResPath, Text = g.Text, ClassName = g.ClassName, Output = src0.Output, Kind = "gdscript" };
                var gfs = new FileState { Source = gsrc, Newline = "\n", Registry = g.ClassName, Declared = g.ClassName, Gd = g };
                gfs.Tree = CSharpSyntaxTree.ParseText(g.Text, s_parse, path: src0.ResPath);
                files.Add(gfs);
                continue;
            }
            ScriptSource src = src0;
            var fs = new FileState { Source = src, Newline = SyntaxUtil.NewlineOf(src.Text) };
            SyntaxTree parsed = CSharpSyntaxTree.ParseText(src.Text, s_parse, path: src.ResPath);
            SyntaxNode root = parsed.GetRoot();
            ClassDeclarationSyntax? primary = SyntaxUtil.FindPrimaryClass(root, SyntaxUtil.StemOf(src.ResPath));
            fs.Declared = primary?.Identifier.ValueText ?? "";
            fs.Registry = src.ClassName.Length > 0 ? src.ClassName : fs.Declared;

            var rewriter = new GodotSharpRewriter(book, fs.Newline, fs.Declared, fs.Registry);
            root = rewriter.Visit(root)!;
            fs.NamespaceRewrites = rewriter.NamespaceRewrites;
            fs.Exports = rewriter.Exports;
            root = AddHeader(root, src.ResPath, fs.Newline, UsesGodotSymbols(parsed.GetRoot()));
            fs.Tree = CSharpSyntaxTree.Create((CSharpSyntaxNode)root, s_parse, src.ResPath, Encoding.UTF8);
            files.Add(fs);
        }

        if (settings.RefsDir != null)
        {
            if (refs == null) result.GeneralErrors.Add(refsProblem ?? "the reference folder is unusable");
            else
            {
                result.Compiled = true;
                CompileAndGenerate(files, refs, result, settings);
            }
        }

        Finish(files, book, result);
        return result;
    }

    // ---- header ----

    private static bool UsesGodotSymbols(SyntaxNode root) =>
        root.DescendantTrivia(descendIntoTrivia: true)
            .Any(t => (t.IsKind(SyntaxKind.IfDirectiveTrivia) || t.IsKind(SyntaxKind.ElifDirectiveTrivia)) && t.ToString().Contains("GODOT", StringComparison.Ordinal));

    private static SyntaxNode AddHeader(SyntaxNode root, string resPath, string nl, bool defineGodot)
    {
        var sb = new StringBuilder();
        sb.Append("// FABRIC: converted from ").Append(resPath).Append(" by Aver.Fabric.Convert (GodotSharp to Aver.Fabric). Lines marked FABRIC need attention.").Append(nl);
        sb.Append("#nullable disable").Append(nl);
        if (defineGodot) foreach (string s in s_godotSymbols) sb.Append("#define ").Append(s).Append(nl);
        SyntaxToken first = root.GetFirstToken(includeZeroWidth: true);
        SyntaxTriviaList header = SyntaxFactory.ParseLeadingTrivia(sb.ToString());
        return root.ReplaceToken(first, first.WithLeadingTrivia(header.AddRange(first.LeadingTrivia)));
    }

    // ---- compile, generate, resolve ----

    private static CSharpCompilation Compile(IEnumerable<FileState> files, List<MetadataReference> refs, ConvertSettings settings)
    {
        var trees = new List<SyntaxTree> { CSharpSyntaxTree.ParseText(ImplicitUsings, s_parse, path: "GlobalUsings.g.cs") };
        trees.AddRange(files.Select(f => f.Tree));
        if (settings.ExtraSources != null)
            foreach ((string path, string text) in settings.ExtraSources) trees.Add(CSharpSyntaxTree.ParseText(text, s_parse, path: path));
        // A new name each time: a caller that loads two results into one process must not meet two assemblies of one identity.
        return CSharpCompilation.Create("ConvertedScripts_" + Interlocked.Increment(ref s_compilations), trees, refs,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, nullableContextOptions: NullableContextOptions.Disable,
                                         optimizationLevel: OptimizationLevel.Debug, deterministic: true));
    }

    private static void SetRoot(FileState f, SyntaxNode root) =>
        f.Tree = CSharpSyntaxTree.Create((CSharpSyntaxNode)root, s_parse, f.Source.ResPath, Encoding.UTF8);

    private static void CompileAndGenerate(List<FileState> files, List<MetadataReference> refs, ConversionResult result, ConvertSettings settings)
    {
        // 1. The members the source generators would write, from the compiler's view of the classes.
        CSharpCompilation first = Compile(files, refs, settings);
        List<GeneratedPlan> plans = GeneratedMembers.Plan(first, files.Select(f => f.Tree).ToList(), t => files.First(f => f.Tree == t).Newline);
        foreach (FileState f in files)
        {
            var mine = plans.Where(p => p.Tree == f.Tree).ToList();
            if (mine.Count == 0) continue;
            f.Signals = mine.Sum(p => p.Signals);
            f.MethodNames = mine.Sum(p => p.Methods);
            f.PropertyNames = mine.Sum(p => p.Properties);
            SetRoot(f, GeneratedMembers.Apply(f.Tree.GetRoot(), mine));
        }

        // 2. Names the implicit usings make ambiguous between System.* and Aver.Fabric (Timer, ...): an alias per file picks Aver.Fabric's.
        for (int pass = 0; pass < 4; pass++)
        {
            CSharpCompilation comp = Compile(files, refs, settings);
            List<Diagnostic> ambiguous = comp.GetDiagnostics().Where(d => d.Id == "CS0104").ToList();
            bool changed = false;
            foreach (FileState f in files)
            {
                var wanted = new List<(string Name, string Target)>();
                var qualify = new List<(GenericNameSyntax Node, string Namespace)>();
                SemanticModel? model = null;
                SyntaxNode root = f.Tree.GetRoot();
                foreach (Diagnostic d in ambiguous.Where(d => d.Location.SourceTree == f.Tree))
                {
                    model ??= comp.GetSemanticModel(f.Tree);
                    SyntaxNode node = root.FindNode(d.Location.SourceSpan, getInnermostNodeForTie: true);
                    if (node is not SimpleNameSyntax name) continue;
                    ISymbol? pick = model.GetSymbolInfo(name).CandidateSymbols
                        .FirstOrDefault(s => s.ContainingNamespace?.ToDisplayString().StartsWith("Aver.Fabric", StringComparison.Ordinal) == true);
                    if (pick is not INamedTypeSymbol t) continue;
                    // An alias cannot name an open generic (Dictionary<,>), so those uses are qualified in place instead.
                    if (name is GenericNameSyntax g) qualify.Add((g, t.ContainingNamespace.ToDisplayString()));
                    else if (!f.Aliases.Contains(name.Identifier.ValueText)) wanted.Add((name.Identifier.ValueText, t.ToDisplayString()));
                }
                if (qualify.Count > 0)
                {
                    root = root.ReplaceNodes(qualify.Select(q => q.Node), (o, _) =>
                        SyntaxFactory.QualifiedName(SyntaxFactory.ParseName("global::" + qualify.First(q => q.Node == o).Namespace), ((GenericNameSyntax)o).WithoutTrivia())
                                     .WithTriviaFrom(o));
                    SetRoot(f, root);
                    changed = true;
                }
                foreach ((string name, string target) in wanted.Distinct())
                {
                    f.Aliases.Add(name);
                    SetRoot(f, AddUsing(f.Tree.GetRoot(), $"using {name} = {target};{f.Newline}"));
                    changed = true;
                }
            }
            if (!changed) break;
        }

        // 3. The verdict: emit, so the errors only the emitter finds count too.
        CSharpCompilation final = Compile(files, refs, settings);
        using var ms = new MemoryStream();
        var emit = final.Emit(ms);
        foreach (Diagnostic d in emit.Diagnostics.Where(d => d.Severity == DiagnosticSeverity.Error))
        {
            FileState? f = files.FirstOrDefault(x => x.Tree == d.Location.SourceTree);
            if (f == null) { result.GeneralErrors.Add($"{d.Id}: {d.GetMessage()}"); continue; }
            FileLinePositionSpan span = d.Location.GetLineSpan();
            string member = f.Tree.GetText().ToString(d.Location.SourceSpan).Trim();
            if (member.Length > 80 || member.Contains('\n')) member = QuotedName(d.GetMessage());
            result.Errors.Add(new CompileError
            {
                ResPath = f.Source.ResPath, Output = f.Source.Output, Line = span.StartLinePosition.Line + 1, Column = span.StartLinePosition.Character + 1,
                Id = d.Id, Message = d.GetMessage(System.Globalization.CultureInfo.InvariantCulture), Member = member,
            });
        }
        if (emit.Success && settings.KeepAssembly) result.Assembly = ms.ToArray();
    }

    private static string QuotedName(string message)
    {
        int a = message.IndexOf('\'');
        int b = a < 0 ? -1 : message.IndexOf('\'', a + 1);
        return a >= 0 && b > a ? message[(a + 1)..b] : "";
    }

    private static SyntaxNode AddUsing(SyntaxNode root, string text)
    {
        var cu = (CompilationUnitSyntax)root;
        UsingDirectiveSyntax u = SyntaxFactory.ParseCompilationUnit(text).Usings[0];
        if (cu.Usings.Count > 0 || cu.Externs.Count > 0) return cu.WithUsings(cu.Usings.Add(u));
        // No usings yet: the new one goes first and takes the header (a #define must come before the first token).
        SyntaxToken firstToken = cu.GetFirstToken(includeZeroWidth: true);
        u = u.WithLeadingTrivia(firstToken.LeadingTrivia);
        cu = cu.ReplaceToken(firstToken, firstToken.WithLeadingTrivia(SyntaxFactory.TriviaList()));
        return cu.WithUsings(cu.Usings.Add(u));
    }

    // ---- the end: comments, lines, report items, text ----

    private static void Finish(List<FileState> files, NoteBook book, ConversionResult result)
    {
        foreach (FileState f in files)
        {
            SyntaxNode root = f.Tree.GetRoot();
            SourceText text = f.Tree.GetText();
            var comments = new Dictionary<int, List<string>>();
            var script = new ConvertedScript { ResPath = f.Source.ResPath, Output = f.Source.Output, ClassName = f.Registry };
            var items = new List<ReportItem>();
            var worst = Rating.Converted;

            foreach (SyntaxNode node in root.GetAnnotatedNodes(NoteBook.AnnotationKind))
            foreach (SyntaxAnnotation a in node.GetAnnotations(NoteBook.AnnotationKind))
            {
                if (a.Data == null || !book.Notes.TryGetValue(a.Data, out Note? note)) continue;
                int line = node.GetLocation().GetLineSpan().StartLinePosition.Line;
                items.Add(new ReportItem
                {
                    File = f.Source.ResPath, Node = f.Registry, Type = note.Type, Property = note.Property,
                    Rating = note.Rating, Reason = note.Reason, Line = line + 1,
                });
                if (note.Rating > worst) worst = note.Rating;
                if (note.Comment) AddComment(comments, line, "FABRIC: " + note.Reason);
            }

            foreach (CompileError e in result.Errors.Where(e => e.ResPath == f.Source.ResPath))
            {
                script.ErrorCount++;
                items.Add(new ReportItem
                {
                    File = f.Source.ResPath, Node = EnclosingType(root, text, e.Line, f.Registry), Type = e.Id, Property = e.Member,
                    Rating = Rating.Unsupported, Line = e.Line,
                    Reason = $"{f.Source.Output}({e.Line},{e.Column}): {e.Message}",
                });
                AddComment(comments, e.Line - 1, e.Member.Length > 0 && s_missingMember.Contains(e.Id)
                    ? $"FABRIC: Aver.Fabric has no '{e.Member}' ({e.Id}: {e.Message})" : $"FABRIC: {e.Id} {e.Message}");
            }

            script.Text = comments.Count == 0 ? root.ToFullString() : ApplyComments(root, text, comments).ToFullString();

            // What the GDScript emitter noted: the tag in its comment says which line of the final file each note belongs to.
            if (f.Gd != null)
            {
                var lineOfTag = new Dictionary<int, int>();
                string[] lines = script.Text.Split('\n');
                for (int i = 0; i < lines.Length; i++)
                    foreach (System.Text.RegularExpressions.Match m in System.Text.RegularExpressions.Regex.Matches(lines[i], @"FABRIC\[(\d+)\]:"))
                        lineOfTag.TryAdd(int.Parse(m.Groups[1].Value, System.Globalization.CultureInfo.InvariantCulture), i + 1);
                foreach (GdScript.EmitNote note in f.Gd.Notes)
                {
                    items.Add(new ReportItem
                    {
                        File = f.Source.ResPath, Node = f.Registry, Type = note.Type, Property = note.Property, Rating = note.Rating, Reason = note.Reason,
                        Line = lineOfTag.GetValueOrDefault(note.Tag, 0),
                    });
                    if (note.Rating > worst) worst = note.Rating;
                }
                foreach (GdScript.ParseError pe in f.Gd.ParseErrors)
                {
                    items.Add(new ReportItem
                    {
                        File = f.Source.ResPath, Node = f.Registry, Type = "parse", Property = "", Rating = Rating.Unsupported, Line = 0,
                        Reason = $"GDScript {pe.Line}:{pe.Col}: {pe.Message} (the statement or member was left out of the C#)",
                    });
                    if (worst < Rating.Approximated) worst = Rating.Approximated;
                }
                script.Text = System.Text.RegularExpressions.Regex.Replace(script.Text, @"FABRIC\[\d+\]:", "FABRIC:");
            }

            // The script as a whole: unsupported only when it does not compile; approximated when something was dropped or it was not compiled.
            Rating whole = script.ErrorCount > 0 ? Rating.Unsupported : (worst != Rating.Converted || !result.Compiled) ? Rating.Approximated : Rating.Converted;
            script.Items.Add(new ReportItem
            {
                File = f.Source.ResPath, Node = f.Registry, Type = f.Gd != null ? "gdscript" : "csharp", Rating = whole, Reason = Summary(f, result.Compiled, script.ErrorCount),
            });
            script.Items.AddRange(items);
            result.Scripts.Add(script);
        }
    }

    private static string Summary(FileState f, bool compiled, int errors)
    {
        var parts = new List<string>();
        if (f.Gd != null)
        {
            parts.Add($"{f.Gd.Members} variable/constant(s), {f.Gd.Functions} function(s) and {f.Gd.Signals} signal(s) written as C#");
            if (f.Signals > 0) parts.Add($"{f.Signals} [Signal] delegate(s) given an event and an EmitSignalX helper");
            if (f.Aliases.Count > 0) parts.Add("using alias for " + string.Join(", ", f.Aliases));
            string v = !compiled ? "not compiled (no reference folder)" : errors == 0 ? "compiles against Aver.Fabric" : $"{errors} compile error(s) remain, marked // FABRIC: in the output";
            return $"GDScript to Aver.Fabric: {string.Join("; ", parts)}; {v}";
        }
        if (f.NamespaceRewrites > 0) parts.Add($"{f.NamespaceRewrites} Godot namespace reference(s) rewritten to Aver.Fabric");
        if (f.Exports > 0) parts.Add($"{f.Exports} [Export] member(s) kept (values arrive through the level's Props record)");
        if (f.Signals > 0) parts.Add($"{f.Signals} [Signal] delegate(s) given an event and an EmitSignalX helper");
        if (f.MethodNames + f.PropertyNames > 0) parts.Add($"SignalName/MethodName/PropertyName generated ({f.MethodNames} method, {f.PropertyNames} property name(s))");
        if (f.Aliases.Count > 0) parts.Add("using alias for " + string.Join(", ", f.Aliases));
        if (f.Registry != f.Declared && f.Registry.Length > 0) parts.Add($"registered as {f.Registry}");
        string what = parts.Count == 0 ? "nothing needed rewriting" : string.Join("; ", parts);
        string verdict = !compiled ? "not compiled (no reference folder)" : errors == 0 ? "compiles against Aver.Fabric" : $"{errors} compile error(s) remain, marked // FABRIC: in the output";
        return $"GodotSharp to Aver.Fabric: {what}; {verdict}";
    }

    private static string EnclosingType(SyntaxNode root, SourceText text, int line1, string fallback)
    {
        if (line1 < 1 || line1 > text.Lines.Count) return fallback;
        TextLine tl = text.Lines[line1 - 1];
        int at = tl.Start;
        while (at < tl.End - 1 && char.IsWhiteSpace(text[at])) at++;
        var names = root.FindToken(at).Parent?.AncestorsAndSelf().OfType<TypeDeclarationSyntax>().Select(t => t.Identifier.ValueText).Reverse().ToList();
        return names == null || names.Count == 0 ? fallback : string.Join("/", names);
    }

    private static void AddComment(Dictionary<int, List<string>> map, int line, string text)
    {
        if (!map.TryGetValue(line, out var list)) map[line] = list = new List<string>();
        text = text.Replace("\r", " ").Replace("\n", " ");
        if (text.Length > 240) text = text[..237] + "...";
        if (!list.Contains(text)) list.Add(text);
    }

    // Adds "// text" after the code on each line, as trailing trivia of the line's last token. A line that ends inside a multi-line
    // token (a verbatim string) or has no code gets none: a comment there would change the program or point at nothing.
    private static SyntaxNode ApplyComments(SyntaxNode root, SourceText text, Dictionary<int, List<string>> byLine)
    {
        var changes = new Dictionary<SyntaxToken, SyntaxToken>();
        foreach ((int line, List<string> list) in byLine)
        {
            if (line < 0 || line >= text.Lines.Count) continue;
            TextLine tl = text.Lines[line];
            SyntaxToken last = default;
            foreach (SyntaxToken t in root.DescendantTokens(TextSpan.FromBounds(tl.Start, tl.End)))
                if (t.Span.Length > 0 && t.Span.Start < tl.End && t.Span.End > tl.Start && t.Span.Start >= last.Span.Start) last = t;
            if (last == default || last.Span.End > tl.End || changes.ContainsKey(last)) continue;

            var trailing = last.TrailingTrivia.ToList();
            int eol = trailing.FindIndex(t => t.IsKind(SyntaxKind.EndOfLineTrivia));
            var add = new List<SyntaxTrivia> { SyntaxFactory.Space, SyntaxFactory.Comment("// " + string.Join("; ", list)) };
            if (eol < 0) trailing.AddRange(add); else trailing.InsertRange(eol, add);
            changes[last] = last.WithTrailingTrivia(trailing);
        }
        return root.ReplaceTokens(changes.Keys, (o, _) => changes[o]);
    }
}
