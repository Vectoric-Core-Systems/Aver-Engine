// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The flat marshalled form of a Variant on the aver_gdext C ABI (fabric_gdext_abi.h): little endian, a type byte then the payload.
// Aver.Fabric.Variant and the host's Variants meet here. Callable and Signal travel as nil; an Object travels as the native
// instance id of a GDExtension peer (0 for a managed object, which the extension cannot see).

using System.Buffers.Binary;
using System.Text;

namespace Aver.Fabric;

/// <summary>Writes and reads <see cref="Variant"/> values in the wire form of the GDExtension C ABI.</summary>
public static class VariantWire
{
    // ---- writing ----

    /// <summary>The wire form of one Variant.</summary>
    public static byte[] Encode(Variant v)
    {
        var buf = new List<byte>(32);
        Write(buf, v);
        return buf.ToArray();
    }

    /// <summary>The wire form of an argument list: an Array of the values.</summary>
    public static byte[] EncodeArguments(ReadOnlySpan<Variant> args)
    {
        var buf = new List<byte>(32 + args.Length * 9);
        buf.Add((byte)Variant.Type.Array);
        PutInt(buf, args.Length);
        foreach (Variant a in args) Write(buf, a);
        return buf.ToArray();
    }

    private static void PutInt(List<byte> b, int v) { Span<byte> s = stackalloc byte[4]; BinaryPrimitives.WriteInt32LittleEndian(s, v); foreach (byte x in s) b.Add(x); }
    private static void PutLong(List<byte> b, long v) { Span<byte> s = stackalloc byte[8]; BinaryPrimitives.WriteInt64LittleEndian(s, v); foreach (byte x in s) b.Add(x); }
    private static void PutDouble(List<byte> b, double v) { PutLong(b, BitConverter.DoubleToInt64Bits(v)); }
    private static void PutFloat(List<byte> b, float v) { PutInt(b, BitConverter.SingleToInt32Bits(v)); }
    private static void PutText(List<byte> b, string s) { byte[] u = Encoding.UTF8.GetBytes(s); PutInt(b, u.Length); b.AddRange(u); }
    private static void PutV2(List<byte> b, Vector2 v) { PutFloat(b, v.X); PutFloat(b, v.Y); }
    private static void PutV2I(List<byte> b, Vector2I v) { PutInt(b, v.X); PutInt(b, v.Y); }
    private static void PutV3(List<byte> b, Vector3 v) { PutFloat(b, v.X); PutFloat(b, v.Y); PutFloat(b, v.Z); }
    private static void PutV4(List<byte> b, Vector4 v) { PutFloat(b, v.X); PutFloat(b, v.Y); PutFloat(b, v.Z); PutFloat(b, v.W); }
    private static void PutColor(List<byte> b, Color c) { PutFloat(b, c.R); PutFloat(b, c.G); PutFloat(b, c.B); PutFloat(b, c.A); }
    private static void PutBasis(List<byte> b, Basis m) { PutV3(b, m.Row0); PutV3(b, m.Row1); PutV3(b, m.Row2); }

