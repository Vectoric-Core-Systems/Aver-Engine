// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Expression: Godot's small expression evaluator (Expression.parse / execute). AnimationTree transitions use it for advance_expression.
// Literals, identifiers (the named inputs, then the base object's properties), member access, indexing, method calls on the base object
// or a value, a few built-in functions, and the operators with GDScript's Variant semantics. docs/plans/fabric/G5_PARITY.md, A2.

using System.Globalization;
using System.Text;

namespace Aver.Fabric;

/// <summary>Parses and evaluates a one-line expression (Godot's Expression).</summary>
public class Expression : RefCounted
{
    private abstract class Ast
    {
        public abstract Variant Eval(Ctx c);
    }

    private sealed class Ctx
    {
        public string[] Names = System.Array.Empty<string>();
        public Collections.Array Inputs = new();
        public GodotObject? Base;
    }

    private Ast? _root;
    private string _error = "";
    private bool _executeFailed;
    private string[] _inputNames = System.Array.Empty<string>();

    /// <summary>Compiles <paramref name="expression"/>; <paramref name="inputNames"/> names the values <see cref="Execute"/> will be given.</summary>
    public Error Parse(string expression, string[]? inputNames = null)
    {
        _inputNames = inputNames ?? System.Array.Empty<string>();
        _error = "";
        _root = null;
        try
        {
            var p = new Parser(expression);
            _root = p.ParseAll();
            return Error.Ok;
        }
        catch (FormatException ex)
        {
            _error = ex.Message;
            return Error.InvalidParameter;
        }
    }

    /// <summary>Evaluates the compiled expression. <paramref name="inputs"/> parallels the names given to <see cref="Parse"/>;
    /// <paramref name="baseInstance"/> answers identifiers and method calls the inputs do not.</summary>
    public Variant Execute(Collections.Array? inputs = null, GodotObject? baseInstance = null, bool showError = true, bool constCallsOnly = false)
    {
        _executeFailed = false;
        if (_root == null)
        {
            _executeFailed = true;
            if (_error.Length == 0) _error = "The expression has not been parsed.";
            return default;
        }
        try
        {
            return _root.Eval(new Ctx { Names = _inputNames, Inputs = inputs ?? new Collections.Array(), Base = baseInstance });
        }
        catch (Exception ex) when (ex is InvalidOperationException or IndexOutOfRangeException or KeyNotFoundException or ArgumentException or InvalidCastException or MissingMethodException or FormatException or DivideByZeroException)
        {
            _executeFailed = true;
            _error = ex.Message;
            if (showError) Aver.Scripting.Log.Warn($"[Fabric] Expression: {ex.Message}");
            return default;
        }
    }

    public bool HasExecuteFailed() => _executeFailed;

    public string GetErrorText() => _error;

    // ---- syntax tree ----

    private sealed class Const(Variant v) : Ast { public override Variant Eval(Ctx c) => v; }

    private sealed class Ident(string name) : Ast
    {
        public string Name => name;

        public override Variant Eval(Ctx c)
        {
            for (int i = 0; i < c.Names.Length; i++)
                if (c.Names[i] == name) return i < c.Inputs.Count ? c.Inputs[i] : default;
            switch (name)
            {
                case "self": return c.Base == null ? default : (Variant)c.Base;
                case "PI": return Math.PI;
                case "TAU": return Math.Tau;
                case "INF": return double.PositiveInfinity;
                case "NAN": return double.NaN;
            }
            if (c.Base != null)
            {
                if (MemberAccess.HasMember(c.Base.GetType(), name)) return c.Base.Get(name);
                Variant v = c.Base.Get(name);
                if (v.VariantType != Variant.Type.Nil) return v;
            }
            throw new InvalidOperationException($"Invalid named index '{name}' for the expression's base.");
        }
    }

    private sealed class Unary(string op, Ast a) : Ast
    {
        public override Variant Eval(Ctx c)
        {
            Variant v = a.Eval(c);
            return op switch
            {
                "-" => GDScript.Neg(v),
                "+" => GDScript.Plus(v),
                _ => !GDScript.Truthy(v),
            };
        }
    }

