// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Basis: a 3x3 rotation/scale matrix stored by rows, with Godot's constructors, Euler orders and operators.

using System.Globalization;

namespace Aver.Fabric;

/// <summary>The order the three Euler rotations compose in (Godot's EulerOrder).</summary>
public enum EulerOrder { Xyz = 0, Xzy = 1, Yxz = 2, Yzx = 3, Zxy = 4, Zyx = 5 }

/// <summary>A 3x3 matrix by rows. The columns are the transformed X, Y and Z axes (Godot's Basis).</summary>
public struct Basis : IEquatable<Basis>
{
    public Vector3 Row0;
    public Vector3 Row1;
    public Vector3 Row2;

    /// <summary>The matrix element in the given column and row (GodotSharp's index order).</summary>
    public float this[int column, int row]
    {
        readonly get => row switch { 0 => Row0[column], 1 => Row1[column], 2 => Row2[column], _ => throw new ArgumentOutOfRangeException(nameof(row)) };
        set
        {
            switch (row)
            {
                case 0: Row0[column] = value; break;
                case 1: Row1[column] = value; break;
                case 2: Row2[column] = value; break;
                default: throw new ArgumentOutOfRangeException(nameof(row));
            }
        }
    }

    /// <summary>Column 0 to 2 as a vector (GDScript's basis[i] and GodotSharp's Basis[i]; Basis[0] is the X axis).</summary>
    public Vector3 this[int column]
    {
        readonly get => column switch { 0 => Column0, 1 => Column1, 2 => Column2, _ => throw new ArgumentOutOfRangeException(nameof(column)) };
        set
        {
            switch (column)
            {
                case 0: Column0 = value; break;
                case 1: Column1 = value; break;
                case 2: Column2 = value; break;
                default: throw new ArgumentOutOfRangeException(nameof(column));
            }
        }
    }

    public Vector3 Column0
    {
        readonly get => new(Row0.X, Row1.X, Row2.X);
        set { Row0.X = value.X; Row1.X = value.Y; Row2.X = value.Z; }
    }

    public Vector3 Column1
    {
        readonly get => new(Row0.Y, Row1.Y, Row2.Y);
        set { Row0.Y = value.X; Row1.Y = value.Y; Row2.Y = value.Z; }
    }

    public Vector3 Column2
    {
        readonly get => new(Row0.Z, Row1.Z, Row2.Z);
        set { Row0.Z = value.X; Row1.Z = value.Y; Row2.Z = value.Z; }
    }

    /// <summary>The basis's X axis (first column).</summary>
    public Vector3 X { readonly get => Column0; set => Column0 = value; }
    public Vector3 Y { readonly get => Column1; set => Column1 = value; }
    public Vector3 Z { readonly get => Column2; set => Column2 = value; }

    /// <summary>Builds a basis from its three columns.</summary>
    public Basis(Vector3 column0, Vector3 column1, Vector3 column2)
    {
        Row0 = new Vector3(column0.X, column1.X, column2.X);
        Row1 = new Vector3(column0.Y, column1.Y, column2.Y);
        Row2 = new Vector3(column0.Z, column1.Z, column2.Z);
    }

    // Rows given directly; the flag only picks this overload.
    internal Basis(Vector3 row0, Vector3 row1, Vector3 row2, bool fromRows)
    {
        Row0 = row0; Row1 = row1; Row2 = row2;
    }

    /// <summary>A rotation of <paramref name="angle"/> radians about a normalized axis.</summary>
    public Basis(Vector3 axis, float angle)
    {
        Vector3 axisSq = new(axis.X * axis.X, axis.Y * axis.Y, axis.Z * axis.Z);
        float cosine = MathF.Cos(angle), sine = MathF.Sin(angle), t = 1f - cosine;
        Row0 = new Vector3(axisSq.X + cosine * (1f - axisSq.X), axis.X * axis.Y * t - axis.Z * sine, axis.X * axis.Z * t + axis.Y * sine);
        Row1 = new Vector3(axis.X * axis.Y * t + axis.Z * sine, axisSq.Y + cosine * (1f - axisSq.Y), axis.Y * axis.Z * t - axis.X * sine);
        Row2 = new Vector3(axis.X * axis.Z * t - axis.Y * sine, axis.Y * axis.Z * t + axis.X * sine, axisSq.Z + cosine * (1f - axisSq.Z));
    }