    private static void Write(List<byte> b, Variant v)
    {
        Variant.Type t = v.VariantType;
        if (t is Variant.Type.Callable or Variant.Type.Signal) t = Variant.Type.Nil;
        b.Add((byte)t);
        switch (t)
        {
            case Variant.Type.Nil: return;
            case Variant.Type.Bool: b.Add(v.AsBool() ? (byte)1 : (byte)0); return;
            case Variant.Type.Int: PutLong(b, v.AsInt64()); return;
            case Variant.Type.Float: PutDouble(b, v.AsDouble()); return;
            case Variant.Type.String: PutText(b, v.AsString()); return;
            case Variant.Type.StringName: PutText(b, v.AsStringName().ToString()); return;
            case Variant.Type.NodePath: PutText(b, v.AsNodePath().ToString()); return;
            case Variant.Type.Vector2: PutV2(b, v.AsVector2()); return;
            case Variant.Type.Vector2I: PutV2I(b, v.AsVector2I()); return;
            case Variant.Type.Rect2: { Rect2 r = v.AsRect2(); PutV2(b, r.Position); PutV2(b, r.Size); return; }
            case Variant.Type.Rect2I: { Rect2I r = v.AsRect2I(); PutV2I(b, r.Position); PutV2I(b, r.Size); return; }
            case Variant.Type.Vector3: PutV3(b, v.AsVector3()); return;
            case Variant.Type.Vector3I: { Vector3I i = v.AsVector3I(); PutInt(b, i.X); PutInt(b, i.Y); PutInt(b, i.Z); return; }
            case Variant.Type.Transform2D: { Transform2D x = v.AsTransform2D(); PutV2(b, x.X); PutV2(b, x.Y); PutV2(b, x.Origin); return; }
            case Variant.Type.Vector4: PutV4(b, v.AsVector4()); return;
            case Variant.Type.Vector4I: { Vector4I i = v.AsVector4I(); PutInt(b, i.X); PutInt(b, i.Y); PutInt(b, i.Z); PutInt(b, i.W); return; }
            case Variant.Type.Plane: { Plane p = v.AsPlane(); PutV3(b, p.Normal); PutFloat(b, p.D); return; }
            case Variant.Type.Quaternion: { Quaternion q = v.AsQuaternion(); PutFloat(b, q.X); PutFloat(b, q.Y); PutFloat(b, q.Z); PutFloat(b, q.W); return; }
            case Variant.Type.Aabb: { Aabb a = v.AsAabb(); PutV3(b, a.Position); PutV3(b, a.Size); return; }
            case Variant.Type.Basis: PutBasis(b, v.AsBasis()); return;
            case Variant.Type.Transform3D: { Transform3D x = v.AsTransform3D(); PutBasis(b, x.Basis); PutV3(b, x.Origin); return; }
            case Variant.Type.Projection: { Projection p = v.AsProjection(); PutV4(b, p.X); PutV4(b, p.Y); PutV4(b, p.Z); PutV4(b, p.W); return; }
            case Variant.Type.Color: PutColor(b, v.AsColor()); return;
            case Variant.Type.Rid: PutLong(b, unchecked((long)v.AsRid().Id)); return;
            case Variant.Type.Object:
                PutLong(b, v.AsGodotObject() is { } o ? GDExtensionManager.NativeInstanceId(o) : 0);
                return;
            case Variant.Type.Array:
            {
                Collections.Array a = v.AsGodotArray();
                PutInt(b, a.Count);
                foreach (Variant e in a) Write(b, e);
                return;
            }
            case Variant.Type.Dictionary:
            {
                Collections.Dictionary d = v.AsGodotDictionary();
                PutInt(b, d.Count);
                foreach (KeyValuePair<Variant, Variant> kv in d) { Write(b, kv.Key); Write(b, kv.Value); }
                return;
            }
            case Variant.Type.PackedByteArray: { byte[] a = v.AsByteArray(); PutInt(b, a.Length); b.AddRange(a); return; }
            case Variant.Type.PackedInt32Array: { int[] a = v.AsInt32Array(); PutInt(b, a.Length); foreach (int x in a) PutInt(b, x); return; }
            case Variant.Type.PackedInt64Array: { long[] a = v.AsInt64Array(); PutInt(b, a.Length); foreach (long x in a) PutLong(b, x); return; }
            case Variant.Type.PackedFloat32Array: { float[] a = v.AsFloat32Array(); PutInt(b, a.Length); foreach (float x in a) PutFloat(b, x); return; }
            case Variant.Type.PackedFloat64Array: { double[] a = v.AsFloat64Array(); PutInt(b, a.Length); foreach (double x in a) PutDouble(b, x); return; }
            case Variant.Type.PackedStringArray: { string[] a = v.AsStringArray(); PutInt(b, a.Length); foreach (string x in a) PutText(b, x); return; }
            case Variant.Type.PackedVector2Array: { Vector2[] a = v.AsVector2Array(); PutInt(b, a.Length); foreach (Vector2 x in a) PutV2(b, x); return; }
            case Variant.Type.PackedVector3Array: { Vector3[] a = v.AsVector3Array(); PutInt(b, a.Length); foreach (Vector3 x in a) PutV3(b, x); return; }
            case Variant.Type.PackedColorArray: { Color[] a = v.AsColorArray(); PutInt(b, a.Length); foreach (Color x in a) PutColor(b, x); return; }
            case Variant.Type.PackedVector4Array: { Vector4[] a = v.AsVector4Array(); PutInt(b, a.Length); foreach (Vector4 x in a) PutV4(b, x); return; }
            default: b[^1] = 0; return;
        }
    }

    // ---- reading ----

