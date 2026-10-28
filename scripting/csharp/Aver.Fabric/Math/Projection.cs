// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Projection: a 4x4 matrix by columns (Godot's Projection), with the perspective/orthogonal/frustum builders.

namespace Aver.Fabric;

/// <summary>A 4x4 matrix stored as four columns, for camera projections (Godot's Projection).</summary>
public struct Projection : IEquatable<Projection>
{
    public Vector4 X;
    public Vector4 Y;
    public Vector4 Z;
    public Vector4 W;

    public Projection(Vector4 x, Vector4 y, Vector4 z, Vector4 w) { X = x; Y = y; Z = z; W = w; }

    /// <summary>The element in the given column and row.</summary>
    public float this[int column, int row]
    {
        readonly get => Column(column)[row];
        set
        {
            switch (column)
            {
                case 0: X[row] = value; break;
                case 1: Y[row] = value; break;
                case 2: Z[row] = value; break;
                case 3: W[row] = value; break;
                default: throw new ArgumentOutOfRangeException(nameof(column));
            }
        }
    }

    private readonly Vector4 Column(int c) => c switch { 0 => X, 1 => Y, 2 => Z, 3 => W, _ => throw new ArgumentOutOfRangeException(nameof(c)) };

    public static Projection Identity => new(new Vector4(1, 0, 0, 0), new Vector4(0, 1, 0, 0), new Vector4(0, 0, 1, 0), new Vector4(0, 0, 0, 1));
    public static Projection Zero => new(Vector4.Zero, Vector4.Zero, Vector4.Zero, Vector4.Zero);

    /// <summary>A perspective projection; the field of view is vertical, in degrees, unless <paramref name="flipFov"/>.</summary>
    public static Projection CreatePerspective(float fovyDegrees, float aspect, float zNear, float zFar, bool flipFov = false)
    {
        if (flipFov) fovyDegrees = GetFovy(fovyDegrees, 1f / aspect);
        float radians = Mathf.DegToRad(fovyDegrees / 2f), deltaZ = zFar - zNear, sine = MathF.Sin(radians);
        if (deltaZ == 0f || sine == 0f || aspect == 0f) return Zero;
        float cotangent = MathF.Cos(radians) / sine;
        Projection p = Identity;
        p[0, 0] = cotangent / aspect;
        p[1, 1] = cotangent;
        p[2, 2] = -(zFar + zNear) / deltaZ;
        p[2, 3] = -1f;
        p[3, 2] = -2f * zNear * zFar / deltaZ;
        p[3, 3] = 0f;
        return p;
    }

    public static Projection CreateOrthogonal(float left, float right, float bottom, float top, float zNear, float zFar)
    {
        Projection p = Identity;
        p[0, 0] = 2f / (right - left);
        p[3, 0] = -((right + left) / (right - left));
        p[1, 1] = 2f / (top - bottom);
        p[3, 1] = -((top + bottom) / (top - bottom));
        p[2, 2] = -2f / (zFar - zNear);
        p[3, 2] = -((zFar + zNear) / (zFar - zNear));
        p[3, 3] = 1f;
        return p;
    }

    public static Projection CreateFrustum(float left, float right, float bottom, float top, float zNear, float zFar)
    {
        Projection p = Zero;
        p[0, 0] = 2f * zNear / (right - left);
        p[1, 1] = 2f * zNear / (top - bottom);
        p[2, 0] = (right + left) / (right - left);
        p[2, 1] = (top + bottom) / (top - bottom);
        p[2, 2] = -(zFar + zNear) / (zFar - zNear);
        p[2, 3] = -1f;
        p[3, 2] = -2f * zFar * zNear / (zFar - zNear);
        return p;
    }

    /// <summary>The vertical field of view that gives a horizontal one of <paramref name="fovx"/> at <paramref name="aspect"/> (degrees).</summary>
    public static float GetFovy(float fovx, float aspect) => Mathf.RadToDeg(MathF.Atan(aspect * MathF.Tan(Mathf.DegToRad(fovx) * 0.5f)) * 2f);

    public readonly bool IsOrthogonal() => Z.W == 0f && W.W == 1f;
    /// <summary>The near and far planes of a perspective projection (an orthogonal one is not covered).</summary>
    public readonly float GetZNear() => W.Z / (Z.Z - 1f);
    public readonly float GetZFar() => W.Z / (Z.Z + 1f);
    public readonly float GetAspect() => Y.Y / X.X;
    public readonly float GetFov() => IsOrthogonal() ? 0f : Mathf.RadToDeg(MathF.Atan(1f / Y.Y) * 2f);