    /// <summary>The rotation a quaternion stands for.</summary>
    public Basis(Quaternion quaternion)
    {
        float d = quaternion.LengthSquared();
        float s = 2f / d;
        float xs = quaternion.X * s, ys = quaternion.Y * s, zs = quaternion.Z * s;
        float wx = quaternion.W * xs, wy = quaternion.W * ys, wz = quaternion.W * zs;
        float xx = quaternion.X * xs, xy = quaternion.X * ys, xz = quaternion.X * zs;
        float yy = quaternion.Y * ys, yz = quaternion.Y * zs, zz = quaternion.Z * zs;
        Row0 = new Vector3(1f - (yy + zz), xy - wz, xz + wy);
        Row1 = new Vector3(xy + wz, 1f - (xx + zz), yz - wx);
        Row2 = new Vector3(xz - wy, yz + wx, 1f - (xx + yy));
    }

    public static Basis Identity => new(new Vector3(1, 0, 0), new Vector3(0, 1, 0), new Vector3(0, 0, 1), true);
    public static Basis FlipX => new(new Vector3(-1, 0, 0), new Vector3(0, 1, 0), new Vector3(0, 0, 1), true);
    public static Basis FlipY => new(new Vector3(1, 0, 0), new Vector3(0, -1, 0), new Vector3(0, 0, 1), true);
    public static Basis FlipZ => new(new Vector3(1, 0, 0), new Vector3(0, 1, 0), new Vector3(0, 0, -1), true);

    public static Basis FromScale(Vector3 scale) => new(new Vector3(scale.X, 0, 0), new Vector3(0, scale.Y, 0), new Vector3(0, 0, scale.Z), true);

    /// <summary>A rotation then a per-axis scale: R * S (what Transform3D.InterpolateWith builds).</summary>
    public static Basis FromQuaternionScale(Quaternion rotation, Vector3 scale) => new Basis(rotation) * FromScale(scale);

    /// <summary>The basis for Euler angles (radians). The default order, YXZ, is Godot's: Z first, then X, then Y.</summary>
    public static Basis FromEuler(Vector3 euler, EulerOrder order = EulerOrder.Yxz)
    {
        float c = MathF.Cos(euler.X), s = MathF.Sin(euler.X);
        Basis xmat = new(new Vector3(1, 0, 0), new Vector3(0, c, -s), new Vector3(0, s, c), true);
        c = MathF.Cos(euler.Y); s = MathF.Sin(euler.Y);
        Basis ymat = new(new Vector3(c, 0, s), new Vector3(0, 1, 0), new Vector3(-s, 0, c), true);
        c = MathF.Cos(euler.Z); s = MathF.Sin(euler.Z);
        Basis zmat = new(new Vector3(c, -s, 0), new Vector3(s, c, 0), new Vector3(0, 0, 1), true);
        return order switch
        {
            EulerOrder.Xyz => xmat * ymat * zmat,
            EulerOrder.Xzy => xmat * zmat * ymat,
            EulerOrder.Yxz => ymat * xmat * zmat,
            EulerOrder.Yzx => ymat * zmat * xmat,
            EulerOrder.Zxy => zmat * xmat * ymat,
            EulerOrder.Zyx => zmat * ymat * xmat,
            _ => throw new ArgumentOutOfRangeException(nameof(order)),
        };
    }

    public readonly float Determinant() =>
        Row0.X * (Row1.Y * Row2.Z - Row1.Z * Row2.Y) - Row0.Y * (Row1.X * Row2.Z - Row1.Z * Row2.X) + Row0.Z * (Row1.X * Row2.Y - Row1.Y * Row2.X);

