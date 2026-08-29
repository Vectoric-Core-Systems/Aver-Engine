#pragma once
// Brush edits to a landscape section's heights: raise, lower, smooth, flatten. Pure CPU, in place --
// like the rest of Aver.Landscape, no rhi:: types, no device. What happens to the SELECTED, BUILT
// quadtree and the RESIDENT gpu meshes an edit can make stale is the caller's job (LandscapeTree::
// build() and LandscapeRenderer::forgetOverlapping() respectively); this module only owns the heights.
#include "aver/formats/OcLand.hpp"

namespace aver::landscape {

using aver::f32;
using aver::u32;

enum class BrushMode { Raise, Lower, Smooth, Flatten };

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
