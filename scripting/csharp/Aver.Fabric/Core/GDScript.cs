// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// GDScript: the semantics a converted .gd script needs that C# does not have. Typed code converts to plain C#; this class is
// what the converter calls for untyped (Variant) operands, the % formatting operator, dynamic member access and the GDScript
// built-ins that have no GodotSharp twin (docs/plans/fabric/G3_SCRIPTS.md, "GDScript to C#").

using System.Globalization;
using System.Text;
using Aver.Fabric.Collections;

namespace Aver.Fabric;

/// <summary>GDScript's operators, conversions, dynamic access and built-in functions, over <see cref="Variant"/>.</summary>
public static class GDScript
{
    private static InvalidOperationException Invalid(string op, Variant a, Variant b) =>
        new($"Invalid operands '{a.VariantType}' and '{b.VariantType}' in operator '{op}'.");

    private static bool IsInt(Variant v) => v.VariantType is Variant.Type.Int or Variant.Type.Bool;
    private static bool IsNum(Variant v) => v.VariantType is Variant.Type.Int or Variant.Type.Float or Variant.Type.Bool;
    private static bool IsText(Variant v) => v.VariantType is Variant.Type.String or Variant.Type.StringName;

    // ---- arithmetic on Variants ----

    public static Variant Add(Variant a, Variant b)
    {
        if (IsInt(a) && IsInt(b)) return unchecked(a.AsInt64() + b.AsInt64());
        if (IsNum(a) && IsNum(b)) return a.AsDouble() + b.AsDouble();
        if (IsText(a) && IsText(b)) return a.AsString() + b.AsString();
        switch (a.VariantType, b.VariantType)
        {
            case (Variant.Type.Vector2, Variant.Type.Vector2): return a.AsVector2() + b.AsVector2();
            case (Variant.Type.Vector3, Variant.Type.Vector3): return a.AsVector3() + b.AsVector3();
            case (Variant.Type.Vector4, Variant.Type.Vector4): return a.AsVector4() + b.AsVector4();
            case (Variant.Type.Vector2I, Variant.Type.Vector2I): return a.AsVector2I() + b.AsVector2I();
            case (Variant.Type.Vector3I, Variant.Type.Vector3I): return a.AsVector3I() + b.AsVector3I();
            case (Variant.Type.Color, Variant.Type.Color): return a.AsColor() + b.AsColor();
            case (Variant.Type.Quaternion, Variant.Type.Quaternion): return a.AsQuaternion() + b.AsQuaternion();
            case (Variant.Type.Basis, Variant.Type.Basis): return a.AsBasis() + b.AsBasis();
            case (Variant.Type.Array, Variant.Type.Array):
            {
                var r = a.AsGodotArray().Duplicate();
                r.AddRange(b.AsGodotArray());
                return r;
            }
        }
        throw Invalid("+", a, b);
    }

    public static Variant Sub(Variant a, Variant b)
    {
        if (IsInt(a) && IsInt(b)) return unchecked(a.AsInt64() - b.AsInt64());
        if (IsNum(a) && IsNum(b)) return a.AsDouble() - b.AsDouble();
        switch (a.VariantType, b.VariantType)
        {
            case (Variant.Type.Vector2, Variant.Type.Vector2): return a.AsVector2() - b.AsVector2();
            case (Variant.Type.Vector3, Variant.Type.Vector3): return a.AsVector3() - b.AsVector3();
            case (Variant.Type.Vector4, Variant.Type.Vector4): return a.AsVector4() - b.AsVector4();
            case (Variant.Type.Vector2I, Variant.Type.Vector2I): return a.AsVector2I() - b.AsVector2I();
            case (Variant.Type.Vector3I, Variant.Type.Vector3I): return a.AsVector3I() - b.AsVector3I();
            case (Variant.Type.Color, Variant.Type.Color): return a.AsColor() - b.AsColor();
            case (Variant.Type.Quaternion, Variant.Type.Quaternion): return a.AsQuaternion() - b.AsQuaternion();
            case (Variant.Type.Basis, Variant.Type.Basis): return a.AsBasis() - b.AsBasis();
        }
        throw Invalid("-", a, b);
    }

