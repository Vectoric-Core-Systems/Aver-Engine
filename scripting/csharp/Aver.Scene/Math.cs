// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// The value types the scripting surface authors transforms with: Vec3, Quat and Rot.

namespace Aver.Scene;

/// <summary>A position or direction: centimetres, +X forward, +Y right, +Z up, left-handed.</summary>
public struct Vec3 : System.IEquatable<Vec3>
{
    public float X;
    public float Y;
    public float Z;

    public Vec3(float x, float y, float z) { X = x; Y = y; Z = z; }

    public static Vec3 Up => new(0, 0, 1);
    public static Vec3 Forward => new(1, 0, 0);
    public static Vec3 Right => new(0, 1, 0);
    public static Vec3 Zero => new(0, 0, 0);
    public static Vec3 One => new(1, 1, 1);

    public static Vec3 operator +(Vec3 a, Vec3 b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
    public static Vec3 operator -(Vec3 a, Vec3 b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
    public static Vec3 operator *(Vec3 a, float s) => new(a.X * s, a.Y * s, a.Z * s);
    public static Vec3 operator *(float s, Vec3 a) => new(a.X * s, a.Y * s, a.Z * s);
    public static Vec3 operator /(Vec3 a, float s) => new(a.X / s, a.Y / s, a.Z / s);
    public static Vec3 operator -(Vec3 a) => new(-a.X, -a.Y, -a.Z);
    public static bool operator ==(Vec3 a, Vec3 b) => a.Equals(b);
    public static bool operator !=(Vec3 a, Vec3 b) => !a.Equals(b);

    /// <summary>Length in centimetres.</summary>
    public float Length => System.MathF.Sqrt(X * X + Y * Y + Z * Z);

    /// <summary>Length squared, in centimetres squared. Prefer it for comparisons.</summary>
    public float LengthSquared => X * X + Y * Y + Z * Z;

    /// <summary>This direction at unit length, or <see cref="Zero"/> if it is too short to have one.</summary>
    public Vec3 Normalized
    {
        get
        {
            float len = Length;
            return len > 1e-6f ? new Vec3(X / len, Y / len, Z / len) : Zero;
        }
    }

    /// <summary>True when this is within a hair of the zero vector.</summary>
    public bool IsNearlyZero => LengthSquared <= 1e-12f;

    /// <summary>The dot product.</summary>
    public static float Dot(Vec3 a, Vec3 b) => a.X * b.X + a.Y * b.Y + a.Z * b.Z;

    /// <summary>The cross product, left-handed: <c>Cross(Forward, Right)</c> is <see cref="Up"/>.</summary>
    public static Vec3 Cross(Vec3 a, Vec3 b) => new(
        a.Y * b.Z - a.Z * b.Y,
        a.Z * b.X - a.X * b.Z,
        a.X * b.Y - a.Y * b.X);

    /// <summary>Distance in centimetres.</summary>
    public static float Distance(Vec3 a, Vec3 b) => (a - b).Length;
    /// <summary>Squared distance, for the same reason as <see cref="LengthSquared"/>.</summary>
    public static float DistanceSquared(Vec3 a, Vec3 b) => (a - b).LengthSquared;

    /// <summary>Linear interpolation; <paramref name="t"/> is clamped to 0..1.</summary>
    public static Vec3 Lerp(Vec3 a, Vec3 b, float t)
    {
        t = t < 0f ? 0f : (t > 1f ? 1f : t);
        return new Vec3(a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t, a.Z + (b.Z - a.Z) * t);
    }

    /// <summary>Steps from <paramref name="a"/> towards <paramref name="b"/> by at most
    /// <paramref name="maxDistance"/> centimetres, stopping on the target rather than overshooting.</summary>
    public static Vec3 MoveTowards(Vec3 a, Vec3 b, float maxDistance)
    {
        Vec3 d = b - a;
        float len = d.Length;
        if (len <= maxDistance || len <= 1e-6f) return b;
        return a + d * (maxDistance / len);
    }

    public bool Equals(Vec3 o) => X == o.X && Y == o.Y && Z == o.Z;
    public override bool Equals(object? o) => o is Vec3 v && Equals(v);
    public override int GetHashCode() => System.HashCode.Combine(X, Y, Z);
    public override string ToString() => $"({X}, {Y}, {Z})";
}

/// <summary>A rotation as a unit quaternion (x, y, z, w), the form the scene stores in CLocal.rotation.</summary>
public struct Quat : System.IEquatable<Quat>
{
    public float X;
    public float Y;
    public float Z;
    public float W;

    public Quat(float x, float y, float z, float w) { X = x; Y = y; Z = z; W = w; }

    public static Quat Identity => new(0, 0, 0, 1);

    /// <summary>A rotation of <paramref name="radians"/> about unit <paramref name="axis"/>. Left-handed.</summary>
    public static Quat FromAxisAngle(Vec3 axis, float radians)
    {
        float half = radians * 0.5f;
        float s = System.MathF.Sin(half);
        return new Quat(axis.X * s, axis.Y * s, axis.Z * s, System.MathF.Cos(half));
    }

    public static Quat operator *(Quat a, Quat b) => new(
        a.W * b.X + a.X * b.W + a.Y * b.Z - a.Z * b.Y,
        a.W * b.Y - a.X * b.Z + a.Y * b.W + a.Z * b.X,
        a.W * b.Z + a.X * b.Y - a.Y * b.X + a.Z * b.W,
        a.W * b.W - a.X * b.X - a.Y * b.Y - a.Z * b.Z);

    /// <summary>The yaw component in degrees.</summary>
    public float YawDegrees()
    {
        float siny = 2f * (W * Z + X * Y);
        float cosy = 1f - 2f * (Y * Y + Z * Z);
        return System.MathF.Atan2(siny, cosy) * (180f / System.MathF.PI);
    }

    public bool Equals(Quat o) => X == o.X && Y == o.Y && Z == o.Z && W == o.W;
    public override bool Equals(object? o) => o is Quat q && Equals(q);
    public override int GetHashCode() => System.HashCode.Combine(X, Y, Z, W);
}

/// <summary>Rotation in degrees: yaw about Up (+Z), pitch about Right (+Y), roll about Forward (+X).
/// Composition is intrinsic yaw -> pitch -> roll, matching <c>.ocmap</c>'s PLACE.</summary>
public struct Rot : System.IEquatable<Rot>
{
    public float Yaw;
    public float Pitch;
    public float Roll;

    public Rot(float yaw, float pitch, float roll) { Yaw = yaw; Pitch = pitch; Roll = roll; }

    public static Rot Zero => new(0, 0, 0);

    /// <summary>The quaternion the scene stores. Intrinsic Z(yaw) then Y(pitch) then X(roll).</summary>
    public Quat ToQuat()
    {
        const float d2r = System.MathF.PI / 180f;
        Quat qy = Quat.FromAxisAngle(Vec3.Up, Yaw * d2r);
        Quat qp = Quat.FromAxisAngle(Vec3.Right, Pitch * d2r);
        Quat qr = Quat.FromAxisAngle(Vec3.Forward, Roll * d2r);
        return qy * qp * qr;
    }

    public bool Equals(Rot o) => Yaw == o.Yaw && Pitch == o.Pitch && Roll == o.Roll;
    public override bool Equals(object? o) => o is Rot r && Equals(r);
    public override int GetHashCode() => System.HashCode.Combine(Yaw, Pitch, Roll);
}