    /// <summary>The Euler angles (radians) that <see cref="FromEuler"/> turns back into this rotation. The basis must be orthonormal.</summary>
    public readonly Vector3 GetEuler(EulerOrder order = EulerOrder.Yxz)
    {
        const float half = MathF.PI * 0.5f;
        Vector3 e = default;
        switch (order)
        {
            case EulerOrder.Xyz:
            {
                float m02 = Row0.Z;
                if (m02 < 1f)
                {
                    if (m02 > -1f) { e.X = MathF.Atan2(-Row1.Z, Row2.Z); e.Y = MathF.Asin(m02); e.Z = MathF.Atan2(-Row0.Y, Row0.X); }
                    else { e.X = MathF.Atan2(Row2.Y, Row1.Y); e.Y = -half; e.Z = 0f; }
                }
                else { e.X = MathF.Atan2(Row2.Y, Row1.Y); e.Y = half; e.Z = 0f; }
                break;
            }
            case EulerOrder.Xzy:
            {
                float m01 = Row0.Y;
                if (m01 < 1f)
                {
                    if (m01 > -1f) { e.X = MathF.Atan2(Row2.Y, Row1.Y); e.Y = MathF.Atan2(Row0.Z, Row0.X); e.Z = MathF.Asin(-m01); }
                    else { e.X = -MathF.Atan2(Row1.Z, Row2.Z); e.Y = 0f; e.Z = half; }
                }
                else { e.X = -MathF.Atan2(Row1.Z, Row2.Z); e.Y = 0f; e.Z = -half; }
                break;
            }
            case EulerOrder.Yxz:
            {
                float m12 = Row1.Z;
                if (m12 < 1f)
                {
                    if (m12 > -1f)
                    {
                        if (Row1.X == 0f && Row0.Y == 0f && Row0.Z == 0f && Row2.X == 0f && Row0.X == 1f)
                        {
                            e.X = MathF.Atan2(-m12, Row1.Y); e.Y = 0f; e.Z = 0f;   // a pure X rotation, in its simplest form
                        }
                        else { e.X = MathF.Asin(-m12); e.Y = MathF.Atan2(Row0.Z, Row2.Z); e.Z = MathF.Atan2(Row1.X, Row1.Y); }
                    }
                    else { e.X = half; e.Y = MathF.Atan2(Row0.Y, Row0.X); e.Z = 0f; }
                }
                else { e.X = -half; e.Y = -MathF.Atan2(Row0.Y, Row0.X); e.Z = 0f; }
                break;
            }
            case EulerOrder.Yzx:
            {
                float m10 = Row1.X;
                if (m10 < 1f)
                {
                    if (m10 > -1f) { e.X = MathF.Atan2(-Row1.Z, Row1.Y); e.Y = MathF.Atan2(-Row2.X, Row0.X); e.Z = MathF.Asin(m10); }
                    else { e.X = MathF.Atan2(Row2.Y, Row2.Z); e.Y = 0f; e.Z = -half; }
                }
                else { e.X = MathF.Atan2(Row2.Y, Row2.Z); e.Y = 0f; e.Z = half; }
                break;
            }
            case EulerOrder.Zxy:
            {
                float m21 = Row2.Y;
                if (m21 < 1f)
                {
                    if (m21 > -1f) { e.X = MathF.Asin(m21); e.Y = MathF.Atan2(-Row2.X, Row2.Z); e.Z = MathF.Atan2(-Row0.Y, Row1.Y); }
                    else { e.X = -half; e.Y = MathF.Atan2(Row0.Z, Row0.X); e.Z = 0f; }
                }
                else { e.X = half; e.Y = MathF.Atan2(Row0.Z, Row0.X); e.Z = 0f; }
                break;
            }
            case EulerOrder.Zyx:
            {
                float m20 = Row2.X;
                if (m20 < 1f)
                {
                    if (m20 > -1f) { e.X = MathF.Atan2(Row2.Y, Row2.Z); e.Y = MathF.Asin(-m20); e.Z = MathF.Atan2(Row1.X, Row0.X); }
                    else { e.X = 0f; e.Y = half; e.Z = -MathF.Atan2(Row0.Y, Row0.Z); }
                }
                else { e.X = 0f; e.Y = -half; e.Z = -MathF.Atan2(Row0.Y, Row0.Z); }
                break;
            }
            default: throw new ArgumentOutOfRangeException(nameof(order));
        }
        return e;
    }

    /// <summary>The rotation part as a unit quaternion (the basis is orthonormalized first).</summary>
    public readonly Quaternion GetRotationQuaternion()
    {
        Basis m = Orthonormalized();
        if (m.Determinant() < 0f) m = m.Scaled(new Vector3(-1, -1, -1));
        return m.ToQuaternionUnchecked();
    }

