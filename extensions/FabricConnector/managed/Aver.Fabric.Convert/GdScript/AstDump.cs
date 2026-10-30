// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// A one-line S-expression form of the GDScript tree. The tests assert on it; the form is listed in G3_SCRIPTS.md.

using System.Globalization;
using System.Text;

namespace Aver.Fabric.Convert.GdScript;

public static class AstDump
{
    public static string Dump(Node n) => n switch
    {
        // expressions
        Literal l => Lit(l),
        Identifier i => i.Name,
        SelfExpr => "self",
        SuperExpr => "super",
        Unary u => $"({u.Op} {Dump(u.Operand)})",
        AwaitExpr a => $"(await {Dump(a.Operand)})",
        Binary b => $"({b.Op} {Dump(b.Left)} {Dump(b.Right)})",
        Ternary t => $"(?: {Dump(t.Then)} {Dump(t.Cond)} {Dump(t.Else)})",
        Assign a => $"({a.Op} {Dump(a.Target)} {Dump(a.Value)})",
        Call c => "(call " + Dump(c.Callee) + Join(c.Args.Select(Dump), " ") + ")",
        MemberAccess m => $"(. {Dump(m.Target)} {m.Name})",
        Subscript s => $"([] {Dump(s.Target)} {Dump(s.Index)})",
        GetNodeExpr g => $"($ {g.Path})",
        ArrayLit a => "(array" + Join(a.Items.Select(Dump), " ") + ")",
        DictLit d => (d.LuaStyle ? "(dict-lua" : "(dict") + Join(d.Entries.Select(e => $"({Dump(e.Key)} {Dump(e.Value)})"), " ") + ")",
        LambdaExpr l => "(lambda" + (l.Name != null ? " " + l.Name : "") + " (" + string.Join(" ", l.Params.Select(Par)) + ")" +
                        (l.ReturnType != null ? " -> " + l.ReturnType : "") + " " + Dump(l.Body) + ")",
        PreloadExpr p => $"(preload {Dump(p.Path)})",
        TypeTest t => $"(is {Dump(t.Value)} {t.Type})",
        Cast c => $"(as {Dump(c.Value)} {c.Type})",
        TypeRef t => t.ToString(),
        Annotation a => "@" + a.Name + (a.Args.Count > 0 ? "(" + string.Join(" ", a.Args.Select(Dump)) + ")" : ""),

        // patterns
        PatValue p => Dump(p.Expr),
        PatBind p => $"(var {p.Name})",
        PatWildcard => "_",
        PatRest => "..",
        PatArray a => "(parr" + Join(a.Items.Select(Dump), " ") + ")",
        PatDict d => "(pdict" + Join(d.Entries.Select(e => e.Value == null ? $"({Dump(e.Key)})" : $"({Dump(e.Key)} {Dump(e.Value)})"), " ") + (d.HasRest ? " .." : "") + ")",

        // statements
        Block b => "(block" + Join(b.Statements.Select(Dump), " ") + ")",
        ExprStmt e => Dump(e.Expr),
        IfStmt i => Pre(i) + "(if" + Join(i.Branches.Select(br => $"({Dump(br.Cond)} {Dump(br.Body)})"), " ") + (i.Else != null ? $" (else {Dump(i.Else)})" : "") + ")",
        WhileStmt w => Pre(w) + $"(while {Dump(w.Cond)} {Dump(w.Body)})",
        ForStmt f => Pre(f) + $"(for {f.VarName}{(f.VarType != null ? " :" + f.VarType : "")} in {Dump(f.Iterable)} {Dump(f.Body)})",
        MatchStmt m => Pre(m) + $"(match {Dump(m.Subject)}" + Join(m.Branches.Select(Branch), " ") + ")",
        BreakStmt => "(break)",
        ContinueStmt => "(continue)",
        PassStmt => "(pass)",
        BreakpointStmt => "(breakpoint)",
        ReturnStmt r => r.Value == null ? "(return)" : $"(return {Dump(r.Value)})",
        AssertStmt a => a.Message == null ? $"(assert {Dump(a.Cond)})" : $"(assert {Dump(a.Cond)} {Dump(a.Message)})",
        ErrorStmt => "(error)",

        // declarations
        VarDecl v => Pre(v) + Var(v),
        ConstDecl c => Pre(c) + "(const " + c.Name + (c.Type != null ? " :" + c.Type : "") + (c.Inferred ? " :=" : " =") + " " + Dump(c.Value!) + ")",
        SignalDecl s => Pre(s) + "(signal " + s.Name + Join(s.Params.Select(Par), " ") + ")",
        EnumDecl e => Pre(e) + "(enum " + (e.Name ?? "<anon>") + Join(e.Values.Select(v => v.Value == null ? v.Name : $"{v.Name}={Dump(v.Value)}"), " ") + ")",
        FuncDecl f => Pre(f) + (f.IsStatic ? "(static func " : "(func ") + f.Name + " (" + string.Join(" ", f.Params.Select(Par)) + ")" +
                      (f.ReturnType != null ? " -> " + f.ReturnType : "") + " " + Dump(f.Body) + ")",
        ClassDecl c => Pre(c) + (c.IsInner ? "(class " : "(script ") + (c.Name ?? "-") + (c.Extends != null ? " extends " + c.Extends : "") + Join(c.Members.Select(Dump), " ") + ")",
        StandaloneAnnotation a => Dump(a.Annotation),
        Param p => Par(p),
        DictEntry e => $"({Dump(e.Key)} {Dump(e.Value)})",
        EnumValue v => v.Value == null ? v.Name : $"{v.Name}={Dump(v.Value)}",
        ExtendsClause x => x.ToString(),
        _ => n.GetType().Name,
    };

