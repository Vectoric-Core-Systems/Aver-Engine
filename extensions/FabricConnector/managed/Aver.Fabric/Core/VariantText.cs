// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// VariantText: reads Godot's text form of a value ("Vector3(1, 2, 3)", "[1, 2]", "{ \"a\": 1 }") into a Variant. Exported
// property values are stored this way in a converted level (COMPONENT Props), written by the converter.

using System.Globalization;
using System.Text;
using Aver.Fabric.Collections;

namespace Aver.Fabric;

/// <summary>Parses Godot's Variant text format. The converter writes it; <c>docs/plans/fabric/G3_FORMATS.md</c> lists what the C++ reader accepts.</summary>
public static class VariantText
{
    /// <summary>Parses one value. Throws FormatException on bad text.</summary>
    public static Variant Parse(string text)
    {
        var p = new Parser(text);
        Variant v = p.Value();
        p.SkipSpace();
        if (!p.AtEnd) throw p.Fail("unexpected text after the value");
        return v;
    }

    public static bool TryParse(string text, out Variant value)
    {
        try { value = Parse(text); return true; }
        catch (FormatException) { value = default; return false; }
    }

    private sealed class Parser
    {
        private readonly string _s;
        private int _i;

        public Parser(string s) => _s = s ?? "";
        public bool AtEnd => _i >= _s.Length;

        public FormatException Fail(string why) => new($"{why} at column {_i + 1} of '{_s}'");

        public void SkipSpace() { while (_i < _s.Length && char.IsWhiteSpace(_s[_i])) _i++; }

        private bool Eat(char c)
        {
            SkipSpace();
            if (_i < _s.Length && _s[_i] == c) { _i++; return true; }
            return false;
        }

        private void Expect(char c) { if (!Eat(c)) throw Fail($"expected '{c}'"); }

        public Variant Value()
        {
            SkipSpace();
            if (AtEnd) throw Fail("a value is missing");
            char c = _s[_i];
            if (c == '"') return ReadString();
            if (c == '&' && _i + 1 < _s.Length && _s[_i + 1] == '"') { _i++; return new StringName(ReadString().AsString()); }
            if (c == '^' && _i + 1 < _s.Length && _s[_i + 1] == '"') { _i++; return new NodePath(ReadString().AsString()); }
            if (c == '[') return ReadArray();
            if (c == '{') return ReadDictionary();
            if (c == '-' || c == '+' || char.IsAsciiDigit(c) || c == '.') return ReadNumber();
            string word = ReadWord();
            switch (word)
            {
                case "null": return default;
                case "true": return true;
                case "false": return false;
                case "inf": return double.PositiveInfinity;
                case "inf_neg": return double.NegativeInfinity;
                case "nan": return double.NaN;
            }
            SkipSpace();
            if (_i < _s.Length && _s[_i] == '[' && word is "Array" or "Dictionary")
            {
                // A typed collection, Array[int]( ... ): the element type is not kept.
                int close = _s.IndexOf(']', _i);
                if (close < 0) throw Fail("unterminated type");
                _i = close + 1;
                SkipSpace();
            }
            if (_i >= _s.Length || _s[_i] != '(') throw Fail($"unknown value '{word}'");
            _i++;
            if (word == "Object") return ReadObject();
            return Constructor(word);
        }

        // Object(Class, "property": value, ...): an inline resource, built by class name with its properties set.
        private Variant ReadObject()
        {
            SkipSpace();
            string cls = ReadWord();
            var props = new List<(string, Variant)>();
            while (Eat(','))
            {
                if (Eat(')')) return ClassDB.Build(cls, props);
                string key = Value().AsString();
                Expect(':');
                props.Add((key, Value()));
            }
            Expect(')');
            return ClassDB.Build(cls, props);
        }

        private string ReadWord()
        {
            int start = _i;
            while (_i < _s.Length && (char.IsAsciiLetterOrDigit(_s[_i]) || _s[_i] == '_')) _i++;
            if (_i == start) throw Fail("unexpected character");
            return _s[start.._i];
        }