    private sealed class Binary(string op, Ast l, Ast r) : Ast
    {
        public override Variant Eval(Ctx c)
        {
            if (op == "&&") { Variant a = l.Eval(c); return GDScript.Truthy(a) ? GDScript.Truthy(r.Eval(c)) : false; }
            if (op == "||") { Variant a = l.Eval(c); return GDScript.Truthy(a) ? true : GDScript.Truthy(r.Eval(c)); }
            Variant x = l.Eval(c), y = r.Eval(c);
            return op switch
            {
                "+" => GDScript.Add(x, y),
                "-" => GDScript.Sub(x, y),
                "*" => GDScript.Mul(x, y),
                "/" => GDScript.Div(x, y),
                "%" => GDScript.Mod(x, y),
                "**" => GDScript.Pow(x, y),
                "==" => GDScript.Eq(x, y),
                "!=" => !GDScript.Eq(x, y),
                "<" => GDScript.Lt(x, y),
                "<=" => GDScript.Le(x, y),
                ">" => GDScript.Gt(x, y),
                ">=" => GDScript.Ge(x, y),
                "in" => GDScript.In(x, y),
                _ => throw new InvalidOperationException($"Unknown operator '{op}'."),
            };
        }
    }

    private sealed class Member(Ast target, string name) : Ast
    {
        public override Variant Eval(Ctx c) => GDScript.GetMember(target.Eval(c), name);
    }

    private sealed class Index(Ast target, Ast key) : Ast
    {
        public override Variant Eval(Ctx c) => GDScript.Index(target.Eval(c), key.Eval(c));
    }

    private sealed class ArrayLit(List<Ast> items) : Ast
    {
        public override Variant Eval(Ctx c)
        {
            var a = new Collections.Array();
            foreach (Ast i in items) a.Add(i.Eval(c));
            return a;
        }
    }

    private sealed class CallNode(Ast? target, string name, List<Ast> args) : Ast
    {
        public override Variant Eval(Ctx c)
        {
            var values = new Variant[args.Count];
            for (int i = 0; i < values.Length; i++) values[i] = args[i].Eval(c);
            if (target != null) return GDScript.CallMethod(target.Eval(c), name, values);
            if (c.Base != null && c.Base.HasMethod(name)) return c.Base.Call(name, values);
            if (Builtins.TryCall(name, values, out Variant result)) return result;
            throw new MissingMethodException($"Invalid call. Nonexistent function '{name}'.");
        }
    }

    private static class Builtins
    {
        public static bool TryCall(string name, Variant[] a, out Variant r)
        {
            r = default;
            double D(int i) => i < a.Length ? a[i].AsDouble() : 0;
            switch (name)
            {
                case "abs": r = GDScript.Abs(a[0]); return true;
                case "min": r = GDScript.Min(a[0], a[1]); return true;
                case "max": r = GDScript.Max(a[0], a[1]); return true;
                case "clamp": r = GDScript.Lt(a[0], a[1]) ? a[1] : GDScript.Gt(a[0], a[2]) ? a[2] : a[0]; return true;
                case "sin": r = Math.Sin(D(0)); return true;
                case "cos": r = Math.Cos(D(0)); return true;
                case "tan": r = Math.Tan(D(0)); return true;
                case "sqrt": r = Math.Sqrt(D(0)); return true;
                case "pow": r = Math.Pow(D(0), D(1)); return true;
                case "floor": r = Math.Floor(D(0)); return true;
                case "ceil": r = Math.Ceiling(D(0)); return true;
                case "round": r = Math.Round(D(0), MidpointRounding.AwayFromZero); return true;
                case "sign": r = GDScript.Sign(D(0)); return true;
                case "lerp": r = GDScript.Lerp(D(0), D(1), D(2)); return true;
                case "is_zero_approx": r = Math.Abs(D(0)) < 1e-5; return true;
                case "is_equal_approx": r = Math.Abs(D(0) - D(1)) <= 1e-5 * Math.Max(1.0, Math.Abs(D(0))); return true;
                case "str": r = string.Concat(a.Select(v => v.AsString())); return true;
                case "int": r = a[0].AsInt64(); return true;
                case "float": r = a[0].AsDouble(); return true;
                case "bool": r = a[0].AsBool(); return true;
                case "len": r = GDScript.Len(a[0]); return true;
                case "Vector2": r = new Vector2((float)D(0), (float)D(1)); return true;
                case "Vector3": r = new Vector3((float)D(0), (float)D(1), (float)D(2)); return true;
                case "Color": r = new Color((float)D(0), (float)D(1), (float)D(2), a.Length > 3 ? (float)D(3) : 1f); return true;
            }
            return false;
        }
    }

