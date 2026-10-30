// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Statements: locals, assignment, branches, loops (range / array / dictionary / string), return, assert.

using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Rs = Microsoft.CodeAnalysis;

namespace Aver.Fabric.Convert.GdScript;

internal sealed partial class GdEmitter
{
    private void EmitBlockBody(Block b)
    {
        foreach (Stmt s in b.Statements) EmitStatement(s);
    }

    private void EmitScoped(string header, Block b)
    {
        _w.Open(header);
        PushScope();
        EmitBlockBody(b);
        PopScope();
        _w.Close();
    }

    private void EmitStatement(Stmt s)
    {
        switch (s)
        {
            case PassStmt or BreakpointStmt: break;
            case BreakStmt: _w.Line("break;"); break;
            case ContinueStmt: _w.Line("continue;"); break;
            case ReturnStmt r: EmitReturn(r); break;
            case ExprStmt es: EmitExprStmt(es); break;
            case VarDecl vd: EmitLocalVar(vd); break;
            case ConstDecl cd: EmitLocalConst(cd); break;
            case IfStmt i: EmitIf(i); break;
            case WhileStmt w: EmitWhile(w); break;
            case ForStmt f: EmitFor(f); break;
            case MatchStmt m: EmitMatch(m); break;
            case AssertStmt a: EmitAssert(a); break;
            case Block b: EmitScoped("", b); break;
            case ErrorStmt e: _w.Line($"// FABRIC: could not parse a statement here (GDScript line {e.Start.Line})"); break;
            case StandaloneAnnotation: break;
            default: _w.Line($"// FABRIC: statement {s.GetType().Name} is not converted"); break;
        }
    }

    // ---- locals ----

    private void EmitLocalVar(VarDecl vd)
    {
        string cs = LocalName(vd, vd.Name);
        GdType t;
        Ex? init = null;
        if (vd.Type != null)
        {
            t = ResolveType(vd.Type);
            if (vd.Init != null) init = Expr(vd.Init, t);
        }
        else if (vd.Init != null)
        {
            init = Expr(vd.Init, null);
            GdType it = GuardScene(CleanInferred(init.Type), vd.Name, (Node?)_ctx.BodyRoot ?? vd);
            t = vd.Inferred ? it : Unify(it, vd.Name, (Node?)_ctx.BodyRoot ?? vd);
            if (it.Kind == GK.Lambda) t = GdType.Callable;
        }
        else t = InferUninitialised(vd);

        bool provisional = vd.Type == null && !vd.Inferred;
        if (provisional && _forced.TryGetValue(vd, out GdType? forced)) t = forced;
        string value = init != null ? Unparen(Adapt(init, t)) : DefaultFor(t);
        _w.Line($"{t.Cs} {cs} = {value};");
        Declare(vd.Name, cs, t, vd).Provisional = provisional && !t.IsVariant;
    }

    // `var x` with no type and no value: if every assignment is a literal of one kind, that kind; else a Variant.
    private GdType InferUninitialised(VarDecl vd)
    {
        GdType? found = null;
        foreach (Assign a in AssignsIn((Node?)_ctx.BodyRoot ?? vd))
        {
            if (a.Op != "=" || !IsNamed(a.Target, vd.Name)) continue;
            GdType? lt = LiteralType(a.Value);
            if (lt == null || lt.Kind == GK.Null) return GdType.Variant;
            if (found == null) found = lt;
            else if (found.IsNumeric && lt.IsNumeric) found = found.Kind == GK.Float || lt.Kind == GK.Float ? GdType.Float : GdType.Int;
            else if (found.Kind != lt.Kind) return GdType.Variant;
        }
        return found ?? GdType.Variant;
    }

    private string DefaultFor(GdType t) => t.Kind switch
    {
        GK.Variant => "default",
        GK.String => "\"\"",
        GK.Object or GK.ScriptClass or GK.Callable or GK.Null => t.Kind == GK.Callable ? "default" : "null",
        _ => DefaultValue(t),
    };

