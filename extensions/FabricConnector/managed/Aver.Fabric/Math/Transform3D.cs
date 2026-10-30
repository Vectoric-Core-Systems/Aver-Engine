// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Transform3D, Plane and Aabb with Godot's members and operators.

using System.Globalization;

namespace Aver.Fabric;

/// <summary>A 3D affine transform: a <see cref="Basis"/> and an origin (Godot's Transform3D).</summary>
public struct Transform3D : IEquatable<Transform3D>
{
    public Basis Basis;
    public Vector3 Origin;

    public Transform3D(Vector3 column0, Vector3 column1, Vector3 column2, Vector3 origin)
    {
        Basis = new Basis(column0, column1, column2);
        Origin = origin;
    }

    public Transform3D(Basis basis, Vector3 origin) { Basis = basis; Origin = origin; }

    public static Transform3D Identity => new(Basis.Identity, Vector3.Zero);
    public static Transform3D FlipX => new(Basis.FlipX, Vector3.Zero);
    public static Transform3D FlipY => new(Basis.FlipY, Vector3.Zero);
    public static Transform3D FlipZ => new(Basis.FlipZ, Vector3.Zero);

    public readonly Transform3D AffineInverse()
    {
        Basis inv = Basis.Inverse();
        return new Transform3D(inv, inv * -Origin);
    }

    public readonly Transform3D Inverse()
    {
        Basis t = Basis.Transposed();
        return new Transform3D(t, t * -Origin);
    }

    public readonly Transform3D InterpolateWith(Transform3D transform, float weight)
    {
        Vector3 srcScale = Basis.GetScale(), dstScale = transform.Basis.GetScale();
        Quaternion srcRot = Basis.GetRotationQuaternion(), dstRot = transform.Basis.GetRotationQuaternion();
        Quaternion rot = srcRot.Slerp(dstRot, weight);
        Vector3 scale = srcScale.Lerp(dstScale, weight);
        Vector3 loc = Origin.Lerp(transform.Origin, weight);
        return new Transform3D(Basis.FromQuaternionScale(rot, scale), loc);
    }

    public readonly bool IsEqualApprox(Transform3D other) => Basis.IsEqualApprox(other.Basis) && Origin.IsEqualApprox(other.Origin);
    public readonly bool IsFinite() => Basis.IsFinite() && Origin.IsFinite();

    /// <summary>This transform turned to look at a point (global to the parent), keeping its origin; -Z faces the target.</summary>
    public readonly Transform3D LookingAt(Vector3 target, Vector3? up = null, bool useModelFront = false) =>
        new(Basis.LookingAt(target - Origin, up, useModelFront), Origin);

    public readonly Transform3D Orthonormalized() => new(Basis.Orthonormalized(), Origin);
    public readonly Transform3D Rotated(Vector3 axis, float angle) => new Transform3D(new Basis(axis, angle), Vector3.Zero) * this;
    public readonly Transform3D RotatedLocal(Vector3 axis, float angle) => new(Basis * new Basis(axis, angle), Origin);
    public readonly Transform3D Scaled(Vector3 scale) => new(Basis.Scaled(scale), Origin * scale);
    public readonly Transform3D ScaledLocal(Vector3 scale) => new(Basis.ScaledLocal(scale), Origin);
    public readonly Transform3D Translated(Vector3 offset) => new(Basis, Origin + offset);
    public readonly Transform3D TranslatedLocal(Vector3 offset) => new(Basis, Origin + Basis * offset);

    public static Transform3D operator *(Transform3D l, Transform3D r) => new(l.Basis * r.Basis, l * r.Origin);
    public static Vector3 operator *(Transform3D t, Vector3 v) => t.Basis * v + t.Origin;
    /// <summary>The inverse transform of a point, for orthonormal transforms.</summary>
    public static Vector3 operator *(Vector3 v, Transform3D t) => (v - t.Origin) * t.Basis;
    public static Aabb operator *(Transform3D t, Aabb box)
    {
        Vector3 min = box.Position, max = box.Position + box.Size;
        Vector3 tmin = t.Origin, tmax = t.Origin;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
            {
                float e = t.Basis[j, i] * min[j], f = t.Basis[j, i] * max[j];
                if (e < f) { tmin[i] += e; tmax[i] += f; }
                else { tmin[i] += f; tmax[i] += e; }
            }
        return new Aabb(tmin, tmax - tmin);
    }
    public static Plane operator *(Transform3D t, Plane p)
    {
        Vector3 point = p.Normal * p.D;
        Vector3 pointDir = point + p.Normal;
        point = t * point;
        pointDir = t * pointDir;
        Vector3 normal = (pointDir - point).Normalized();
        return new Plane(normal, normal.Dot(point));
    }
    public static bool operator ==(Transform3D l, Transform3D r) => l.Equals(r);
    public static bool operator !=(Transform3D l, Transform3D r) => !l.Equals(r);

