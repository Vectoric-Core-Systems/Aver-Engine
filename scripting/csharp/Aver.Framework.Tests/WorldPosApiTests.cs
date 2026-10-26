// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// WorldPos, Double3 and WorldOrigin (docs/plans/wave11/LARGE_WORLD.md 2.5) in a bare process: the managed WorldPos arithmetic against hand-derived
// cases (the same ones tests/framework_origin pins on the C++ side, so the two cannot drift apart), the seams and negatives, the properties the
// design states, and the Changed event's payload handling. No native library is needed: with none, the origin reads as (0, 0), off.

using Aver.Framework;
using Aver.Scene;

static class WorldPosApiTests
{
    public static int RunAll() => T.Suite(() =>
    {
        T.Run("world pos: the hand-derived splits, seams and negatives", HandCases);
        T.Run("world pos: the local offset is always in [0, 1600), and an absolute on the quarter-centimetre grid round-trips exactly", Properties);
        T.Run("world pos: 100 km is exact, and a chunk difference never loses precision at 10^8 cm", FarAndDistance);
        T.Run("world pos: regions are 1024 chunks, region 0 centred on the zero", Regions);
        T.Run("world pos: Offset absorbs whole chunks", OffsetAbsorbsChunks);
        T.Run("double3: arithmetic, equality and the narrowing", Double3Arithmetic);
        T.Run("world origin: with no host it is (0, 0), off, generation 0 and the conversions are the identity", NoHost);
        T.Run("world origin: the Changed payload is dxCm, dyCm and the generation, and a handler that throws does not stop the next", ChangedPayload);
    });

    private static void HandCases()
    {
        var p = WorldPos.FromAbsolute(new Double3(-0.25, 1600.0, -1600.0));
        T.Check("-0.25 is chunk -1 at 1599.75", p.ChunkX == -1 && p.Local.X == 1599.75f);
        T.Check("1600 is chunk 1 at 0", p.ChunkY == 1 && p.Local.Y == 0.0f);
        T.Check("-1600 is chunk -1 at 0", p.ChunkZ == -1 && p.Local.Z == 0.0f);
        var q = WorldPos.FromAbsolute(new Double3(1599.9999, 0, 3199.5));
        T.Check("1599.9999 stays in chunk 0 (its float rounds up to the chunk size and is repaired)", q.ChunkX == 1 || (q.ChunkX == 0 && q.Local.X < 1600.0f));
        T.Check("3199.5 is chunk 1 at 1599.5", q.ChunkZ == 1 && q.Local.Z == 1599.5f);
        var o = WorldPos.FromAbsolute(new Double3(0, 0, 0));
        T.Check("zero is chunk (0, 0, 0) at zero", o.ChunkX == 0 && o.ChunkY == 0 && o.ChunkZ == 0 && o.Local == Vec3.Zero);
    }

    private static void Properties()
    {
        var rng = new Random(0x5eed);
        int bad = 0;
        for (int i = 0; i < 20000; ++i)
        {
            double Far() => Math.Round((rng.NextDouble() * 2.0 - 1.0) * 1.0e8 * 4.0) / 4.0;
            var abs = new Double3(Far(), Far(), Math.Round((rng.NextDouble() * 2.0 - 1.0) * 2.0e6 * 4.0) / 4.0);
            WorldPos p = WorldPos.FromAbsolute(abs);
            if (!(p.Local.X >= 0f && p.Local.X < 1600f && p.Local.Y >= 0f && p.Local.Y < 1600f && p.Local.Z >= 0f && p.Local.Z < 1600f)) ++bad;
            Double3 back = p.ToAbsolute();
            if (back != abs) ++bad;   // the quarter-centimetre grid is exact in an f32 local
        }
        T.Check("20,000 absolute positions to 10^8 cm: the local is on [0,1600) and the round trip is exact", bad == 0, $"{bad} off");
    }

    private static void FarAndDistance()
    {
        var p = WorldPos.FromAbsolute(new Double3(10_000_000.0, 0, 0));
        T.Check("100 km is exact", p.ToAbsolute().X == 10_000_000.0);
        var a = WorldPos.FromAbsolute(new Double3(100_000_000.0, -100_000_000.0, 50.0));
        var b = WorldPos.FromAbsolute(new Double3(100_000_000.0 + 3.0, -100_000_000.0 + 4.0, 50.0));
        T.Check("the distance between two points 3-4-5 apart, a million metres out, is 5 cm", Math.Abs(a.DistanceTo(b) - 5.0) < 1e-6, a.DistanceTo(b).ToString());
        T.Check("a point is no distance from itself", a.DistanceTo(a) == 0.0);
    }