    /// <summary>The quaternion of an already orthonormal, right-handed basis (Godot's Quaternion(Basis)).</summary>
    internal readonly Quaternion ToQuaternionUnchecked()
    {
        float trace = Row0.X + Row1.Y + Row2.Z;
        float x, y, z, w;
        if (trace > 0f)
        {
            float s = MathF.Sqrt(trace + 1f) * 2f, inv = 1f / s;
            x = (Row2.Y - Row1.Z) * inv; y = (Row0.Z - Row2.X) * inv; z = (Row1.X - Row0.Y) * inv; w = s * 0.25f;
        }
        else if (Row0.X > Row1.Y && Row0.X > Row2.Z)
        {
            float s = MathF.Sqrt(Row0.X - Row1.Y - Row2.Z + 1f) * 2f, inv = 1f / s;
            x = s * 0.25f; y = (Row1.X + Row0.Y) * inv; z = (Row0.Z + Row2.X) * inv; w = (Row2.Y - Row1.Z) * inv;
        }
        else if (Row1.Y > Row2.Z)
        {
            float s = MathF.Sqrt(Row1.Y - Row0.X - Row2.Z + 1f) * 2f, inv = 1f / s;
            x = (Row1.X + Row0.Y) * inv; y = s * 0.25f; z = (Row2.Y + Row1.Z) * inv; w = (Row0.Z - Row2.X) * inv;
        }
        else
        {
            float s = MathF.Sqrt(Row2.Z - Row0.X - Row1.Y + 1f) * 2f, inv = 1f / s;
            x = (Row0.Z + Row2.X) * inv; y = (Row2.Y + Row1.Z) * inv; z = s * 0.25f; w = (Row1.X - Row0.Y) * inv;
        }
        return new Quaternion(x, y, z, w);
    }

    /// <summary>The per-axis scale (negative when the basis is mirrored).</summary>
    public readonly Vector3 GetScale()
    {
        float detSign = Mathf.Sign(Determinant());
        if (detSign == 0) detSign = 1;
        return detSign * new Vector3(Column0.Length(), Column1.Length(), Column2.Length());
    }

    public readonly Basis Inverse()
    {
        float c00 = Row1.Y * Row2.Z - Row1.Z * Row2.Y;
        float c01 = Row0.Z * Row2.Y - Row0.Y * Row2.Z;
        float c02 = Row0.Y * Row1.Z - Row0.Z * Row1.Y;
        float det = Row0.X * c00 + Row1.X * c01 + Row2.X * c02;
        if (det == 0f) throw new InvalidOperationException("Matrix determinant is zero and cannot be inverted.");
        float s = 1f / det;
        return new Basis(
            new Vector3(c00 * s, c01 * s, c02 * s),
            new Vector3((Row1.Z * Row2.X - Row1.X * Row2.Z) * s, (Row0.X * Row2.Z - Row0.Z * Row2.X) * s, (Row0.Z * Row1.X - Row0.X * Row1.Z) * s),
            new Vector3((Row1.X * Row2.Y - Row1.Y * Row2.X) * s, (Row0.Y * Row2.X - Row0.X * Row2.Y) * s, (Row0.X * Row1.Y - Row0.Y * Row1.X) * s),
            true);
    }

    public readonly bool IsConformal()
    {
        Vector3 x = Column0, y = Column1, z = Column2;
        float xLen = x.LengthSquared();
        return Mathf.IsEqualApprox(xLen, y.LengthSquared()) && Mathf.IsEqualApprox(xLen, z.LengthSquared())
            && Mathf.IsZeroApprox(x.Dot(y)) && Mathf.IsZeroApprox(x.Dot(z)) && Mathf.IsZeroApprox(y.Dot(z));
    }

    public readonly bool IsEqualApprox(Basis other) => Row0.IsEqualApprox(other.Row0) && Row1.IsEqualApprox(other.Row1) && Row2.IsEqualApprox(other.Row2);
    public readonly bool IsFinite() => Row0.IsFinite() && Row1.IsFinite() && Row2.IsFinite();

    /// <summary>The rotation looking along <paramref name="target"/> (a direction, not a point) with the given up; -Z is forward unless <paramref name="useModelFront"/>.</summary>
    public static Basis LookingAt(Vector3 target, Vector3? up = null, bool useModelFront = false)
    {
        Vector3 upv = up ?? Vector3.Up;
        Vector3 vz = target.Normalized();
        if (!useModelFront) vz = -vz;
        Vector3 vx = upv.Cross(vz);
        if (vx.IsZeroApprox()) throw new ArgumentException("The target vector and up vector can't be zero or parallel to each other.");
        vx = vx.Normalized();
        Vector3 vy = vz.Cross(vx);
        return new Basis(vx, vy, vz);
    }