    // ---- parser ----

    private sealed class Parser
    {
        private readonly string _s;
        private int _i;

        public Parser(string s) { _s = s; }

        public Ast ParseAll()
        {
            Ast a = Or();
            Skip();
            if (_i < _s.Length) throw Err($"Unexpected '{_s[_i]}'");
            return a;
        }

        private FormatException Err(string what) => new($"Parse error at column {_i + 1}: {what} in '{_s}'");

        private void Skip() { while (_i < _s.Length && char.IsWhiteSpace(_s[_i])) _i++; }

        private bool Peek(string tok)
        {
            Skip();
            return string.CompareOrdinal(_s, _i, tok, 0, tok.Length) == 0;
        }

        private bool Take(string tok)
        {
            if (!Peek(tok)) return false;
            _i += tok.Length;
            return true;
        }

        // A word operator ("and", "or", "not", "in") must not be the start of a longer identifier.
        private bool TakeWord(string word)
        {
            Skip();
            if (string.CompareOrdinal(_s, _i, word, 0, word.Length) != 0) return false;
            int end = _i + word.Length;
            if (end < _s.Length && (char.IsLetterOrDigit(_s[end]) || _s[end] == '_')) return false;
            _i = end;
            return true;
        }

        private Ast Or()
        {
            Ast l = And();
            while (true)
            {
                if (Take("||") || TakeWord("or")) l = new Binary("||", l, And());
                else return l;
            }
        }

        private Ast And()
        {
            Ast l = Cmp();
            while (true)
            {
                if (Take("&&") || TakeWord("and")) l = new Binary("&&", l, Cmp());
                else return l;
            }
        }

        private Ast Cmp()
        {
            Ast l = Add();
            while (true)
            {
                string? op = null;
                foreach (string t in new[] { "==", "!=", "<=", ">=", "<", ">" })
                    if (Peek(t)) { op = t; break; }
                if (op != null) { _i += op.Length; l = new Binary(op, l, Add()); continue; }
                if (TakeWord("in")) { l = new Binary("in", l, Add()); continue; }
                return l;
            }
        }

        private Ast Add()
        {
            Ast l = Mul();
            while (true)
            {
                if (Take("+")) l = new Binary("+", l, Mul());
                else if (Peek("-")) { _i++; l = new Binary("-", l, Mul()); }
                else return l;
            }
        }

        private Ast Mul()
        {
            Ast l = Unary();
            while (true)
            {
                if (Peek("**")) return l;
                if (Take("*")) l = new Binary("*", l, Unary());
                else if (Take("/")) l = new Binary("/", l, Unary());
                else if (Take("%")) l = new Binary("%", l, Unary());
                else return l;
            }
        }

        private Ast Unary()
        {
            Skip();
            if (Peek("!") && !Peek("!=")) { _i++; return new Unary("!", Unary()); }
            if (TakeWord("not")) return new Unary("!", Unary());
            if (Take("-")) return new Unary("-", Unary());
            if (Take("+")) return new Unary("+", Unary());
            return Pow();
        }

        private Ast Pow()
        {
            Ast l = Postfix();
            if (Take("**")) return new Binary("**", l, Unary());
            return l;
        }

