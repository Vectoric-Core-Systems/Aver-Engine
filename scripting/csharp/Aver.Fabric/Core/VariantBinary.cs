// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Godot's Variant binary encoding (what PacketPeer.put_var / StreamPeer.put_var / var_to_bytes write): a little-endian u32 header (type in the low 16 bits,
// flags above), then the value; strings and packed byte arrays are padded to 4 bytes. Reals are 32-bit, as in a default Godot build; an int outside 32 bits and
// a float that does not survive being a 32-bit float carry the 64-bit flag. Not carried: full objects (a class name and its properties, which a running
// Godot would instantiate) -- encoding one answers Error.Unavailable and decoding one Error.Unauthorized or Error.Unavailable; an object travels as its
// instance id (EncodedObjectAsId) when the caller did not ask for full objects. Callables and signals encode as their header alone and decode as empty.

using System.Text;
using GArray = Aver.Fabric.Collections.Array;
using GDict = Aver.Fabric.Collections.Dictionary;

namespace Aver.Fabric;

/// <summary>What a Variant holding an object becomes after a round trip without full objects: only the instance id survives (Godot's EncodedObjectAsID).</summary>
public class EncodedObjectAsId : RefCounted
{
    public ulong ObjectId { get; set; }
}

internal static class VariantBinary
{
    private const uint Flag64 = 1u << 16;
    private const uint FlagObjectAsId = 1u << 16;
    private const int MaxDepth = 256;
    private const int MaxCount = 1 << 26;   // elements of one container; a hostile count must not allocate a gigabyte

    // ---- encode ----

    internal static Error Encode(Variant v, List<byte> o, bool fullObjects = false) => EncodeInner(v, o, fullObjects, 0);

    private static void U32(List<byte> o, uint v) { o.Add((byte)v); o.Add((byte)(v >> 8)); o.Add((byte)(v >> 16)); o.Add((byte)(v >> 24)); }
    private static void U64(List<byte> o, ulong v) { U32(o, (uint)v); U32(o, (uint)(v >> 32)); }
    private static void F32(List<byte> o, float v) => U32(o, BitConverter.SingleToUInt32Bits(v));
    private static void F64(List<byte> o, double v) => U64(o, BitConverter.DoubleToUInt64Bits(v));

    private static void Str(List<byte> o, string s)
    {
        byte[] b = Encoding.UTF8.GetBytes(s);
        U32(o, (uint)b.Length);
        o.AddRange(b);
        Pad(o, b.Length);
    }

    private static void Pad(List<byte> o, int len) { for (int i = len % 4; i != 0 && i < 4; i++) o.Add(0); }

