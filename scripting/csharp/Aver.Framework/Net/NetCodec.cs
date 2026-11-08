// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The replication wire codec: little-endian primitives, bounds-checked reads, and the value types a replicated property or RPC argument may use.

using System.Text;
using Aver.Scene;

namespace Aver.Framework.Net;

/// <summary>A packet that is truncated, oversized or otherwise not what the codec expects.</summary>
public sealed class NetFormatException : Exception
{
    public NetFormatException(string message) : base(message) { }
}

/// <summary>Appends little-endian values to a growing buffer.</summary>
public sealed class NetWriter
{
    private byte[] _buf = new byte[64];
    private int _len;

    public int Length => _len;
    public byte[] ToArray() => _buf.AsSpan(0, _len).ToArray();

    private Span<byte> Reserve(int n)
    {
        if (_len + n > _buf.Length) Array.Resize(ref _buf, Math.Max(_buf.Length * 2, _len + n));
        Span<byte> s = _buf.AsSpan(_len, n);
        _len += n;
        return s;
    }

    public void U8(byte v) => Reserve(1)[0] = v;
    public void U16(ushort v) => System.Buffers.Binary.BinaryPrimitives.WriteUInt16LittleEndian(Reserve(2), v);
    public void U32(uint v) => System.Buffers.Binary.BinaryPrimitives.WriteUInt32LittleEndian(Reserve(4), v);
    public void U64(ulong v) => System.Buffers.Binary.BinaryPrimitives.WriteUInt64LittleEndian(Reserve(8), v);
    public void F32(float v) => U32(BitConverter.SingleToUInt32Bits(v));
    public void F64(double v) => U64(BitConverter.DoubleToUInt64Bits(v));

    /// <summary>A length (7 bits per byte, low first).</summary>
    public void Var(uint v)
    {
        while (v >= 0x80) { U8((byte)(v | 0x80)); v >>= 7; }
        U8((byte)v);
    }

    public void Bytes(ReadOnlySpan<byte> v) { Var((uint)v.Length); v.CopyTo(Reserve(v.Length)); }
    public void Raw(ReadOnlySpan<byte> v) => v.CopyTo(Reserve(v.Length));
    public void Str(string v) => Bytes(Encoding.UTF8.GetBytes(v));
}

/// <summary>Reads what <see cref="NetWriter"/> wrote; every read checks the bounds and throws <see cref="NetFormatException"/>.</summary>
public sealed class NetReader
{
    private readonly byte[] _buf;
    private int _pos;

    public NetReader(byte[] buffer, int start = 0) { _buf = buffer; _pos = start; }

    public int Remaining => _buf.Length - _pos;

    private ReadOnlySpan<byte> Take(int n)
    {
        if (n < 0 || n > Remaining) throw new NetFormatException("packet truncated");
        ReadOnlySpan<byte> s = _buf.AsSpan(_pos, n);
        _pos += n;
        return s;
    }

    public byte U8() => Take(1)[0];
    public ushort U16() => System.Buffers.Binary.BinaryPrimitives.ReadUInt16LittleEndian(Take(2));
    public uint U32() => System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(Take(4));
    public ulong U64() => System.Buffers.Binary.BinaryPrimitives.ReadUInt64LittleEndian(Take(8));
    public float F32() => BitConverter.UInt32BitsToSingle(U32());
    public double F64() => BitConverter.UInt64BitsToDouble(U64());

    public uint Var()
    {
        uint v = 0;
        for (int shift = 0; shift < 35; shift += 7)
        {
            byte b = U8();
            v |= (uint)(b & 0x7F) << shift;
            if ((b & 0x80) == 0) return v;
        }
        throw new NetFormatException("length too long");
    }

    public byte[] Bytes()
    {
        uint n = Var();
        if (n > (uint)Remaining) throw new NetFormatException("length beyond the packet");
        return Take((int)n).ToArray();
    }

    public string Str() => Encoding.UTF8.GetString(Bytes());
}

