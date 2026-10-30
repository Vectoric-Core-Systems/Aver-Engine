// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Quaternion: a rotation with Godot's constructors, Slerp and Euler conversions.

using System.Globalization;

namespace Aver.Fabric;

/// <summary>A unit quaternion for rotations (Godot's Quaternion): X, Y, Z imaginary, W real.</summary>
public struct Quaternion : IEquatable<Quaternion>
{
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

    public Quaternion(float x, float y, float z, float w) { X = x; Y = y; Z = z; W = w; }

    /// <summary>The rotation of a basis.</summary>
    public Quaternion(Basis basis)
    {
        this = basis.GetRotationQuaternion();
    }

    /// <summary>A rotation of <paramref name="angle"/> radians about <paramref name="axis"/> (the axis need not be normalized).</summary>
    public Quaternion(Vector3 axis, float angle)
    {
        float d = axis.Length();
        if (d == 0f) { X = 0; Y = 0; Z = 0; W = 0; return; }
        float sin = MathF.Sin(angle * 0.5f), cos = MathF.Cos(angle * 0.5f), s = sin / d;
        X = axis.X * s; Y = axis.Y * s; Z = axis.Z * s; W = cos;
    }

    /// <summary>The shortest rotation taking direction <paramref name="arcFrom"/> to <paramref name="arcTo"/> (both normalized).</summary>
    public Quaternion(Vector3 arcFrom, Vector3 arcTo)
    {
        Vector3 c = arcFrom.Cross(arcTo);
        float d = arcFrom.Dot(arcTo);
        if (d < -1f + 1e-5f) { X = 0f; Y = 1f; Z = 0f; W = 0f; }
        else
        {
            float s = MathF.Sqrt((1f + d) * 2f), rs = 1f / s;
            X = c.X * rs; Y = c.Y * rs; Z = c.Z * rs; W = s * 0.5f;
        }
    }

    public static Quaternion Identity => new(0, 0, 0, 1);

    /// <summary>The rotation for Euler angles in Godot's default YXZ order.</summary>
    public static Quaternion FromEuler(Vector3 euler)
    {
        float halfA1 = euler.Y * 0.5f, halfA2 = euler.X * 0.5f, halfA3 = euler.Z * 0.5f;
        float cosA1 = MathF.Cos(halfA1), sinA1 = MathF.Sin(halfA1);
        float cosA2 = MathF.Cos(halfA2), sinA2 = MathF.Sin(halfA2);
        float cosA3 = MathF.Cos(halfA3), sinA3 = MathF.Sin(halfA3);
        return new Quaternion(
            sinA1 * cosA2 * sinA3 + cosA1 * sinA2 * cosA3,
            sinA1 * cosA2 * cosA3 - cosA1 * sinA2 * sinA3,
            cosA1 * cosA2 * sinA3 - sinA1 * sinA2 * cosA3,
            sinA1 * sinA2 * sinA3 + cosA1 * cosA2 * cosA3);
    }

    public readonly float AngleTo(Quaternion to)
    {
        float d = Dot(to);
        return MathF.Acos(Math.Clamp(d * d * 2f - 1f, -1f, 1f));
    }

    public readonly Quaternion Normalized() => this / Length();
    public readonly float Dot(Quaternion b) => X * b.X + Y * b.Y + Z * b.Z + W * b.W;
    public readonly float Length() => MathF.Sqrt(LengthSquared());
    public readonly float LengthSquared() => Dot(this);
    public readonly bool IsNormalized() => Math.Abs(LengthSquared() - 1f) < 1e-05f;
    public readonly bool IsFinite() => float.IsFinite(X) && float.IsFinite(Y) && float.IsFinite(Z) && float.IsFinite(W);
    public readonly bool IsEqualApprox(Quaternion other) =>
        Mathf.IsEqualApprox(X, other.X) && Mathf.IsEqualApprox(Y, other.Y) && Mathf.IsEqualApprox(Z, other.Z) && Mathf.IsEqualApprox(W, other.W);
    public readonly Quaternion Inverse() => new(-X, -Y, -Z, W);