    private static Error EncodeInner(Variant v, List<byte> o, bool full, int depth)
    {
        if (depth > MaxDepth) return Error.Failed;
        uint type = (uint)v.VariantType;
        switch (v.VariantType)
        {
            case Variant.Type.Nil: U32(o, type); break;
            case Variant.Type.Bool: U32(o, type); U32(o, v.AsBool() ? 1u : 0u); break;
            case Variant.Type.Int:
            {
                long n = v.AsInt64();
                if (n > int.MaxValue || n < int.MinValue) { U32(o, type | Flag64); U64(o, (ulong)n); }
                else { U32(o, type); U32(o, (uint)(int)n); }
                break;
            }
            case Variant.Type.Float:
            {
                double d = v.AsDouble();
                if ((double)(float)d != d) { U32(o, type | Flag64); F64(o, d); }
                else { U32(o, type); F32(o, (float)d); }
                break;
            }
            case Variant.Type.String: U32(o, type); Str(o, v.AsString()); break;
            case Variant.Type.StringName: U32(o, type); Str(o, v.AsString()); break;
            case Variant.Type.Vector2: { U32(o, type); Vector2 a = v.AsVector2(); F32(o, a.X); F32(o, a.Y); break; }
            case Variant.Type.Vector2I: { U32(o, type); Vector2I a = v.AsVector2I(); U32(o, (uint)a.X); U32(o, (uint)a.Y); break; }
            case Variant.Type.Rect2: { U32(o, type); Rect2 a = v.AsRect2(); F32(o, a.Position.X); F32(o, a.Position.Y); F32(o, a.Size.X); F32(o, a.Size.Y); break; }
            case Variant.Type.Rect2I: { U32(o, type); Rect2I a = v.AsRect2I(); U32(o, (uint)a.Position.X); U32(o, (uint)a.Position.Y); U32(o, (uint)a.Size.X); U32(o, (uint)a.Size.Y); break; }
            case Variant.Type.Vector3: { U32(o, type); Vector3 a = v.AsVector3(); F32(o, a.X); F32(o, a.Y); F32(o, a.Z); break; }
            case Variant.Type.Vector3I: { U32(o, type); Vector3I a = v.AsVector3I(); U32(o, (uint)a.X); U32(o, (uint)a.Y); U32(o, (uint)a.Z); break; }
            case Variant.Type.Transform2D:
            {
                U32(o, type);
                Transform2D a = v.AsTransform2D();
                F32(o, a.X.X); F32(o, a.X.Y); F32(o, a.Y.X); F32(o, a.Y.Y); F32(o, a.Origin.X); F32(o, a.Origin.Y);
                break;
            }
            case Variant.Type.Vector4: { U32(o, type); Vector4 a = v.AsVector4(); F32(o, a.X); F32(o, a.Y); F32(o, a.Z); F32(o, a.W); break; }
            case Variant.Type.Vector4I: { U32(o, type); Vector4I a = v.AsVector4I(); U32(o, (uint)a.X); U32(o, (uint)a.Y); U32(o, (uint)a.Z); U32(o, (uint)a.W); break; }
            case Variant.Type.Plane: { U32(o, type); Plane a = v.AsPlane(); F32(o, a.Normal.X); F32(o, a.Normal.Y); F32(o, a.Normal.Z); F32(o, a.D); break; }
            case Variant.Type.Quaternion: { U32(o, type); Quaternion a = v.AsQuaternion(); F32(o, a.X); F32(o, a.Y); F32(o, a.Z); F32(o, a.W); break; }
            case Variant.Type.Aabb:
            {
                U32(o, type);
                Aabb a = v.AsAabb();
                F32(o, a.Position.X); F32(o, a.Position.Y); F32(o, a.Position.Z); F32(o, a.Size.X); F32(o, a.Size.Y); F32(o, a.Size.Z);
                break;
            }
            case Variant.Type.Basis: { U32(o, type); PutBasis(o, v.AsBasis()); break; }
            case Variant.Type.Transform3D:
            {
                U32(o, type);
                Transform3D a = v.AsTransform3D();
                PutBasis(o, a.Basis);
                F32(o, a.Origin.X); F32(o, a.Origin.Y); F32(o, a.Origin.Z);
                break;
            }
            case Variant.Type.Projection:
            {
                U32(o, type);
                Projection a = v.AsProjection();
                foreach (Vector4 c in new[] { a.X, a.Y, a.Z, a.W }) { F32(o, c.X); F32(o, c.Y); F32(o, c.Z); F32(o, c.W); }
                break;
            }
            case Variant.Type.Color: { U32(o, type); Color a = v.AsColor(); F32(o, a.R); F32(o, a.G); F32(o, a.B); F32(o, a.A); break; }
            case Variant.Type.NodePath:
            {
                U32(o, type);
                NodePath np = v.AsNodePath();
                U32(o, 0x80000000u | (uint)np.GetNameCount());
                U32(o, (uint)np.GetSubnameCount());
                U32(o, np.IsAbsolute() ? 1u : 0u);
                for (int i = 0; i < np.GetNameCount(); i++) Str(o, np.GetName(i));
                for (int i = 0; i < np.GetSubnameCount(); i++) Str(o, np.GetSubname(i));
                break;
            }
            case Variant.Type.Rid: U32(o, type); U64(o, v.AsRid().Id); break;
            case Variant.Type.Object:
            {
                GodotObject? obj = v.AsGodotObject();
                if (obj is null) { U32(o, (uint)Variant.Type.Nil); break; }
                if (full) return Error.Unavailable;
                U32(o, type | FlagObjectAsId);
                U64(o, obj.GetInstanceId());
                break;
            }
            case Variant.Type.Callable: case Variant.Type.Signal: U32(o, type); break;
            case Variant.Type.Dictionary:
            {
                GDict d = v.AsGodotDictionary();
                U32(o, type);
                U32(o, (uint)d.Count);
                foreach (KeyValuePair<Variant, Variant> kv in d)
                {
                    Error e = EncodeInner(kv.Key, o, full, depth + 1);
                    if (e != Error.Ok) return e;
                    e = EncodeInner(kv.Value, o, full, depth + 1);
                    if (e != Error.Ok) return e;
                }
                break;
            }
            case Variant.Type.Array:
            {
                GArray a = v.AsGodotArray();
                U32(o, type);
                U32(o, (uint)a.Count);
                foreach (Variant item in a)
                {
                    Error e = EncodeInner(item, o, full, depth + 1);
                    if (e != Error.Ok) return e;
                }
                break;
            }
            case Variant.Type.PackedByteArray: { byte[] a = v.AsByteArray(); U32(o, type); U32(o, (uint)a.Length); o.AddRange(a); Pad(o, a.Length); break; }
            case Variant.Type.PackedInt32Array: { int[] a = v.AsInt32Array(); U32(o, type); U32(o, (uint)a.Length); foreach (int x in a) U32(o, (uint)x); break; }
            case Variant.Type.PackedInt64Array: { long[] a = v.AsInt64Array(); U32(o, type); U32(o, (uint)a.Length); foreach (long x in a) U64(o, (ulong)x); break; }
            case Variant.Type.PackedFloat32Array: { float[] a = v.AsFloat32Array(); U32(o, type); U32(o, (uint)a.Length); foreach (float x in a) F32(o, x); break; }
            case Variant.Type.PackedFloat64Array: { double[] a = v.AsFloat64Array(); U32(o, type); U32(o, (uint)a.Length); foreach (double x in a) F64(o, x); break; }
            case Variant.Type.PackedStringArray: { string[] a = v.AsStringArray(); U32(o, type); U32(o, (uint)a.Length); foreach (string s in a) Str(o, s); break; }
            case Variant.Type.PackedVector2Array: { Vector2[] a = v.AsVector2Array(); U32(o, type); U32(o, (uint)a.Length); foreach (Vector2 x in a) { F32(o, x.X); F32(o, x.Y); } break; }
            case Variant.Type.PackedVector3Array: { Vector3[] a = v.AsVector3Array(); U32(o, type); U32(o, (uint)a.Length); foreach (Vector3 x in a) { F32(o, x.X); F32(o, x.Y); F32(o, x.Z); } break; }
            case Variant.Type.PackedColorArray: { Color[] a = v.AsColorArray(); U32(o, type); U32(o, (uint)a.Length); foreach (Color x in a) { F32(o, x.R); F32(o, x.G); F32(o, x.B); F32(o, x.A); } break; }
            case Variant.Type.PackedVector4Array: { Vector4[] a = v.AsVector4Array(); U32(o, type); U32(o, (uint)a.Length); foreach (Vector4 x in a) { F32(o, x.X); F32(o, x.Y); F32(o, x.Z); F32(o, x.W); } break; }
            default: return Error.Bug;
        }
        return Error.Ok;
    }