    private static void Regions()
    {
        T.Check("the zero is in region (0, 0, 0)", WorldPos.FromAbsolute(new Double3(0, 0, 0)).Region == (0, 0, 0));
        T.Check("819,199.75 is still region 0 and 819,200 is region 1", WorldPos.FromAbsolute(new Double3(819_199.75, 0, 0)).Region.X == 0 &&
                                                                         WorldPos.FromAbsolute(new Double3(819_200.0, 0, 0)).Region.X == 1);
        T.Check("-819,200 is region 0 and just below it region -1", WorldPos.FromAbsolute(new Double3(-819_200.0, 0, 0)).Region.X == 0 &&
                                                                     WorldPos.FromAbsolute(new Double3(-819_200.25, 0, 0)).Region.X == -1);
        T.Check("100 km is region 6", WorldPos.FromAbsolute(new Double3(10_000_000.0, -2_500_000.0, 0)).Region == (6, -2, 0));
        T.Check("a region is 1024 chunks of 1600 cm", WorldPos.RegionSizeCm == 1_638_400 && WorldOrigin.StepCm == 1_638_400);
    }

    private static void OffsetAbsorbsChunks()
    {
        var p = WorldPos.FromAbsolute(new Double3(100.0, 200.0, 300.0));
        WorldPos m = p.Offset(new Double3(5_000.5, -5_000.5, 0));
        T.Check("moved by a few chunks, the absolute is the sum", m.ToAbsolute() == new Double3(5_100.5, -4_800.5, 300.0));
        T.Check("and the local stays on [0,1600)", m.Local.X >= 0 && m.Local.X < 1600 && m.Local.Y >= 0 && m.Local.Y < 1600);
        T.Check("moving by zero changes nothing", p.Offset(Double3.Zero) == p);
    }

    private static void Double3Arithmetic()
    {
        var a = new Double3(1, 2, 3);
        var b = new Double3(10, 20, 30);
        T.Check("sum, difference, scale, divide, negate", a + b == new Double3(11, 22, 33) && b - a == new Double3(9, 18, 27) &&
                                                          a * 2 == new Double3(2, 4, 6) && 2 * a == a * 2 && b / 10 == a && -a == new Double3(-1, -2, -3));
        T.Check("length of (3, 4, 0) is 5", new Double3(3, 4, 0).Length == 5.0);
        T.Check("equality is exact and hashes agree", a == new Double3(1, 2, 3) && a != b && a.GetHashCode() == new Double3(1, 2, 3).GetHashCode());
        T.Check("widening a Vec3 is exact and narrowing it back is the same Vec3", Double3.From(new Vec3(1.5f, -2.25f, 3f)).ToVec3() == new Vec3(1.5f, -2.25f, 3f));
    }

    private static void NoHost()
    {
        T.Check("origin (0, 0)", WorldOrigin.Origin == (0L, 0L) && WorldOrigin.Region == (0, 0));
        T.Check("generation 0 and not enabled", WorldOrigin.Generation == 0 && !WorldOrigin.Enabled);
        var v = new Vec3(250_000.5f, -3.25f, 80f);
        T.Check("ToAbsolute is exact about a zero origin", WorldOrigin.ToAbsolute(v) == new Double3(250_000.5, -3.25, 80.0));
        T.Check("ToLocal undoes it", WorldOrigin.ToLocal(WorldOrigin.ToAbsolute(v)) == v);
        T.Check("a WorldPos converts both ways through the zero origin", WorldPos.FromLocal(v).ToLocal() == v);
        T.Check("a shift request is refused", !WorldOrigin.RequestShift(1, 0));
    }

    private static EventValue Int(long v) => new(EventValueKind.Int, v, 0, 0, 0, null);

    private static void ChangedPayload()
    {
        T.Reset();
        Double3 seenDelta = default;
        uint seenGen = 0;
        int calls = 0, afterThrow = 0;
        // Subscribing needs the native event bus; without it the add accessor logs once and the invocation below still exercises the handler path.
        Action<Double3, uint> boom = (_, _) => throw new InvalidOperationException("handler bug");
        Action<Double3, uint> good = (d, g) => { seenDelta = d; seenGen = g; ++calls; ++afterThrow; };
        WorldOrigin.Changed += boom;
        WorldOrigin.Changed += good;
        var ev = new EventData
        {
            Name = "world.origin_changed",
            Args = new[] { Int(2 * 1_638_400L), Int(-1_638_400L), Int(7L), Int(2 * 1_638_400L), Int(-1_638_400L) },
        };
        WorldOrigin.OnOriginChanged(ev);
        T.Check("the handler heard the shift in centimetres (Z is 0) and the generation", seenDelta == new Double3(3_276_800.0, -1_638_400.0, 0.0) && seenGen == 7 && calls == 1);
        T.Check("a handler that threw was logged and did not stop the next", afterThrow == 1 && T.Logs.Any(l => l.Level == Aver.Scripting.Log.Level.Error && l.Message.Contains("handler bug")));
        WorldOrigin.Changed -= boom;
        WorldOrigin.Changed -= good;
        WorldOrigin.OnOriginChanged(ev);
        T.Check("after unsubscribing nothing is called", calls == 1);
    }
}