        private Variant ReadNumber()
        {
            int start = _i;
            if (_s[_i] is '-' or '+') _i++;
            if (_i + 1 < _s.Length && _s[_i] == '0' && _s[_i + 1] is 'x' or 'X')
            {
                _i += 2;
                int hs = _i;
                while (_i < _s.Length && Uri.IsHexDigit(_s[_i])) _i++;
                long h = long.Parse(_s[hs.._i], NumberStyles.HexNumber, CultureInfo.InvariantCulture);
                return _s[start] == '-' ? -h : h;
            }
            bool isFloat = false;
            while (_i < _s.Length)
            {
                char c = _s[_i];
                if (char.IsAsciiDigit(c)) _i++;
                else if (c == '.') { isFloat = true; _i++; }
                else if (c is 'e' or 'E')
                {
                    isFloat = true; _i++;
                    if (_i < _s.Length && _s[_i] is '+' or '-') _i++;
                }
                else break;
            }
            string t = _s[start.._i];
            if (!isFloat && long.TryParse(t, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out long l)) return l;
            if (double.TryParse(t, NumberStyles.Float, CultureInfo.InvariantCulture, out double d)) return d;
            throw Fail($"bad number '{t}'");
        }

        private Variant ReadString()
        {
            _i++;   // the opening quote
            var sb = new StringBuilder();
            while (true)
            {
                if (_i >= _s.Length) throw Fail("unterminated string");
                char c = _s[_i++];
                if (c == '"') break;
                if (c != '\\') { sb.Append(c); continue; }
                if (_i >= _s.Length) throw Fail("unterminated escape");
                char e = _s[_i++];
                switch (e)
                {
                    case 'n': sb.Append('\n'); break;
                    case 't': sb.Append('\t'); break;
                    case 'r': sb.Append('\r'); break;
                    case 'b': sb.Append('\b'); break;
                    case 'f': sb.Append('\f'); break;
                    case 'a': sb.Append('\a'); break;
                    case 'v': sb.Append('\v'); break;
                    case 'u':
                    case 'U':
                    {
                        int n = e == 'u' ? 4 : 6;
                        if (_i + n > _s.Length) throw Fail("bad \\u escape");
                        int cp = int.Parse(_s.AsSpan(_i, n), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
                        _i += n;
                        sb.Append(char.ConvertFromUtf32(cp));
                        break;
                    }
                    default: sb.Append(e); break;
                }
            }
            return sb.ToString();
        }

        private Variant ReadArray()
        {
            _i++;   // [
            var a = new Collections.Array();
            if (Eat(']')) return a;
            do { a.Add(Value()); } while (Eat(','));
            Expect(']');
            return a;
        }

        private Variant ReadDictionary()
        {
            _i++;   // {
            var d = new Collections.Dictionary();
            if (Eat('}')) return d;
            do
            {
                Variant k = Value();
                Expect(':');
                d[k] = Value();
            } while (Eat(','));
            Expect('}');
            return d;
        }

        // The arguments of Name( ... ), after the '('; the closing ')' is consumed.
        private List<Variant> Args()
        {
            var list = new List<Variant>();
            if (Eat(')')) return list;
            do { list.Add(Value()); } while (Eat(','));
            Expect(')');
            return list;
        }

        private static float[] F(List<Variant> a, int n, string name)
        {
            if (a.Count != n) throw new FormatException($"{name} takes {n} numbers, not {a.Count}");
            return a.Select(v => (float)v.AsDouble()).ToArray();
        }

        private Variant Constructor(string name)
        {
            List<Variant> a = Args();
            float[] f;
            switch (name)
            {
                case "Vector2": f = F(a, 2, name); return new Vector2(f[0], f[1]);
                case "Vector2i": return new Vector2I((int)a[0].AsInt64(), (int)a[1].AsInt64());
                case "Vector3": f = F(a, 3, name); return new Vector3(f[0], f[1], f[2]);
                case "Vector3i": return new Vector3I((int)a[0].AsInt64(), (int)a[1].AsInt64(), (int)a[2].AsInt64());
                case "Vector4": f = F(a, 4, name); return new Vector4(f[0], f[1], f[2], f[3]);
                case "Vector4i": return new Vector4I((int)a[0].AsInt64(), (int)a[1].AsInt64(), (int)a[2].AsInt64(), (int)a[3].AsInt64());
                case "Color":
                    if (a.Count == 3) { f = F(a, 3, name); return new Color(f[0], f[1], f[2]); }
                    f = F(a, 4, name); return new Color(f[0], f[1], f[2], f[3]);
                case "Quaternion": f = F(a, 4, name); return new Quaternion(f[0], f[1], f[2], f[3]);
                case "Plane": f = F(a, 4, name); return new Plane(f[0], f[1], f[2], f[3]);
                case "Rect2": f = F(a, 4, name); return new Rect2(f[0], f[1], f[2], f[3]);
                case "Rect2i": return new Rect2I((int)a[0].AsInt64(), (int)a[1].AsInt64(), (int)a[2].AsInt64(), (int)a[3].AsInt64());
                case "AABB": f = F(a, 6, name); return new Aabb(new Vector3(f[0], f[1], f[2]), new Vector3(f[3], f[4], f[5]));
                case "Basis":
                    f = F(a, 9, name);
                    return new Basis(new Vector3(f[0], f[1], f[2]), new Vector3(f[3], f[4], f[5]), new Vector3(f[6], f[7], f[8]), true);
                case "Transform3D":
                    f = F(a, 12, name);
                    return new Transform3D(new Basis(new Vector3(f[0], f[1], f[2]), new Vector3(f[3], f[4], f[5]), new Vector3(f[6], f[7], f[8]), true),
                                           new Vector3(f[9], f[10], f[11]));
                case "Transform2D":
                    f = F(a, 6, name);
                    return new Transform2D(new Vector2(f[0], f[1]), new Vector2(f[2], f[3]), new Vector2(f[4], f[5]));
                case "Projection":
                    f = F(a, 16, name);
                    return new Projection(new Vector4(f[0], f[1], f[2], f[3]), new Vector4(f[4], f[5], f[6], f[7]),
                                          new Vector4(f[8], f[9], f[10], f[11]), new Vector4(f[12], f[13], f[14], f[15]));
                case "NodePath": return new NodePath(a.Count > 0 ? a[0].AsString() : "");
                case "StringName": return new StringName(a.Count > 0 ? a[0].AsString() : "");
                case "RID": return new Rid(a.Count > 0 ? (ulong)a[0].AsInt64() : 0);
                case "Array":
                    // Array([..]) and typed Array[T]([..]) both reach here as Array(...); the element list is the last argument.
                    return a.Count > 0 && a[^1].VariantType == Variant.Type.Array ? a[^1] : new Collections.Array(a);
                case "Dictionary": return a.Count > 0 ? a[^1] : new Collections.Dictionary();
                case "PackedByteArray":
                    if (a.Count == 1 && a[0].VariantType == Variant.Type.String) return Convert.FromBase64String(a[0].AsString());
                    return a.Select(v => (byte)v.AsInt64()).ToArray();
                case "PackedInt32Array": return a.Select(v => (int)v.AsInt64()).ToArray();
                case "PackedInt64Array": return a.Select(v => v.AsInt64()).ToArray();
                case "PackedFloat32Array": return a.Select(v => (float)v.AsDouble()).ToArray();
                case "PackedFloat64Array": return a.Select(v => v.AsDouble()).ToArray();
                case "PackedStringArray": return a.Select(v => v.AsString()).ToArray();
                case "PackedVector2Array":
                    f = a.Select(v => (float)v.AsDouble()).ToArray();
                    return Enumerable.Range(0, f.Length / 2).Select(i => new Vector2(f[i * 2], f[i * 2 + 1])).ToArray();
                case "PackedVector3Array":
                    f = a.Select(v => (float)v.AsDouble()).ToArray();
                    return Enumerable.Range(0, f.Length / 3).Select(i => new Vector3(f[i * 3], f[i * 3 + 1], f[i * 3 + 2])).ToArray();
                case "PackedColorArray":
                    f = a.Select(v => (float)v.AsDouble()).ToArray();
                    return Enumerable.Range(0, f.Length / 4).Select(i => new Color(f[i * 4], f[i * 4 + 1], f[i * 4 + 2], f[i * 4 + 3])).ToArray();
                case "Resource":
                    return a.Count > 0 ? ResourceLoader.Load(a[0].AsString()) : null;
                default:
                    throw Fail($"unknown constructor '{name}'");
            }
        }
    }
}
