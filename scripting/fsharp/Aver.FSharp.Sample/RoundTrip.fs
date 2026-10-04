// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The F# half of the smallest real round trip: a function C# calls and gets a value back from.

namespace Aver.FSharp.Sample

/// <summary>The F# side of the host round trip. Deliberately tiny, and deliberately written in
/// constructs that only exist because FSharp.Core is live at run time.</summary>
module RoundTrip =

    /// One integer avalanche step. Written with F#'s own operators (^^^, >>>) so the C# mirror in
    /// FSharpRoundTripBehaviour has to agree with a *separately compiled* implementation rather than
    /// with itself. Both languages leave int arithmetic unchecked and both shift right arithmetically,
    /// so the two are required to produce the same bits.
    let private mix (x: int) : int =
        let a = x ^^^ (x >>> 15)
        let b = a * 0x2c1b3c6d
        let c = b ^^^ (b >>> 12)
        c * 0x297a2d39

    /// <summary>Folds <paramref name="count"/> avalanche steps over a seed and returns the result.</summary>
    /// <remarks>The list, the pipeline, List.map and List.fold are not decoration: every one of them
    /// is an FSharp.Core type or function, so a run that returns the right number has proved that
    /// FSharp.Core resolved inside the host's collectible load context. A pure-arithmetic body would
    /// have compiled to IL that needs nothing but the BCL and would have proved far less.</remarks>
    [<CompiledName("Checksum")>]
    let checksum (seed: int) (count: int) : int =
        [ 1 .. count ]
        |> List.map (fun i -> mix (seed + i))
        |> List.fold (fun acc v -> mix (acc ^^^ v)) seed

    /// <summary>Names this assembly and the FSharp.Core the host actually bound it to.</summary>
    /// <remarks>`typeof&lt;int list&gt;` is FSharp.Core's FSharpList, so the version reported is the
    /// one the load context resolved, not the one the .fsproj asked for. sprintf is used on purpose:
    /// it is one of the heaviest things in FSharp.Core, so it fails loudly if the binding is wrong.</remarks>
    [<CompiledName("Describe")>]
    let describe () : string =
        let self = System.Reflection.Assembly.GetExecutingAssembly().GetName()
        let core = typeof<int list>.Assembly.GetName()
        sprintf "%s v%O (F#-compiled) bound to %s v%O" self.Name self.Version core.Name core.Version
