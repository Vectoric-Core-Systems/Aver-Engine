// The general PCG API. Clouds are one consumer of this, not its design.

namespace Aver.Pcg

open System

/// <summary>Deterministic hashing. The seed contract of this whole API rests on this module.</summary>
/// <remarks>
/// SYSTEM.RANDOM IS NOT USED, AND THAT IS NOT A STYLE CHOICE. Its algorithm is explicitly
/// undocumented and it has already changed once: .NET Core 2.x and .NET Core 3.0+ produce different
/// sequences from the same seed. "Same seed, same world" is the entire promise of seeded PCG, and
/// building it on a sequence Microsoft is free to change would mean a user's saved seed silently
/// generating a different world after a runtime update. So the mixing function lives here, in code
/// this repo owns, and is a pure function of its inputs.
///
/// It is also STATELESS on purpose. There is no cursor to advance, so generation can be evaluated
/// in any order, in parallel, or partially, and still produce identical output -- which is what lets
/// the GPU fill a volume the same way the CPU reference below does.
/// </remarks>
module Rand =

    /// <summary>splitmix32 finaliser: one integer in, a well-avalanched integer out.</summary>
    let hash (x: uint32) : uint32 =
        let mutable z = x + 0x9E3779B9u
        z <- (z ^^^ (z >>> 16)) * 0x21F0AAADu
        z <- (z ^^^ (z >>> 15)) * 0x735A2D97u
        z ^^^ (z >>> 15)

    /// <summary>Hashes a seed together with an index. Order-independent by construction.</summary>
    let hash2 (seed: int) (i: int) : uint32 =
        hash (uint32 seed ^^^ hash (uint32 i))

    /// <summary>Hashes a seed with a 3D integer coordinate.</summary>
    let hash3 (seed: int) (x: int) (y: int) (z: int) : uint32 =
        hash (uint32 seed ^^^ hash (uint32 x ^^^ hash (uint32 y ^^^ hash (uint32 z))))

    /// <summary>A hash as a float in [0, 1).</summary>
    /// <remarks>Takes the TOP 24 bits, not the bottom ones, and divides by 2^24. float32 has a
    /// 24-bit mantissa, so this is exact and every value is representable; using the low bits of a
    /// multiply-based hash would sample its weakest bits.</remarks>
    let float01 (h: uint32) : float32 =
        float32 (h >>> 8) / 16777216.0f

    /// <summary>A float in [lo, hi) from a seed and an index.</summary>
    let range (seed: int) (i: int) (lo: float32) (hi: float32) : float32 =
        lo + (hi - lo) * float01 (hash2 seed i)


/// <summary>What to scatter, where, and how much to vary it.</summary>
[<CLIMutable>]
type ScatterSpec =
    { Region: Bounds
      /// How many placements to attempt. MinSpacing may reduce the number actually returned.
      Count: int
      Seed: int
      /// Chosen per placement by hash. Empty means every placement carries an empty asset path.
      Assets: string[]
      /// Random rotation about the world up axis (+Z). The engine is +Z up.
      RandomYaw: bool
      ScaleMin: float32
      ScaleMax: float32
      /// Minimum separation between placements, in CENTIMETRES. Zero disables the check.
      MinSpacing: float32 }

    static member Default =
        { Region = { Min = Vec3.Zero; Max = Vec3.One }
          Count = 0
          Seed = 0
          Assets = [||]
          RandomYaw = true
          ScaleMin = 1.0f
          ScaleMax = 1.0f
          MinSpacing = 0.0f }