    private void EmitLocalConst(ConstDecl cd)
    {
        string cs = LocalName(cd, cd.Name);
        GdType t = cd.Type != null ? ResolveType(cd.Type) : GdType.Variant;
        Ex v = cd.Value != null ? Expr(cd.Value, cd.Type != null ? t : null) : new Ex("default", GdType.Variant);
        if (cd.Type == null) t = CleanInferred(v.Type);
        string value = Adapt(v, t);
        bool isConst = v.IsConstExpr && t.Kind is GK.Int or GK.Float or GK.Bool or GK.String;
        _w.Line($"{(isConst ? "const " : "")}{t.Cs} {cs} = {value};");
        Declare(cd.Name, cs, t, cd, constant: true);
    }

    // ---- expression statements and assignment ----

    private void EmitExprStmt(ExprStmt es)
    {
        switch (es.Expr)
        {
            case Assign a: EmitAssign(a); return;
            case AwaitExpr aw:
            {
                string code = AwaitCode(aw, resultUsed: false, out _);
                _w.Line(code + ";");
                return;
            }
        }
        Ex e = Expr(es.Expr, null);
        if (e.Code.Length == 0) return;
        _w.Line(AsStatement(e.Code) + ";");
    }

    // The code as a statement: a call stands for itself; anything else (a bare name, an operator) is discarded explicitly.
    private static string AsStatement(string code)
    {
        try
        {
            StatementSyntax st = SyntaxFactory.ParseStatement(code + ";");
            if (st is ExpressionStatementSyntax es && es.Expression is InvocationExpressionSyntax or ObjectCreationExpressionSyntax or ImplicitObjectCreationExpressionSyntax
                or AssignmentExpressionSyntax or AwaitExpressionSyntax or PrefixUnaryExpressionSyntax or PostfixUnaryExpressionSyntax)
                return code;
        }
        catch (Exception) { }
        return "_ = " + code;
    }

    private static readonly HashSet<string> s_nativeCompound = new() { "+", "-", "*", "/", "%", "&", "|", "^" };

    private void EmitAssign(Assign a)
    {
        Ex target = Expr(a.Target, null);
        string op = a.Op == "=" ? "" : a.Op[..^1];
        Ex value = Expr(a.Value, target.Type.IsVariant ? null : target.Type);

        string newValue;
        if (op.Length == 0) { CheckProvisional(target, value.Type); newValue = Unparen(Adapt(value, target.Type)); }
        else
        {
            Ex bin = BinaryOp(op, target, value, a);
            CheckProvisional(target, bin.Type);
            if (s_nativeCompound.Contains(op) && target.SetTemplate == null && target.Parent == null && target.Type.IsNumeric && value.Type.IsNumeric
                && !(op == "/" && target.Type.Kind == GK.Int && value.Type.Kind == GK.Float))
            {
                _w.Line($"{target.Code} {op}= {Adapt(value, target.Type)};");
                return;
            }
            if (op is "+" && target.Type.Kind == GK.String && value.Type.Kind == GK.String && target.SetTemplate == null && target.Parent == null)
            {
                _w.Line($"{target.Code} += {value.Code};");
                return;
            }
            newValue = Adapt(bin, target.Type);
        }
        _w.Line(AssignCode(target, newValue) + ";");
    }

    // Writes a value into an assignment target: a call for dynamic targets, `X = X with { f = v }` for a field of a struct property.
    private string AssignCode(Ex t, string value)
    {
        if (t.SetTemplate != null) return t.SetTemplate.Replace("\u0001", value);
        if (t.Parent != null && t.Parent.Type.Kind == GK.Struct && !t.Parent.Addressable)
            return AssignCode(t.Parent, $"{t.Parent.Code} with {{ {t.FieldCs} = {value} }}");
        return $"{t.Code} = {value}";
    }

    // ---- return, assert ----