    public static Variant Mul(Variant a, Variant b)
    {
        if (IsInt(a) && IsInt(b)) return unchecked(a.AsInt64() * b.AsInt64());
        if (IsNum(a) && IsNum(b)) return a.AsDouble() * b.AsDouble();
        if (IsNum(b))
        {
            float s = (float)b.AsDouble();
            switch (a.VariantType)
            {
                case Variant.Type.Vector2: return a.AsVector2() * s;
                case Variant.Type.Vector3: return a.AsVector3() * s;
                case Variant.Type.Vector4: return a.AsVector4() * s;
                case Variant.Type.Color: return a.AsColor() * s;
                case Variant.Type.Quaternion: return a.AsQuaternion() * s;
                case Variant.Type.Basis: return a.AsBasis() * s;
                case Variant.Type.Vector2I when IsInt(b): return a.AsVector2I() * (int)b.AsInt64();
                case Variant.Type.Vector3I when IsInt(b): return a.AsVector3I() * (int)b.AsInt64();
            }
        }
        if (IsNum(a))
        {
            float s = (float)a.AsDouble();
            switch (b.VariantType)
            {
                case Variant.Type.Vector2: return s * b.AsVector2();
                case Variant.Type.Vector3: return s * b.AsVector3();
                case Variant.Type.Vector4: return s * b.AsVector4();
                case Variant.Type.Color: return s * b.AsColor();
                case Variant.Type.Quaternion: return s * b.AsQuaternion();
            }
        }
        switch (a.VariantType, b.VariantType)
        {
            case (Variant.Type.Vector2, Variant.Type.Vector2): return a.AsVector2() * b.AsVector2();
            case (Variant.Type.Vector3, Variant.Type.Vector3): return a.AsVector3() * b.AsVector3();
            case (Variant.Type.Vector4, Variant.Type.Vector4): return a.AsVector4() * b.AsVector4();
            case (Variant.Type.Vector2I, Variant.Type.Vector2I): return a.AsVector2I() * b.AsVector2I();
            case (Variant.Type.Vector3I, Variant.Type.Vector3I): return a.AsVector3I() * b.AsVector3I();
            case (Variant.Type.Color, Variant.Type.Color): return a.AsColor() * b.AsColor();
            case (Variant.Type.Quaternion, Variant.Type.Quaternion): return a.AsQuaternion() * b.AsQuaternion();
            case (Variant.Type.Quaternion, Variant.Type.Vector3): return a.AsQuaternion() * b.AsVector3();
            case (Variant.Type.Basis, Variant.Type.Basis): return a.AsBasis() * b.AsBasis();
            case (Variant.Type.Basis, Variant.Type.Vector3): return a.AsBasis() * b.AsVector3();
            case (Variant.Type.Vector3, Variant.Type.Basis): return a.AsVector3() * b.AsBasis();
            case (Variant.Type.Transform3D, Variant.Type.Transform3D): return a.AsTransform3D() * b.AsTransform3D();
            case (Variant.Type.Transform3D, Variant.Type.Vector3): return a.AsTransform3D() * b.AsVector3();
        }
        throw Invalid("*", a, b);
    }

    public static Variant Div(Variant a, Variant b)
    {
        if (IsInt(a) && IsInt(b))
        {
            long d = b.AsInt64();
            if (d == 0) throw new DivideByZeroException("Division by zero error in operator '/'.");
            return a.AsInt64() / d;
        }
        if (IsNum(a) && IsNum(b)) return a.AsDouble() / b.AsDouble();
        if (IsNum(b))
        {
            float s = (float)b.AsDouble();
            switch (a.VariantType)
            {
                case Variant.Type.Vector2: return a.AsVector2() / s;
                case Variant.Type.Vector3: return a.AsVector3() / s;
                case Variant.Type.Vector4: return a.AsVector4() / s;
                case Variant.Type.Color: return a.AsColor() / s;
                case Variant.Type.Quaternion: return a.AsQuaternion() / s;
            }
        }
        switch (a.VariantType, b.VariantType)
        {
            case (Variant.Type.Vector2, Variant.Type.Vector2): return a.AsVector2() / b.AsVector2();
            case (Variant.Type.Vector3, Variant.Type.Vector3): return a.AsVector3() / b.AsVector3();
            case (Variant.Type.Vector4, Variant.Type.Vector4): return a.AsVector4() / b.AsVector4();
        }
        throw Invalid("/", a, b);
    }

    public static Variant Mod(Variant a, Variant b)
    {
        if (IsInt(a) && IsInt(b))
        {
            long d = b.AsInt64();
            if (d == 0) throw new DivideByZeroException("Division by zero error in operator '%'.");
            return a.AsInt64() % d;
        }
        if (IsText(a)) return Format(a.AsString(), b);
        if (IsNum(a) && IsNum(b)) return a.AsDouble() % b.AsDouble();
        switch (a.VariantType, b.VariantType)
        {
            case (Variant.Type.Vector2, Variant.Type.Vector2): return a.AsVector2() % b.AsVector2();
            case (Variant.Type.Vector3, Variant.Type.Vector3): return a.AsVector3() % b.AsVector3();
        }
        throw Invalid("%", a, b);
    }

    /// <summary>The ** operator: int ** int stays an int (a negative exponent gives 0, as an integer power truncates), anything else is a float.</summary>
    public static Variant Pow(Variant a, Variant b)
    {
        if (IsInt(a) && IsInt(b)) return IntPow(a.AsInt64(), b.AsInt64());
        if (IsNum(a) && IsNum(b)) return Math.Pow(a.AsDouble(), b.AsDouble());
        throw Invalid("**", a, b);
    }

    public static long IntPow(long b, long e)
    {
        if (e < 0) return b == 1 ? 1 : b == -1 ? (e % 2 == 0 ? 1 : -1) : 0;
        long r = 1;
        while (e > 0)
        {
            if ((e & 1) != 0) r = unchecked(r * b);
            b = unchecked(b * b);
            e >>= 1;
        }
        return r;
    }

    public static Variant Neg(Variant a) => a.VariantType switch
    {
        Variant.Type.Int or Variant.Type.Bool => unchecked(-a.AsInt64()),
        Variant.Type.Float => -a.AsDouble(),
        Variant.Type.Vector2 => -a.AsVector2(),
        Variant.Type.Vector3 => -a.AsVector3(),
        Variant.Type.Vector4 => -a.AsVector4(),
        Variant.Type.Vector2I => -a.AsVector2I(),
        Variant.Type.Vector3I => -a.AsVector3I(),
        Variant.Type.Quaternion => -a.AsQuaternion(),
        Variant.Type.Plane => -a.AsPlane(),
        _ => throw new InvalidOperationException($"Invalid operand '{a.VariantType}' in unary operator '-'."),
    };

    public static Variant Plus(Variant a) =>
        IsNum(a) || a.VariantType is Variant.Type.Vector2 or Variant.Type.Vector3 or Variant.Type.Vector4 ? a : throw new InvalidOperationException($"Invalid operand '{a.VariantType}' in unary operator '+'.");

