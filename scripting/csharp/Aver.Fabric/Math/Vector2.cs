// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Vector2 and Vector2I with Godot's members and operators. 2D is not converted yet, but the types are scripts' vocabulary.

using System.Globalization;

namespace Aver.Fabric;

/// <summary>A 2D vector of floats (Godot's Vector2).</summary>
public struct Vector2 : IEquatable<Vector2>, IComparable<Vector2>
{
    public enum Axis { X = 0, Y = 1 }

    public float X;
    public float Y;

    public float this[int index]
    {
        readonly get => index switch { 0 => X, 1 => Y, _ => throw new ArgumentOutOfRangeException(nameof(index)) };
        set { switch (index) { case 0: X = value; break; case 1: Y = value; break; default: throw new ArgumentOutOfRangeException(nameof(index)); } }
    }

    public Vector2(float x, float y) { X = x; Y = y; }

    public static Vector2 Zero => new(0, 0);
    public static Vector2 One => new(1, 1);
    public static Vector2 Inf => new(float.PositiveInfinity, float.PositiveInfinity);
    public static Vector2 Left => new(-1, 0);
    public static Vector2 Right => new(1, 0);
    public static Vector2 Up => new(0, -1);
    public static Vector2 Down => new(0, 1);

    public readonly Vector2 Abs() => new(Math.Abs(X), Math.Abs(Y));
    public readonly float Angle() => MathF.Atan2(Y, X);
    public readonly float AngleTo(Vector2 to) => MathF.Atan2(Cross(to), Dot(to));
    public readonly float AngleToPoint(Vector2 to) => MathF.Atan2(to.Y - Y, to.X - X);
    public readonly float Aspect() => X / Y;
    public readonly Vector2 Bounce(Vector2 normal) => -Reflect(normal);
    public readonly Vector2 Ceil() => new(MathF.Ceiling(X), MathF.Ceiling(Y));
    public readonly Vector2 Clamp(Vector2 min, Vector2 max) => new(Math.Clamp(X, min.X, max.X), Math.Clamp(Y, min.Y, max.Y));
    public readonly float Cross(Vector2 with) => X * with.Y - Y * with.X;
    public readonly Vector2 CubicInterpolate(Vector2 b, Vector2 preA, Vector2 postB, float weight) => new(
        Mathf.CubicInterpolate(X, b.X, preA.X, postB.X, weight), Mathf.CubicInterpolate(Y, b.Y, preA.Y, postB.Y, weight));
    public readonly Vector2 BezierInterpolate(Vector2 control1, Vector2 control2, Vector2 end, float t) => new(
        Mathf.BezierInterpolate(X, control1.X, control2.X, end.X, t), Mathf.BezierInterpolate(Y, control1.Y, control2.Y, end.Y, t));
    public readonly Vector2 DirectionTo(Vector2 to) => (to - this).Normalized();
    public readonly float DistanceSquaredTo(Vector2 to) => (to - this).LengthSquared();
    public readonly float DistanceTo(Vector2 to) => (to - this).Length();
    public readonly float Dot(Vector2 with) => X * with.X + Y * with.Y;
    public readonly Vector2 Floor() => new(MathF.Floor(X), MathF.Floor(Y));
    public static Vector2 FromAngle(float angle) => new(MathF.Cos(angle), MathF.Sin(angle));
    public readonly Vector2 Inverse() => new(1 / X, 1 / Y);
    public readonly bool IsEqualApprox(Vector2 other) => Mathf.IsEqualApprox(X, other.X) && Mathf.IsEqualApprox(Y, other.Y);
    public readonly bool IsFinite() => float.IsFinite(X) && float.IsFinite(Y);
    public readonly bool IsNormalized() => Math.Abs(LengthSquared() - 1f) < 1e-05f;
    public readonly bool IsZeroApprox() => Mathf.IsZeroApprox(X) && Mathf.IsZeroApprox(Y);
    public readonly float Length() => MathF.Sqrt(X * X + Y * Y);
    public readonly float LengthSquared() => X * X + Y * Y;
    public readonly Vector2 Lerp(Vector2 to, float weight) => new(Mathf.Lerp(X, to.X, weight), Mathf.Lerp(Y, to.Y, weight));
    public readonly Vector2 LimitLength(float length = 1f)
    {
        Vector2 v = this;
        float l = Length();
        if (l > 0f && length < l) v = v / l * length;
        return v;
    }
    public readonly Axis MaxAxisIndex() => X < Y ? Axis.Y : Axis.X;
    public readonly Axis MinAxisIndex() => X < Y ? Axis.X : Axis.Y;
    public readonly Vector2 Max(Vector2 with) => new(Math.Max(X, with.X), Math.Max(Y, with.Y));
    public readonly Vector2 Min(Vector2 with) => new(Math.Min(X, with.X), Math.Min(Y, with.Y));
    public readonly Vector2 MoveToward(Vector2 to, float delta)
    {
        Vector2 v = this, vd = to - v;
        float len = vd.Length();
        return len <= delta || len < Mathf.Epsilon ? to : v + vd / len * delta;
    }
    public readonly Vector2 Normalized()
    {
        Vector2 v = this;
        float l = X * X + Y * Y;
        if (l != 0f) { l = MathF.Sqrt(l); v.X /= l; v.Y /= l; }
        return v;
    }
    public readonly Vector2 Orthogonal() => new(Y, -X);
    public readonly Vector2 PosMod(float mod) => new(Mathf.PosMod(X, mod), Mathf.PosMod(Y, mod));
    public readonly Vector2 PosMod(Vector2 modv) => new(Mathf.PosMod(X, modv.X), Mathf.PosMod(Y, modv.Y));
    public readonly Vector2 Project(Vector2 onNormal) => onNormal * (Dot(onNormal) / onNormal.LengthSquared());
    public readonly Vector2 Reflect(Vector2 normal) => 2f * Dot(normal) * normal - this;
    public readonly Vector2 Rotated(float angle)
    {
        float sine = MathF.Sin(angle), cosi = MathF.Cos(angle);
        return new Vector2(X * cosi - Y * sine, X * sine + Y * cosi);
    }
    public readonly Vector2 Round() => new(MathF.Round(X, MidpointRounding.AwayFromZero), MathF.Round(Y, MidpointRounding.AwayFromZero));
    public readonly Vector2 Sign() => new(Mathf.Sign(X), Mathf.Sign(Y));
    public readonly Vector2 Slerp(Vector2 to, float weight)
    {
        float startLengthSq = LengthSquared(), endLengthSq = to.LengthSquared();
        if (startLengthSq == 0f || endLengthSq == 0f) return Lerp(to, weight);
        float startLength = MathF.Sqrt(startLengthSq);
        float resultLength = Mathf.Lerp(startLength, MathF.Sqrt(endLengthSq), weight);
        float angle = AngleTo(to);
        return Rotated(angle * weight) * (resultLength / startLength);
    }
    public readonly Vector2 Slide(Vector2 normal) => this - normal * Dot(normal);
    public readonly Vector2 Snapped(Vector2 step) => new(Mathf.Snapped(X, step.X), Mathf.Snapped(Y, step.Y));
    public readonly Vector2 Snapped(float step) => new(Mathf.Snapped(X, step), Mathf.Snapped(Y, step));

