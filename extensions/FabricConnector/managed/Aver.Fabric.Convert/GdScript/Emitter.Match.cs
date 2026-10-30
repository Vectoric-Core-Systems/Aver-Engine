// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// match: a C# switch when every pattern is a constant of the subject's kind, an if/else chain (first match wins) otherwise.

namespace Aver.Fabric.Convert.GdScript;

internal sealed partial class GdEmitter
{
    private static bool BreaksOut(Node n, bool inLoop)
    {
        foreach (Node c in n.Children())
        {
            if (c is BreakStmt && !inLoop) return true;
            if (c is LambdaExpr) continue;
            if (BreaksOut(c, inLoop || c is WhileStmt or ForStmt)) return true;
        }
        return false;
    }

    private static bool EndsInJump(Block b) => b.Statements.Count > 0 && b.Statements[^1] is ReturnStmt or ContinueStmt or BreakStmt;

    private void EmitMatch(MatchStmt m)
    {
        Ex subj = Expr(m.Subject, null);
        if (TrySwitch(m, subj)) return;

        // The subject is evaluated once.
        string value = subj.Code;
        bool simple = subj.Prec == P.Primary && subj.Code.All(c => char.IsLetterOrDigit(c) || c == '_' || c == '.' || c == '@');
        Ex sv = subj;
        if (!simple)
        {
            string tmp = Fresh("matchValue");
            _w.Line($"var {tmp} = {subj.Code};");
            sv = new Ex(tmp, subj.Type) { Addressable = true };
        }

        bool firstBranch = true;
        bool closedWithElse = false;
        for (int i = 0; i < m.Branches.Count; i++)
        {
            MatchBranch br = m.Branches[i];
            PushScope();
            var conds = new List<string>();
            bool always = false;
            foreach (Pattern p in br.Patterns)
            {
                string c = PatternCond(p, sv);
                if (c == "true") always = true;
                conds.Add(c);
            }
            string cond = conds.Count == 1 ? conds[0] : string.Join(" || ", conds.Select(c => c.Contains("&&") ? "(" + c + ")" : c));
            if (br.Guard != null)
            {
                string guard = Truth(Expr(br.Guard, GdType.Bool));
                cond = always ? guard : (conds.Count > 1 ? "(" + cond + ")" : cond) + " && " + guard;
                always = false;
            }
            if (always && br.Body.Statements.All(x => x is PassStmt) && br.Guard == null && !firstBranch)
            {
                PopScope();
                break;   // `_: pass` at the end says nothing
            }
            if (always && i == m.Branches.Count - 1 && !firstBranch)
            {
                _w.Line("else");
                closedWithElse = true;
            }
            else if (always && firstBranch) _w.Line("if (true)");
            else _w.Line((firstBranch ? "if (" : "else if (") + cond + ")");
            _w.Line("{"); _w.Indent++;
            EmitBlockBody(br.Body);
            _w.Indent--; _w.Line("}");
            PopScope();
            firstBranch = false;
            if (closedWithElse) break;
        }
    }

    // ---- switch ----

    private bool TrySwitch(MatchStmt m, Ex subj)
    {
        if (!(subj.Type.Kind is GK.Int or GK.String or GK.Enum or GK.StringName)) return false;
        if (m.Branches.Any(b => b.Guard != null) || m.Branches.Any(b => BreaksOut(b.Body, false))) return false;
        var labels = new List<List<string>>();
        int defaultIndex = -1;
        for (int i = 0; i < m.Branches.Count; i++)
        {
            MatchBranch br = m.Branches[i];
            var list = new List<string>();
            foreach (Pattern p in br.Patterns)
            {
                switch (p)
                {
                    case PatValue pv:
                    {
                        Ex v = Expr(pv.Expr, subj.Type);
                        string label = SwitchLabel(v, subj.Type);
                        if (label == "" || !(v.IsConstExpr || v.Const != null)) return false;
                        list.Add(label);
                        break;
                    }
                    case PatWildcard or PatBind:
                        if (i != m.Branches.Count - 1 || br.Patterns.Count != 1) return false;
                        defaultIndex = i;
                        break;
                    default: return false;
                }
            }
            labels.Add(list);
        }
        string subject = subj.Type.Kind == GK.StringName ? "(string)" + Wrap(subj, P.Unary) : subj.Code;
        _w.Open($"switch ({subject})");
        for (int i = 0; i < m.Branches.Count; i++)
        {
            MatchBranch br = m.Branches[i];
            foreach (string l in labels[i]) _w.Line($"case {l}:");
            if (i == defaultIndex) _w.Line("default:");
            _w.Line("{"); _w.Indent++;
            PushScope();
            if (br.Patterns[0] is PatBind pb)
            {
                string cs = LocalName(pb, pb.Name);
                _w.Line($"var {cs} = {subj.Code};");
                Declare(pb.Name, cs, subj.Type, pb);
            }
            EmitBlockBody(br.Body);
            PopScope();
            if (!EndsInJump(br.Body)) _w.Line("break;");
            _w.Indent--; _w.Line("}");
        }
        _w.Close();
        return true;
    }