    /// <summary>Reads one Variant. Throws <see cref="FormatException"/> on malformed bytes.</summary>
    public static Variant Decode(ReadOnlySpan<byte> bytes)
    {
        int pos = 0;
        Variant v = Read(bytes, ref pos, 0);
        return v;
    }

    /// <summary>Reads an argument list (an Array in wire form).</summary>
    public static Variant[] DecodeArguments(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length == 0) return System.Array.Empty<Variant>();
        Collections.Array a = Decode(bytes).AsGodotArray();
        return a.ToArray();
    }

    private static int GetInt(ReadOnlySpan<byte> s, ref int p) { Need(s, p, 4); int v = BinaryPrimitives.ReadInt32LittleEndian(s[p..]); p += 4; return v; }
    private static long GetLong(ReadOnlySpan<byte> s, ref int p) { Need(s, p, 8); long v = BinaryPrimitives.ReadInt64LittleEndian(s[p..]); p += 8; return v; }
    private static float GetFloat(ReadOnlySpan<byte> s, ref int p) => BitConverter.Int32BitsToSingle(GetInt(s, ref p));
    private static double GetDouble(ReadOnlySpan<byte> s, ref int p) => BitConverter.Int64BitsToDouble(GetLong(s, ref p));
    private static void Need(ReadOnlySpan<byte> s, int p, int n) { if (p < 0 || n < 0 || p + n > s.Length) throw new FormatException("the wire form ends early"); }
    private static string GetText(ReadOnlySpan<byte> s, ref int p)
    {
        int n = GetInt(s, ref p);
        Need(s, p, n);
        string text = Encoding.UTF8.GetString(s.Slice(p, n));
        p += n;
        return text;
    }
    private static Vector2 GetV2(ReadOnlySpan<byte> s, ref int p) => new(GetFloat(s, ref p), GetFloat(s, ref p));
    private static Vector2I GetV2I(ReadOnlySpan<byte> s, ref int p) => new(GetInt(s, ref p), GetInt(s, ref p));
    private static Vector3 GetV3(ReadOnlySpan<byte> s, ref int p) => new(GetFloat(s, ref p), GetFloat(s, ref p), GetFloat(s, ref p));
    private static Vector4 GetV4(ReadOnlySpan<byte> s, ref int p) => new(GetFloat(s, ref p), GetFloat(s, ref p), GetFloat(s, ref p), GetFloat(s, ref p));
    private static Color GetColor(ReadOnlySpan<byte> s, ref int p) => new(GetFloat(s, ref p), GetFloat(s, ref p), GetFloat(s, ref p), GetFloat(s, ref p));
    private static Basis GetBasis(ReadOnlySpan<byte> s, ref int p)
    {
        Vector3 r0 = GetV3(s, ref p), r1 = GetV3(s, ref p), r2 = GetV3(s, ref p);
        return new Basis { Row0 = r0, Row1 = r1, Row2 = r2 };
    }
    private static int GetCount(ReadOnlySpan<byte> s, ref int p)
    {
        int n = GetInt(s, ref p);
        if (n < 0 || n > s.Length) throw new FormatException("bad element count");
        return n;
    }

    private static Variant Read(ReadOnlySpan<byte> s, ref int p, int depth)
    {
        if (depth > 64) throw new FormatException("the wire form nests too deeply");
        Need(s, p, 1);
        var t = (Variant.Type)s[p++];
        switch (t)
        {
            case Variant.Type.Nil: return default;
            case Variant.Type.Bool: Need(s, p, 1); return s[p++] != 0;
            case Variant.Type.Int: return GetLong(s, ref p);
            case Variant.Type.Float: return GetDouble(s, ref p);
            case Variant.Type.String: return GetText(s, ref p);
            case Variant.Type.StringName: return new StringName(GetText(s, ref p));
            case Variant.Type.NodePath: return new NodePath(GetText(s, ref p));
            case Variant.Type.Vector2: return GetV2(s, ref p);
            case Variant.Type.Vector2I: return GetV2I(s, ref p);
            case Variant.Type.Rect2: { Vector2 a = GetV2(s, ref p), z = GetV2(s, ref p); return new Rect2(a, z); }
            case Variant.Type.Rect2I: { Vector2I a = GetV2I(s, ref p), z = GetV2I(s, ref p); return new Rect2I(a, z); }
            case Variant.Type.Vector3: return GetV3(s, ref p);
            case Variant.Type.Vector3I: return new Vector3I(GetInt(s, ref p), GetInt(s, ref p), GetInt(s, ref p));
            case Variant.Type.Transform2D: { Vector2 x = GetV2(s, ref p), y = GetV2(s, ref p), o = GetV2(s, ref p); return new Transform2D(x, y, o); }
            case Variant.Type.Vector4: return GetV4(s, ref p);
            case Variant.Type.Vector4I: return new Vector4I(GetInt(s, ref p), GetInt(s, ref p), GetInt(s, ref p), GetInt(s, ref p));
            case Variant.Type.Plane: { Vector3 n = GetV3(s, ref p); float d = GetFloat(s, ref p); return new Plane(n, d); }
            case Variant.Type.Quaternion: return new Quaternion(GetFloat(s, ref p), GetFloat(s, ref p), GetFloat(s, ref p), GetFloat(s, ref p));
            case Variant.Type.Aabb: { Vector3 a = GetV3(s, ref p), z = GetV3(s, ref p); return new Aabb(a, z); }
            case Variant.Type.Basis: return GetBasis(s, ref p);
            case Variant.Type.Transform3D: { Basis m = GetBasis(s, ref p); Vector3 o = GetV3(s, ref p); return new Transform3D(m, o); }
            case Variant.Type.Projection: { Vector4 x = GetV4(s, ref p), y = GetV4(s, ref p), z = GetV4(s, ref p), w = GetV4(s, ref p); return new Projection(x, y, z, w); }
            case Variant.Type.Color: return GetColor(s, ref p);
            case Variant.Type.Rid: return new Rid(unchecked((ulong)GetLong(s, ref p)));
            case Variant.Type.Object: return GDExtensionManager.ObjectFromNativeId(GetLong(s, ref p));
            case Variant.Type.Array:
            {
                int n = GetCount(s, ref p);
                var a = new Collections.Array();
                for (int i = 0; i < n; i++) a.Add(Read(s, ref p, depth + 1));
                return a;
            }
            case Variant.Type.Dictionary:
            {
                int n = GetCount(s, ref p);
                var d = new Collections.Dictionary();
                for (int i = 0; i < n; i++)
                {
                    Variant k = Read(s, ref p, depth + 1);
                    d[k] = Read(s, ref p, depth + 1);
                }
                return d;
            }
            case Variant.Type.PackedByteArray: { int n = GetCount(s, ref p); Need(s, p, n); byte[] a = s.Slice(p, n).ToArray(); p += n; return a; }
            case Variant.Type.PackedInt32Array: { int n = GetCount(s, ref p); var a = new int[n]; for (int i = 0; i < n; i++) a[i] = GetInt(s, ref p); return a; }
            case Variant.Type.PackedInt64Array: { int n = GetCount(s, ref p); var a = new long[n]; for (int i = 0; i < n; i++) a[i] = GetLong(s, ref p); return a; }
            case Variant.Type.PackedFloat32Array: { int n = GetCount(s, ref p); var a = new float[n]; for (int i = 0; i < n; i++) a[i] = GetFloat(s, ref p); return a; }
            case Variant.Type.PackedFloat64Array: { int n = GetCount(s, ref p); var a = new double[n]; for (int i = 0; i < n; i++) a[i] = GetDouble(s, ref p); return a; }
            case Variant.Type.PackedStringArray: { int n = GetCount(s, ref p); var a = new string[n]; for (int i = 0; i < n; i++) a[i] = GetText(s, ref p); return a; }
            case Variant.Type.PackedVector2Array: { int n = GetCount(s, ref p); var a = new Vector2[n]; for (int i = 0; i < n; i++) a[i] = GetV2(s, ref p); return a; }
            case Variant.Type.PackedVector3Array: { int n = GetCount(s, ref p); var a = new Vector3[n]; for (int i = 0; i < n; i++) a[i] = GetV3(s, ref p); return a; }
            case Variant.Type.PackedColorArray: { int n = GetCount(s, ref p); var a = new Color[n]; for (int i = 0; i < n; i++) a[i] = GetColor(s, ref p); return a; }
            case Variant.Type.PackedVector4Array: { int n = GetCount(s, ref p); var a = new Vector4[n]; for (int i = 0; i < n; i++) a[i] = GetV4(s, ref p); return Variant.CreateFrom(a); }
            default: throw new FormatException($"unknown variant type {(int)t} in the wire form");
        }
    }
}