    private static void PutBasis(List<byte> o, Basis b)
    {
        foreach (Vector3 r in new[] { b.Row0, b.Row1, b.Row2 }) { F32(o, r.X); F32(o, r.Y); F32(o, r.Z); }
    }

    // ---- decode ----

    /// <summary>Reads one Variant from the start of <paramref name="data"/>; <paramref name="used"/> is the bytes it took.</summary>
    internal static Error Decode(ReadOnlySpan<byte> data, bool allowObjects, out Variant value, out int used)
    {
        var r = new Reader(data);
        Error e = DecodeInner(r, allowObjects, 0, out value);
        used = r.Pos;
        return e;
    }

    private sealed class Reader
    {
        private readonly byte[] _d;
        public int Pos;
        public Reader(ReadOnlySpan<byte> d) { _d = d.ToArray(); Pos = 0; }
        public int Left => _d.Length - Pos;
        public bool Has(long n) => n >= 0 && n <= Left;
        public uint U32() { uint v = (uint)(_d[Pos] | (_d[Pos + 1] << 8) | (_d[Pos + 2] << 16) | (_d[Pos + 3] << 24)); Pos += 4; return v; }
        public ulong U64() { ulong lo = U32(); ulong hi = U32(); return lo | (hi << 32); }
        public float F32() => BitConverter.UInt32BitsToSingle(U32());
        public double F64() => BitConverter.UInt64BitsToDouble(U64());
        public ReadOnlySpan<byte> Take(int n) { var s = new ReadOnlySpan<byte>(_d, Pos, n); Pos += n; return s; }
        public void Skip(int n) => Pos += n;
    }