/// <summary>The PCG entry points. Every one is a pure function of its arguments.</summary>
module Pcg =

    /// <summary>A quaternion for a rotation of <paramref name="radians"/> about +Z.</summary>
    let private yawQuat (radians: float32) : Quat =
        let h = radians * 0.5f
        { X = 0.0f; Y = 0.0f; Z = sin h; W = cos h }

    let private distanceSq (a: Vec3) (b: Vec3) =
        let dx = a.X - b.X
        let dy = a.Y - b.Y
        let dz = a.Z - b.Z
        dx * dx + dy * dy + dz * dz

    /// <summary>Scatters placements through a region.</summary>
    /// <remarks>
    /// Returns an ARRAY, not a list, because this crosses into C#.
    ///
    /// MinSpacing is enforced by rejection against the placements already accepted, in index order.
    /// That is O(n^2) and is honest about it: it is fine for the hundreds-to-low-thousands a hand-
    /// authored scatter uses, and a Poisson-disc grid is the answer above that. Because rejection
    /// walks a FIXED index order and the hash is stateless, the result is identical every run --
    /// a rejection scheme driven by a stateful RNG would not be, since a rejected candidate would
    /// shift every later draw.
    /// </remarks>
    [<CompiledName("Scatter")>]
    let scatter (spec: ScatterSpec) : Placement[] =
        if spec.Count <= 0 then [||]
        else
            let size = spec.Region.Size
            let minSq = spec.MinSpacing * spec.MinSpacing
            let accepted = ResizeArray<Placement>(spec.Count)

            for i in 0 .. spec.Count - 1 do
                // Four independent streams per placement. Distinct index spaces rather than a
                // running counter, so adding a future stream cannot shift the existing ones.
                let px = Rand.float01 (Rand.hash2 spec.Seed (i * 4))
                let py = Rand.float01 (Rand.hash2 spec.Seed (i * 4 + 1))
                let pz = Rand.float01 (Rand.hash2 spec.Seed (i * 4 + 2))
                let extra = Rand.hash2 spec.Seed (i * 4 + 3)

                let pos =
                    { X = spec.Region.Min.X + size.X * px
                      Y = spec.Region.Min.Y + size.Y * py
                      Z = spec.Region.Min.Z + size.Z * pz }

                let farEnough =
                    spec.MinSpacing <= 0.0f
                    || not (accepted |> Seq.exists (fun p -> distanceSq p.Position pos < minSq))

                if farEnough then
                    let yaw =
                        if spec.RandomYaw then yawQuat (Rand.float01 extra * 6.2831853f)
                        else Quat.Identity

                    let s =
                        if spec.ScaleMax <= spec.ScaleMin then spec.ScaleMin
                        else spec.ScaleMin + (spec.ScaleMax - spec.ScaleMin) * Rand.float01 (Rand.hash extra)

                    let asset =
                        if spec.Assets.Length = 0 then ""
                        else spec.Assets.[int (Rand.hash (extra ^^^ 0x5bd1u) % uint32 spec.Assets.Length)]

                    accepted.Add
                        { Position = pos
                          Rotation = yaw
                          Scale = { X = s; Y = s; Z = s }
                          Asset = asset }

            accepted.ToArray()

    /// <summary>Standard fBm layers for a density volume.</summary>
    [<CompiledName("FbmLayers")>]
    let fbmLayers (count: int) (baseFrequency: float32) : NoiseLayer[] =
        Array.init (max 0 count) (fun i ->
            { Frequency = baseFrequency * (2.0f ** float32 i)
              Amplitude = 1.0f / (2.0f ** float32 i)
              Octaves = 4
              Lacunarity = 2.0f
              Gain = 0.5f
              SeedOffset = (i + 1) * 7919 })

    /// <summary>Builds a density-volume recipe. Produces PARAMETERS; the GPU produces the volume.</summary>
    [<CompiledName("DensityVolume")>]
    let densityVolume (seed: int) (region: Bounds) (res: int) (layers: NoiseLayer[]) : VolumeSpec =
        let r = max 1 res
        { Region = region
          ResX = r
          ResY = r
          ResZ = r
          Seed = seed
          Layers = layers
          CoverageFloor = 0.0f
          CoverageBias = 1.0f }

    /// <summary>The CPU reference for one voxel of a <see cref="VolumeSpec"/>.</summary>
    /// <remarks>
    /// THIS IS A CONTRACT, NOT A RENDERER. Nothing should fill a volume by calling this in a loop --
    /// that is the per-voxel CPU cost the whole design exists to avoid. It is here so the compute
    /// shader has something to be checked AGAINST: the same spec and coordinate must give the same
    /// density on both sides, the way the atmosphere model is mirrored function-for-function between
    /// C++ and HLSL. A GPU implementation with no CPU reference is a GPU implementation nobody can
    /// prove correct.
    /// </remarks>
    [<CompiledName("SampleDensity")>]
    let sampleDensity (spec: VolumeSpec) (x: int) (y: int) (z: int) : float32 =
        let mutable total = 0.0f
        let mutable norm = 0.0f

        for layer in spec.Layers do
            let mutable amp = layer.Amplitude
            let mutable freq = layer.Frequency
            for _octave in 1 .. max 1 layer.Octaves do
                let cx = int (float32 x * freq / float32 spec.ResX)
                let cy = int (float32 y * freq / float32 spec.ResY)
                let cz = int (float32 z * freq / float32 spec.ResZ)
                let v = Rand.float01 (Rand.hash3 (spec.Seed + layer.SeedOffset) cx cy cz)
                total <- total + v * amp
                norm <- norm + amp
                amp <- amp * layer.Gain
                freq <- freq * layer.Lacunarity

        if norm <= 0.0f then 0.0f
        else
            let d = total / norm
            if d < spec.CoverageFloor then 0.0f
            else
                let remapped = (d - spec.CoverageFloor) / max 1e-6f (1.0f - spec.CoverageFloor)
                remapped ** spec.CoverageBias

    /// <summary>Samples an INFINITE field at a world position, in centimetres.</summary>
    /// <remarks>
    /// The whole of "infinite expanse" is this function. It takes no region and no resolution: the
    /// lattice coordinate is world position divided by cell size, which is defined for every input
    /// including negative ones, so the field exists everywhere without anything being allocated.
    ///
    /// FLOOR, NOT TRUNCATION, and this is the one line where the infinite case genuinely differs
    /// from the bounded one. A bounded volume indexes from 0 upward, where int() and floor() agree.
    /// An infinite field is sampled at negative coordinates too, and there int() rounds TOWARD ZERO
    /// -- so cells -0.5 and +0.5 would both map to 0, making a lattice cell of double width
    /// straddling the origin. That is a visible seam at world zero and nowhere else, which is a
    /// wonderful bug to be handed with no explanation.
    /// </remarks>
    [<CompiledName("SampleInfinite")>]
    let sampleInfinite (spec: InfiniteSpec) (wx: float32) (wy: float32) (wz: float32) : float32 =
        let cell = if spec.CellSizeCm > 0.0f then spec.CellSizeCm else 1.0f
        let mutable total = 0.0f
        let mutable norm = 0.0f
        for layer in spec.Layers do
            let mutable amp = layer.Amplitude
            let mutable freq = layer.Frequency
            for _octave in 1 .. max 1 layer.Octaves do
                let s = freq / cell
                let cx = int (floor (wx * s))
                let cy = int (floor (wy * s))
                let cz = int (floor (wz * s))
                let v = Rand.float01 (Rand.hash3 (spec.Seed + layer.SeedOffset) cx cy cz)
                total <- total + v * amp
                norm <- norm + amp
                amp <- amp * layer.Gain
                freq <- freq * layer.Lacunarity

        if norm <= 0.0f then 0.0f
        else
            let d = total / norm
            if d < spec.CoverageFloor then 0.0f
            else
                let remapped = (d - spec.CoverageFloor) / max 1e-6f (1.0f - spec.CoverageFloor)
                remapped ** spec.CoverageBias

    /// <summary>An infinite field from a seed, with sensible fBm layers.</summary>
    [<CompiledName("InfiniteExpanse")>]
    let infiniteExpanse (seed: int) (cellSizeCm: float32) (octaveCount: int) : InfiniteSpec =
        { Seed = seed
          Layers = fbmLayers octaveCount 1.0f
          CellSizeCm = if cellSizeCm > 0.0f then cellSizeCm else 100.0f
          CoverageFloor = 0.0f
          CoverageBias = 1.0f }