    private void EmitReturn(ReturnStmt r)
    {
        if (r.Value == null)
        {
            _ctx.ReturnsWithoutValue = true;
            _w.Line("return;");
            return;
        }
        if (_ctx.InferReturn)
        {
            Ex v = Expr(r.Value, null);
            _ctx.ReturnTypes.Add(v.Type.Kind == GK.Lambda ? GdType.Callable : v.Type);
            _w.Line("return " + v.Code + ";");
            return;
        }
        GdType ret = _ctx.Return;
        if (ret.Kind == GK.Void)
        {
            Ex v = Expr(r.Value, null);
            _w.Line(AsStatement(v.Code) + ";");
            _w.Line("return;");
            return;
        }
        Ex val = Expr(r.Value, ret);
        _w.Line("return " + Unparen(Adapt(val, ret)) + ";");
    }

    private void EmitAssert(AssertStmt a)
    {
        Ex c = Expr(a.Cond, GdType.Bool);
        string cond = Truth(c);
        string msg = a.Message != null ? ", " + Adapt(Expr(a.Message, GdType.String), GdType.String) : "";
        _w.Line($"GDScript.Assert({cond}{msg});");
    }

    // ---- conditions ----

    /// <summary>A condition as C# bool. In an `and` chain, `x is T` binds a pattern variable the rest of the chain and the branch use.</summary>
    private string CondCode(Expr e, bool narrow = true)
    {
        if (e is Binary { Op: "and" } b)
        {
            string l = CondCode(b.Left, narrow);
            string r = CondCode(b.Right, narrow);
            return WrapIf(l, b.Left is Binary { Op: "or" }) + " && " + WrapIf(r, b.Right is Binary { Op: "or" } or Ternary);
        }
        if (narrow && e is TypeTest tt && tt.Value is Identifier id && TryNarrow(id, tt, out string? code)) return code!;
        Ex ex = Expr(e, GdType.Bool);
        return Truth(ex);
    }

    private static string WrapIf(string code, bool wrap) => wrap ? "(" + code + ")" : code;

    private bool TryNarrow(Identifier id, TypeTest tt, out string? code)
    {
        code = null;
        Ex target = Expr(id, null);
        GdType t = ResolveType(tt.Type);
        if (target.Type.IsVariant || !(t.Kind is GK.Object or GK.ScriptClass) || target.Type.Kind is GK.Struct) return false;
        string name = Fresh(Naming.Camel(t.Cs.Split('.').Last()));
        code = $"{target.Code} is {t.Cs} {name}";
        _ctx.Narrow[id.Name] = (name, t);
        return true;
    }

    // `if x is A or x is B:` leaves x typed as the nearest API class A and B share inside the branch, so `x.get_rid()` there is a typed call.
    // The cast is safe because the condition holds; a branch that assigns x keeps it as it was.
    private void NarrowOrChain(Expr cond, Block body)
    {
        var tests = new List<TypeTest>();
        if (!CollectOrTests(cond, tests) || tests.Count < 2 || tests[0].Value is not Identifier id) return;
        if (tests.Any(t => t.Value is not Identifier other || other.Name != id.Name) || AssignsTo(body, id.Name)) return;
        Ex target = Expr(id, null);
        if (target.Type.Kind != GK.Object) return;
        Rs.INamedTypeSymbol? common = null;
        foreach (TypeTest t in tests)
        {
            GdType ty = ResolveType(t.Type);
            if (ty.Kind != GK.Object || ty.Sym == null) return;
            common = common == null ? ty.Sym : CommonBase(common, ty.Sym);
            if (common == null) return;
        }
        if (!ApiModel.IsGodotDerived(common) || common!.Name == "GodotObject" || (target.Type.Sym != null && GdType.Derives(target.Type.Sym, common))) return;
        GdType narrowed = GdType.Obj(common);
        _ctx.Narrow[id.Name] = ($"(({narrowed.Cs}){target.Code})", narrowed);
    }