    private static Error ReadString(Reader r, out string s)
    {
        s = "";
        if (!r.Has(4)) return Error.InvalidData;
        uint len = r.U32();
        if (len > int.MaxValue || !r.Has(len)) return Error.InvalidData;
        s = Encoding.UTF8.GetString(r.Take((int)len));
        int pad = (int)(len % 4 == 0 ? 0 : 4 - len % 4);
        if (!r.Has(pad)) return Error.InvalidData;
        r.Skip(pad);
        return Error.Ok;
    }

    private static Error ReadCount(Reader r, long elementBytes, out int count)
    {
        count = 0;
        if (!r.Has(4)) return Error.InvalidData;
        uint c = r.U32() & 0x7FFFFFFFu;
        if (c > MaxCount || !r.Has((long)c * elementBytes)) return Error.InvalidData;
        count = (int)c;
        return Error.Ok;
    }

    // A typed container's element type description (Godot 4.4): nothing, a builtin type, a class name, or a class name and a script path.
    private static Error SkipContainerType(Reader r, uint mode)
    {
        switch (mode)
        {
            case 0: return Error.Ok;
            case 1: if (!r.Has(4)) return Error.InvalidData; r.Skip(4); return Error.Ok;
            case 2: return ReadString(r,out _);
            case 3: { Error e = ReadString(r,out _); return e != Error.Ok ? e : ReadString(r,out _); }
            default: return Error.InvalidData;
        }
    }