    public readonly Projection Flipped() => new(X, new Vector4(Y.X, -Y.Y, Y.Z, Y.W), new Vector4(Z.X, -Z.Y, Z.Z, Z.W), new Vector4(W.X, -W.Y, W.Z, W.W));

    public readonly float Determinant()
    {
        float[,] m = ToArray();
        return Det4(m);
    }

    private readonly float[,] ToArray()
    {
        float[,] m = new float[4, 4];
        for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) m[r, c] = this[c, r];
        return m;
    }

    private static float Det4(float[,] m)
    {
        float det = 0;
        for (int c = 0; c < 4; c++)
        {
            float[,] minor = new float[3, 3];
            for (int r = 1; r < 4; r++)
            {
                int cc = 0;
                for (int k = 0; k < 4; k++) { if (k == c) continue; minor[r - 1, cc++] = m[r, k]; }
            }
            float d3 = minor[0, 0] * (minor[1, 1] * minor[2, 2] - minor[1, 2] * minor[2, 1])
                     - minor[0, 1] * (minor[1, 0] * minor[2, 2] - minor[1, 2] * minor[2, 0])
                     + minor[0, 2] * (minor[1, 0] * minor[2, 1] - minor[1, 1] * minor[2, 0]);
            det += (c % 2 == 0 ? 1 : -1) * m[0, c] * d3;
        }
        return det;
    }

    /// <summary>The inverse by Gauss-Jordan elimination with partial pivoting.</summary>
    public readonly Projection Inverse()
    {
        float[,] a = ToArray();
        float[,] inv = new float[4, 4];
        for (int i = 0; i < 4; i++) inv[i, i] = 1f;
        for (int col = 0; col < 4; col++)
        {
            int pivot = col;
            for (int r = col + 1; r < 4; r++) if (Math.Abs(a[r, col]) > Math.Abs(a[pivot, col])) pivot = r;
            if (a[pivot, col] == 0f) throw new InvalidOperationException("Matrix determinant is zero and cannot be inverted.");
            if (pivot != col)
                for (int k = 0; k < 4; k++)
                {
                    (a[col, k], a[pivot, k]) = (a[pivot, k], a[col, k]);
                    (inv[col, k], inv[pivot, k]) = (inv[pivot, k], inv[col, k]);
                }
            float d = a[col, col];
            for (int k = 0; k < 4; k++) { a[col, k] /= d; inv[col, k] /= d; }
            for (int r = 0; r < 4; r++)
            {
                if (r == col) continue;
                float f = a[r, col];
                if (f == 0f) continue;
                for (int k = 0; k < 4; k++) { a[r, k] -= f * a[col, k]; inv[r, k] -= f * inv[col, k]; }
            }
        }
        Projection p = Zero;
        for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) p[c, r] = inv[r, c];
        return p;
    }

    public static Projection operator *(Projection l, Projection r)
    {
        Projection p = Zero;
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++)
            {
                float s = 0;
                for (int k = 0; k < 4; k++) s += l[k, row] * r[c, k];
                p[c, row] = s;
            }
        return p;
    }

    public static Vector4 operator *(Projection p, Vector4 v) => new(
        p.X.X * v.X + p.Y.X * v.Y + p.Z.X * v.Z + p.W.X * v.W, p.X.Y * v.X + p.Y.Y * v.Y + p.Z.Y * v.Z + p.W.Y * v.W,
        p.X.Z * v.X + p.Y.Z * v.Y + p.Z.Z * v.Z + p.W.Z * v.W, p.X.W * v.X + p.Y.W * v.Y + p.Z.W * v.Z + p.W.W * v.W);
    public static bool operator ==(Projection l, Projection r) => l.Equals(r);
    public static bool operator !=(Projection l, Projection r) => !l.Equals(r);

    public readonly bool Equals(Projection other) => X == other.X && Y == other.Y && Z == other.Z && W == other.W;
    public override readonly bool Equals(object? obj) => obj is Projection p && Equals(p);
    public override readonly int GetHashCode() => HashCode.Combine(X, Y, Z, W);
    public override readonly string ToString() => $"{X.X}, {X.Y}, {X.Z}, {X.W}\n{Y.X}, {Y.Y}, {Y.Z}, {Y.W}\n{Z.X}, {Z.Y}, {Z.Z}, {Z.W}\n{W.X}, {W.Y}, {W.Z}, {W.W}";
}