    public readonly bool Equals(Transform3D other) => Basis == other.Basis && Origin == other.Origin;
    public override readonly bool Equals(object? obj) => obj is Transform3D t && Equals(t);
    public override readonly int GetHashCode() => HashCode.Combine(Basis, Origin);
    public override readonly string ToString() => string.Create(CultureInfo.InvariantCulture, $"[X: {Basis.Column0}, Y: {Basis.Column1}, Z: {Basis.Column2}, O: {Origin}]");
}

/// <summary>A plane: all points with <c>Normal.Dot(p) == D</c> (Godot's Plane).</summary>
public struct Plane : IEquatable<Plane>
{
    public Vector3 Normal;
    public float D;

    public float X { readonly get => Normal.X; set => Normal.X = value; }
    public float Y { readonly get => Normal.Y; set => Normal.Y = value; }
    public float Z { readonly get => Normal.Z; set => Normal.Z = value; }

    public Plane(float a, float b, float c, float d) { Normal = new Vector3(a, b, c); D = d; }
    public Plane(Vector3 normal) { Normal = normal; D = 0f; }
    public Plane(Vector3 normal, float d) { Normal = normal; D = d; }
    public Plane(Vector3 normal, Vector3 point) { Normal = normal; D = normal.Dot(point); }

    /// <summary>The plane through three points, clockwise as seen from the normal's side.</summary>
    public Plane(Vector3 v1, Vector3 v2, Vector3 v3)
    {
        Normal = (v1 - v3).Cross(v1 - v2).Normalized();
        D = Normal.Dot(v1);
    }

    public static Plane PlaneYZ => new(1, 0, 0, 0);
    public static Plane PlaneXZ => new(0, 1, 0, 0);
    public static Plane PlaneXY => new(0, 0, 1, 0);

    public readonly Vector3 Center() => Normal * D;
    public readonly float DistanceTo(Vector3 point) => Normal.Dot(point) - D;
    public readonly bool HasPoint(Vector3 point, float tolerance = 1e-05f) => Math.Abs(Normal.Dot(point) - D) <= tolerance;
    public readonly bool IsEqualApprox(Plane other) => Normal.IsEqualApprox(other.Normal) && Mathf.IsEqualApprox(D, other.D);
    public readonly bool IsFinite() => Normal.IsFinite() && float.IsFinite(D);
    public readonly bool IsPointOver(Vector3 point) => Normal.Dot(point) > D;

    public readonly Plane Normalized()
    {
        float l = Normal.Length();
        return l == 0f ? new Plane(0, 0, 0, 0) : new Plane(Normal / l, D / l);
    }

    public readonly Vector3 Project(Vector3 point) => point - Normal * DistanceTo(point);

    /// <summary>The point where a ray meets the plane, or null.</summary>
    public readonly Vector3? IntersectsRay(Vector3 from, Vector3 dir)
    {
        float den = Normal.Dot(dir);
        if (Mathf.IsZeroApprox(den)) return null;
        float dist = (Normal.Dot(from) - D) / den;
        return dist > 1e-05f ? null : from + dir * -dist;
    }

    public readonly Vector3? IntersectsSegment(Vector3 begin, Vector3 end)
    {
        Vector3 segment = begin - end;
        float den = Normal.Dot(segment);
        if (Mathf.IsZeroApprox(den)) return null;
        float dist = (Normal.Dot(begin) - D) / den;
        return dist < -1e-05f || dist > 1f + 1e-05f ? null : begin + segment * -dist;
    }

    /// <summary>The single point where three planes meet, or null.</summary>
    public readonly Vector3? Intersect3(Plane b, Plane c)
    {
        float denom = Normal.Cross(b.Normal).Dot(c.Normal);
        if (Mathf.IsZeroApprox(denom)) return null;
        Vector3 r = b.Normal.Cross(c.Normal) * D + c.Normal.Cross(Normal) * b.D + Normal.Cross(b.Normal) * c.D;
        return r / denom;
    }

