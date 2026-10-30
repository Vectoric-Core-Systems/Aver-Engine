// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Vector3 and Vector3I with Godot's members and operators. Godot space: metres, +Y up, -Z forward, right-handed.

using System.Globalization;

namespace Aver.Fabric;

/// <summary>A 3D vector of floats (Godot's Vector3).</summary>
public struct Vector3 : IEquatable<Vector3>, IComparable<Vector3>
{
    public enum Axis { X = 0, Y = 1, Z = 2 }

    public float X;
    public float Y;
    public float Z;

    public float this[int index]
    {
        readonly get => index switch { 0 => X, 1 => Y, 2 => Z, _ => throw new ArgumentOutOfRangeException(nameof(index)) };
        set
        {
            switch (index)
            {
                case 0: X = value; break;
                case 1: Y = value; break;
                case 2: Z = value; break;
                default: throw new ArgumentOutOfRangeException(nameof(index));
            }
        }
    }

    public Vector3(float x, float y, float z) { X = x; Y = y; Z = z; }

    public static Vector3 Zero => new(0, 0, 0);
    public static Vector3 One => new(1, 1, 1);
    public static Vector3 Inf => new(float.PositiveInfinity, float.PositiveInfinity, float.PositiveInfinity);
    public static Vector3 Left => new(-1, 0, 0);
    public static Vector3 Right => new(1, 0, 0);
    public static Vector3 Up => new(0, 1, 0);
    public static Vector3 Down => new(0, -1, 0);
    public static Vector3 Forward => new(0, 0, -1);
    public static Vector3 Back => new(0, 0, 1);
    public static Vector3 ModelLeft => new(1, 0, 0);
    public static Vector3 ModelRight => new(-1, 0, 0);
    public static Vector3 ModelTop => new(0, 1, 0);
    public static Vector3 ModelBottom => new(0, -1, 0);
    public static Vector3 ModelFront => new(0, 0, 1);
    public static Vector3 ModelRear => new(0, 0, -1);