    private static bool CollectOrTests(Expr e, List<TypeTest> into)
    {
        switch (e)
        {
            case TypeTest tt: into.Add(tt); return true;
            case Binary { Op: "or" } b: return CollectOrTests(b.Left, into) && CollectOrTests(b.Right, into);
            default: return false;
        }
    }

    private static Rs.INamedTypeSymbol? CommonBase(Rs.INamedTypeSymbol a, Rs.INamedTypeSymbol b)
    {
        for (Rs.INamedTypeSymbol? t = a; t != null; t = t.BaseType)
            if (GdType.Derives(b, t)) return t;
        return null;
    }

    private void EmitIf(IfStmt s)
    {
        for (int i = 0; i < s.Branches.Count; i++)
        {
            IfBranch br = s.Branches[i];
            var saved = new Dictionary<string, (string, GdType)>(_ctx.Narrow);
            string cond = CondCode(br.Cond);
            NarrowOrChain(br.Cond, br.Body);
            if (i > 0) _w.Line("else if (" + cond + ")");
            else _w.Line("if (" + cond + ")");
            _w.Line("{"); _w.Indent++;
            PushScope(); EmitBlockBody(br.Body); PopScope();
            _w.Indent--; _w.Line("}");
            _ctx.Narrow = saved;
        }
        if (s.Else != null)
        {
            _w.Line("else");
            _w.Line("{"); _w.Indent++;
            PushScope(); EmitBlockBody(s.Else); PopScope();
            _w.Indent--; _w.Line("}");
        }
    }

    private void EmitWhile(WhileStmt w)
    {
        var saved = new Dictionary<string, (string, GdType)>(_ctx.Narrow);
        string cond = CondCode(w.Cond);
        EmitScoped($"while ({cond})", w.Body);
        _ctx.Narrow = saved;
    }

    // ---- for ----

    private bool AssignsTo(Node scope, string name) => AssignsIn(scope).Any(a => IsNamed(a.Target, name));

    // A bound can stay in the loop condition when nothing can change it while the loop runs: a literal, a constant, or a local the body
    // never assigns. Anything else (items.size(), a field) is read once, as range() reads it.
    private bool IsPlainBound(Expr source, Ex e, Block body)
    {
        if (e.Const != null && source is Literal or Unary) return true;
        if (source is Identifier id)
        {
            if (FindLocal(id.Name) != null) return !AssignsTo(body, id.Name);
            return FindScriptMember(id.Name).Member is { Kind: MK.Const };
        }
        return false;
    }