/// <summary>Encodes and decodes the value types replication supports: bool, the integer and float types, string, byte[], enums,
/// <see cref="Vec3"/>, <see cref="Quat"/> and one-dimensional arrays of those.</summary>
public static class NetCodec
{
    /// <summary>Whether <paramref name="t"/> can cross the wire.</summary>
    public static bool IsSupported(Type t)
    {
        if (t.IsEnum) return true;
        if (t.IsArray) return t.GetArrayRank() == 1 && IsSupported(t.GetElementType()!);
        return t == typeof(bool) || t == typeof(byte) || t == typeof(sbyte) || t == typeof(short) || t == typeof(ushort) || t == typeof(int)
            || t == typeof(uint) || t == typeof(long) || t == typeof(ulong) || t == typeof(float) || t == typeof(double)
            || t == typeof(string) || t == typeof(Vec3) || t == typeof(Quat);
    }

    public static void Write(NetWriter w, Type t, object? v)
    {
        if (t.IsEnum) { w.U64((ulong)Convert.ToInt64(v)); return; }
        if (t.IsArray)
        {
            var a = (Array?)v ?? Array.CreateInstance(t.GetElementType()!, 0);
            w.Var((uint)a.Length);
            foreach (object? e in a) Write(w, t.GetElementType()!, e);
            return;
        }
        if (t == typeof(bool)) w.U8((bool)v! ? (byte)1 : (byte)0);
        else if (t == typeof(byte)) w.U8((byte)v!);
        else if (t == typeof(sbyte)) w.U8(unchecked((byte)(sbyte)v!));
        else if (t == typeof(short)) w.U16(unchecked((ushort)(short)v!));
        else if (t == typeof(ushort)) w.U16((ushort)v!);
        else if (t == typeof(int)) w.U32(unchecked((uint)(int)v!));
        else if (t == typeof(uint)) w.U32((uint)v!);
        else if (t == typeof(long)) w.U64(unchecked((ulong)(long)v!));
        else if (t == typeof(ulong)) w.U64((ulong)v!);
        else if (t == typeof(float)) w.F32((float)v!);
        else if (t == typeof(double)) w.F64((double)v!);
        else if (t == typeof(string)) w.Str((string?)v ?? "");
        else if (t == typeof(Vec3)) { var p = (Vec3)v!; w.F32(p.X); w.F32(p.Y); w.F32(p.Z); }
        else if (t == typeof(Quat)) { var q = (Quat)v!; w.F32(q.X); w.F32(q.Y); w.F32(q.Z); w.F32(q.W); }
        else throw new NetFormatException("type not supported on the wire: " + t.Name);
    }

    public static object? Read(NetReader r, Type t)
    {
        if (t.IsEnum) return Enum.ToObject(t, unchecked((long)r.U64()));
        if (t.IsArray)
        {
            uint n = r.Var();
            if (n > (uint)r.Remaining) throw new NetFormatException("array longer than the packet");
            Type et = t.GetElementType()!;
            var a = Array.CreateInstance(et, (int)n);
            for (int i = 0; i < n; i++) a.SetValue(Read(r, et), i);
            return a;
        }
        if (t == typeof(bool)) return r.U8() != 0;
        if (t == typeof(byte)) return r.U8();
        if (t == typeof(sbyte)) return unchecked((sbyte)r.U8());
        if (t == typeof(short)) return unchecked((short)r.U16());
        if (t == typeof(ushort)) return r.U16();
        if (t == typeof(int)) return unchecked((int)r.U32());
        if (t == typeof(uint)) return r.U32();
        if (t == typeof(long)) return unchecked((long)r.U64());
        if (t == typeof(ulong)) return r.U64();
        if (t == typeof(float)) return r.F32();
        if (t == typeof(double)) return r.F64();
        if (t == typeof(string)) return r.Str();
        if (t == typeof(Vec3)) return new Vec3(r.F32(), r.F32(), r.F32());
        if (t == typeof(Quat)) { float x = r.F32(), y = r.F32(), z = r.F32(), wv = r.F32(); return new Quat(x, y, z, wv); }
        throw new NetFormatException("type not supported on the wire: " + t.Name);
    }
}
