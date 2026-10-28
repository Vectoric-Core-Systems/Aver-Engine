// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Transform2D, Rect2 and Rect2I with Godot's members. 2D is not converted yet; scripts still use the types.

using System.Globalization;

namespace Aver.Fabric;

/// <summary>A 2D affine transform by columns X, Y and Origin (Godot's Transform2D).</summary>
public struct Transform2D : IEquatable<Transform2D>
{
    public Vector2 X;
    public Vector2 Y;
    public Vector2 Origin;

    public Transform2D(Vector2 xAxis, Vector2 yAxis, Vector2 origin) { X = xAxis; Y = yAxis; Origin = origin; }

    public Transform2D(float rotation, Vector2 position)
    {
        float c = MathF.Cos(rotation), s = MathF.Sin(rotation);
        X = new Vector2(c, s); Y = new Vector2(-s, c); Origin = position;
    }

    public Transform2D(float rotation, Vector2 scale, float skew, Vector2 position)
    {
        float c = MathF.Cos(rotation), s = MathF.Sin(rotation);
        X = new Vector2(c * scale.X, s * scale.X);
        Y = new Vector2(-s * scale.Y + c * MathF.Tan(skew) * scale.Y, c * scale.Y + s * MathF.Tan(skew) * scale.Y);
        Origin = position;
    }

    public static Transform2D Identity => new(new Vector2(1, 0), new Vector2(0, 1), Vector2.Zero);
    public static Transform2D FlipX => new(new Vector2(-1, 0), new Vector2(0, 1), Vector2.Zero);
    public static Transform2D FlipY => new(new Vector2(1, 0), new Vector2(0, -1), Vector2.Zero);

    public readonly float Determinant() => X.X * Y.Y - X.Y * Y.X;

    public readonly Transform2D AffineInverse()
    {
        float det = Determinant();
        if (det == 0f) throw new InvalidOperationException("Matrix determinant is zero and cannot be inverted.");
        float idet = 1f / det;
        Transform2D t = new(new Vector2(Y.Y * idet, -X.Y * idet), new Vector2(-Y.X * idet, X.X * idet), Vector2.Zero);
        t.Origin = t.BasisXform(-Origin);
        return t;
    }

    public readonly Transform2D Inverse()
    {
        Transform2D t = new(new Vector2(X.X, Y.X), new Vector2(X.Y, Y.Y), Vector2.Zero);
        t.Origin = t.BasisXform(-Origin);
        return t;
    }

    public readonly Vector2 BasisXform(Vector2 v) => new(X.X * v.X + Y.X * v.Y, X.Y * v.X + Y.Y * v.Y);
    public readonly Vector2 BasisXformInv(Vector2 v) => new(X.Dot(v), Y.Dot(v));
    public readonly float GetRotation() => MathF.Atan2(X.Y, X.X);
    public readonly Vector2 GetScale()
    {
        float detSign = Determinant() < 0f ? -1f : 1f;
        return new Vector2(X.Length(), detSign * Y.Length());
    }
    public readonly float GetSkew()
    {
        float det = Determinant();
        return MathF.Acos(X.Normalized().Dot(Y.Normalized() * (det < 0f ? -1f : 1f))) - MathF.PI * 0.5f;
    }

    public readonly Transform2D InterpolateWith(Transform2D m, float weight)
    {
        float r1 = GetRotation(), r2 = m.GetRotation();
        Vector2 s1 = GetScale(), s2 = m.GetScale();
        // Interpolate the rotation as a quaternion would: along the shorter arc.
        Vector2 v1 = new Vector2(MathF.Cos(r1), MathF.Sin(r1));
        Vector2 v2 = new Vector2(MathF.Cos(r2), MathF.Sin(r2));
        float dot = Math.Clamp(v1.Dot(v2), -1f, 1f);
        Vector2 v;
        if (dot > 0.9995f) v = v1.Lerp(v2, weight).Normalized();
        else
        {
            float angle = weight * MathF.Acos(dot);
            Vector2 v3 = (v2 - v1 * dot).Normalized();
            v = v1 * MathF.Cos(angle) + v3 * MathF.Sin(angle);
        }
        Vector2 scale = s1.Lerp(s2, weight);
        Vector2 origin = Origin.Lerp(m.Origin, weight);
        return new Transform2D(new Vector2(v.X * scale.X, v.Y * scale.X), new Vector2(-v.Y * scale.Y, v.X * scale.Y), origin);
    }

    public readonly bool IsEqualApprox(Transform2D other) => X.IsEqualApprox(other.X) && Y.IsEqualApprox(other.Y) && Origin.IsEqualApprox(other.Origin);
    public readonly bool IsFinite() => X.IsFinite() && Y.IsFinite() && Origin.IsFinite();

    public readonly Transform2D Orthonormalized()
    {
        Vector2 x = X, y = Y;
        x = x.Normalized();
        y = (y - x * x.Dot(y)).Normalized();
        return new Transform2D(x, y, Origin);
    }

