// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Vector4 and Vector4I with Godot's members and operators.

using System.Globalization;

namespace Aver.Fabric;

/// <summary>A 4D vector of floats (Godot's Vector4).</summary>
public struct Vector4 : IEquatable<Vector4>
{
    public enum Axis { X = 0, Y = 1, Z = 2, W = 3 }

    public float X;
    public float Y;
    public float Z;
    public float W;

    public float this[int index]
    {
        readonly get => index switch { 0 => X, 1 => Y, 2 => Z, 3 => W, _ => throw new ArgumentOutOfRangeException(nameof(index)) };
        set
        {
            switch (index)
            {
                case 0: X = value; break;
                case 1: Y = value; break;
                case 2: Z = value; break;
                case 3: W = value; break;
                default: throw new ArgumentOutOfRangeException(nameof(index));
            }
        }
    }

    public Vector4(float x, float y, float z, float w) { X = x; Y = y; Z = z; W = w; }

    public static Vector4 Zero => new(0, 0, 0, 0);
    public static Vector4 One => new(1, 1, 1, 1);
    public static Vector4 Inf => new(float.PositiveInfinity, float.PositiveInfinity, float.PositiveInfinity, float.PositiveInfinity);

    public readonly Vector4 Abs() => new(Math.Abs(X), Math.Abs(Y), Math.Abs(Z), Math.Abs(W));
    public readonly Vector4 Ceil() => new(MathF.Ceiling(X), MathF.Ceiling(Y), MathF.Ceiling(Z), MathF.Ceiling(W));
    public readonly Vector4 Clamp(Vector4 min, Vector4 max) => new(Math.Clamp(X, min.X, max.X), Math.Clamp(Y, min.Y, max.Y), Math.Clamp(Z, min.Z, max.Z), Math.Clamp(W, min.W, max.W));
    public readonly Vector4 DirectionTo(Vector4 to) => (to - this).Normalized();
    public readonly float DistanceSquaredTo(Vector4 to) => (to - this).LengthSquared();
    public readonly float DistanceTo(Vector4 to) => (to - this).Length();
    public readonly float Dot(Vector4 with) => X * with.X + Y * with.Y + Z * with.Z + W * with.W;
    public readonly Vector4 Floor() => new(MathF.Floor(X), MathF.Floor(Y), MathF.Floor(Z), MathF.Floor(W));
    public readonly Vector4 Inverse() => new(1 / X, 1 / Y, 1 / Z, 1 / W);
    public readonly bool IsEqualApprox(Vector4 other) =>
        Mathf.IsEqualApprox(X, other.X) && Mathf.IsEqualApprox(Y, other.Y) && Mathf.IsEqualApprox(Z, other.Z) && Mathf.IsEqualApprox(W, other.W);
    public readonly bool IsFinite() => float.IsFinite(X) && float.IsFinite(Y) && float.IsFinite(Z) && float.IsFinite(W);
    public readonly bool IsNormalized() => Math.Abs(LengthSquared() - 1f) < 1e-05f;
    public readonly bool IsZeroApprox() => Mathf.IsZeroApprox(X) && Mathf.IsZeroApprox(Y) && Mathf.IsZeroApprox(Z) && Mathf.IsZeroApprox(W);
    public readonly float Length() => MathF.Sqrt(LengthSquared());
    public readonly float LengthSquared() => X * X + Y * Y + Z * Z + W * W;
    public readonly Vector4 Lerp(Vector4 to, float weight) =>
        new(Mathf.Lerp(X, to.X, weight), Mathf.Lerp(Y, to.Y, weight), Mathf.Lerp(Z, to.Z, weight), Mathf.Lerp(W, to.W, weight));
    public readonly Axis MaxAxisIndex()
    {
        int best = 0;
        for (int i = 1; i < 4; i++) if (this[i] > this[best]) best = i;
        return (Axis)best;
    }
    public readonly Axis MinAxisIndex()
    {
        int best = 0;
        for (int i = 1; i < 4; i++) if (this[i] < this[best]) best = i;
        return (Axis)best;
    }
    public readonly Vector4 Normalized()
    {
        Vector4 v = this;
        float l = LengthSquared();
        if (l != 0f) { l = MathF.Sqrt(l); v.X /= l; v.Y /= l; v.Z /= l; v.W /= l; }
        return v;
    }
    public readonly Vector4 PosMod(float mod) => new(Mathf.PosMod(X, mod), Mathf.PosMod(Y, mod), Mathf.PosMod(Z, mod), Mathf.PosMod(W, mod));
    public readonly Vector4 Round() => new(MathF.Round(X, MidpointRounding.AwayFromZero), MathF.Round(Y, MidpointRounding.AwayFromZero),
        MathF.Round(Z, MidpointRounding.AwayFromZero), MathF.Round(W, MidpointRounding.AwayFromZero));
    public readonly Vector4 Sign() => new(Mathf.Sign(X), Mathf.Sign(Y), Mathf.Sign(Z), Mathf.Sign(W));
    public readonly Vector4 Snapped(Vector4 step) => new(Mathf.Snapped(X, step.X), Mathf.Snapped(Y, step.Y), Mathf.Snapped(Z, step.Z), Mathf.Snapped(W, step.W));