    private static Error DecodeInner(Reader r, bool allowObjects, int depth, out Variant value)
    {
        value = default;
        if (depth > MaxDepth) return Error.InvalidData;
        if (!r.Has(4)) return Error.InvalidData;
        uint header = r.U32();
        uint flags = header & 0xFFFF0000u;
        int ti = (int)(header & 0xFFFF);
        if (ti >= (int)Variant.Type.Max) return Error.InvalidData;
        var type = (Variant.Type)ti;
        bool wide = (flags & Flag64) != 0;

        bool Need(long n) => r.Has(n);

        switch (type)
        {
            case Variant.Type.Nil: return Error.Ok;
            case Variant.Type.Bool: if (!Need(4)) return Error.InvalidData; value = r.U32() != 0; return Error.Ok;
            case Variant.Type.Int:
                if (wide) { if (!Need(8)) return Error.InvalidData; value = (long)r.U64(); }
                else { if (!Need(4)) return Error.InvalidData; value = (long)(int)r.U32(); }
                return Error.Ok;
            case Variant.Type.Float:
                if (wide) { if (!Need(8)) return Error.InvalidData; value = r.F64(); }
                else { if (!Need(4)) return Error.InvalidData; value = (double)r.F32(); }
                return Error.Ok;
            case Variant.Type.String: { Error e = ReadString(r,out string s); if (e == Error.Ok) value = s; return e; }
            case Variant.Type.StringName: { Error e = ReadString(r,out string s); if (e == Error.Ok) value = new StringName(s); return e; }
            case Variant.Type.Vector2: if (!Need(8)) return Error.InvalidData; value = new Vector2(r.F32(), r.F32()); return Error.Ok;
            case Variant.Type.Vector2I: if (!Need(8)) return Error.InvalidData; value = new Vector2I((int)r.U32(), (int)r.U32()); return Error.Ok;
            case Variant.Type.Rect2: { if (!Need(16)) return Error.InvalidData; float x = r.F32(), y = r.F32(), w = r.F32(), h = r.F32(); value = new Rect2(x, y, w, h); return Error.Ok; }
            case Variant.Type.Rect2I: { if (!Need(16)) return Error.InvalidData; int x = (int)r.U32(), y = (int)r.U32(), w = (int)r.U32(), h = (int)r.U32(); value = new Rect2I(x, y, w, h); return Error.Ok; }
            case Variant.Type.Vector3: if (!Need(12)) return Error.InvalidData; value = new Vector3(r.F32(), r.F32(), r.F32()); return Error.Ok;
            case Variant.Type.Vector3I: if (!Need(12)) return Error.InvalidData; value = new Vector3I((int)r.U32(), (int)r.U32(), (int)r.U32()); return Error.Ok;
            case Variant.Type.Transform2D:
            {
                if (!Need(24)) return Error.InvalidData;
                var x = new Vector2(r.F32(), r.F32()); var y = new Vector2(r.F32(), r.F32()); var origin = new Vector2(r.F32(), r.F32());
                value = new Transform2D(x, y, origin);
                return Error.Ok;
            }
            case Variant.Type.Vector4: if (!Need(16)) return Error.InvalidData; value = new Vector4(r.F32(), r.F32(), r.F32(), r.F32()); return Error.Ok;
            case Variant.Type.Vector4I: if (!Need(16)) return Error.InvalidData; value = new Vector4I((int)r.U32(), (int)r.U32(), (int)r.U32(), (int)r.U32()); return Error.Ok;
            case Variant.Type.Plane: { if (!Need(16)) return Error.InvalidData; float a = r.F32(), b = r.F32(), c = r.F32(), d = r.F32(); value = new Plane(a, b, c, d); return Error.Ok; }
            case Variant.Type.Quaternion: { if (!Need(16)) return Error.InvalidData; float x = r.F32(), y = r.F32(), z = r.F32(), w = r.F32(); value = new Quaternion(x, y, z, w); return Error.Ok; }
            case Variant.Type.Aabb:
            {
                if (!Need(24)) return Error.InvalidData;
                var p = new Vector3(r.F32(), r.F32(), r.F32()); var s = new Vector3(r.F32(), r.F32(), r.F32());
                value = new Aabb(p, s);
                return Error.Ok;
            }
            case Variant.Type.Basis: if (!Need(36)) return Error.InvalidData; value = ReadBasis(r); return Error.Ok;
            case Variant.Type.Transform3D:
            {
                if (!Need(48)) return Error.InvalidData;
                Basis b = ReadBasis(r);
                var origin = new Vector3(r.F32(), r.F32(), r.F32());
                value = new Transform3D(b, origin);
                return Error.Ok;
            }
            case Variant.Type.Projection:
            {
                if (!Need(64)) return Error.InvalidData;
                var c0 = new Vector4(r.F32(), r.F32(), r.F32(), r.F32()); var c1 = new Vector4(r.F32(), r.F32(), r.F32(), r.F32());
                var c2 = new Vector4(r.F32(), r.F32(), r.F32(), r.F32()); var c3 = new Vector4(r.F32(), r.F32(), r.F32(), r.F32());
                value = new Projection(c0, c1, c2, c3);
                return Error.Ok;
            }
            case Variant.Type.Color: { if (!Need(16)) return Error.InvalidData; float cr = r.F32(), cg = r.F32(), cb = r.F32(), ca = r.F32(); value = new Color(cr, cg, cb, ca); return Error.Ok; }
            case Variant.Type.NodePath:
            {
                if (!Need(4)) return Error.InvalidData;
                uint first = r.U32();
                if ((first & 0x80000000u) == 0)
                {
                    // the old format: the path as one string whose length came first
                    if (first > int.MaxValue || !Need(first)) return Error.InvalidData;
                    string text = Encoding.UTF8.GetString(r.Take((int)first));
                    int pad = (int)(first % 4 == 0 ? 0 : 4 - first % 4);
                    if (!Need(pad)) return Error.InvalidData;
                    r.Skip(pad);
                    value = new NodePath(text);
                    return Error.Ok;
                }
                if (!Need(8)) return Error.InvalidData;
                uint names = first & 0x7FFFFFFFu;
                uint subs = r.U32();
                uint pflags = r.U32();
                if (names > MaxCount || subs > MaxCount) return Error.InvalidData;
                var parts = new StringBuilder();
                if ((pflags & 1) != 0) parts.Append('/');
                for (uint i = 0; i < names; i++)
                {
                    Error e = ReadString(r,out string n);
                    if (e != Error.Ok) return e;
                    if (i > 0) parts.Append('/');
                    parts.Append(n);
                }
                for (uint i = 0; i < subs; i++)
                {
                    Error e = ReadString(r,out string n);
                    if (e != Error.Ok) return e;
                    parts.Append(':').Append(n);
                }
                value = new NodePath(parts.ToString());
                return Error.Ok;
            }
            case Variant.Type.Rid: if (!Need(8)) return Error.InvalidData; value = new Rid(r.U64()); return Error.Ok;
            case Variant.Type.Object:
            {
                if ((flags & FlagObjectAsId) != 0)
                {
                    if (!Need(8)) return Error.InvalidData;
                    value = new EncodedObjectAsId { ObjectId = r.U64() };
                    return Error.Ok;
                }
                if (!allowObjects) return Error.Unauthorized;
                return Error.Unavailable;
            }
            case Variant.Type.Callable: value = new Callable(); return Error.Ok;
            case Variant.Type.Signal: value = new Signal(); return Error.Ok;
            case Variant.Type.Dictionary:
            {
                Error te = SkipContainerType(r,(flags >> 16) & 3);
                if (te == Error.Ok) te = SkipContainerType(r,(flags >> 18) & 3);
                if (te != Error.Ok) return te;
                Error ce = ReadCount(r,8, out int count);
                if (ce != Error.Ok) return ce;
                var d = new GDict();
                for (int i = 0; i < count; i++)
                {
                    Error e = DecodeInner(r,allowObjects, depth + 1, out Variant k);
                    if (e != Error.Ok) return e;
                    e = DecodeInner(r,allowObjects, depth + 1, out Variant val);
                    if (e != Error.Ok) return e;
                    d[k] = val;
                }
                value = d;
                return Error.Ok;
            }
            case Variant.Type.Array:
            {
                Error te = SkipContainerType(r,(flags >> 16) & 3);
                if (te != Error.Ok) return te;
                Error ce = ReadCount(r,4, out int count);
                if (ce != Error.Ok) return ce;
                var a = new GArray();
                for (int i = 0; i < count; i++)
                {
                    Error e = DecodeInner(r,allowObjects, depth + 1, out Variant item);
                    if (e != Error.Ok) return e;
                    a.Add(item);
                }
                value = a;
                return Error.Ok;
            }
            case Variant.Type.PackedByteArray:
            {
                Error ce = ReadCount(r,1, out int n);
                if (ce != Error.Ok) return ce;
                byte[] b = r.Take(n).ToArray();
                int pad = n % 4 == 0 ? 0 : 4 - n % 4;
                if (!Need(pad)) return Error.InvalidData;
                r.Skip(pad);
                value = b;
                return Error.Ok;
            }
            case Variant.Type.PackedInt32Array: { Error ce = ReadCount(r,4, out int n); if (ce != Error.Ok) return ce; var a = new int[n]; for (int i = 0; i < n; i++) a[i] = (int)r.U32(); value = a; return Error.Ok; }
            case Variant.Type.PackedInt64Array: { Error ce = ReadCount(r,8, out int n); if (ce != Error.Ok) return ce; var a = new long[n]; for (int i = 0; i < n; i++) a[i] = (long)r.U64(); value = a; return Error.Ok; }
            case Variant.Type.PackedFloat32Array: { Error ce = ReadCount(r,4, out int n); if (ce != Error.Ok) return ce; var a = new float[n]; for (int i = 0; i < n; i++) a[i] = r.F32(); value = a; return Error.Ok; }
            case Variant.Type.PackedFloat64Array: { Error ce = ReadCount(r,8, out int n); if (ce != Error.Ok) return ce; var a = new double[n]; for (int i = 0; i < n; i++) a[i] = r.F64(); value = a; return Error.Ok; }
            case Variant.Type.PackedStringArray:
            {
                Error ce = ReadCount(r,4, out int n);
                if (ce != Error.Ok) return ce;
                var a = new string[n];
                for (int i = 0; i < n; i++) { Error e = ReadString(r, out string item); if (e != Error.Ok) return e; a[i] = item; }
                value = Variant.CreateFrom(a);
                return Error.Ok;
            }
            case Variant.Type.PackedVector2Array: { Error ce = ReadCount(r,8, out int n); if (ce != Error.Ok) return ce; var a = new Vector2[n]; for (int i = 0; i < n; i++) a[i] = new Vector2(r.F32(), r.F32()); value = a; return Error.Ok; }
            case Variant.Type.PackedVector3Array: { Error ce = ReadCount(r,12, out int n); if (ce != Error.Ok) return ce; var a = new Vector3[n]; for (int i = 0; i < n; i++) a[i] = new Vector3(r.F32(), r.F32(), r.F32()); value = a; return Error.Ok; }
            case Variant.Type.PackedColorArray: { Error ce = ReadCount(r,16, out int n); if (ce != Error.Ok) return ce; var a = new Color[n]; for (int i = 0; i < n; i++) { float cr = r.F32(), cg = r.F32(), cb = r.F32(), ca = r.F32(); a[i] = new Color(cr, cg, cb, ca); } value = a; return Error.Ok; }
            case Variant.Type.PackedVector4Array: { Error ce = ReadCount(r,16, out int n); if (ce != Error.Ok) return ce; var a = new Vector4[n]; for (int i = 0; i < n; i++) a[i] = new Vector4(r.F32(), r.F32(), r.F32(), r.F32()); value = Variant.CreateFrom(a); return Error.Ok; }
            default: return Error.InvalidData;
        }
    }