    public static Variant BitNot(Variant a) => IsInt(a) ? ~a.AsInt64() : throw new InvalidOperationException($"Invalid operand '{a.VariantType}' in unary operator '~'.");
    public static Variant BitAnd(Variant a, Variant b) => IsInt(a) && IsInt(b) ? a.AsInt64() & b.AsInt64() : throw Invalid("&", a, b);
    public static Variant BitOr(Variant a, Variant b) => IsInt(a) && IsInt(b) ? a.AsInt64() | b.AsInt64() : throw Invalid("|", a, b);
    public static Variant BitXor(Variant a, Variant b) => IsInt(a) && IsInt(b) ? a.AsInt64() ^ b.AsInt64() : throw Invalid("^", a, b);
    public static Variant Shl(Variant a, Variant b) => IsInt(a) && IsInt(b) ? a.AsInt64() << (int)b.AsInt64() : throw Invalid("<<", a, b);
    public static Variant Shr(Variant a, Variant b) => IsInt(a) && IsInt(b) ? a.AsInt64() >> (int)b.AsInt64() : throw Invalid(">>", a, b);

    // ---- comparison ----

    public static bool Eq(Variant a, Variant b) => a == b;

    private static int Compare(string op, Variant a, Variant b)
    {
        if (IsNum(a) && IsNum(b)) return IsInt(a) && IsInt(b) ? a.AsInt64().CompareTo(b.AsInt64()) : a.AsDouble().CompareTo(b.AsDouble());
        if (IsText(a) && IsText(b)) return string.CompareOrdinal(a.AsString(), b.AsString());
        switch (a.VariantType, b.VariantType)
        {
            case (Variant.Type.Vector2, Variant.Type.Vector2): return a.AsVector2().CompareTo(b.AsVector2());
            case (Variant.Type.Vector3, Variant.Type.Vector3): return a.AsVector3().CompareTo(b.AsVector3());
            case (Variant.Type.Array, Variant.Type.Array):
            {
                var x = a.AsGodotArray(); var y = b.AsGodotArray();
                for (int i = 0; i < Math.Min(x.Count, y.Count); i++)
                {
                    int c = Compare(op, x[i], y[i]);
                    if (c != 0) return c;
                }
                return x.Count.CompareTo(y.Count);
            }
        }
        throw Invalid(op, a, b);
    }

    // A NaN compares false in every direction, as in GDScript.
    private static bool NaNs(Variant a, Variant b) =>
        (a.VariantType == Variant.Type.Float && double.IsNaN(a.AsDouble())) || (b.VariantType == Variant.Type.Float && double.IsNaN(b.AsDouble()));

    public static bool Lt(Variant a, Variant b) => !NaNs(a, b) && Compare("<", a, b) < 0;
    public static bool Le(Variant a, Variant b) => !NaNs(a, b) && Compare("<=", a, b) <= 0;
    public static bool Gt(Variant a, Variant b) => !NaNs(a, b) && Compare(">", a, b) > 0;
    public static bool Ge(Variant a, Variant b) => !NaNs(a, b) && Compare(">=", a, b) >= 0;

    /// <summary>The <c>in</c> operator: an element of an Array, a key of a Dictionary, a substring, a property of an Object.</summary>
    public static bool In(Variant item, Variant container)
    {
        switch (container.VariantType)
        {
            case Variant.Type.Array: return container.AsGodotArray().Contains(item);
            case Variant.Type.Dictionary: return container.AsGodotDictionary().ContainsKey(item);
            case Variant.Type.String:
            case Variant.Type.StringName: return container.AsString().Contains(item.AsString(), StringComparison.Ordinal);
            case Variant.Type.Object:
                return container.AsGodotObject() is { } o && (o.GetExportedPropertyNames().Contains(item.AsString()) || o.HasMethod(item.AsString()));
            case Variant.Type.PackedStringArray: return container.AsStringArray().Contains(item.AsString());
            case Variant.Type.PackedInt32Array:
            case Variant.Type.PackedInt64Array: return container.AsInt64Array().Contains(item.AsInt64());
            case Variant.Type.PackedFloat32Array:
            case Variant.Type.PackedFloat64Array: return container.AsFloat64Array().Contains(item.AsDouble());
            case Variant.Type.PackedVector3Array: return container.AsVector3Array().Contains(item.AsVector3());
            case Variant.Type.PackedVector2Array: return container.AsVector2Array().Contains(item.AsVector2());
        }
        throw new InvalidOperationException($"Invalid operands '{item.VariantType}' and '{container.VariantType}' in operator 'in'.");
    }

    /// <summary>GDScript truthiness (the same as <see cref="Variant.Booleanize"/>).</summary>
    public static bool Truthy(Variant v) => v.Booleanize();

    // ---- integer helpers C# lacks ----

    /// <summary>Godot's posmod: the remainder takes the sign of the divisor.</summary>
    public static long PosMod(long a, long b)
    {
        if (b == 0) throw new DivideByZeroException("Division by zero error in posmod.");
        long r = a % b;
        return r != 0 && (r < 0) != (b < 0) ? r + b : r;
    }

    public static double FPosMod(double a, double b)
    {
        double r = a % b;
        return r != 0 && (r < 0) != (b < 0) ? r + b : r;
    }

    public static long Wrap(long value, long min, long max)
    {
        long range = max - min;
        return range == 0 ? min : min + PosMod(value - min, range);
    }

    public static double Wrap(double value, double min, double max)
    {
        double range = max - min;
        return Mathf.IsZeroApprox(range) ? min : value - range * Math.Floor((value - min) / range);
    }

