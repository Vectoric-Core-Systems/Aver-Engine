// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
namespace Aver.Pcg.SampleRules

open Aver.Pcg

/// <summary>
/// The sky, as a PCG graph.
/// </summary>
/// <remarks>
/// <para>
/// This is the reference a project's own <c>Sky.fs</c> is scaffolded from. It builds an
/// <see cref="T:Aver.Pcg.InfiniteSpec"/> — the same infinite fBm field type the rest of the PCG
/// library uses — and reads the cloud layer's parameters off it, so the sky is DESCRIBED by a field
/// rather than by eight loose numbers typed into a level file.
/// </para>
/// <para>
/// WHAT THE ENGINE ACTUALLY DOES WITH THIS, stated plainly because the gap matters. The cloud
/// raymarch in the renderer has its own value noise; it does not evaluate this field per sample.
/// What crosses the boundary is the field's SEED — hashed into a translation of the renderer's
/// noise domain, so two seeds give two different skies — and a coverage derived from the field's
/// own occupancy. So this graph parameterises the sky; it does not replace the renderer's noise.
/// Saying otherwise would be the kind of claim that reads as a feature and is not one.
/// </para>
/// <para>
/// The coverage is MEASURED FROM THE FIELD rather than declared. Sampling the spec on a coarse
/// lattice and counting what clears the floor is what makes this a graph and not a constant: change
/// the octaves, the cell size or the floor and the coverage the sky renders at moves with them,
/// because it was computed from the same field those knobs describe.
/// </para>
/// </remarks>
module Sky =

    /// <summary>How the sky's field is shaped. Every number here is authored per project.</summary>
    type SkySpec =
        { /// Which sky this is. Two seeds give different cloud fields from one set of settings.
          Seed: int
          /// World size of one lattice cell, in centimetres. 1600 is one chunk.
          CellSizeCm: float32
          /// fBm layers. More octaves means finer structure at the same cell size.
          Octaves: int
          /// Density below which the field counts as empty. HIGHER means LESS cloud.
          CoverageFloor: float32
          /// World Z of the layer's base and top, in centimetres.
          BottomCm: float32
          TopCm: float32
          /// Drift, centimetres per second.
          WindXCmPerSec: float32
          WindYCmPerSec: float32 }

        /// <summary>A temperate afternoon sky: broken cloud, slow drift.</summary>
        static member Default =
            { Seed = 3
              CellSizeCm = 1600.0f
              Octaves = 4
              CoverageFloor = 0.45f
              BottomCm = 150000.0f
              TopCm = 280000.0f
              WindXCmPerSec = 900.0f
              WindYCmPerSec = 260.0f }

    /// <summary>The infinite density field this sky is. INFINITE because a sky has no bounds.</summary>
    [<CompiledName("Field")>]
    let field (spec: SkySpec) : InfiniteSpec =
        let baseField = Pcg.infiniteExpanse spec.Seed spec.CellSizeCm spec.Octaves
        { baseField with
            CoverageFloor = spec.CoverageFloor
            CoverageBias = 1.6f }

    /// <summary>
    /// The fraction of the field that clears its floor, sampled on a coarse lattice.
    /// </summary>
    /// <remarks>
    /// A 16x16 sample over one cell's span, at the layer's mid height. Coarse ON PURPOSE: this runs
    /// at level load on the game thread and the answer feeds a single scalar, so a finer grid would
    /// cost real milliseconds to move a number the eye cannot resolve. 256 samples puts the
    /// sampling error at roughly a percentage point, well under what a coverage change of 0.05
    /// looks like.
    /// </remarks>
    [<CompiledName("MeasureCoverage")>]
    let measureCoverage (spec: SkySpec) : float32 =
        let f = field spec
        let span = spec.CellSizeCm * 16.0f
        let z = (spec.BottomCm + spec.TopCm) * 0.5f
        let mutable hits = 0
        for iy in 0 .. 15 do
            for ix in 0 .. 15 do
                let x = float32 ix / 16.0f * span
                let y = float32 iy / 16.0f * span
                if Pcg.sampleInfinite f x y z > 0.0f then hits <- hits + 1
        float32 hits / 256.0f

    /// <summary>Everything the host needs, derived from the field.</summary>
    [<CompiledName("Resolve")>]
    let resolve (spec: SkySpec) =
        struct (spec.Seed,
                measureCoverage spec,
                spec.BottomCm,
                spec.TopCm,
                spec.WindXCmPerSec,
                spec.WindYCmPerSec)