    public readonly Vector3 Abs() => new(Math.Abs(X), Math.Abs(Y), Math.Abs(Z));
    public readonly float AngleTo(Vector3 to) => MathF.Atan2(Cross(to).Length(), Dot(to));
    public readonly Vector3 BezierDerivative(Vector3 control1, Vector3 control2, Vector3 end, float t) => new(
        BezierDeriv(X, control1.X, control2.X, end.X, t), BezierDeriv(Y, control1.Y, control2.Y, end.Y, t), BezierDeriv(Z, control1.Z, control2.Z, end.Z, t));
    private static float BezierDeriv(float s, float c1, float c2, float e, float t)
    {
        float omt = 1f - t, omt2 = omt * omt, t2 = t * t;
        return (c1 - s) * 3f * omt2 + (c2 - c1) * 6f * omt * t + (e - c2) * 3f * t2;
    }
    public readonly Vector3 BezierInterpolate(Vector3 control1, Vector3 control2, Vector3 end, float t) => new(
        Mathf.BezierInterpolate(X, control1.X, control2.X, end.X, t), Mathf.BezierInterpolate(Y, control1.Y, control2.Y, end.Y, t),
        Mathf.BezierInterpolate(Z, control1.Z, control2.Z, end.Z, t));
    public readonly Vector3 Bounce(Vector3 normal) => -Reflect(normal);
    public readonly Vector3 Ceil() => new(MathF.Ceiling(X), MathF.Ceiling(Y), MathF.Ceiling(Z));
    public readonly Vector3 Clamp(Vector3 min, Vector3 max) => new(Math.Clamp(X, min.X, max.X), Math.Clamp(Y, min.Y, max.Y), Math.Clamp(Z, min.Z, max.Z));
    public readonly Vector3 Cross(Vector3 with) => new(Y * with.Z - Z * with.Y, Z * with.X - X * with.Z, X * with.Y - Y * with.X);
    public readonly Vector3 CubicInterpolate(Vector3 b, Vector3 preA, Vector3 postB, float weight) => new(
        Mathf.CubicInterpolate(X, b.X, preA.X, postB.X, weight), Mathf.CubicInterpolate(Y, b.Y, preA.Y, postB.Y, weight),
        Mathf.CubicInterpolate(Z, b.Z, preA.Z, postB.Z, weight));
    public readonly Vector3 DirectionTo(Vector3 to) => (to - this).Normalized();
    public readonly float DistanceSquaredTo(Vector3 to) => (to - this).LengthSquared();
    public readonly float DistanceTo(Vector3 to) => (to - this).Length();
    public readonly float Dot(Vector3 with) => X * with.X + Y * with.Y + Z * with.Z;
    public readonly Vector3 Floor() => new(MathF.Floor(X), MathF.Floor(Y), MathF.Floor(Z));
    public readonly Vector3 Inverse() => new(1 / X, 1 / Y, 1 / Z);
    public readonly bool IsEqualApprox(Vector3 other) => Mathf.IsEqualApprox(X, other.X) && Mathf.IsEqualApprox(Y, other.Y) && Mathf.IsEqualApprox(Z, other.Z);
    public readonly bool IsFinite() => float.IsFinite(X) && float.IsFinite(Y) && float.IsFinite(Z);
    public readonly bool IsNormalized() => Math.Abs(LengthSquared() - 1f) < 1e-05f;
    public readonly bool IsZeroApprox() => Mathf.IsZeroApprox(X) && Mathf.IsZeroApprox(Y) && Mathf.IsZeroApprox(Z);
    public readonly float Length() => MathF.Sqrt(X * X + Y * Y + Z * Z);
    public readonly float LengthSquared() => X * X + Y * Y + Z * Z;
    public readonly Vector3 Lerp(Vector3 to, float weight) => new(Mathf.Lerp(X, to.X, weight), Mathf.Lerp(Y, to.Y, weight), Mathf.Lerp(Z, to.Z, weight));
    public readonly Vector3 LimitLength(float length = 1f)
    {
        Vector3 v = this;
        float l = Length();
        if (l > 0f && length < l) v = v / l * length;
        return v;
    }
    public readonly Axis MaxAxisIndex() => X < Y ? (Y < Z ? Axis.Z : Axis.Y) : (X < Z ? Axis.Z : Axis.X);
    public readonly Axis MinAxisIndex() => X < Y ? (X < Z ? Axis.X : Axis.Z) : (Y < Z ? Axis.Y : Axis.Z);
    public readonly Vector3 Max(Vector3 with) => new(Math.Max(X, with.X), Math.Max(Y, with.Y), Math.Max(Z, with.Z));
    public readonly Vector3 Min(Vector3 with) => new(Math.Min(X, with.X), Math.Min(Y, with.Y), Math.Min(Z, with.Z));
    public readonly Vector3 MoveToward(Vector3 to, float delta)
    {
        Vector3 v = this, vd = to - v;
        float len = vd.Length();
        return len <= delta || len < Mathf.Epsilon ? to : v + vd / len * delta;
    }
    public readonly Vector3 Normalized()
    {
        Vector3 v = this;
        float l = X * X + Y * Y + Z * Z;
        if (l != 0f) { l = MathF.Sqrt(l); v.X /= l; v.Y /= l; v.Z /= l; }
        return v;
    }
    public readonly Basis Outer(Vector3 with) => new(
        new Vector3(X * with.X, X * with.Y, X * with.Z), new Vector3(Y * with.X, Y * with.Y, Y * with.Z), new Vector3(Z * with.X, Z * with.Y, Z * with.Z), true);
    public readonly Vector3 PosMod(float mod) => new(Mathf.PosMod(X, mod), Mathf.PosMod(Y, mod), Mathf.PosMod(Z, mod));
    public readonly Vector3 PosMod(Vector3 modv) => new(Mathf.PosMod(X, modv.X), Mathf.PosMod(Y, modv.Y), Mathf.PosMod(Z, modv.Z));
    public readonly Vector3 Project(Vector3 onNormal) => onNormal * (Dot(onNormal) / onNormal.LengthSquared());
    public readonly Vector3 Reflect(Vector3 normal) => 2f * Dot(normal) * normal - this;
    public readonly Vector3 Rotated(Vector3 axis, float angle) => new Basis(axis, angle) * this;
    public readonly Vector3 Round() => new(MathF.Round(X, MidpointRounding.AwayFromZero), MathF.Round(Y, MidpointRounding.AwayFromZero), MathF.Round(Z, MidpointRounding.AwayFromZero));
    public readonly Vector3 Sign() => new(Mathf.Sign(X), Mathf.Sign(Y), Mathf.Sign(Z));
    public readonly float SignedAngleTo(Vector3 to, Vector3 axis)
    {
        Vector3 crossTo = Cross(to);
        float unsignedAngle = MathF.Atan2(crossTo.Length(), Dot(to));
        float sign = crossTo.Dot(axis);
        return sign < 0f ? -unsignedAngle : unsignedAngle;
    }
    public readonly Vector3 Slerp(Vector3 to, float weight)
    {
        float startLengthSq = LengthSquared(), endLengthSq = to.LengthSquared();
        if (startLengthSq == 0f || endLengthSq == 0f) return Lerp(to, weight);
        Vector3 axis = Cross(to);
        float axisLengthSq = axis.LengthSquared();
        if (axisLengthSq == 0f) return Lerp(to, weight);
        axis /= MathF.Sqrt(axisLengthSq);
        float startLength = MathF.Sqrt(startLengthSq);
        float resultLength = Mathf.Lerp(startLength, MathF.Sqrt(endLengthSq), weight);
        float angle = AngleTo(to);
        return Rotated(axis, angle * weight) * (resultLength / startLength);
    }
    public readonly Vector3 Slide(Vector3 normal) => this - normal * Dot(normal);
    public readonly Vector3 Snapped(Vector3 step) => new(Mathf.Snapped(X, step.X), Mathf.Snapped(Y, step.Y), Mathf.Snapped(Z, step.Z));
    public readonly Vector3 Snapped(float step) => new(Mathf.Snapped(X, step), Mathf.Snapped(Y, step), Mathf.Snapped(Z, step));