    private static string Join(IEnumerable<string> parts, string sep)
    {
        string s = string.Join(sep, parts);
        return s.Length == 0 ? "" : " " + s;
    }

    private static string Pre(Stmt s) => s.Annotations.Count == 0 ? "" : string.Join(" ", s.Annotations.Select(Dump)) + " ";

    private static string Par(Param p) =>
        (p.IsRest ? "..." : "") + p.Name + (p.Type != null ? ":" + p.Type : "") + (p.Default != null ? (p.Inferred ? ":=" : "=") + Dump(p.Default) : "");

    private static string Branch(MatchBranch b) =>
        "(branch (" + string.Join(" ", b.Patterns.Select(Dump)) + ")" + (b.Guard != null ? " when " + Dump(b.Guard) : "") + " " + Dump(b.Body) + ")";

    private static string Var(VarDecl v)
    {
        var sb = new StringBuilder(v.IsStatic ? "(static var " : "(var ");
        sb.Append(v.Name);
        if (v.Type != null) sb.Append(" :").Append(v.Type);
        if (v.Init != null) sb.Append(v.Inferred ? " := " : " = ").Append(Dump(v.Init));
        foreach (PropertyAccessor a in new[] { v.Setter, v.Getter }.Where(x => x != null).Select(x => x!))
        {
            sb.Append(a.IsSetter ? " (set" : " (get");
            if (a.ParamName != null) sb.Append(' ').Append(a.ParamName);
            if (a.Delegate != null) sb.Append(" = ").Append(a.Delegate);
            if (a.Body != null) sb.Append(' ').Append(Dump(a.Body));
            sb.Append(')');
        }
        return sb.Append(')').ToString();
    }

    private static string Lit(Literal l)
    {
        switch (l.Kind)
        {
            case LitKind.Int: return ((long)l.Value!).ToString(CultureInfo.InvariantCulture);
            case LitKind.Float:
            {
                string s = ((double)l.Value!).ToString("R", CultureInfo.InvariantCulture);
                return s.Contains('.') || s.Contains('E') || s.Contains('N') || s.Contains('I') ? s : s + ".0";
            }
            case LitKind.String: return Quote((string)l.Value!);
            case LitKind.StringName: return "&" + Quote((string)l.Value!);
            case LitKind.NodePath: return "^" + Quote((string)l.Value!);
            case LitKind.Bool: return (bool)l.Value! ? "true" : "false";
            default: return "null";
        }
    }

    private static string Quote(string s) =>
        "\"" + s.Replace("\\", "\\\\").Replace("\"", "\\\"").Replace("\n", "\\n").Replace("\t", "\\t").Replace("\r", "\\r") + "\"";
}