    public static long Snapped(long value, long step) => step == 0 ? value : (long)Math.Floor(value / (double)step + 0.5) * step;
    public static double Snapped(double value, double step) => step == 0 ? value : Math.Floor(value / step + 0.5) * step;

    public static long Sign(long v) => Math.Sign(v);
    public static double Sign(double v) => double.IsNaN(v) ? 0.0 : Math.Sign(v);

    public static double PingPong(double value, double length)
    {
        if (length == 0) return 0;
        double w = Wrap(value, 0, length * 2);
        return w > length ? length * 2 - w : w;
    }

    public static double LinearToDb(double linear) => Math.Log(linear) * 8.6858896380650365530225783783321;
    public static double DbToLinear(double db) => Math.Exp(db * 0.11512925464970228420089957273422);

    public static long Clamp(long v, long min, long max) => v < min ? min : v > max ? max : v;
    public static double Clamp(double v, double min, double max) => v < min ? min : v > max ? max : v;

    public static double Lerp(double from, double to, double weight) => from + (to - from) * weight;

    /// <summary>The Variant that lerp(a, b, t) makes: numbers, vectors, colours, quaternions and angles of any one kind.</summary>
    public static Variant Lerp(Variant from, Variant to, Variant weight)
    {
        double w = weight.AsDouble();
        float f = (float)w;
        switch (from.VariantType, to.VariantType)
        {
            case (Variant.Type.Vector2, Variant.Type.Vector2): return from.AsVector2().Lerp(to.AsVector2(), f);
            case (Variant.Type.Vector3, Variant.Type.Vector3): return from.AsVector3().Lerp(to.AsVector3(), f);
            case (Variant.Type.Color, Variant.Type.Color): return from.AsColor().Lerp(to.AsColor(), f);
        }
        if (IsNum(from) && IsNum(to)) return Lerp(from.AsDouble(), to.AsDouble(), w);
        throw Invalid("lerp", from, to);
    }

    public static Variant Min(Variant a, Variant b) => Lt(b, a) ? b : a;
    public static Variant Max(Variant a, Variant b) => Gt(b, a) ? b : a;

    public static Variant Abs(Variant a) => a.VariantType switch
    {
        Variant.Type.Int => Math.Abs(a.AsInt64()),
        Variant.Type.Float => Math.Abs(a.AsDouble()),
        Variant.Type.Vector2 => a.AsVector2().Abs(),
        Variant.Type.Vector3 => a.AsVector3().Abs(),
        _ => throw new InvalidOperationException($"Invalid argument '{a.VariantType}' in abs()."),
    };

    // ---- conversions (int(), float(), bool(), str()) ----

    public static long ToInt(Variant v) => v.AsInt64();
    public static long ToInt(double d) => double.IsNaN(d) ? 0 : (long)Math.Clamp(d, long.MinValue, long.MaxValue);
    public static long ToInt(string s) => Variant.From(s).AsInt64();
    public static long ToInt(bool b) => b ? 1 : 0;
    public static double ToFloat(Variant v) => v.AsDouble();
    public static double ToFloat(string s) => Variant.From(s).AsDouble();
    public static double ToFloat(bool b) => b ? 1.0 : 0.0;

    // ---- the % formatting operator ----

    /// <summary>GDScript's <c>format % values</c>: an Array spreads over the placeholders, anything else is the single value.</summary>
    public static string Format(string fmt, Variant values)
    {
        IList<Variant> args = values.VariantType == Variant.Type.Array ? values.AsGodotArray() : new[] { values };
        var sb = new StringBuilder();
        int next = 0;
        Variant Take(string what)
        {
            if (next >= args.Count) throw new InvalidOperationException("not enough arguments for format string");
            return args[next++];
        }
        for (int i = 0; i < fmt.Length; i++)
        {
            char c = fmt[i];
            if (c != '%') { sb.Append(c); continue; }
            i++;
            if (i >= fmt.Length) throw new InvalidOperationException("incomplete format");
            if (fmt[i] == '%') { sb.Append('%'); continue; }
            bool left = false, plus = false, zero = false, space = false;
            for (; i < fmt.Length; i++)
            {
                if (fmt[i] == '-') left = true;
                else if (fmt[i] == '+') plus = true;
                else if (fmt[i] == '0') zero = true;
                else if (fmt[i] == ' ') space = true;
                else break;
            }
            int width = 0;
            if (i < fmt.Length && fmt[i] == '*') { width = (int)Take("*").AsInt64(); i++; }
            else while (i < fmt.Length && char.IsAsciiDigit(fmt[i])) width = width * 10 + (fmt[i++] - '0');
            int prec = -1;
            if (i < fmt.Length && fmt[i] == '.')
            {
                i++;
                prec = 0;
                if (i < fmt.Length && fmt[i] == '*') { prec = (int)Take("*").AsInt64(); i++; }
                else while (i < fmt.Length && char.IsAsciiDigit(fmt[i])) prec = prec * 10 + (fmt[i++] - '0');
            }
            if (i >= fmt.Length) throw new InvalidOperationException("incomplete format");
            string body;
            bool numeric = true;
            switch (fmt[i])
            {
                case 's': body = Take("s").AsString(); if (prec >= 0 && prec < body.Length) body = body[..prec]; numeric = false; break;
                case 'c': body = ((char)Take("c").AsInt64()).ToString(); numeric = false; break;
                case 'd':
                {
                    long n = Take("d").AsInt64();
                    body = Math.Abs((decimal)n).ToString(CultureInfo.InvariantCulture);
                    if (prec > body.Length) body = body.PadLeft(prec, '0');
                    body = (n < 0 ? "-" : plus ? "+" : space ? " " : "") + body;
                    break;
                }
                case 'f':
                {
                    double d = Take("f").AsDouble();
                    string t = double.IsNaN(d) ? "nan" : double.IsInfinity(d) ? (d > 0 ? "inf" : "-inf") : Math.Round(d, Math.Min(Math.Max(prec < 0 ? 6 : prec, 0), 15), MidpointRounding.AwayFromZero).ToString("F" + (prec < 0 ? 6 : prec), CultureInfo.InvariantCulture);
                    body = (d >= 0 && !t.StartsWith('-') ? (plus ? "+" : space ? " " : "") : "") + t;
                    break;
                }
                case 'e':
                {
                    double d = Take("e").AsDouble();
                    string t = d.ToString((prec < 0 ? "0.000000" : prec == 0 ? "0" : "0." + new string('0', prec)) + "e+00", CultureInfo.InvariantCulture);
                    body = (d >= 0 ? (plus ? "+" : space ? " " : "") : "") + t;
                    break;
                }
                case 'x': body = Convert.ToString(Take("x").AsInt64(), 16); break;
                case 'X': body = Convert.ToString(Take("X").AsInt64(), 16).ToUpperInvariant(); break;
                case 'o': body = Convert.ToString(Take("o").AsInt64(), 8); break;
                case 'v':
                {
                    Variant v = Take("v");
                    body = v.VariantType switch
                    {
                        Variant.Type.Vector2 => FormatVec(prec, v.AsVector2().X, v.AsVector2().Y),
                        Variant.Type.Vector3 => FormatVec(prec, v.AsVector3().X, v.AsVector3().Y, v.AsVector3().Z),
                        _ => v.AsString(),
                    };
                    numeric = false;
                    break;
                }
                default: throw new InvalidOperationException($"unsupported format character '{fmt[i]}'");
            }
            if (body.Length < width)
            {
                if (left) body = body.PadRight(width);
                else if (zero && numeric)
                {
                    bool sign = body.Length > 0 && (body[0] == '-' || body[0] == '+' || body[0] == ' ');
                    body = sign ? body[0] + body[1..].PadLeft(width - 1, '0') : body.PadLeft(width, '0');
                }
                else body = body.PadLeft(width);
            }
            sb.Append(body);
        }
        if (next < args.Count) throw new InvalidOperationException("not all arguments converted during string formatting");
        return sb.ToString();
    }

