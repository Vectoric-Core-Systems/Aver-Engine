// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// The C# end of the F# round trip: call an F#-compiled function and check its answer against a
// C#-compiled one that was written to the same spec.

using System.Runtime.CompilerServices;
using System.Runtime.Loader;

using Aver.Scripting;

using FS = Aver.FSharp.Sample.RoundTrip;

namespace Aver.Scripting.SampleFSharp;

/// <summary>Proves the F# seam by calling into an F#-compiled assembly from a normal AverBehaviour
/// and reporting the value that comes back through the engine log.</summary>
/// <remarks>Staged to <c>bin/FSharpScripts/</c>; run <c>Sandbox.exe --scripts FSharpScripts</c>.</remarks>
public sealed class FSharpRoundTripBehaviour : AverBehaviour
{
    // Fixed so two runs are comparable, and so the F# and C# sides are asked the identical question.
    private const int Seed = 20260801;
    private const int Steps = 64;

    /// <summary>Calls the F# function and reports whether the C#-compiled mirror agrees with it.</summary>
    /// <remarks>THE CHECK IS RELATIONAL AND NEEDS NO BASELINE. The same integer recurrence exists twice
    /// in this repo: once in RoundTrip.fs compiled by fsc, once in Mix/ChecksumInCSharp below compiled
    /// by csc. Exactly one thing differs between them — the compiler. Equal answers therefore mean the
    /// F# assembly loaded, its FSharp.Core bound, and its code actually ran; there is no way for a
    /// non-executing F# side to produce this number, and a hard-coded expected value would have proved
    /// none of it. Perturb any constant in RoundTrip.fs and this logs MISMATCH with both values.</remarks>
    public override void OnStart()
    {
        try
        {
            RunRoundTrip();
        }
        catch (Exception ex)
        {
            // Named rather than left to the bridge's generic handler, because the one failure this is
            // most likely to hit is FSharp.Core not resolving inside the collectible load context, and
            // that reads as an unhelpful FileNotFoundException unless something says what it means.
            Log.Error($"[FSharp] the round trip could not run: {ex.GetType().Name}: {ex.Message} - "
                      + "if this names FSharp.Core, the script load context could not resolve the F# "
                      + "runtime library from the scripts directory");
        }
    }

    // NoInlining keeps every reference to the F# assembly inside a method that only runs under the
    // try above: an inlined body would put those references in OnStart's own frame, where a failed
    // resolution surfaces before the catch is in scope.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void RunRoundTrip()
    {
        Log.Info($"[FSharp] {FS.Describe()}");

        // Reported, not asserted. Which context each assembly landed in is the single most useful fact
        // when a script stops being discovered, and the scripts-directory probe made it worth printing:
        // Aver.Scripting must stay OUTSIDE the collectible 'AverScripts' context or AverBehaviour ends up
        // with two identities. There is no error branch here because that state is not reachable from
        // inside a discovered behaviour — if it ever happens, this class is not constructed at all and
        // the engine's own "loaded ...: 0 behaviour(s)" line is what says so.
        Log.Info($"[FSharp] load contexts: this assembly in "
                 + $"'{AssemblyLoadContext.GetLoadContext(typeof(FSharpRoundTripBehaviour).Assembly)?.Name}', "
                 + $"Aver.Scripting in "
                 + $"'{AssemblyLoadContext.GetLoadContext(typeof(AverBehaviour).Assembly)?.Name}'");

        int fromFSharp = FS.Checksum(Seed, Steps);
        int fromCSharp = ChecksumInCSharp(Seed, Steps);

        if (fromFSharp == fromCSharp)
            Log.Info($"[FSharp] round trip OK: RoundTrip.Checksum({Seed}, {Steps}) returned "
                     + $"{fromFSharp} (0x{fromFSharp:X8}) and the separately compiled C# mirror agrees");
        else
            Log.Error($"[FSharp] round trip MISMATCH: F# returned {fromFSharp} (0x{fromFSharp:X8}) but "
                      + $"the C# mirror computed {fromCSharp} (0x{fromCSharp:X8}) - the two "
                      + "implementations of the same recurrence have diverged");
    }

    // The mirror of RoundTrip.fs's private `mix`. C# and F# both leave int multiplication unchecked
    // and both shift a signed int arithmetically, so the two are obliged to produce the same bits.
    private static int Mix(int x)
    {
        int a = x ^ (x >> 15);
        int b = a * 0x2c1b3c6d;
        int c = b ^ (b >> 12);
        return c * 0x297a2d39;
    }

    // The mirror of RoundTrip.fs's `checksum`: fold `count` steps over `seed`. Written as a loop
    // rather than with LINQ so the arithmetic is the only thing being compared.
    private static int ChecksumInCSharp(int seed, int count)
    {
        int acc = seed;
        for (int i = 1; i <= count; ++i)
            acc = Mix(acc ^ Mix(seed + i));
        return acc;
    }
}
