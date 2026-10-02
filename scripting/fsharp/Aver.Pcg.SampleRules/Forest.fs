// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// A worked example of the F# half of the PCG split: RULES, and nothing else.
//
// This is what a project author writes. It decides WHAT goes WHERE and hands back placements; it
// never touches an entity, a device or an asset. The C# behaviour next door is what makes any of it
// exist in the world, and the two are separate assemblies so this one stays testable with no engine
// running at all.
//
// Everything here is a pure function of its arguments. Same seed, same forest, on any machine and in
// any order -- which is the property the whole seeded-PCG contract rests on, and the reason
// Aver.Pcg's mixer is splitmix32 in code this repo owns rather than System.Random.

namespace Aver.Pcg.SampleRules

open Aver.Pcg

/// <summary>A small clearing-and-treeline forest, generated from one seed.</summary>
module Forest =

    /// Metres to the engine's unit, which is the CENTIMETRE. Written once, here, because a generator
    /// that thinks in metres produces a world a hundred times too small and it looks like a scale
    /// bug in the renderer rather than a units bug in the rules.
    let private m (v: float32) = v * 100.0f

    /// <summary>The region a forest of `sizeMetres` across occupies, centred on the origin.</summary>
    [<CompiledName("Region")>]
    let region (sizeMetres: float32) : Bounds =
        let h = m sizeMetres * 0.5f
        { Min = { X = -h; Y = -h; Z = 0.0f }
          Max = { X =  h; Y =  h; Z = 0.0f } }

    /// <summary>Trees: many, well separated, freely rotated, varied in size.</summary>
    /// <remarks>MinSpacing is what stops a forest looking like a pile. It is enforced by rejection,
    /// so the returned count is USUALLY LESS than `count` -- that is the honest behaviour of a
    /// spacing constraint and the caller is expected to read the array's length rather than assume
    /// it got what it asked for.</remarks>
    [<CompiledName("Trees")>]
    let trees (seed: int) (sizeMetres: float32) (count: int) : Placement[] =
        Pcg.scatter
            { ScatterSpec.Default with
                Region     = region sizeMetres
                Count      = count
                Seed       = seed
                Assets     = [| "Tree_Pine"; "Tree_Oak" |]
                RandomYaw  = true
                ScaleMin   = 0.85f
                ScaleMax   = 1.35f
                MinSpacing = m 4.0f }

    /// <summary>Undergrowth: dense, unspaced, small, and seeded off the SAME master seed.</summary>
    /// <remarks>The seed offset is a constant rather than `seed + 1`: adjacent seeds produce
    /// correlated output from any hash, so two layers built on `seed` and `seed + 1` would grow
    /// their bushes suspiciously near their trees. A wide offset decorrelates them.</remarks>
    [<CompiledName("Undergrowth")>]
    let undergrowth (seed: int) (sizeMetres: float32) (count: int) : Placement[] =
        Pcg.scatter
            { ScatterSpec.Default with
                Region    = region sizeMetres
                Count     = count
                Seed      = seed + 7919
                Assets    = [| "Bush_Small" |]
                RandomYaw = true
                ScaleMin  = 0.6f
                ScaleMax  = 1.1f
                MinSpacing = 0.0f }

    /// <summary>Everything, in one call, as a caller usually wants it.</summary>
    [<CompiledName("Generate")>]
    let generate (seed: int) (sizeMetres: float32) : Placement[] =
        Array.append (trees seed sizeMetres 400) (undergrowth seed sizeMetres 900)

    /// <summary>A one-line summary a caller can log, so a run says what it generated.</summary>
    [<CompiledName("Describe")>]
    let describe (seed: int) (sizeMetres: float32) : string =
        let t = trees seed sizeMetres 400
        let u = undergrowth seed sizeMetres 900
        sprintf "seed %d over %.0f m: %d tree(s) of 400 requested (spacing rejected %d), %d undergrowth"
                seed sizeMetres t.Length (400 - t.Length) u.Length
