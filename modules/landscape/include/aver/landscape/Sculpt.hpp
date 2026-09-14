#pragma once
// Brush edits to a landscape section's heights: raise, lower, smooth, flatten, ramp, noise. Pure CPU,
// in place -- like the rest of Aver.Landscape, no rhi:: types, no device. What happens to the
// SELECTED, BUILT quadtree and the RESIDENT gpu meshes an edit can make stale is the caller's job
// (LandscapeTree::build() and LandscapeRenderer::forgetOverlapping() respectively); this module only
// owns the heights.
//
// EXPLICITLY OUT OF SCOPE: hydraulic/thermal erosion. That is a materially bigger CPU algorithm (an
// iterative sediment-transport simulation, not a per-sample closed form like every brush below) and
// is its own planned item -- see plan item 5.2's own notes.
#include "aver/formats/OcLand.hpp"

namespace aver::landscape {

using aver::f32;
using aver::u32;

enum class BrushMode { Raise, Lower, Smooth, Flatten, Ramp, Noise };

// One brush application. `centerCm` is the world XY the cursor ray hit (see HeightfieldRay.hpp);
// radiusCm and strength are the brush's own. `flattenTargetCm` is read only by Flatten, and is
// normally the height the FIRST sample of a stroke picked, captured once and held for the whole
// drag -- so a stroke flattens toward one plane instead of chasing wherever the cursor is now.
struct BrushParams {
    f32 centerCm[2] = {0.0f, 0.0f};
    f32 radiusCm = 500.0f;
    f32 strength = 200.0f;      // cm/sample applied at the brush centre at full weight (amount = 1)
    BrushMode mode = BrushMode::Raise;
    f32 flattenTargetCm = 0.0f;

    // Edge softness, 0..1. The radial weight was a fixed smoothstep with no way to reach it, which
    // made every brush in the editor the same shape -- fine for blocking terrain out, wrong for
    // cutting a road edge or building a ridge, where a hard rim is the entire point.
    //
    // 1 is that original smoothstep, so a caller that never sets this gets exactly the previous
    // behaviour. 0 is a hard disc: full weight to the rim, nothing past it. Values between raise the
    // curve toward a plateau, widening the flat top and narrowing the shoulder.
    f32 falloff = 1.0f;

    // Ramp only: its OTHER endpoint. A ramp needs two (position, height) pairs to interpolate
    // between. `centerCm` above -- the brush's current position, which is all a moving stroke ever
    // gives a caller to work with -- already supplies one end (position) each tick; `strength` above
    // supplies its height, reused as the ramp's total RISE from start to current rather than a
    // per-tick amount, the same way Raise/Lower already overload it as "how far", just measured
    // end-to-end instead of per tick. That leaves exactly one thing a moving centerCm cannot give:
    // where the stroke BEGAN. `rampStartCm`/`rampStartHeightCm` capture that, and -- like
    // flattenTargetCm above -- must be captured exactly ONCE, at the first sample of the stroke, or
    // the ramp's start would chase the cursor along with everything else and never hold a fixed
    // slope. The direction of the ramp is then simply centerCm - rampStartCm; no separate direction
    // field is needed.
    f32 rampStartCm[2] = {0.0f, 0.0f};
    f32 rampStartHeightCm = 0.0f;

    // Noise only: the per-stroke seed. Passed straight through to TerrainNoiseParams::seed (see
    // TerrainNoise.hpp), which is already a PURE function of world (x, y) and this seed -- no
    // internal state, no wall-clock read -- so the determinism this needs ("same stroke twice,
    // identical result") falls out for free as long as the CALLER seeds it from something stable
    // rather than the clock. Captured once per stroke, same idiom as flattenTargetCm/rampStartCm:
    // the editor seeds it from the stroke's start position (see SandboxApp.cpp's handleSculpt), so
    // replaying a recorded stroke -- or a test constructing BrushParams directly -- reproduces the
    // exact same terrain every time.
    u32 noiseSeed = 0;
};

// The inclusive sample-space rectangle a brush can reach, clamped to the section's grid.
struct BrushRect {
    u32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty = true;   // true when the brush never overlaps a sample (off-grid, or radius <= 0)
};

// The rect a brush touches, without editing anything -- what a caller invalidates ahead of a stroke,
// or checks before bothering to call applyBrush() at all.
BrushRect brushRect(const fmt::OcLandData& data, const BrushParams& p);

// Applies one stroke TICK to `data.heights` in place, with a smoothstep radial falloff (1 at the
// centre, 0 at the rim). `amount` scales the per-mode strength, so a caller integrating over a
// frame's dt (or a single discrete click, at amount = 1) controls how much one tick moves the
// surface. Recomputes `data.boundsMin`/`boundsMax` when anything actually changed.
//
// Returns the same rect brushRect() would for these params -- what the caller must invalidate --
// empty when the brush touched no sample, in which case `data` is left untouched.
BrushRect applyBrush(fmt::OcLandData& data, const BrushParams& p, f32 amount = 1.0f);

} // namespace aver::landscape