    public static Vector3 operator +(Vector3 l, Vector3 r) => new(l.X + r.X, l.Y + r.Y, l.Z + r.Z);
    public static Vector3 operator -(Vector3 l, Vector3 r) => new(l.X - r.X, l.Y - r.Y, l.Z - r.Z);
    public static Vector3 operator -(Vector3 v) => new(-v.X, -v.Y, -v.Z);
    public static Vector3 operator *(Vector3 v, float s) => new(v.X * s, v.Y * s, v.Z * s);
    public static Vector3 operator *(float s, Vector3 v) => new(v.X * s, v.Y * s, v.Z * s);
    public static Vector3 operator *(Vector3 l, Vector3 r) => new(l.X * r.X, l.Y * r.Y, l.Z * r.Z);
    public static Vector3 operator /(Vector3 v, float s) => new(v.X / s, v.Y / s, v.Z / s);
    public static Vector3 operator /(Vector3 l, Vector3 r) => new(l.X / r.X, l.Y / r.Y, l.Z / r.Z);
    public static Vector3 operator %(Vector3 v, float s) => new(v.X % s, v.Y % s, v.Z % s);
    public static Vector3 operator %(Vector3 l, Vector3 r) => new(l.X % r.X, l.Y % r.Y, l.Z % r.Z);
    public static bool operator ==(Vector3 l, Vector3 r) => l.Equals(r);
    public static bool operator !=(Vector3 l, Vector3 r) => !l.Equals(r);
    public static bool operator <(Vector3 l, Vector3 r) => l.X == r.X ? (l.Y == r.Y ? l.Z < r.Z : l.Y < r.Y) : l.X < r.X;
    public static bool operator >(Vector3 l, Vector3 r) => l.X == r.X ? (l.Y == r.Y ? l.Z > r.Z : l.Y > r.Y) : l.X > r.X;
    public static bool operator <=(Vector3 l, Vector3 r) => l.X == r.X ? (l.Y == r.Y ? l.Z <= r.Z : l.Y < r.Y) : l.X < r.X;
    public static bool operator >=(Vector3 l, Vector3 r) => l.X == r.X ? (l.Y == r.Y ? l.Z >= r.Z : l.Y > r.Y) : l.X > r.X;
    public static explicit operator Vector3I(Vector3 v) => new((int)v.X, (int)v.Y, (int)v.Z);

    public readonly void Deconstruct(out float x, out float y, out float z) { x = X; y = Y; z = Z; }
    public readonly bool Equals(Vector3 other) => X == other.X && Y == other.Y && Z == other.Z;
    public override readonly bool Equals(object? obj) => obj is Vector3 v && Equals(v);
    public readonly int CompareTo(Vector3 other) => this < other ? -1 : this > other ? 1 : 0;
    public override readonly int GetHashCode() => HashCode.Combine(X, Y, Z);
    public override readonly string ToString() => string.Create(CultureInfo.InvariantCulture, $"({X}, {Y}, {Z})");
}

/// <summary>A 3D vector of ints (Godot's Vector3I).</summary>
public struct Vector3I : IEquatable<Vector3I>
{
    public enum Axis { X = 0, Y = 1, Z = 2 }

    public int X;
    public int Y;
    public int Z;

    public int this[int index]
    {
        readonly get => index switch { 0 => X, 1 => Y, 2 => Z, _ => throw new ArgumentOutOfRangeException(nameof(index)) };
        set
        {
            switch (index)
            {
                case 0: X = value; break;
                case 1: Y = value; break;
                case 2: Z = value; break;
                default: throw new ArgumentOutOfRangeException(nameof(index));
            }
        }
    }

    public Vector3I(int x, int y, int z) { X = x; Y = y; Z = z; }