    private static Basis ReadBasis(Reader r)
    {
        var b = new Basis();
        b.Row0 = new Vector3(r.F32(), r.F32(), r.F32());
        b.Row1 = new Vector3(r.F32(), r.F32(), r.F32());
        b.Row2 = new Vector3(r.F32(), r.F32(), r.F32());
        return b;
    }
}

public static partial class GD
{
    /// <summary>Serializes a Variant to Godot's binary encoding (objects travel as instance ids). Empty when the value cannot be encoded.</summary>
    public static byte[] VarToBytes(Variant var)
    {
        var o = new List<byte>();
        return VariantBinary.Encode(var, o) == Error.Ok ? o.ToArray() : System.Array.Empty<byte>();
    }

    /// <summary>Like <see cref="VarToBytes"/> but with full objects requested; this runtime cannot carry them, so an object answers empty.</summary>
    public static byte[] VarToBytesWithObjects(Variant var)
    {
        var o = new List<byte>();
        return VariantBinary.Encode(var, o, true) == Error.Ok ? o.ToArray() : System.Array.Empty<byte>();
    }

    /// <summary>Reads a Variant from Godot's binary encoding; nil when the bytes are not one.</summary>
    public static Variant BytesToVar(byte[] bytes) => VariantBinary.Decode(bytes, false, out Variant v, out _) == Error.Ok ? v : default;

    public static Variant BytesToVarWithObjects(byte[] bytes) => VariantBinary.Decode(bytes, true, out Variant v, out _) == Error.Ok ? v : default;
}
