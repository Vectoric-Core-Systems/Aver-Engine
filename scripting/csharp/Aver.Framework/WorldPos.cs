// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// A position that stays exact at any distance: a global chunk index plus a float offset inside the chunk (docs/plans/wave11/LARGE_WORLD.md
// 2.1, 2.5). The arithmetic is aver/world/WorldPos.hpp restated; a test pins the two against each other.

using Aver.Scene;

namespace Aver.Framework;

/// <summary>A double-precision vector in centimetres: an absolute position (or a distance between two) where a <see cref="Vec3"/> would
/// lose precision. Exact to 2^53 cm.</summary>
public readonly struct Double3 : IEquatable<Double3>
{
    /// <summary>X, centimetres.</summary>
    public readonly double X;
    /// <summary>Y, centimetres.</summary>
    public readonly double Y;
    /// <summary>Z, centimetres.</summary>
    public readonly double Z;

    /// <summary>Makes a vector from its three components.</summary>
    public Double3(double x, double y, double z) { X = x; Y = y; Z = z; }

    /// <summary>The zero vector.</summary>
    public static Double3 Zero => default;

    /// <summary>Componentwise sum.</summary>
    public static Double3 operator +(Double3 a, Double3 b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
    /// <summary>Componentwise difference.</summary>
    public static Double3 operator -(Double3 a, Double3 b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
    /// <summary>Scales by a number.</summary>
    public static Double3 operator *(Double3 a, double s) => new(a.X * s, a.Y * s, a.Z * s);
    /// <summary>Scales by a number.</summary>
    public static Double3 operator *(double s, Double3 a) => new(a.X * s, a.Y * s, a.Z * s);
    /// <summary>Divides by a number.</summary>
    public static Double3 operator /(Double3 a, double s) => new(a.X / s, a.Y / s, a.Z / s);
    /// <summary>Negation.</summary>
    public static Double3 operator -(Double3 a) => new(-a.X, -a.Y, -a.Z);
    /// <summary>Exact equality.</summary>
    public static bool operator ==(Double3 a, Double3 b) => a.Equals(b);
    /// <summary>Exact inequality.</summary>
    public static bool operator !=(Double3 a, Double3 b) => !a.Equals(b);

    /// <summary>Length in centimetres.</summary>
    public double Length => Math.Sqrt(X * X + Y * Y + Z * Z);

    /// <summary>The nearest <see cref="Vec3"/>: one rounding to single precision, so only meaningful for a vector near the origin.</summary>
    public Vec3 ToVec3() => new((float)X, (float)Y, (float)Z);

    /// <summary>Widens a <see cref="Vec3"/>; exact.</summary>
    public static Double3 From(Vec3 v) => new(v.X, v.Y, v.Z);

    /// <inheritdoc/>
    public bool Equals(Double3 o) => X == o.X && Y == o.Y && Z == o.Z;
    /// <inheritdoc/>
    public override bool Equals(object? obj) => obj is Double3 o && Equals(o);
    /// <inheritdoc/>
    public override int GetHashCode() => HashCode.Combine(X, Y, Z);
    /// <inheritdoc/>
    public override string ToString() => $"({X:R}, {Y:R}, {Z:R})";
}

/// <summary>A position in the whole world: a global chunk index (16 m chunks, three integers) and a float offset inside that chunk, in
/// [0, 1600) centimetres. 1.2 micrometres at any distance, where a bare float is 1 cm at 100 km.</summary>
/// <remarks>The scene, physics and every other API speak origin-relative floats (<see cref="Entity.WorldPosition"/>); this is what a
/// script stores when a position must outlive a floating-origin shift. Region 0 is centred on the world's zero; a region is
/// 1024 chunks (16.384 km) on a side. Convert with <see cref="WorldOrigin"/> or the members here.</remarks>
public readonly struct WorldPos : IEquatable<WorldPos>
{
    /// <summary>Chunk edge in centimetres.</summary>
    public const int ChunkSizeCm = 1600;
    /// <summary>Chunks along one region edge.</summary>
    public const int ChunksPerRegion = 1024;
    /// <summary>Region edge in centimetres: the floating origin moves in whole steps of this.</summary>
    public const long RegionSizeCm = (long)ChunksPerRegion * ChunkSizeCm;

    /// <summary>Global chunk index on X.</summary>
    public readonly int ChunkX;
    /// <summary>Global chunk index on Y.</summary>
    public readonly int ChunkY;
    /// <summary>Global chunk index on Z.</summary>
    public readonly int ChunkZ;
    /// <summary>Offset inside the chunk, centimetres, each component in [0, 1600).</summary>
    public readonly Vec3 Local;

    /// <summary>Makes a position from a chunk and an offset. The offset is not normalised; use <see cref="FromAbsolute"/> for that.</summary>
    public WorldPos(int chunkX, int chunkY, int chunkZ, Vec3 local) { ChunkX = chunkX; ChunkY = chunkY; ChunkZ = chunkZ; Local = local; }

    // One axis of the split with the invariant local in [0, 1600) enforced: the f64 division can land a value one chunk off, and
    // narrowing a remainder just under the chunk size can round up to it. Both are repaired, exactly like world::detail::splitAxis.
    private static void SplitAxis(double v, out int chunk, out float local)
    {
        const double s = ChunkSizeCm;
        double q = Math.Floor(v / s);
        double r = v - q * s;
        if (r < 0.0) { q -= 1.0; r += s; }
        if (r >= s) { q += 1.0; r -= s; }
        float l = (float)r;
        if (l >= (float)ChunkSizeCm) { q += 1.0; l = 0.0f; }
        chunk = (int)q;
        local = l;
    }

    /// <summary>The position of an absolute point, in centimetres from the world's zero.</summary>
    public static WorldPos FromAbsolute(Double3 cm)
    {
        SplitAxis(cm.X, out int cx, out float lx);
        SplitAxis(cm.Y, out int cy, out float ly);
        SplitAxis(cm.Z, out int cz, out float lz);
        return new WorldPos(cx, cy, cz, new Vec3(lx, ly, lz));
    }

    /// <summary>The absolute point this names, in centimetres from the world's zero. Exact to 2^53 cm.</summary>
    public Double3 ToAbsolute() => new((double)((long)ChunkX * ChunkSizeCm) + Local.X,
                                       (double)((long)ChunkY * ChunkSizeCm) + Local.Y,
                                       (double)((long)ChunkZ * ChunkSizeCm) + Local.Z);

    /// <summary>The origin-relative position the scene uses for this point, through the current floating origin. One rounding.</summary>
    public Vec3 ToLocal()
    {
        (long ox, long oy) = WorldOrigin.Origin;
        return new Vec3((float)((long)ChunkX * ChunkSizeCm - ox) + Local.X,
                        (float)((long)ChunkY * ChunkSizeCm - oy) + Local.Y,
                        (float)((long)ChunkZ * ChunkSizeCm) + Local.Z);
    }

    /// <summary>The position of an origin-relative point (a <see cref="Entity.WorldPosition"/>), through the current floating origin.</summary>
    public static WorldPos FromLocal(Vec3 local) => FromAbsolute(WorldOrigin.ToAbsolute(local));

    /// <summary>The straight-line distance to another position in centimetres: the chunk difference is exact, the rest is in double.</summary>
    public double DistanceTo(WorldPos other)
    {
        double dx = (double)(((long)ChunkX - other.ChunkX) * ChunkSizeCm) + ((double)Local.X - other.Local.X);
        double dy = (double)(((long)ChunkY - other.ChunkY) * ChunkSizeCm) + ((double)Local.Y - other.Local.Y);
        double dz = (double)(((long)ChunkZ - other.ChunkZ) * ChunkSizeCm) + ((double)Local.Z - other.Local.Z);
        return Math.Sqrt(dx * dx + dy * dy + dz * dz);
    }

    /// <summary>This position moved by a distance in centimetres; whole chunks go into the chunk index, so precision never degrades.</summary>
    public WorldPos Offset(Double3 deltaCm)
    {
        SplitAxis((double)Local.X + deltaCm.X, out int dcx, out float lx);
        SplitAxis((double)Local.Y + deltaCm.Y, out int dcy, out float ly);
        SplitAxis((double)Local.Z + deltaCm.Z, out int dcz, out float lz);
        return new WorldPos(ChunkX + dcx, ChunkY + dcy, ChunkZ + dcz, new Vec3(lx, ly, lz));
    }

    private static int RegionOfAxis(int chunk) => (int)Math.Floor((chunk + ChunksPerRegion / 2) / (double)ChunksPerRegion);

    /// <summary>The region (1024 chunks, 16.384 km a side, region 0 centred on the world's zero) this point is in.</summary>
    public (int X, int Y, int Z) Region => (RegionOfAxis(ChunkX), RegionOfAxis(ChunkY), RegionOfAxis(ChunkZ));

    /// <inheritdoc/>
    public bool Equals(WorldPos o) => ChunkX == o.ChunkX && ChunkY == o.ChunkY && ChunkZ == o.ChunkZ && Local == o.Local;
    /// <inheritdoc/>
    public override bool Equals(object? obj) => obj is WorldPos o && Equals(o);
    /// <inheritdoc/>
    public override int GetHashCode() => HashCode.Combine(ChunkX, ChunkY, ChunkZ, Local.X, Local.Y, Local.Z);
    /// <summary>Exact equality.</summary>
    public static bool operator ==(WorldPos a, WorldPos b) => a.Equals(b);
    /// <summary>Exact inequality.</summary>
    public static bool operator !=(WorldPos a, WorldPos b) => !a.Equals(b);
    /// <inheritdoc/>
    public override string ToString() => $"chunk ({ChunkX}, {ChunkY}, {ChunkZ}) + ({Local.X}, {Local.Y}, {Local.Z}) cm";
}
