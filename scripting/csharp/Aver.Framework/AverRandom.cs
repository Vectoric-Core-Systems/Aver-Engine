// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The shared random generator a script and the Random graph nodes draw from.

using Aver.Scene;

namespace Aver.Framework;

/// <summary>One process-wide pseudo-random generator for gameplay, reseedable so a sequence can repeat.</summary>
/// <remarks>Single game thread only. Unseeded it starts from a random seed; <see cref="Seed"/> makes every later
/// draw repeat for the same seed. Graph and C# draws share the one stream, so a seeded graph and script interleave
/// reproducibly.</remarks>
public static class AverRandom
{
    private static Random s_rng = new();

    /// <summary>Restarts the generator from <paramref name="seed"/>; the same seed repeats the same sequence.</summary>
    public static void Seed(int seed) => s_rng = new Random(seed);

    /// <summary>A float in [0, 1).</summary>
    public static float NextFloat() => s_rng.NextSingle();

    /// <summary>A float in [<paramref name="min"/>, <paramref name="max"/>). Reversed bounds are swapped; equal bounds give that value.</summary>
    public static float NextFloat(float min, float max)
    {
        if (max < min) (min, max) = (max, min);
        return min + (max - min) * s_rng.NextSingle();
    }

    /// <summary>An integer in [<paramref name="min"/>, <paramref name="max"/>] with BOTH ends reachable. Reversed bounds are swapped.</summary>
    public static int NextInt(int min, int max)
    {
        if (max < min) (min, max) = (max, min);
        return (int)s_rng.NextInt64(min, (long)max + 1);
    }

    /// <summary>True with probability <paramref name="chance"/> (0 never, 1 always; NaN never).</summary>
    public static bool NextBool(float chance = 0.5f)
    {
        if (chance <= 0f) return false;
        if (chance >= 1f) return true;
        return s_rng.NextSingle() < chance;
    }

    /// <summary>A unit direction uniformly distributed over the sphere.</summary>
    public static Vec3 UnitVector()
    {
        float z = NextFloat(-1f, 1f);
        float phi = NextFloat(0f, 2f * MathF.PI);
        float r = MathF.Sqrt(MathF.Max(0f, 1f - z * z));
        return new Vec3(r * MathF.Cos(phi), r * MathF.Sin(phi), z);
    }
}