    public static Vector2 operator +(Vector2 l, Vector2 r) => new(l.X + r.X, l.Y + r.Y);
    public static Vector2 operator -(Vector2 l, Vector2 r) => new(l.X - r.X, l.Y - r.Y);
    public static Vector2 operator -(Vector2 v) => new(-v.X, -v.Y);
    public static Vector2 operator *(Vector2 v, float s) => new(v.X * s, v.Y * s);
    public static Vector2 operator *(float s, Vector2 v) => new(v.X * s, v.Y * s);
    public static Vector2 operator *(Vector2 l, Vector2 r) => new(l.X * r.X, l.Y * r.Y);
    public static Vector2 operator /(Vector2 v, float s) => new(v.X / s, v.Y / s);
    public static Vector2 operator /(Vector2 l, Vector2 r) => new(l.X / r.X, l.Y / r.Y);
    public static Vector2 operator %(Vector2 v, float s) => new(v.X % s, v.Y % s);
    public static Vector2 operator %(Vector2 l, Vector2 r) => new(l.X % r.X, l.Y % r.Y);
    public static bool operator ==(Vector2 l, Vector2 r) => l.Equals(r);
    public static bool operator !=(Vector2 l, Vector2 r) => !l.Equals(r);
    public static bool operator <(Vector2 l, Vector2 r) => l.X == r.X ? l.Y < r.Y : l.X < r.X;
    public static bool operator >(Vector2 l, Vector2 r) => l.X == r.X ? l.Y > r.Y : l.X > r.X;
    public static bool operator <=(Vector2 l, Vector2 r) => l.X == r.X ? l.Y <= r.Y : l.X < r.X;
    public static bool operator >=(Vector2 l, Vector2 r) => l.X == r.X ? l.Y >= r.Y : l.X > r.X;
    public static explicit operator Vector2I(Vector2 v) => new((int)v.X, (int)v.Y);