    private static string FormatVec(int prec, params float[] parts) =>
        "(" + string.Join(", ", parts.Select(p => p.ToString("F" + (prec < 0 ? 6 : prec), CultureInfo.InvariantCulture))) + ")";

    /// <summary>The Array a Variant holds, or null when it holds anything else (match's array pattern).</summary>
    public static Collections.Array? AsArrayOrNull(Variant v) => v.VariantType == Variant.Type.Array ? v.AsGodotArray() : null;

    /// <summary>The Dictionary a Variant holds, or null (match's dictionary pattern).</summary>
    public static Collections.Dictionary? AsDictOrNull(Variant v) => v.VariantType == Variant.Type.Dictionary ? v.AsGodotDictionary() : null;

    // ---- ranges and iteration ----

    public static IEnumerable<long> Range(long to) { for (long i = 0; i < to; i++) yield return i; }
    public static IEnumerable<long> Range(long from, long to) { for (long i = from; i < to; i++) yield return i; }

    public static IEnumerable<long> Range(long from, long to, long step)
    {
        if (step == 0) throw new ArgumentException("range() step must not be 0.");
        if (step > 0) for (long i = from; i < to; i += step) yield return i;
        else for (long i = from; i > to; i += step) yield return i;
    }

    /// <summary>What <c>for x in v</c> walks over when v's type is not known: an Array's elements, a Dictionary's keys, a String's characters, an int's 0..n-1.</summary>
    public static IEnumerable<Variant> Iterate(Variant v)
    {
        switch (v.VariantType)
        {
            case Variant.Type.Int: for (long i = 0; i < v.AsInt64(); i++) yield return i; break;
            case Variant.Type.Float: for (double d = 0; d < v.AsDouble(); d += 1.0) yield return d; break;
            case Variant.Type.String: foreach (char ch in v.AsString()) yield return ch.ToString(); break;
            case Variant.Type.Array: foreach (Variant e in v.AsGodotArray().ToList()) yield return e; break;
            case Variant.Type.Dictionary: foreach (Variant k in v.AsGodotDictionary().Keys) yield return k; break;
            case Variant.Type.Vector2I: for (int i = 0; i < v.AsVector2I().X; i++) yield return i; break;
            case Variant.Type.PackedStringArray: foreach (string s in v.AsStringArray()) yield return s; break;
            case Variant.Type.PackedInt32Array: foreach (int s in v.AsInt32Array()) yield return s; break;
            case Variant.Type.PackedInt64Array: foreach (long s in v.AsInt64Array()) yield return s; break;
            case Variant.Type.PackedFloat32Array: foreach (float s in v.AsFloat32Array()) yield return s; break;
            case Variant.Type.PackedFloat64Array: foreach (double s in v.AsFloat64Array()) yield return s; break;
            case Variant.Type.PackedVector2Array: foreach (Vector2 s in v.AsVector2Array()) yield return s; break;
            case Variant.Type.PackedVector3Array: foreach (Vector3 s in v.AsVector3Array()) yield return s; break;
            case Variant.Type.PackedColorArray: foreach (Color s in v.AsColorArray()) yield return s; break;
            default: throw new InvalidOperationException($"Unable to iterate on object of type '{v.VariantType}'.");
        }
    }

