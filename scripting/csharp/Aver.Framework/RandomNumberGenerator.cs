// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// RandomNumberGenerator: a PCG32 generator (O'Neill's pcg32: 64-bit LCG state, XSH-RR output), the shape of
// Godot's RandomNumberGenerator. Written from the published algorithm; tested against its reference output.

namespace Aver.Framework;

/// <summary>A seedable PCG32 generator with Godot's RandomNumberGenerator methods. Instances are independent;
/// <see cref="AverRandom"/> holds the shared one.</summary>
public sealed class RandomNumberGenerator
{
    private const ulong Multiplier = 6364136223846793005UL;
    /// <summary>The stream every generator uses unless told otherwise (PCG's default increment).</summary>
    public const ulong DefaultStream = 1442695040888963407UL;

    private ulong _state, _inc, _seed;

    /// <summary>A generator seeded from the clock and a counter, so two made at once differ.</summary>
    public RandomNumberGenerator() => Randomize();

    /// <summary>A generator seeded with <paramref name="seed"/>.</summary>
    public RandomNumberGenerator(ulong seed, ulong stream = DefaultStream) => SetSeed(seed, stream);

    /// <summary>The seed last set; setting it restarts the sequence.</summary>
    public ulong Seed
    {
        get => _seed;
        set => SetSeed(value, DefaultStream);
    }

    /// <summary>The generator's position: save it and set it back to replay from there.</summary>
    public ulong State
    {
        get => _state;
        set => _state = value;
    }

    private static long s_counter;

    /// <summary>Reseeds from the clock and a process counter.</summary>
    public void Randomize() =>
        SetSeed((ulong)Environment.TickCount64 * 0x9E3779B97F4A7C15UL ^ (ulong)System.Threading.Interlocked.Increment(ref s_counter)
                ^ (ulong)DateTime.UtcNow.Ticks, DefaultStream);

    // pcg32_srandom: state 0, the odd increment, one step, add the seed, one step.
    private void SetSeed(ulong seed, ulong stream)
    {
        _seed = seed;
        _state = 0;
        _inc = (stream << 1) | 1UL;
        Randi();
        _state += seed;
        Randi();
    }

    /// <summary>A uniform 32-bit value (pcg32: XSH-RR of the old state).</summary>
    public uint Randi()
    {
        ulong old = _state;
        _state = unchecked(old * Multiplier + _inc);
        uint xorshifted = (uint)(((old >> 18) ^ old) >> 27);
        int rot = (int)(old >> 59);
        return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
    }

    /// <summary>A uniform value in [0, <paramref name="bound"/>), unbiased (pcg32_boundedrand). 0 for a bound of 0.</summary>
    public uint Randi(uint bound)
    {
        if (bound == 0) return 0;
        uint threshold = unchecked((uint)-bound) % bound;
        for (;;)
        {
            uint r = Randi();
            if (r >= threshold) return r % bound;
        }
    }

    /// <summary>A float in [0, 1), from the top 24 bits so every value is exact.</summary>
    public float Randf() => (Randi() >> 8) * (1.0f / 16777216.0f);

    /// <summary>A float in [<paramref name="from"/>, <paramref name="to"/>).</summary>
    public float RandfRange(float from, float to) => from + (to - from) * Randf();

    /// <summary>An integer in [<paramref name="from"/>, <paramref name="to"/>], both ends reachable; reversed bounds are swapped.</summary>
    public int RandiRange(int from, int to)
    {
        if (to < from) (from, to) = (to, from);
        ulong span = (ulong)((long)to - from) + 1;
        if (span > uint.MaxValue) return (int)((long)from + Randi());
        return (int)((long)from + Randi((uint)span));
    }

    /// <summary>A normally distributed float (Box-Muller) with this <paramref name="mean"/> and <paramref name="deviation"/>.</summary>
    public float Randfn(float mean = 0f, float deviation = 1f)
    {
        float u1 = 1f - Randf();   // (0, 1], so the log is finite
        float u2 = Randf();
        return mean + deviation * MathF.Sqrt(-2f * MathF.Log(u1)) * MathF.Cos(2f * MathF.PI * u2);
    }
}