        private Ast Postfix()
        {
            Ast a = Primary();
            while (true)
            {
                if (Take("."))
                {
                    string name = Word();
                    if (Take("(")) a = new CallNode(a, name, Args(")"));
                    else a = new Member(a, name);
                }
                else if (Take("[")) { Ast k = Or(); if (!Take("]")) throw Err("Expected ']'"); a = new Index(a, k); }
                else if (a is Ident id && Peek("(")) { _i++; a = new CallNode(null, id.Name, Args(")")); }
                else return a;
            }
        }

        private List<Ast> Args(string close)
        {
            var list = new List<Ast>();
            if (Take(close)) return list;
            while (true)
            {
                list.Add(Or());
                if (Take(",")) continue;
                if (Take(close)) return list;
                throw Err($"Expected ',' or '{close}'");
            }
        }

        private string Word()
        {
            Skip();
            int s = _i;
            while (_i < _s.Length && (char.IsLetterOrDigit(_s[_i]) || _s[_i] == '_')) _i++;
            if (s == _i) throw Err("Expected a name");
            return _s[s.._i];
        }

        private Ast Primary()
        {
            Skip();
            if (_i >= _s.Length) throw Err("Unexpected end of expression");
            char ch = _s[_i];
            if (ch == '(') { _i++; Ast a = Or(); if (!Take(")")) throw Err("Expected ')'"); return a; }
            if (ch == '[') { _i++; return new ArrayLit(Args("]")); }
            if (ch == '"' || ch == '\'') return new Const(Str(ch));
            if (char.IsDigit(ch) || (ch == '.' && _i + 1 < _s.Length && char.IsDigit(_s[_i + 1]))) return new Const(Number());
            if (char.IsLetter(ch) || ch == '_')
            {
                string w = Word();
                return w switch
                {
                    "true" => new Const(true),
                    "false" => new Const(false),
                    "null" => new Const(default(Variant)),
                    _ => new Ident(w),
                };
            }
            throw Err($"Unexpected '{ch}'");
        }

        private Variant Str(char quote)
        {
            _i++;
            var sb = new StringBuilder();
            while (_i < _s.Length && _s[_i] != quote)
            {
                if (_s[_i] == '\\' && _i + 1 < _s.Length)
                {
                    _i++;
                    sb.Append(_s[_i] switch { 'n' => '\n', 't' => '\t', 'r' => '\r', _ => _s[_i] });
                }
                else sb.Append(_s[_i]);
                _i++;
            }
            if (_i >= _s.Length) throw Err("Unterminated string");
            _i++;
            return sb.ToString();
        }

        private Variant Number()
        {
            int s = _i;
            if (_s[_i] == '0' && _i + 1 < _s.Length && (_s[_i + 1] == 'x' || _s[_i + 1] == 'X'))
            {
                _i += 2;
                int h = _i;
                while (_i < _s.Length && Uri.IsHexDigit(_s[_i])) _i++;
                return long.Parse(_s[h.._i], NumberStyles.HexNumber, CultureInfo.InvariantCulture);
            }
            bool real = false;
            while (_i < _s.Length && (char.IsDigit(_s[_i]) || _s[_i] == '_')) _i++;
            if (_i < _s.Length && _s[_i] == '.' && !(_i + 1 < _s.Length && _s[_i + 1] == '.')) { real = true; _i++; while (_i < _s.Length && char.IsDigit(_s[_i])) _i++; }
            if (_i < _s.Length && (_s[_i] == 'e' || _s[_i] == 'E'))
            {
                int save = _i;
                _i++;
                if (_i < _s.Length && (_s[_i] == '+' || _s[_i] == '-')) _i++;
                if (_i < _s.Length && char.IsDigit(_s[_i])) { real = true; while (_i < _s.Length && char.IsDigit(_s[_i])) _i++; }
                else _i = save;
            }
            string text = _s[s.._i].Replace("_", "");
            if (real) return double.Parse(text, NumberStyles.Float, CultureInfo.InvariantCulture);
            return long.Parse(text, CultureInfo.InvariantCulture);
        }
    }
}