    private string SwitchLabel(Ex v, GdType subject)
    {
        switch (subject.Kind)
        {
            case GK.Int:
                if (v.Type.Kind == GK.Int && v.Const is long) return v.Code;
                if (v.Type.Kind == GK.Enum) return "(" + subject.Cs + ")" + Wrap(v, P.Unary);
                return v.Type.Kind == GK.Int ? v.Code : "";
            case GK.String or GK.StringName:
                return v.Type.Kind is GK.String or GK.StringName && v.Const is string ? v.Code : "";
            case GK.Enum:
                if (v.Type.Kind == GK.Enum && v.Type.Cs == subject.Cs) return v.Code;
                if (v.Type.Kind == GK.Int && v.Const is long) return "(" + subject.Cs + ")" + v.Code;
                return "";
        }
        return "";
    }

    // ---- pattern conditions ----

    /// <summary>The C# condition that is true when <paramref name="value"/> matches the pattern; variables a pattern binds are declared.</summary>
    private string PatternCond(Pattern p, Ex value)
    {
        switch (p)
        {
            case PatWildcard: return "true";
            case PatBind b:
            {
                string cs = LocalName(b, b.Name);
                Declare(b.Name, cs, value.Type, b);
                return $"{value.Code} is var {cs}";
            }
            case PatValue pv:
            {
                Ex v = Expr(pv.Expr, value.Type);
                Ex eq = BinaryOp("==", value, v, pv);
                return eq.Code;
            }
            case PatRest: return "true";
            case PatArray pa:
            {
                string arr = Fresh("array");
                int fixedCount = pa.Items.Count(i => i is not PatRest);
                string cond = $"GDScript.AsArrayOrNull({VariantFromTyped(value)}) is {{ }} {arr} && {arr}.Count {(pa.HasRest ? ">=" : "==")} {fixedCount}";
                for (int i = 0; i < pa.Items.Count; i++)
                {
                    if (pa.Items[i] is PatRest) continue;
                    string sub = PatternCond(pa.Items[i], new Ex($"{arr}[{i}]", GdType.Variant));
                    if (sub != "true") cond += " && " + (sub.Contains(" || ") ? "(" + sub + ")" : sub);
                }
                return cond;
            }
            case PatDict pd:
            {
                string dict = Fresh("dict");
                string cond = $"GDScript.AsDictOrNull({VariantFromTyped(value)}) is {{ }} {dict}" + (pd.HasRest ? "" : $" && {dict}.Count == {pd.Entries.Count}");
                foreach (PatDictEntry e in pd.Entries)
                {
                    Ex key = Expr(e.Key, null);
                    string k = Adapt(key, GdType.Variant);
                    cond += $" && {dict}.ContainsKey({k})";
                    if (e.Value != null)
                    {
                        string sub = PatternCond(e.Value, new Ex($"{dict}[{k}]", GdType.Variant));
                        if (sub != "true") cond += " && " + (sub.Contains(" || ") ? "(" + sub + ")" : sub);
                    }
                }
                return cond;
            }
        }
        return "false";
    }
}