    public static Vector4 operator +(Vector4 l, Vector4 r) => new(l.X + r.X, l.Y + r.Y, l.Z + r.Z, l.W + r.W);
    public static Vector4 operator -(Vector4 l, Vector4 r) => new(l.X - r.X, l.Y - r.Y, l.Z - r.Z, l.W - r.W);
    public static Vector4 operator -(Vector4 v) => new(-v.X, -v.Y, -v.Z, -v.W);
    public static Vector4 operator *(Vector4 v, float s) => new(v.X * s, v.Y * s, v.Z * s, v.W * s);
    public static Vector4 operator *(float s, Vector4 v) => new(v.X * s, v.Y * s, v.Z * s, v.W * s);
    public static Vector4 operator *(Vector4 l, Vector4 r) => new(l.X * r.X, l.Y * r.Y, l.Z * r.Z, l.W * r.W);
    public static Vector4 operator /(Vector4 v, float s) => new(v.X / s, v.Y / s, v.Z / s, v.W / s);
    public static Vector4 operator /(Vector4 l, Vector4 r) => new(l.X / r.X, l.Y / r.Y, l.Z / r.Z, l.W / r.W);
    public static bool operator ==(Vector4 l, Vector4 r) => l.Equals(r);
    public static bool operator !=(Vector4 l, Vector4 r) => !l.Equals(r);

    public readonly bool Equals(Vector4 other) => X == other.X && Y == other.Y && Z == other.Z && W == other.W;
    public override readonly bool Equals(object? obj) => obj is Vector4 v && Equals(v);
    public override readonly int GetHashCode() => HashCode.Combine(X, Y, Z, W);
    public override readonly string ToString() => string.Create(CultureInfo.InvariantCulture, $"({X}, {Y}, {Z}, {W})");
}

/// <summary>A 4D vector of ints (Godot's Vector4I).</summary>
public struct Vector4I : IEquatable<Vector4I>
{
    public int X;
    public int Y;
    public int Z;
    public int W;

    public int this[int index]
    {
        readonly get => index switch { 0 => X, 1 => Y, 2 => Z, 3 => W, _ => throw new ArgumentOutOfRangeException(nameof(index)) };
        set
        {
            switch (index)
            {
                case 0: X = value; break;
                case 1: Y = value; break;
                case 2: Z = value; break;
                case 3: W = value; break;
                default: throw new ArgumentOutOfRangeException(nameof(index));
            }
        }
    }

    public Vector4I(int x, int y, int z, int w) { X = x; Y = y; Z = z; W = w; }

    public static Vector4I Zero => new(0, 0, 0, 0);
    public static Vector4I One => new(1, 1, 1, 1);

    public readonly Vector4I Abs() => new(Math.Abs(X), Math.Abs(Y), Math.Abs(Z), Math.Abs(W));
    public readonly Vector4I Clamp(Vector4I min, Vector4I max) => new(Math.Clamp(X, min.X, max.X), Math.Clamp(Y, min.Y, max.Y), Math.Clamp(Z, min.Z, max.Z), Math.Clamp(W, min.W, max.W));
    public readonly float Length() => MathF.Sqrt(LengthSquared());
    public readonly int LengthSquared() => X * X + Y * Y + Z * Z + W * W;
    public readonly Vector4I Sign() => new(Mathf.Sign(X), Mathf.Sign(Y), Mathf.Sign(Z), Mathf.Sign(W));

    public static Vector4I operator +(Vector4I l, Vector4I r) => new(l.X + r.X, l.Y + r.Y, l.Z + r.Z, l.W + r.W);
    public static Vector4I operator -(Vector4I l, Vector4I r) => new(l.X - r.X, l.Y - r.Y, l.Z - r.Z, l.W - r.W);
    public static Vector4I operator -(Vector4I v) => new(-v.X, -v.Y, -v.Z, -v.W);
    public static Vector4I operator *(Vector4I v, int s) => new(v.X * s, v.Y * s, v.Z * s, v.W * s);
    public static Vector4I operator *(int s, Vector4I v) => new(v.X * s, v.Y * s, v.Z * s, v.W * s);
    public static Vector4I operator /(Vector4I v, int s) => new(v.X / s, v.Y / s, v.Z / s, v.W / s);
    public static bool operator ==(Vector4I l, Vector4I r) => l.Equals(r);
    public static bool operator !=(Vector4I l, Vector4I r) => !l.Equals(r);
    public static implicit operator Vector4(Vector4I v) => new(v.X, v.Y, v.Z, v.W);
    public static explicit operator Vector4I(Vector4 v) => new((int)v.X, (int)v.Y, (int)v.Z, (int)v.W);

    public readonly bool Equals(Vector4I other) => X == other.X && Y == other.Y && Z == other.Z && W == other.W;
    public override readonly bool Equals(object? obj) => obj is Vector4I v && Equals(v);
    public override readonly int GetHashCode() => HashCode.Combine(X, Y, Z, W);
    public override readonly string ToString() => $"({X}, {Y}, {Z}, {W})";
}