    private void EmitFor(ForStmt f)
    {
        string cs = LocalName(f, f.VarName);
        GdType declared = f.VarType != null ? ResolveType(f.VarType) : GdType.Variant;

        // range(n), range(a, b), range(a, b, step) and a bare integer.
        List<Expr>? rangeArgs = null;
        if (f.Iterable is Call { Callee: Identifier { Name: "range" } } rc && rc.Args.Count is >= 1 and <= 3 && FindLocal("range") == null) rangeArgs = rc.Args;
        else if (TypeOf(f.Iterable).Kind == GK.Int) rangeArgs = new List<Expr> { f.Iterable };

        if (rangeArgs != null && !AssignsTo(f.Body, f.VarName))
        {
            Ex[] a = rangeArgs.Select(x => Expr(x, GdType.Int)).ToArray();
            if (a.All(x => x.Type.Kind == GK.Int || x.Type.IsVariant || x.Type.Kind == GK.Enum))
            {
                bool step3 = a.Length == 3;
                long step = 1;
                bool stepKnown = !step3 || (a[2].Const is long sl && sl != 0 && (step = sl) != 0);
                if (stepKnown)
                {
                    GdType vt = declared.Kind == GK.Int ? declared : GdType.Int;
                    string start = a.Length == 1 ? "0" : Adapt(a[0], vt);
                    Ex endEx = a.Length == 1 ? a[0] : a[1];
                    string end = Adapt(endEx, GdType.Int);
                    string decl = $"{vt.Cs} {cs} = {start}";
                    string limit = end;
                    if (!IsPlainBound(rangeArgs[a.Length == 1 ? 0 : 1], endEx, f.Body))
                    {
                        string endName = Fresh(cs.TrimStart('@') + "End");
                        decl += $", {endName} = {end}";
                        limit = endName;
                    }
                    string cond = step > 0 ? $"{cs} < {limit}" : $"{cs} > {limit}";
                    string incr = step == 1 ? $"{cs}++" : step == -1 ? $"{cs}--" : step > 0 ? $"{cs} += {step}" : $"{cs} -= {-step}";
                    _w.Open($"for ({decl}; {cond}; {incr})");
                    PushScope(); Declare(f.VarName, cs, vt, f); EmitBlockBody(f.Body); PopScope();
                    _w.Close();
                    return;
                }
            }
            // A step that is not a literal: let the runtime count.
            string args = string.Join(", ", a.Select(x => Adapt(x, GdType.Int)));
            _w.Open($"foreach (long {cs} in GDScript.Range({args}))");
            PushScope(); Declare(f.VarName, cs, GdType.Int, f); EmitBlockBody(f.Body); PopScope();
            _w.Close();
            return;
        }
        if (rangeArgs != null)
        {
            Ex[] a = rangeArgs.Select(x => Expr(x, GdType.Int)).ToArray();
            _w.Open($"foreach (long {cs} in GDScript.Range({string.Join(", ", a.Select(x => Adapt(x, GdType.Int)))}))");
            PushScope(); Declare(f.VarName, cs, GdType.Int, f); EmitBlockBody(f.Body); PopScope();
            _w.Close();
            return;
        }

        Ex it = Expr(f.Iterable, null);
        string source = it.Code;
        GdType elem;
        switch (it.Type.Kind)
        {
            case GK.Array: elem = it.Type.Elem ?? GdType.Variant; break;
            case GK.Packed: elem = it.Type.Elem ?? GdType.Variant; break;
            case GK.Dict: source = Wrap(it, P.Primary) + ".Keys"; elem = it.Type.Key ?? GdType.Variant; break;
            case GK.String: source = "GDScript.Chars(" + it.Code + ")"; elem = GdType.String; break;
            case GK.Struct when it.Type.Sym?.Name == "Vector2I": source = "GDScript.Iterate(" + VariantFromTyped(it) + ")"; elem = GdType.Variant; break;
            default: source = "GDScript.Iterate(" + VariantFromTyped(it) + ")"; elem = GdType.Variant; break;
        }
        GdType vtype = declared.IsVariant ? elem : declared;
        // A Variant element that the loop declares a concrete type for converts through Variant's own explicit operators when it has one.
        bool direct = elem.IsVariant && vtype.Kind is GK.Int or GK.Float or GK.Bool or GK.String;
        if (elem.IsVariant && !vtype.IsVariant && !direct)
        {
            string item = Fresh(cs.TrimStart('@') + "Item");
            _w.Open($"foreach (Variant {item} in {source})");
            _w.Line($"{vtype.Cs} {cs} = {FromVariant(item, vtype)};");
            PushScope(); Declare(f.VarName, cs, vtype, f); EmitBlockBody(f.Body); PopScope();
            _w.Close();
            return;
        }
        if (!elem.IsVariant && !declared.IsVariant && !declared.Accepts(elem))
        {
            string item = Fresh(cs.TrimStart('@') + "Item");
            _w.Open($"foreach ({elem.Cs} {item} in {source})");
            _w.Line($"{declared.Cs} {cs} = {Adapt(new Ex(item, elem), declared)};");
            PushScope(); Declare(f.VarName, cs, declared, f); EmitBlockBody(f.Body); PopScope();
            _w.Close();
            return;
        }
        _w.Open($"foreach ({vtype.Cs} {cs} in {source})");
        PushScope(); Declare(f.VarName, cs, vtype, f); EmitBlockBody(f.Body); PopScope();
        _w.Close();
    }
}