    public readonly Transform2D Rotated(float angle) => new Transform2D(angle, Vector2.Zero) * this;
    public readonly Transform2D RotatedLocal(float angle) => this * new Transform2D(angle, Vector2.Zero);
    public readonly Transform2D Scaled(Vector2 scale) => new(X * scale, Y * scale, Origin * scale);
    public readonly Transform2D ScaledLocal(Vector2 scale) => new(X * scale.X, Y * scale.Y, Origin);
    public readonly Transform2D Translated(Vector2 offset) => new(X, Y, Origin + offset);
    public readonly Transform2D TranslatedLocal(Vector2 offset) => new(X, Y, Origin + BasisXform(offset));

    public static Transform2D operator *(Transform2D l, Transform2D r)
    {
        Vector2 origin = l * r.Origin;
        return new Transform2D(l.BasisXform(r.X), l.BasisXform(r.Y), origin);
    }
    public static Vector2 operator *(Transform2D t, Vector2 v) => t.BasisXform(v) + t.Origin;
    public static Vector2 operator *(Vector2 v, Transform2D t) => t.BasisXformInv(v - t.Origin);
    public static Rect2 operator *(Transform2D t, Rect2 r)
    {
        Vector2 a = t * r.Position, b = t * new Vector2(r.Position.X + r.Size.X, r.Position.Y);
        Vector2 c = t * new Vector2(r.Position.X, r.Position.Y + r.Size.Y), d = t * (r.Position + r.Size);
        Vector2 min = new(Math.Min(Math.Min(a.X, b.X), Math.Min(c.X, d.X)), Math.Min(Math.Min(a.Y, b.Y), Math.Min(c.Y, d.Y)));
        Vector2 max = new(Math.Max(Math.Max(a.X, b.X), Math.Max(c.X, d.X)), Math.Max(Math.Max(a.Y, b.Y), Math.Max(c.Y, d.Y)));
        return new Rect2(min, max - min);
    }
    public static bool operator ==(Transform2D l, Transform2D r) => l.Equals(r);
    public static bool operator !=(Transform2D l, Transform2D r) => !l.Equals(r);

    public readonly bool Equals(Transform2D other) => X == other.X && Y == other.Y && Origin == other.Origin;
    public override readonly bool Equals(object? obj) => obj is Transform2D t && Equals(t);
    public override readonly int GetHashCode() => HashCode.Combine(X, Y, Origin);
    public override readonly string ToString() => string.Create(CultureInfo.InvariantCulture, $"[X: {X}, Y: {Y}, O: {Origin}]");
}

/// <summary>A 2D rectangle of floats (Godot's Rect2).</summary>
public struct Rect2 : IEquatable<Rect2>
{
    public Vector2 Position;
    public Vector2 Size;

    public Vector2 End { readonly get => Position + Size; set => Size = value - Position; }

    public Rect2(Vector2 position, Vector2 size) { Position = position; Size = size; }
    public Rect2(Vector2 position, float width, float height) { Position = position; Size = new Vector2(width, height); }
    public Rect2(float x, float y, Vector2 size) { Position = new Vector2(x, y); Size = size; }
    public Rect2(float x, float y, float width, float height) { Position = new Vector2(x, y); Size = new Vector2(width, height); }

    public readonly Rect2 Abs() => new(new Vector2(Position.X + Math.Min(Size.X, 0), Position.Y + Math.Min(Size.Y, 0)), Size.Abs());
    public readonly bool Encloses(Rect2 b) =>
        b.Position.X >= Position.X && b.Position.Y >= Position.Y && b.Position.X + b.Size.X <= Position.X + Size.X && b.Position.Y + b.Size.Y <= Position.Y + Size.Y;
    public readonly Rect2 Expand(Vector2 to)
    {
        Vector2 begin = Position, end = Position + Size;
        if (to.X < begin.X) begin.X = to.X;
        if (to.Y < begin.Y) begin.Y = to.Y;
        if (to.X > end.X) end.X = to.X;
        if (to.Y > end.Y) end.Y = to.Y;
        return new Rect2(begin, end - begin);
    }
    public readonly float GetArea() => Size.X * Size.Y;
    public readonly Vector2 GetCenter() => Position + Size * 0.5f;
    public readonly Rect2 Grow(float by) => new(Position.X - by, Position.Y - by, Size.X + by * 2, Size.Y + by * 2);
    public readonly Rect2 GrowIndividual(float left, float top, float right, float bottom) =>
        new(Position.X - left, Position.Y - top, Size.X + left + right, Size.Y + top + bottom);
    public readonly bool HasArea() => Size.X > 0f && Size.Y > 0f;
    public readonly bool HasPoint(Vector2 point) =>
        point.X >= Position.X && point.Y >= Position.Y && point.X < Position.X + Size.X && point.Y < Position.Y + Size.Y;
    public readonly Rect2 Intersection(Rect2 b)
    {
        Rect2 newRect = b;
        if (!Intersects(newRect)) return default;
        newRect.Position = new Vector2(Math.Max(b.Position.X, Position.X), Math.Max(b.Position.Y, Position.Y));
        Vector2 bEnd = b.Position + b.Size, end = Position + Size;
        newRect.Size = new Vector2(Math.Min(bEnd.X, end.X), Math.Min(bEnd.Y, end.Y)) - newRect.Position;
        return newRect;
    }
    public readonly bool Intersects(Rect2 b, bool includeBorders = false)
    {
        if (includeBorders)
        {
            if (Position.X > b.Position.X + b.Size.X) return false;
            if (Position.X + Size.X < b.Position.X) return false;
            if (Position.Y > b.Position.Y + b.Size.Y) return false;
            if (Position.Y + Size.Y < b.Position.Y) return false;
        }
        else
        {
            if (Position.X >= b.Position.X + b.Size.X) return false;
            if (Position.X + Size.X <= b.Position.X) return false;
            if (Position.Y >= b.Position.Y + b.Size.Y) return false;
            if (Position.Y + Size.Y <= b.Position.Y) return false;
        }
        return true;
    }
    public readonly bool IsEqualApprox(Rect2 other) => Position.IsEqualApprox(other.Position) && Size.IsEqualApprox(other.Size);
    public readonly bool IsFinite() => Position.IsFinite() && Size.IsFinite();
    public readonly Rect2 Merge(Rect2 b)
    {
        Vector2 min = new(Math.Min(b.Position.X, Position.X), Math.Min(b.Position.Y, Position.Y));
        Vector2 max = new(Math.Max(b.Position.X + b.Size.X, Position.X + Size.X), Math.Max(b.Position.Y + b.Size.Y, Position.Y + Size.Y));
        return new Rect2(min, max - min);
    }

