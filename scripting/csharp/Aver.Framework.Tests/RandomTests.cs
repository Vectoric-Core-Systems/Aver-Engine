// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// RandomTests: the PCG32 generator against the algorithm's published reference output, and its ranges.

using Aver.Framework;

static class RandomTests
{
    public static int RunAll() => T.Suite(() =>
    {
        T.Run("pcg32: seed 42, stream 54 gives the reference sequence", Reference);
        T.Run("pcg32: a seed or a saved state replays", Replay);
        T.Run("pcg32: ranges stay in bounds and reach both ends", Ranges);
    });

    private static void Reference()
    {
        var r = new RandomNumberGenerator(42, 54);
        uint[] expect = { 0xa15c02b7, 0x7b47f409, 0xba1d3330, 0x83d2f293, 0xbfa4784b, 0xcbed606e };
        bool same = true;
        foreach (uint e in expect) same &= r.Randi() == e;
        T.Check("the first six outputs match pcg32's reference", same);
    }

    private static void Replay()
    {
        var a = new RandomNumberGenerator(7);
        var b = new RandomNumberGenerator(7);
        bool same = true;
        for (int i = 0; i < 100; ++i) same &= a.Randi() == b.Randi();
        T.Check("the same seed gives the same sequence", same);
        ulong state = a.State;
        uint next = a.Randi();
        a.State = state;
        T.Check("setting a saved state back replays the next value", a.Randi() == next);
        AverRandom.Seed(3);
        float f1 = AverRandom.NextFloat();
        AverRandom.Seed(3);
        T.Check("the shared generator replays from a seed", AverRandom.NextFloat() == f1);
    }

    private static void Ranges()
    {
        var r = new RandomNumberGenerator(1);
        bool lo = false, hi = false, inRange = true, fOk = true;
        for (int i = 0; i < 20000; ++i)
        {
            int v = r.RandiRange(-2, 2);
            inRange &= v >= -2 && v <= 2;
            lo |= v == -2; hi |= v == 2;
            float f = r.Randf();
            fOk &= f >= 0f && f < 1f;
        }
        T.Check("RandiRange stays in [-2, 2]", inRange);
        T.Check("and reaches both ends", lo && hi);
        T.Check("Randf stays in [0, 1)", fOk);
        T.Check("reversed bounds are swapped", r.RandiRange(5, 5) == 5 && r.RandiRange(3, 1) is >= 1 and <= 3);
        double sum = 0;
        for (int i = 0; i < 20000; ++i) sum += r.Randfn(10f, 2f);
        T.Check("Randfn centres on its mean", Math.Abs(sum / 20000 - 10.0) < 0.1);
    }
}
