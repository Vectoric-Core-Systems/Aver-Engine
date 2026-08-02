// The C# end of the PCG split: run the F# rules, apply the result, and report what happened.
//
// This is the whole architecture in one file. F# decided what goes where; nothing below decides
// anything about content, and if a generation rule ever appears here it is in the wrong file.

using System.Runtime.CompilerServices;

using Aver.Scripting;
using Aver.Framework;
using Aver.Pcg.Apply;

using Rules = Aver.Pcg.SampleRules.Forest;

namespace Aver.Scripting.SamplePcg;

/// <summary>Generates a forest from a seed and spawns it, reporting the counts at each step.</summary>
/// <remarks>Staged to <c>bin/PcgScripts/</c>; run <c>Sandbox.exe --scripts PcgScripts</c>.</remarks>
public sealed class PcgScatterBehaviour : AverBehaviour
{
    // Fixed so two runs are comparable. Change it and the forest changes completely; change it back
    // and the identical forest returns, which is the contract the whole system is built on.
    private const int Seed = 20260802;
    private const float SizeMetres = 120.0f;

    private readonly List<Entity> _spawned = new();

    /// <summary>Generates, applies, and reports.</summary>
    public override void OnStart()
    {
        try
        {
            Run();
        }
        catch (Exception ex)
        {
            // Named rather than left to the bridge's generic handler: the likeliest failure is
            // FSharp.Core not resolving inside the collectible load context, which surfaces as an
            // unhelpful FileNotFoundException unless something says what it means.
            Log.Error($"[PCG] the sample could not run: {ex.GetType().Name}: {ex.Message} - "
                      + "if this names FSharp.Core, the script load context could not resolve the "
                      + "F# runtime library from the scripts directory");
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private void Run()
    {
        Log.Info($"[PCG] {Rules.Describe(Seed, SizeMetres)}");

        // GENERATED TWICE, deliberately, and compared. This is a relational check that needs no
        // stored baseline: two independent calls with the same seed must agree placement for
        // placement. It costs a few milliseconds and it is the only thing standing between "the
        // generator is deterministic" and "the generator has not obviously broken yet".
        var a = Rules.Generate(Seed, SizeMetres);
        var b = Rules.Generate(Seed, SizeMetres);
        if (!SameForest(a, b))
        {
            Log.Error("[PCG] DETERMINISM BROKEN: two calls with the same seed disagreed. "
                      + "Nothing below this line is trustworthy.");
            return;
        }
        Log.Info($"[PCG] {a.Length} placement(s), determinism check passed (two identical generations)");

        // And a DIFFERENT seed must disagree, or the first check passes vacuously -- a generator
        // that returns a constant array satisfies "same seed, same output" perfectly.
        var other = Rules.Generate(Seed + 1, SizeMetres);
        if (SameForest(a, other))
        {
            Log.Error("[PCG] the seed does nothing: a different seed produced an identical forest");
            return;
        }

        ApplyResult r = PcgApply.Apply(a, _spawned);
        Log.Info($"[PCG] applied: {r}");

        // UnknownClass is the EXPECTED outcome in a project that has no Tree_Pine actor, and saying
        // so is the point: the applicator counts what it skipped rather than silently producing an
        // empty forest, which is the failure mode that wastes an afternoon.
        if (r.Spawned == 0 && r.UnknownClass > 0)
            Log.Info("[PCG] nothing spawned because this project declares no Tree_Pine/Tree_Oak/"
                     + "Bush_Small actor classes - the generation ran correctly and had nowhere to put "
                     + "its output. Declare those classes to see the forest.");
    }

    /// <summary>True when two generations agree placement for placement.</summary>
    /// <remarks>Compares the BITS of every float, not an epsilon. Determinism here means identical,
    /// not close: a tolerance would hide exactly the drift this is looking for.</remarks>
    private static bool SameForest(Aver.Pcg.Placement[] a, Aver.Pcg.Placement[] b)
    {
        if (a.Length != b.Length) return false;
        for (int i = 0; i < a.Length; ++i)
        {
            if (BitConverter.SingleToInt32Bits(a[i].Position.X) != BitConverter.SingleToInt32Bits(b[i].Position.X)) return false;
            if (BitConverter.SingleToInt32Bits(a[i].Position.Y) != BitConverter.SingleToInt32Bits(b[i].Position.Y)) return false;
            if (BitConverter.SingleToInt32Bits(a[i].Position.Z) != BitConverter.SingleToInt32Bits(b[i].Position.Z)) return false;
            if (BitConverter.SingleToInt32Bits(a[i].Rotation.Z) != BitConverter.SingleToInt32Bits(b[i].Rotation.Z)) return false;
            if (BitConverter.SingleToInt32Bits(a[i].Scale.X)    != BitConverter.SingleToInt32Bits(b[i].Scale.X))    return false;
            if (!string.Equals(a[i].Asset, b[i].Asset, StringComparison.Ordinal)) return false;
        }
        return true;
    }
}
