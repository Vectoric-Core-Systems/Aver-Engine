// The boundary types of the PCG API: what F# hands to C#.
//
// EVERY TYPE HERE IS DELIBERATELY BORING. Records of primitives and arrays, no F# lists, no
// options, no discriminated unions, no tuples. Those are excellent F# and they are poor C#: an
// FSharpList<T> has no C# literal, FSharpOption<T> is not a C# nullable, and a tuple crossing the
// boundary arrives as System.Tuple with .Item1 fields. The rule agreed for this API is that F#'s
// expressive types are for F#'s INSIDE, and the boundary is arrays and plain records.
//
// UNITS. The engine's unit is the CENTIMETRE, +Z is up, +X is forward, and the basis is
// left-handed. A generator that thinks in metres will silently produce a world 100x too small,
// which looks like a scale bug in the renderer and is not one. Every distance in this API is
// centimetres.

namespace Aver.Pcg

open System

/// <summary>A position or direction in engine units (centimetres, +Z up).</summary>
[<CLIMutable>]
type Vec3 =
    { X: float32
      Y: float32
      Z: float32 }
    static member Zero = { X = 0.0f; Y = 0.0f; Z = 0.0f }
    static member One  = { X = 1.0f; Y = 1.0f; Z = 1.0f }

/// <summary>A rotation. Components match the engine's quaternion layout.</summary>
[<CLIMutable>]
type Quat =
    { X: float32
      Y: float32
      Z: float32
      W: float32 }
    static member Identity = { X = 0.0f; Y = 0.0f; Z = 0.0f; W = 1.0f }

/// <summary>An axis-aligned region in world space, in centimetres.</summary>
[<CLIMutable>]
type Bounds =
    { Min: Vec3
      Max: Vec3 }
    member this.Size =
        { X = this.Max.X - this.Min.X
          Y = this.Max.Y - this.Min.Y
          Z = this.Max.Z - this.Min.Z }

/// <summary>One thing to place in the world: what, where, and how oriented.</summary>
/// <remarks>Asset is a forward-slash relative path under Content/, because that is exactly what the
/// engine hashes with FNV-1a-64 to produce an ObjectId. Handing back a backslash path, or an
/// absolute one, produces a different hash and therefore a reference to nothing.</remarks>
[<CLIMutable>]
type Placement =
    { Position: Vec3
      Rotation: Quat
      Scale: Vec3
      Asset: string }

/// <summary>One layer of a procedural density field.</summary>
/// <remarks>THIS IS A PARAMETER BLOCK, NOT VOXELS. The decision recorded for this API is that F#
/// owns the RULES and a compute shader owns the volume: a 128^3 field is 2 million voxels and
/// 256^3 is 16 million, and filling those in idiomatic F# means allocating per voxel. Written with
/// flat arrays and mutable loops it would be fast enough and would also have stopped being F# worth
/// writing. So this type is kilobytes that describe a volume, and the GPU produces the volume.</remarks>
[<CLIMutable>]
type NoiseLayer =
    { /// Cells per world axis at the base octave. Larger = finer detail.
      Frequency: float32
      /// Contribution of this layer before normalisation.
      Amplitude: float32
      /// Number of fBm octaves.
      Octaves: int
      /// Frequency multiplier between octaves. 2.0 is standard.
      Lacunarity: float32
      /// Amplitude multiplier between octaves. 0.5 is standard.
      Gain: float32
      /// Per-layer seed offset, so two layers of one field never correlate.
      SeedOffset: int }

/// <summary>A density field with NO BOUNDS: defined everywhere, forever.</summary>
/// <remarks>
/// THE INFINITE CASE IS THE NATURAL ONE, and the bounded VolumeSpec below is the special case.
/// Hash noise is a pure function of a lattice coordinate, so it already has a value at every
/// integer cell in every direction; a region only exists to map voxel INDICES onto world space.
/// Drop that mapping and sample world position directly and the field is simply infinite -- the
/// sky, the ocean, a cave system, weather. Nothing is generated until something asks.
///
/// It is also what makes seeded chunk generation work. Because the value at a world position
/// depends on NOTHING but the seed and that position, a chunk can be generated alone, out of order,
/// on another thread or a year later, and agree with every neighbour it has never met. A field that
/// needed its region to be evaluated could not do that, which is why streaming systems that bolt
/// bounds onto noise end up with seams.
///
/// CellSizeCm is the only extra parameter: how much world distance one lattice cell spans. It plays
/// the part Res played for a bounded volume, and it is in CENTIMETRES like everything else.
/// </remarks>
[<CLIMutable>]
type InfiniteSpec =
    { /// Master seed. Same seed and same layers give the same infinite field, always.
      Seed: int
      Layers: NoiseLayer[]
      /// World centimetres spanned by one lattice cell.
      CellSizeCm: float32
      CoverageFloor: float32
      CoverageBias: float32 }

/// <summary>The full recipe for a BOUNDED density volume, ready to hand to a compute shader.</summary>
[<CLIMutable>]
type VolumeSpec =
    { /// The volume's world extent, in centimetres.
      Region: Bounds
      /// Resolution per axis. The GPU allocates this; F# never does.
      ResX: int
      ResY: int
      ResZ: int
      /// Master seed. Same seed and same layers must give the same volume, always.
      Seed: int
      Layers: NoiseLayer[]
      /// Density below this is cleared to zero, which is what carves shapes out of noise.
      CoverageFloor: float32
      /// Density is remapped so CoverageFloor..1 becomes 0..1 before this power is applied.
      /// Above 1 sharpens edges, below 1 softens them.
      CoverageBias: float32 }