    public static Plane operator -(Plane p) => new(-p.Normal, -p.D);
    public static bool operator ==(Plane l, Plane r) => l.Equals(r);
    public static bool operator !=(Plane l, Plane r) => !l.Equals(r);

    public readonly bool Equals(Plane other) => Normal == other.Normal && D == other.D;
    public override readonly bool Equals(object? obj) => obj is Plane p && Equals(p);
    public override readonly int GetHashCode() => HashCode.Combine(Normal, D);
    public override readonly string ToString() => string.Create(CultureInfo.InvariantCulture, $"{Normal}, {D}");
}

/// <summary>An axis-aligned box: a position and a size (Godot's Aabb).</summary>
public struct Aabb : IEquatable<Aabb>
{
    public Vector3 Position;
    public Vector3 Size;

    public Vector3 End { readonly get => Position + Size; set => Size = value - Position; }

    public Aabb(Vector3 position, Vector3 size) { Position = position; Size = size; }

    public readonly Aabb Abs()
    {
        Vector3 end = End;
        Vector3 min = new(Math.Min(Position.X, end.X), Math.Min(Position.Y, end.Y), Math.Min(Position.Z, end.Z));
        return new Aabb(min, Size.Abs());
    }

    public readonly bool Encloses(Aabb with)
    {
        Vector3 srcMin = Position, srcMax = Position + Size, dstMin = with.Position, dstMax = with.Position + with.Size;
        return srcMin.X <= dstMin.X && srcMax.X >= dstMax.X && srcMin.Y <= dstMin.Y && srcMax.Y >= dstMax.Y && srcMin.Z <= dstMin.Z && srcMax.Z >= dstMax.Z;
    }

    public readonly Aabb Expand(Vector3 point)
    {
        Vector3 begin = Position, end = Position + Size;
        if (point.X < begin.X) begin.X = point.X;
        if (point.Y < begin.Y) begin.Y = point.Y;
        if (point.Z < begin.Z) begin.Z = point.Z;
        if (point.X > end.X) end.X = point.X;
        if (point.Y > end.Y) end.Y = point.Y;
        if (point.Z > end.Z) end.Z = point.Z;
        return new Aabb(begin, end - begin);
    }

    public readonly Vector3 GetCenter() => Position + Size * 0.5f;
    public readonly float GetVolume() => Size.X * Size.Y * Size.Z;
    public readonly Vector3 GetEndpoint(int idx) => idx switch
    {
        0 => new Vector3(Position.X, Position.Y, Position.Z),
        1 => new Vector3(Position.X, Position.Y, Position.Z + Size.Z),
        2 => new Vector3(Position.X, Position.Y + Size.Y, Position.Z),
        3 => new Vector3(Position.X, Position.Y + Size.Y, Position.Z + Size.Z),
        4 => new Vector3(Position.X + Size.X, Position.Y, Position.Z),
        5 => new Vector3(Position.X + Size.X, Position.Y, Position.Z + Size.Z),
        6 => new Vector3(Position.X + Size.X, Position.Y + Size.Y, Position.Z),
        7 => new Vector3(Position.X + Size.X, Position.Y + Size.Y, Position.Z + Size.Z),
        _ => throw new ArgumentOutOfRangeException(nameof(idx)),
    };
    public readonly Vector3 GetLongestAxis() => Size.MaxAxisIndex() switch { Vector3.Axis.X => Vector3.Right, Vector3.Axis.Y => Vector3.Up, _ => Vector3.Back };
    public readonly Vector3.Axis GetLongestAxisIndex() => Size.MaxAxisIndex();
    public readonly float GetLongestAxisSize() => Math.Max(Size.X, Math.Max(Size.Y, Size.Z));
    public readonly Vector3 GetShortestAxis() => Size.MinAxisIndex() switch { Vector3.Axis.X => Vector3.Right, Vector3.Axis.Y => Vector3.Up, _ => Vector3.Back };
    public readonly Vector3.Axis GetShortestAxisIndex() => Size.MinAxisIndex();
    public readonly float GetShortestAxisSize() => Math.Min(Size.X, Math.Min(Size.Y, Size.Z));
    public readonly Vector3 GetSupport(Vector3 dir)
    {
        Vector3 half = Size * 0.5f, ofs = Position + half;
        return new Vector3(dir.X > 0f ? -half.X : half.X, dir.Y > 0f ? -half.Y : half.Y, dir.Z > 0f ? -half.Z : half.Z) + ofs;
    }