    public readonly void Deconstruct(out float x, out float y) { x = X; y = Y; }
    public readonly bool Equals(Vector2 other) => X == other.X && Y == other.Y;
    public override readonly bool Equals(object? obj) => obj is Vector2 v && Equals(v);
    public readonly int CompareTo(Vector2 other) => this < other ? -1 : this > other ? 1 : 0;
    public override readonly int GetHashCode() => HashCode.Combine(X, Y);
    public override readonly string ToString() => string.Create(CultureInfo.InvariantCulture, $"({X}, {Y})");
}

/// <summary>A 2D vector of ints (Godot's Vector2I).</summary>
public struct Vector2I : IEquatable<Vector2I>
{
    public enum Axis { X = 0, Y = 1 }

    public int X;
    public int Y;

    public int this[int index]
    {
        readonly get => index switch { 0 => X, 1 => Y, _ => throw new ArgumentOutOfRangeException(nameof(index)) };
        set { switch (index) { case 0: X = value; break; case 1: Y = value; break; default: throw new ArgumentOutOfRangeException(nameof(index)); } }
    }

    public Vector2I(int x, int y) { X = x; Y = y; }

    public static Vector2I Zero => new(0, 0);
    public static Vector2I One => new(1, 1);
    public static Vector2I Min => new(int.MinValue, int.MinValue);
    public static Vector2I Max => new(int.MaxValue, int.MaxValue);
    public static Vector2I Left => new(-1, 0);
    public static Vector2I Right => new(1, 0);
    public static Vector2I Up => new(0, -1);
    public static Vector2I Down => new(0, 1);

    public readonly Vector2I Abs() => new(Math.Abs(X), Math.Abs(Y));
    public readonly float Aspect() => (float)X / Y;
    public readonly Vector2I Clamp(Vector2I min, Vector2I max) => new(Math.Clamp(X, min.X, max.X), Math.Clamp(Y, min.Y, max.Y));
    public readonly float DistanceTo(Vector2I to) => (to - this).Length();
    public readonly int DistanceSquaredTo(Vector2I to) => (to - this).LengthSquared();
    public readonly float Length() => MathF.Sqrt(X * X + Y * Y);
    public readonly int LengthSquared() => X * X + Y * Y;
    public readonly Axis MaxAxisIndex() => X < Y ? Axis.Y : Axis.X;
    public readonly Axis MinAxisIndex() => X < Y ? Axis.X : Axis.Y;
    public readonly Vector2I Sign() => new(Mathf.Sign(X), Mathf.Sign(Y));
    public readonly Vector2I Snapped(Vector2I step) => new(Mathf.Snapped(X, step.X), Mathf.Snapped(Y, step.Y));

    public static Vector2I operator +(Vector2I l, Vector2I r) => new(l.X + r.X, l.Y + r.Y);
    public static Vector2I operator -(Vector2I l, Vector2I r) => new(l.X - r.X, l.Y - r.Y);
    public static Vector2I operator -(Vector2I v) => new(-v.X, -v.Y);
    public static Vector2I operator *(Vector2I v, int s) => new(v.X * s, v.Y * s);
    public static Vector2I operator *(int s, Vector2I v) => new(v.X * s, v.Y * s);
    public static Vector2I operator *(Vector2I l, Vector2I r) => new(l.X * r.X, l.Y * r.Y);
    public static Vector2I operator /(Vector2I v, int s) => new(v.X / s, v.Y / s);
    public static Vector2I operator /(Vector2I l, Vector2I r) => new(l.X / r.X, l.Y / r.Y);
    public static Vector2I operator %(Vector2I v, int s) => new(v.X % s, v.Y % s);
    public static bool operator ==(Vector2I l, Vector2I r) => l.Equals(r);
    public static bool operator !=(Vector2I l, Vector2I r) => !l.Equals(r);
    public static bool operator <(Vector2I l, Vector2I r) => l.X == r.X ? l.Y < r.Y : l.X < r.X;
    public static bool operator >(Vector2I l, Vector2I r) => l.X == r.X ? l.Y > r.Y : l.X > r.X;
    public static bool operator <=(Vector2I l, Vector2I r) => l.X == r.X ? l.Y <= r.Y : l.X < r.X;
    public static bool operator >=(Vector2I l, Vector2I r) => l.X == r.X ? l.Y >= r.Y : l.X > r.X;
    public static implicit operator Vector2(Vector2I v) => new(v.X, v.Y);

    public readonly void Deconstruct(out int x, out int y) { x = X; y = Y; }
    public readonly bool Equals(Vector2I other) => X == other.X && Y == other.Y;
    public override readonly bool Equals(object? obj) => obj is Vector2I v && Equals(v);
    public override readonly int GetHashCode() => HashCode.Combine(X, Y);
    public override readonly string ToString() => $"({X}, {Y})";
}