    public static Vector3I Zero => new(0, 0, 0);
    public static Vector3I One => new(1, 1, 1);
    public static Vector3I Min => new(int.MinValue, int.MinValue, int.MinValue);
    public static Vector3I Max => new(int.MaxValue, int.MaxValue, int.MaxValue);
    public static Vector3I Left => new(-1, 0, 0);
    public static Vector3I Right => new(1, 0, 0);
    public static Vector3I Up => new(0, 1, 0);
    public static Vector3I Down => new(0, -1, 0);
    public static Vector3I Forward => new(0, 0, -1);
    public static Vector3I Back => new(0, 0, 1);

    public readonly Vector3I Abs() => new(Math.Abs(X), Math.Abs(Y), Math.Abs(Z));
    public readonly Vector3I Clamp(Vector3I min, Vector3I max) => new(Math.Clamp(X, min.X, max.X), Math.Clamp(Y, min.Y, max.Y), Math.Clamp(Z, min.Z, max.Z));
    public readonly float DistanceTo(Vector3I to) => (to - this).Length();
    public readonly int DistanceSquaredTo(Vector3I to) => (to - this).LengthSquared();
    public readonly float Length() => MathF.Sqrt(X * X + Y * Y + Z * Z);
    public readonly int LengthSquared() => X * X + Y * Y + Z * Z;
    public readonly Axis MaxAxisIndex() => X < Y ? (Y < Z ? Axis.Z : Axis.Y) : (X < Z ? Axis.Z : Axis.X);
    public readonly Axis MinAxisIndex() => X < Y ? (X < Z ? Axis.X : Axis.Z) : (Y < Z ? Axis.Y : Axis.Z);
    public readonly Vector3I Sign() => new(Mathf.Sign(X), Mathf.Sign(Y), Mathf.Sign(Z));
    public readonly Vector3I Snapped(Vector3I step) => new(Mathf.Snapped(X, step.X), Mathf.Snapped(Y, step.Y), Mathf.Snapped(Z, step.Z));

    public static Vector3I operator +(Vector3I l, Vector3I r) => new(l.X + r.X, l.Y + r.Y, l.Z + r.Z);
    public static Vector3I operator -(Vector3I l, Vector3I r) => new(l.X - r.X, l.Y - r.Y, l.Z - r.Z);
    public static Vector3I operator -(Vector3I v) => new(-v.X, -v.Y, -v.Z);
    public static Vector3I operator *(Vector3I v, int s) => new(v.X * s, v.Y * s, v.Z * s);
    public static Vector3I operator *(int s, Vector3I v) => new(v.X * s, v.Y * s, v.Z * s);
    public static Vector3I operator *(Vector3I l, Vector3I r) => new(l.X * r.X, l.Y * r.Y, l.Z * r.Z);
    public static Vector3I operator /(Vector3I v, int s) => new(v.X / s, v.Y / s, v.Z / s);
    public static Vector3I operator /(Vector3I l, Vector3I r) => new(l.X / r.X, l.Y / r.Y, l.Z / r.Z);
    public static Vector3I operator %(Vector3I v, int s) => new(v.X % s, v.Y % s, v.Z % s);
    public static bool operator ==(Vector3I l, Vector3I r) => l.Equals(r);
    public static bool operator !=(Vector3I l, Vector3I r) => !l.Equals(r);
    public static bool operator <(Vector3I l, Vector3I r) => l.X == r.X ? (l.Y == r.Y ? l.Z < r.Z : l.Y < r.Y) : l.X < r.X;
    public static bool operator >(Vector3I l, Vector3I r) => l.X == r.X ? (l.Y == r.Y ? l.Z > r.Z : l.Y > r.Y) : l.X > r.X;
    public static bool operator <=(Vector3I l, Vector3I r) => l.X == r.X ? (l.Y == r.Y ? l.Z <= r.Z : l.Y < r.Y) : l.X < r.X;
    public static bool operator >=(Vector3I l, Vector3I r) => l.X == r.X ? (l.Y == r.Y ? l.Z >= r.Z : l.Y > r.Y) : l.X > r.X;
    public static implicit operator Vector3(Vector3I v) => new(v.X, v.Y, v.Z);

    public readonly void Deconstruct(out int x, out int y, out int z) { x = X; y = Y; z = Z; }
    public readonly bool Equals(Vector3I other) => X == other.X && Y == other.Y && Z == other.Z;
    public override readonly bool Equals(object? obj) => obj is Vector3I v && Equals(v);
    public override readonly int GetHashCode() => HashCode.Combine(X, Y, Z);
    public override readonly string ToString() => $"({X}, {Y}, {Z})";
}