    public readonly float GetAngle() => 2f * MathF.Acos(W);
    public readonly Vector3 GetAxis()
    {
        if (Math.Abs(W) > 1f - 1e-5f) return new Vector3(X, Y, Z);
        float r = 1f / MathF.Sqrt(1f - W * W);
        return new Vector3(X * r, Y * r, Z * r);
    }

    /// <summary>The Euler angles (radians) in the given order; the quaternion must be normalized.</summary>
    public readonly Vector3 GetEuler(EulerOrder order = EulerOrder.Yxz) => new Basis(this).GetEuler(order);

    public readonly Quaternion Slerp(Quaternion to, float weight)
    {
        float cosom = Dot(to);
        Quaternion to1 = to;
        if (cosom < 0f) { cosom = -cosom; to1 = -to; }
        float scale0, scale1;
        if (1f - cosom > 1e-5f)
        {
            float omega = MathF.Acos(cosom), sinom = MathF.Sin(omega);
            scale0 = MathF.Sin((1f - weight) * omega) / sinom;
            scale1 = MathF.Sin(weight * omega) / sinom;
        }
        else { scale0 = 1f - weight; scale1 = weight; }
        return new Quaternion(scale0 * X + scale1 * to1.X, scale0 * Y + scale1 * to1.Y, scale0 * Z + scale1 * to1.Z, scale0 * W + scale1 * to1.W);
    }

    /// <summary>Slerp without taking the shorter arc.</summary>
    public readonly Quaternion Slerpni(Quaternion to, float weight)
    {
        float dot = Dot(to);
        if (Math.Abs(dot) > 0.9999f) return this;
        float theta = MathF.Acos(dot), sinT = 1f / MathF.Sin(theta);
        float newFactor = MathF.Sin(weight * theta) * sinT, invFactor = MathF.Sin((1f - weight) * theta) * sinT;
        return new Quaternion(invFactor * X + newFactor * to.X, invFactor * Y + newFactor * to.Y, invFactor * Z + newFactor * to.Z, invFactor * W + newFactor * to.W);
    }

    public static Quaternion operator *(Quaternion l, Quaternion r) => new(
        l.W * r.X + l.X * r.W + l.Y * r.Z - l.Z * r.Y,
        l.W * r.Y + l.Y * r.W + l.Z * r.X - l.X * r.Z,
        l.W * r.Z + l.Z * r.W + l.X * r.Y - l.Y * r.X,
        l.W * r.W - l.X * r.X - l.Y * r.Y - l.Z * r.Z);

    /// <summary>Rotates a vector.</summary>
    public static Vector3 operator *(Quaternion q, Vector3 v)
    {
        Vector3 u = new(q.X, q.Y, q.Z);
        Vector3 uv = u.Cross(v);
        return v + (uv * q.W + u.Cross(uv)) * 2f;
    }

    /// <summary>Rotates a vector by the inverse rotation.</summary>
    public static Vector3 operator *(Vector3 v, Quaternion q) => q.Inverse() * v;

    public static Quaternion operator +(Quaternion l, Quaternion r) => new(l.X + r.X, l.Y + r.Y, l.Z + r.Z, l.W + r.W);
    public static Quaternion operator -(Quaternion l, Quaternion r) => new(l.X - r.X, l.Y - r.Y, l.Z - r.Z, l.W - r.W);
    public static Quaternion operator -(Quaternion q) => new(-q.X, -q.Y, -q.Z, -q.W);
    public static Quaternion operator *(Quaternion q, float s) => new(q.X * s, q.Y * s, q.Z * s, q.W * s);
    public static Quaternion operator *(float s, Quaternion q) => new(q.X * s, q.Y * s, q.Z * s, q.W * s);
    public static Quaternion operator /(Quaternion q, float s) => q * (1f / s);
    public static bool operator ==(Quaternion l, Quaternion r) => l.Equals(r);
    public static bool operator !=(Quaternion l, Quaternion r) => !l.Equals(r);

    public readonly bool Equals(Quaternion other) => X == other.X && Y == other.Y && Z == other.Z && W == other.W;
    public override readonly bool Equals(object? obj) => obj is Quaternion q && Equals(q);
    public override readonly int GetHashCode() => HashCode.Combine(X, Y, Z, W);
    public override readonly string ToString() => string.Create(CultureInfo.InvariantCulture, $"({X}, {Y}, {Z}, {W})");
}