    public readonly Aabb Grow(float by)
    {
        Aabb g = this;
        g.Position = new Vector3(Position.X - by, Position.Y - by, Position.Z - by);
        g.Size = new Vector3(Size.X + 2 * by, Size.Y + 2 * by, Size.Z + 2 * by);
        return g;
    }

    public readonly bool HasPoint(Vector3 point)
    {
        if (point.X < Position.X || point.Y < Position.Y || point.Z < Position.Z) return false;
        if (point.X > Position.X + Size.X || point.Y > Position.Y + Size.Y || point.Z > Position.Z + Size.Z) return false;
        return true;
    }

    public readonly bool HasSurface() => Size.X > 0 || Size.Y > 0 || Size.Z > 0;
    public readonly bool HasVolume() => Size.X > 0 && Size.Y > 0 && Size.Z > 0;

    public readonly Aabb Intersection(Aabb with)
    {
        Vector3 srcMin = Position, srcMax = Position + Size, dstMin = with.Position, dstMax = with.Position + with.Size;
        Vector3 min, max;
        if (srcMin.X > dstMax.X || srcMax.X < dstMin.X) return default;
        min.X = srcMin.X > dstMin.X ? srcMin.X : dstMin.X;
        max.X = srcMax.X < dstMax.X ? srcMax.X : dstMax.X;
        if (srcMin.Y > dstMax.Y || srcMax.Y < dstMin.Y) return default;
        min.Y = srcMin.Y > dstMin.Y ? srcMin.Y : dstMin.Y;
        max.Y = srcMax.Y < dstMax.Y ? srcMax.Y : dstMax.Y;
        if (srcMin.Z > dstMax.Z || srcMax.Z < dstMin.Z) return default;
        min.Z = srcMin.Z > dstMin.Z ? srcMin.Z : dstMin.Z;
        max.Z = srcMax.Z < dstMax.Z ? srcMax.Z : dstMax.Z;
        return new Aabb(min, max - min);
    }

    public readonly bool Intersects(Aabb with)
    {
        if (Position.X >= with.Position.X + with.Size.X) return false;
        if (Position.X + Size.X <= with.Position.X) return false;
        if (Position.Y >= with.Position.Y + with.Size.Y) return false;
        if (Position.Y + Size.Y <= with.Position.Y) return false;
        if (Position.Z >= with.Position.Z + with.Size.Z) return false;
        if (Position.Z + Size.Z <= with.Position.Z) return false;
        return true;
    }

    public readonly bool IntersectsPlane(Plane plane)
    {
        bool over = false, under = false;
        for (int i = 0; i < 8; i++)
        {
            if (plane.DistanceTo(GetEndpoint(i)) > 0) over = true; else under = true;
        }
        return under && over;
    }

    public readonly Aabb Merge(Aabb with)
    {
        Vector3 beg1 = Position, beg2 = with.Position, end1 = Position + Size, end2 = with.Position + with.Size;
        Vector3 min = new(Math.Min(beg1.X, beg2.X), Math.Min(beg1.Y, beg2.Y), Math.Min(beg1.Z, beg2.Z));
        Vector3 max = new(Math.Max(end1.X, end2.X), Math.Max(end1.Y, end2.Y), Math.Max(end1.Z, end2.Z));
        return new Aabb(min, max - min);
    }

    public readonly bool IsEqualApprox(Aabb other) => Position.IsEqualApprox(other.Position) && Size.IsEqualApprox(other.Size);
    public readonly bool IsFinite() => Position.IsFinite() && Size.IsFinite();

    public static bool operator ==(Aabb l, Aabb r) => l.Equals(r);
    public static bool operator !=(Aabb l, Aabb r) => !l.Equals(r);

    public readonly bool Equals(Aabb other) => Position == other.Position && Size == other.Size;
    public override readonly bool Equals(object? obj) => obj is Aabb a && Equals(a);
    public override readonly int GetHashCode() => HashCode.Combine(Position, Size);
    public override readonly string ToString() => $"{Position}, {Size}";
}