    public static bool operator ==(Rect2 l, Rect2 r) => l.Equals(r);
    public static bool operator !=(Rect2 l, Rect2 r) => !l.Equals(r);
    public static explicit operator Rect2I(Rect2 r) => new((Vector2I)r.Position, (Vector2I)r.Size);

    public readonly bool Equals(Rect2 other) => Position == other.Position && Size == other.Size;
    public override readonly bool Equals(object? obj) => obj is Rect2 r && Equals(r);
    public override readonly int GetHashCode() => HashCode.Combine(Position, Size);
    public override readonly string ToString() => $"{Position}, {Size}";
}

/// <summary>A 2D rectangle of ints (Godot's Rect2I).</summary>
public struct Rect2I : IEquatable<Rect2I>
{
    public Vector2I Position;
    public Vector2I Size;

    public Vector2I End { readonly get => Position + Size; set => Size = value - Position; }

    public Rect2I(Vector2I position, Vector2I size) { Position = position; Size = size; }
    public Rect2I(int x, int y, int width, int height) { Position = new Vector2I(x, y); Size = new Vector2I(width, height); }

    public readonly Rect2I Abs() => new(new Vector2I(Position.X + Math.Min(Size.X, 0), Position.Y + Math.Min(Size.Y, 0)), Size.Abs());
    public readonly bool Encloses(Rect2I b) =>
        b.Position.X >= Position.X && b.Position.Y >= Position.Y && b.Position.X + b.Size.X <= Position.X + Size.X && b.Position.Y + b.Size.Y <= Position.Y + Size.Y;
    public readonly int GetArea() => Size.X * Size.Y;
    public readonly Vector2I GetCenter() => Position + Size / 2;
    public readonly Rect2I Grow(int by) => new(Position.X - by, Position.Y - by, Size.X + by * 2, Size.Y + by * 2);
    public readonly bool HasArea() => Size.X > 0 && Size.Y > 0;
    public readonly bool HasPoint(Vector2I point) =>
        point.X >= Position.X && point.Y >= Position.Y && point.X < Position.X + Size.X && point.Y < Position.Y + Size.Y;
    public readonly bool Intersects(Rect2I b) =>
        !(Position.X >= b.Position.X + b.Size.X || Position.X + Size.X <= b.Position.X || Position.Y >= b.Position.Y + b.Size.Y || Position.Y + Size.Y <= b.Position.Y);
    public readonly Rect2I Merge(Rect2I b)
    {
        Vector2I min = new(Math.Min(b.Position.X, Position.X), Math.Min(b.Position.Y, Position.Y));
        Vector2I max = new(Math.Max(b.Position.X + b.Size.X, Position.X + Size.X), Math.Max(b.Position.Y + b.Size.Y, Position.Y + Size.Y));
        return new Rect2I(min, max - min);
    }

    public static bool operator ==(Rect2I l, Rect2I r) => l.Equals(r);
    public static bool operator !=(Rect2I l, Rect2I r) => !l.Equals(r);
    public static implicit operator Rect2(Rect2I r) => new(r.Position, r.Size);

    public readonly bool Equals(Rect2I other) => Position == other.Position && Size == other.Size;
    public override readonly bool Equals(object? obj) => obj is Rect2I r && Equals(r);
    public override readonly int GetHashCode() => HashCode.Combine(Position, Size);
    public override readonly string ToString() => $"{Position}, {Size}";
}
