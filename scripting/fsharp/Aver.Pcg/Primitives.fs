// Additional PCG primitives: surfaces, splines and filtering.
//
// Every one is a pure function of its arguments and shares Rand's stateless splitmix32, so they
// compose freely and any of them can be evaluated in any order, in parallel, or partially, and
// still produce identical output. That is not a nicety: it is what lets a chunked world generate
// region 5 before region 2 and get the same world either way.

namespace Aver.Pcg

open System

/// <summary>A point on a surface: where it is, and which way is up there.</summary>
[<CLIMutable>]
type SurfacePoint =
    { Position: Vec3
      Normal: Vec3 }

/// <summary>One control point of a path. Positions are in CENTIMETRES, like everything else.</summary>
[<CLIMutable>]
type SplinePoint =
    { Position: Vec3 }

/// <summary>Placement primitives beyond a uniform scatter.</summary>
module Primitives =

    let private lerp (a: float32) (b: float32) (t: float32) = a + (b - a) * t

    let private lerpVec (a: Vec3) (b: Vec3) (t: float32) =
        { X = lerp a.X b.X t; Y = lerp a.Y b.Y t; Z = lerp a.Z b.Z t }

    let private lengthVec (v: Vec3) = sqrt (v.X * v.X + v.Y * v.Y + v.Z * v.Z)

    let private yawQuat (radians: float32) : Quat =
        let h = radians * 0.5f
        { X = 0.0f; Y = 0.0f; Z = sin h; W = cos h }

    /// <summary>Scatters over a surface described by a height function.</summary>
    /// <remarks>
    /// The caller supplies the surface because PCG must not know what a landscape is: given (x, y)
    /// it returns a point and a normal. That keeps this usable over a heightfield, a mesh, an
    /// analytic function or a stub, and keeps Aver.Pcg free of any engine reference.
    ///
    /// MaxSlope rejects steep ground, which is what stops trees growing on cliff faces. It is a
    /// rejection like MinSpacing, so the returned count is usually LESS than asked for and the
    /// caller reads the array length rather than assuming it got what it requested.
    /// </remarks>
    [<CompiledName("ScatterOnSurface")>]
    let scatterOnSurface (seed: int) (region: Bounds) (count: int)
                         (maxSlopeDegrees: float32) (assets: string[])
                         (surfaceAt: Func<float32, float32, SurfacePoint>) : Placement[] =
        if count <= 0 || isNull (box surfaceAt) then [||]
        else
            let size = region.Size
            // cos of the slope limit, compared against the normal's Z. +Z is up in this engine, so
            // flat ground has normal.Z = 1 and a vertical wall has 0.
            let minUp = cos (maxSlopeDegrees * 0.017453292f)
            let accepted = ResizeArray<Placement>(count)
            for i in 0 .. count - 1 do
                let px = Rand.float01 (Rand.hash2 seed (i * 3))
                let py = Rand.float01 (Rand.hash2 seed (i * 3 + 1))
                let extra = Rand.hash2 seed (i * 3 + 2)
                let wx = region.Min.X + size.X * px
                let wy = region.Min.Y + size.Y * py
                let sp = surfaceAt.Invoke(wx, wy)
                if sp.Normal.Z >= minUp then
                    let asset =
                        if assets.Length = 0 then ""
                        else assets.[int (Rand.hash extra % uint32 assets.Length)]
                    accepted.Add
                        { Position = sp.Position
                          Rotation = yawQuat (Rand.float01 extra * 6.2831853f)
                          Scale = Vec3.One
                          Asset = asset }
            accepted.ToArray()

    /// <summary>Places along a polyline at a fixed spacing, oriented along it.</summary>
    /// <remarks>
    /// Walks by ARC LENGTH rather than by control point, so spacing is uniform whether the control
    /// points are dense or sparse. One placement per segment is the obvious implementation and
    /// gives fence posts that bunch on curves and stretch on straights.
    ///
    /// Jitter is applied ACROSS the path, never along it, so a jittered fence still reads as a
    /// fence rather than as a queue with gaps.
    /// </remarks>
    [<CompiledName("ScatterAlongSpline")>]
    let scatterAlongSpline (seed: int) (points: SplinePoint[]) (spacingCm: float32)
                           (jitterCm: float32) (assets: string[]) : Placement[] =
        if points.Length < 2 || spacingCm <= 0.0f then [||]
        else
            let out = ResizeArray<Placement>()
            let mutable carry = 0.0f      // distance already consumed into the next segment
            let mutable index = 0
            for s in 0 .. points.Length - 2 do
                let a = points.[s].Position
                let b = points.[s + 1].Position
                let seg = { X = b.X - a.X; Y = b.Y - a.Y; Z = b.Z - a.Z }
                let segLen = lengthVec seg
                if segLen > 1e-4f then
                    let dirX = seg.X / segLen
                    let dirY = seg.Y / segLen
                    let mutable t = carry
                    while t < segLen do
                        let basePos = lerpVec a b (t / segLen)
                        let h = Rand.hash2 seed index
                        // Perpendicular in the XY plane: a fence leans sideways, not upward.
                        let j = if jitterCm > 0.0f then (Rand.float01 h - 0.5f) * 2.0f * jitterCm else 0.0f
                        let pos = { X = basePos.X - dirY * j; Y = basePos.Y + dirX * j; Z = basePos.Z }
                        let asset =
                            if assets.Length = 0 then ""
                            else assets.[int (Rand.hash (h ^^^ 0x9e37u) % uint32 assets.Length)]
                        out.Add
                            { Position = pos
                              Rotation = yawQuat (atan2 dirY dirX)
                              Scale = Vec3.One
                              Asset = asset }
                        index <- index + 1
                        t <- t + spacingCm
                    // Carry the overshoot into the next segment. Resetting to zero here is what
                    // makes spacing restart at every control point.
                    carry <- t - segLen
            out.ToArray()

    /// <summary>Keeps the placements a predicate accepts. Order is preserved.</summary>
    /// <remarks>Order preservation is the contract that matters: it is what lets a caller filter,
    /// then filter again, and get the same result as filtering once with both predicates.</remarks>
    [<CompiledName("Filter")>]
    let filter (predicate: Func<Placement, bool>) (placements: Placement[]) : Placement[] =
        if isNull (box predicate) then placements
        else placements |> Array.filter predicate.Invoke

    /// <summary>Keeps placements whose density sample clears a threshold.</summary>
    /// <remarks>
    /// THE BRIDGE BETWEEN THE TWO HALVES OF THIS API: a density field decides where scattered
    /// things survive, so "trees only where the forest mask is strong" is one call rather than a
    /// hand-written loop. Samples the CPU reference, so it suits hundreds-to-thousands of
    /// placements; a dense field belongs on the GPU.
    /// </remarks>
    [<CompiledName("FilterByDensity")>]
    let filterByDensity (spec: VolumeSpec) (threshold: float32) (placements: Placement[]) : Placement[] =
        let region = spec.Region
        let size = region.Size
        let axis (v: float32) (lo: float32) (span: float32) (res: int) =
            if span <= 0.0f then 0
            else
                let i = int ((v - lo) / span * float32 res)
                if i < 0 then 0 elif i >= res then res - 1 else i
        placements
        |> Array.filter (fun p ->
            let x = axis p.Position.X region.Min.X size.X spec.ResX
            let y = axis p.Position.Y region.Min.Y size.Y spec.ResY
            let z = axis p.Position.Z region.Min.Z size.Z spec.ResZ
            Pcg.sampleDensity spec x y z >= threshold)