    public readonly Basis Orthonormalized()
    {
        Vector3 x = Column0, y = Column1, z = Column2;
        x = x.Normalized();
        y = (y - x * x.Dot(y)).Normalized();
        z = (z - x * x.Dot(z) - y * y.Dot(z)).Normalized();
        return new Basis(x, y, z);
    }

    /// <summary>This rotated about a global axis: Basis(axis, angle) * this.</summary>
    public readonly Basis Rotated(Vector3 axis, float angle) => new Basis(axis, angle) * this;

    /// <summary>This scaled along the global axes (rows scaled).</summary>
    public readonly Basis Scaled(Vector3 scale) =>
        new(new Vector3(Row0.X * scale.X, Row0.Y * scale.X, Row0.Z * scale.X), new Vector3(Row1.X * scale.Y, Row1.Y * scale.Y, Row1.Z * scale.Y),
            new Vector3(Row2.X * scale.Z, Row2.Y * scale.Z, Row2.Z * scale.Z), true);

    /// <summary>This scaled along its own axes (columns scaled).</summary>
    public readonly Basis ScaledLocal(Vector3 scale) => this * FromScale(scale);

    public readonly Basis Slerp(Basis target, float weight)
    {
        Quaternion from = new(this), to = new(target);
        Basis b = new(from.Slerp(to, weight));
        Vector3 sc = new(Mathf.Lerp(Column0.Length(), target.Column0.Length(), weight), Mathf.Lerp(Column1.Length(), target.Column1.Length(), weight),
                         Mathf.Lerp(Column2.Length(), target.Column2.Length(), weight));
        return b.ScaledLocal(sc);
    }

    public readonly float Tdotx(Vector3 with) => Row0.X * with.X + Row1.X * with.Y + Row2.X * with.Z;
    public readonly float Tdoty(Vector3 with) => Row0.Y * with.X + Row1.Y * with.Y + Row2.Y * with.Z;
    public readonly float Tdotz(Vector3 with) => Row0.Z * with.X + Row1.Z * with.Y + Row2.Z * with.Z;

    public readonly Basis Transposed() => new(Row0, Row1, Row2);   // the columns of the transpose are this basis's rows

    public static Basis operator *(Basis l, Basis r) => new(
        new Vector3(r.Tdotx(l.Row0), r.Tdoty(l.Row0), r.Tdotz(l.Row0)),
        new Vector3(r.Tdotx(l.Row1), r.Tdoty(l.Row1), r.Tdotz(l.Row1)),
        new Vector3(r.Tdotx(l.Row2), r.Tdoty(l.Row2), r.Tdotz(l.Row2)), true);
    public static Basis operator +(Basis l, Basis r) => new(l.Row0 + r.Row0, l.Row1 + r.Row1, l.Row2 + r.Row2, true);
    public static Basis operator -(Basis l, Basis r) => new(l.Row0 - r.Row0, l.Row1 - r.Row1, l.Row2 - r.Row2, true);
    public static Basis operator *(Basis b, float s) => new(b.Row0 * s, b.Row1 * s, b.Row2 * s, true);
    public static Basis operator /(Basis b, float s) => new(b.Row0 / s, b.Row1 / s, b.Row2 / s, true);
    public static Vector3 operator *(Basis b, Vector3 v) => new(b.Row0.Dot(v), b.Row1.Dot(v), b.Row2.Dot(v));
    public static Vector3 operator *(Vector3 v, Basis b) => new(b.Tdotx(v), b.Tdoty(v), b.Tdotz(v));
    public static bool operator ==(Basis l, Basis r) => l.Equals(r);
    public static bool operator !=(Basis l, Basis r) => !l.Equals(r);
    public static explicit operator Quaternion(Basis b) => b.GetRotationQuaternion();

    public readonly bool Equals(Basis other) => Row0 == other.Row0 && Row1 == other.Row1 && Row2 == other.Row2;
    public override readonly bool Equals(object? obj) => obj is Basis b && Equals(b);
    public override readonly int GetHashCode() => HashCode.Combine(Row0, Row1, Row2);
    public override readonly string ToString() => string.Create(CultureInfo.InvariantCulture, $"[X: {Column0}, Y: {Column1}, Z: {Column2}]");
}