    public static IEnumerable<string> Chars(string s) { foreach (char c in s) yield return c.ToString(); }

    // ---- len, typeof, char, ord ----

    public static long Len(Variant v) => v.VariantType switch
    {
        Variant.Type.String or Variant.Type.StringName or Variant.Type.NodePath => v.AsString().Length,
        Variant.Type.Array => v.AsGodotArray().Count,
        Variant.Type.Dictionary => v.AsGodotDictionary().Count,
        Variant.Type.PackedByteArray => v.AsByteArray().Length,
        Variant.Type.PackedInt32Array => v.AsInt32Array().Length,
        Variant.Type.PackedInt64Array => v.AsInt64Array().Length,
        Variant.Type.PackedFloat32Array => v.AsFloat32Array().Length,
        Variant.Type.PackedFloat64Array => v.AsFloat64Array().Length,
        Variant.Type.PackedStringArray => v.AsStringArray().Length,
        Variant.Type.PackedVector2Array => v.AsVector2Array().Length,
        Variant.Type.PackedVector3Array => v.AsVector3Array().Length,
        Variant.Type.PackedColorArray => v.AsColorArray().Length,
        _ => throw new InvalidOperationException($"Value of type '{v.VariantType}' can't provide a length."),
    };

    public static long TypeOf(Variant v) => (long)v.VariantType;

    public static string TypeString(long type) =>
        type >= 0 && type < (long)Variant.Type.Max ? ((Variant.Type)type).ToString() : "Unknown";

    public static string Char(long code) => char.ConvertFromUtf32((int)code);
    public static long Ord(string s) => s.Length == 0 ? throw new ArgumentException("Expected a string of length 1, received a string of length 0.") : char.ConvertToUtf32(s, 0);

    public static void Assert(bool condition, string message = "")
    {
        if (!condition) GD.PushError(message.Length > 0 ? "Assertion failed: " + message : "Assertion failed.");
    }

    // ---- await ----

    /// <summary>What <c>await some_signal</c> waits on when the signal is a Variant value.</summary>
    public static SignalAwaiter ToSignal(Variant signal)
    {
        Signal s = signal.AsSignal();
        if (s.IsNull) throw new InvalidOperationException("await needs a signal.");
        return new SignalAwaiter(s.Owner!, s.Name!);
    }

    /// <summary>A named lambda: the Callable can call itself through the function it is handed (func fact(n): ... fact(n - 1)).</summary>
    public static Callable Named(Func<Func<Callable>, Callable> make)
    {
        Callable self = default;
        self = make(() => self);
        return self;
    }

    /// <summary>The value <c>await signal</c> produces: nothing is null, one argument is that argument, several are an Array.</summary>
    public static Variant AwaitResult(Variant[] results) =>
        results.Length == 0 ? default : results.Length == 1 ? results[0] : (Variant)new Collections.Array(results);

    // ---- dynamic access: the Variant fallbacks ----

    public static Variant Index(Variant target, Variant key)
    {
        switch (target.VariantType)
        {
            case Variant.Type.Array:
            {
                var a = target.AsGodotArray();
                long i = key.AsInt64();
                if (i < 0) i += a.Count;
                if (i < 0 || i >= a.Count) throw new IndexOutOfRangeException($"Out of bounds get index '{key.AsInt64()}' (on base: 'Array')");
                return a[(int)i];
            }
            case Variant.Type.Dictionary: return target.AsGodotDictionary()[key];
            case Variant.Type.String:
            {
                string s = target.AsString();
                long i = key.AsInt64();
                if (i < 0) i += s.Length;
                return s[(int)i].ToString();
            }
            case Variant.Type.Vector2: return target.AsVector2()[(int)key.AsInt64()];
            case Variant.Type.Vector3: return target.AsVector3()[(int)key.AsInt64()];
            case Variant.Type.Color: return target.AsColor()[(int)key.AsInt64()];
            case Variant.Type.PackedStringArray: return target.AsStringArray()[key.AsInt64()];
            case Variant.Type.PackedInt32Array: return target.AsInt32Array()[key.AsInt64()];
            case Variant.Type.PackedInt64Array: return target.AsInt64Array()[key.AsInt64()];
            case Variant.Type.PackedFloat32Array: return target.AsFloat32Array()[key.AsInt64()];
            case Variant.Type.PackedFloat64Array: return target.AsFloat64Array()[key.AsInt64()];
            case Variant.Type.PackedVector3Array: return target.AsVector3Array()[key.AsInt64()];
            case Variant.Type.PackedVector2Array: return target.AsVector2Array()[key.AsInt64()];
            case Variant.Type.Object: return target.AsGodotObject()?.Get(key.AsString()) ?? default;
        }
        throw new InvalidOperationException($"Invalid access to index on a base object of type '{target.VariantType}'.");
    }

    public static void SetIndex(Variant target, Variant key, Variant value)
    {
        switch (target.VariantType)
        {
            case Variant.Type.Array:
            {
                var a = target.AsGodotArray();
                long i = key.AsInt64();
                if (i < 0) i += a.Count;
                a[(int)i] = value;
                return;
            }
            case Variant.Type.Dictionary: target.AsGodotDictionary()[key] = value; return;
            case Variant.Type.Object: target.AsGodotObject()?.Set(key.AsString(), value); return;
        }
        throw new InvalidOperationException($"Invalid assignment of index on a base object of type '{target.VariantType}'.");
    }

    /// <summary>A member of a value whose type is not known: a property by name; x/y/z/w/r/g/b/a of vectors and colours; a key of a dictionary.</summary>
    public static Variant GetMember(Variant target, string name)
    {
        switch (target.VariantType)
        {
            case Variant.Type.Object: return target.AsGodotObject()?.Get(name) ?? default;
            case Variant.Type.Dictionary: return target.AsGodotDictionary().Get(name);
            case Variant.Type.Vector2: { var v = target.AsVector2(); return name switch { "x" => v.X, "y" => v.Y, _ => Bad(target, name) }; }
            case Variant.Type.Vector3: { var v = target.AsVector3(); return name switch { "x" => v.X, "y" => v.Y, "z" => v.Z, _ => Bad(target, name) }; }
            case Variant.Type.Vector4: { var v = target.AsVector4(); return name switch { "x" => v.X, "y" => v.Y, "z" => v.Z, "w" => v.W, _ => Bad(target, name) }; }
            case Variant.Type.Color: { var v = target.AsColor(); return name switch { "r" => v.R, "g" => v.G, "b" => v.B, "a" => v.A, _ => Bad(target, name) }; }
        }
        return Bad(target, name);
    }

    private static Variant Bad(Variant target, string name) =>
        throw new InvalidOperationException($"Invalid access to property or key '{name}' on a base object of type '{target.VariantType}'.");

    public static void SetMember(Variant target, string name, Variant value)
    {
        switch (target.VariantType)
        {
            case Variant.Type.Object: target.AsGodotObject()?.Set(name, value); return;
            case Variant.Type.Dictionary: target.AsGodotDictionary()[name] = value; return;
        }
        throw new InvalidOperationException($"Invalid assignment of property or key '{name}' with value of type '{value.VariantType}' on a base object of type '{target.VariantType}'.");
    }

    /// <summary>A method call on a value whose type is not known: objects by name (Godot's or C#'s), and the common methods of the built-in types.</summary>
    public static Variant CallMethod(Variant target, string method, params Variant[] args)
    {
        switch (target.VariantType)
        {
            case Variant.Type.Object:
                return target.AsGodotObject() is { } o ? o.Call(method, args) : throw new InvalidOperationException($"Invalid call. Nonexistent function '{method}' in base 'Nil'.");
            case Variant.Type.Array: return CallArray(target.AsGodotArray(), method, args);
            case Variant.Type.Dictionary: return CallDictionary(target.AsGodotDictionary(), method, args);
            case Variant.Type.String:
            case Variant.Type.StringName: return CallString(target.AsString(), method, args);
            case Variant.Type.Callable when method is "call": return target.AsCallable().Call(args);
            case Variant.Type.Callable when method is "callv": return target.AsCallable().Callv(args[0].AsGodotArray());
            case Variant.Type.Callable when method is "bind": return target.AsCallable().Bind(args);
            case Variant.Type.Signal:
            {
                Signal s = target.AsSignal();
                switch (method)
                {
                    case "emit": return (long)s.Emit(args);
                    case "connect": return (long)s.Connect(args[0].AsCallable(), args.Length > 1 ? args[1].AsUInt32() : 0);
                    case "disconnect": s.Disconnect(args[0].AsCallable()); return default;
                    case "is_connected": return s.IsConnected(args[0].AsCallable());
                }
                break;
            }
            case Variant.Type.Vector2:
            case Variant.Type.Vector3:
            case Variant.Type.Color:
            case Variant.Type.Quaternion:
            case Variant.Type.Basis:
            case Variant.Type.Transform3D:
                return CallByReflection(target.ToType(target.VariantType switch
                {
                    Variant.Type.Vector2 => typeof(Vector2), Variant.Type.Vector3 => typeof(Vector3), Variant.Type.Color => typeof(Color),
                    Variant.Type.Quaternion => typeof(Quaternion), Variant.Type.Basis => typeof(Basis), _ => typeof(Transform3D),
                })!, method, args);
        }
        throw new InvalidOperationException($"Invalid call. Nonexistent function '{method}' in base '{target.VariantType}'.");
    }

    // The math types' methods by their Godot name, through reflection: this is the untyped fallback, not a hot path.
    private static Variant CallByReflection(object value, string method, Variant[] args)
    {
        string pascal = string.Concat(method.Split('_', StringSplitOptions.RemoveEmptyEntries).Select(p => char.ToUpperInvariant(p[0]) + p[1..]));
        foreach (System.Reflection.MethodInfo m in value.GetType().GetMethods().Where(m => m.Name == pascal && m.GetParameters().Length == args.Length))
        {
            try
            {
                object?[] converted = m.GetParameters().Select((p, i) => args[i].ToType(p.ParameterType)).ToArray();
                return Variant.FromObject(m.Invoke(value, converted));
            }
            catch (Exception ex) when (ex is ArgumentException or InvalidCastException) { }
        }
        throw new InvalidOperationException($"Invalid call. Nonexistent function '{method}' in base '{value.GetType().Name}'.");
    }

    private static Variant CallArray(Collections.Array a, string method, Variant[] args)
    {
        switch (method)
        {
            case "size": return a.Count;
            case "is_empty": return a.Count == 0;
            case "append": case "push_back": a.Add(args[0]); return default;
            case "push_front": a.PushFront(args[0]); return default;
            case "pop_back": return a.Pop();
            case "pop_front": return a.PopFront();
            case "has": return a.Contains(args[0]);
            case "find": return a.IndexOf(args[0]);
            case "erase": a.Remove(args[0]); return default;
            case "remove_at": a.RemoveAt((int)args[0].AsInt64()); return default;
            case "insert": a.Insert((int)args[0].AsInt64(), args[1]); return default;
            case "clear": a.Clear(); return default;
            case "front": return a.Front();
            case "back": return a.Back();
            case "reverse": a.Reverse(); return default;
            case "shuffle": a.Shuffle(); return default;
            case "sort": a.Sort(); return default;
            case "pick_random": return a.PickRandom();
            case "duplicate": return a.Duplicate(args.Length > 0 && args[0].AsBool());
            case "resize": return (long)a.Resize((int)args[0].AsInt64());
            case "append_array": foreach (Variant v in args[0].AsGodotArray().ToList()) a.Add(v); return default;
            case "slice": return a.Slice((int)args[0].AsInt64(), args.Length > 1 ? (int)args[1].AsInt64() : int.MaxValue);
            case "map": return a.Map(v => args[0].AsCallable().Call(v));
            case "filter": return a.Filter(v => args[0].AsCallable().Call(v).AsBool());
            case "reduce":
            {
                Variant acc = args.Length > 1 ? args[1] : (a.Count > 0 ? a[0] : default);
                for (int i = args.Length > 1 ? 0 : 1; i < a.Count; i++) acc = args[0].AsCallable().Call(acc, a[i]);
                return acc;
            }
            case "any": return a.Any(v => args[0].AsCallable().Call(v).AsBool());
            case "all": return a.All(v => args[0].AsCallable().Call(v).AsBool());
            case "min": return a.Count == 0 ? default : a.Aggregate((x, y) => Lt(y, x) ? y : x);
            case "max": return a.Count == 0 ? default : a.Aggregate((x, y) => Gt(y, x) ? y : x);
            case "count": return a.Count(v => v == args[0]);
            case "fill": for (int i = 0; i < a.Count; i++) a[i] = args[0]; return default;
            case "sort_custom":
            {
                Callable cmp = args[0].AsCallable();
                var sorted = a.ToList();
                MergeSort(sorted, (x, y) => cmp.Call(x, y).AsBool());
                a.Clear();
                foreach (Variant v in sorted) a.Add(v);
                return default;
            }
        }
        throw new InvalidOperationException($"Invalid call. Nonexistent function '{method}' in base 'Array'.");
    }

    // GDScript's sort_custom calls less(a, b); it is not guaranteed stable, a stable merge sort is the safe choice.
    internal static void MergeSort(List<Variant> list, Func<Variant, Variant, bool> less)
    {
        if (list.Count < 2) return;
        var left = list.GetRange(0, list.Count / 2);
        var right = list.GetRange(list.Count / 2, list.Count - list.Count / 2);
        MergeSort(left, less);
        MergeSort(right, less);
        int l = 0, r = 0, o = 0;
        while (l < left.Count && r < right.Count) list[o++] = less(right[r], left[l]) ? right[r++] : left[l++];
        while (l < left.Count) list[o++] = left[l++];
        while (r < right.Count) list[o++] = right[r++];
    }

    private static Variant CallDictionary(Collections.Dictionary d, string method, Variant[] args)
    {
        switch (method)
        {
            case "size": return d.Count;
            case "is_empty": return d.Count == 0;
            case "has": return d.ContainsKey(args[0]);
            case "has_all": return args[0].AsGodotArray().All(k => d.ContainsKey(k));
            case "get": return d.Get(args[0], args.Length > 1 ? args[1] : default);
            case "get_or_add":
                if (!d.ContainsKey(args[0])) d[args[0]] = args.Length > 1 ? args[1] : default;
                return d[args[0]];
            case "erase": return d.Remove(args[0]);
            case "clear": d.Clear(); return default;
            case "keys": return d.KeysArray();
            case "values": return d.ValuesArray();
            case "merge": d.Merge(args[0].AsGodotDictionary(), args.Length > 1 && args[1].AsBool()); return default;
            case "duplicate": return d.Duplicate(args.Length > 0 && args[0].AsBool());
            case "find_key":
                foreach (var kv in d) if (kv.Value == args[0]) return kv.Key;
                return default;
        }
        throw new InvalidOperationException($"Invalid call. Nonexistent function '{method}' in base 'Dictionary'.");
    }

    private static Variant CallString(string s, string method, Variant[] args)
    {
        switch (method)
        {
            case "length": return s.Length;
            case "is_empty": return s.Length == 0;
            case "to_upper": return s.ToUpperInvariant();
            case "to_lower": return s.ToLowerInvariant();
            case "begins_with": return s.StartsWith(args[0].AsString(), StringComparison.Ordinal);
            case "ends_with": return s.EndsWith(args[0].AsString(), StringComparison.Ordinal);
            case "contains": return s.Contains(args[0].AsString(), StringComparison.Ordinal);
            case "find": return s.Find(args[0].AsString(), args.Length > 1 ? args[1].AsInt64() : 0);
            case "replace": return s.Replace(args[0].AsString(), args[1].AsString(), StringComparison.Ordinal);
            case "strip_edges": return s.StripEdges();
            case "substr": return s.Substr(args[0].AsInt64(), args.Length > 1 ? args[1].AsInt64() : -1);
            case "to_int": return s.ToInt();
            case "to_float": return s.ToFloat();
            case "is_valid_int": return s.IsValidInt();
            case "is_valid_float": return s.IsValidFloat();
            case "split": return new Collections.Array(s.Split(args[0].AsString(), args.Length > 1 ? args[1].AsBool() : true).Select(x => (Variant)x));
            case "capitalize": return s.Capitalize();
            case "format": return s.Format(args[0]);
            case "left": return s.Left(args[0].AsInt64());
            case "right": return s.Right(args[0].AsInt64());
            case "pad_zeros": return s.PadZeros(args[0].AsInt64());
        }
        throw new InvalidOperationException($"Invalid call. Nonexistent function '{method}' in base 'String'.");
    }
}
