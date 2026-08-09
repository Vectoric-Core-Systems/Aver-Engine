#pragma once
// A ray against one section's surface: what a cursor ray needs to find a sculpt brush's centre.
// Pure CPU, like the rest of Aver.Landscape -- no rhi:: types, no device. The caller (an editor's
// viewportRay()) already turned screen space into a world-space ray; this only walks it.
#include "aver/formats/OcLand.hpp"

namespace aver::landscape {

using aver::f32;
using aver::u32;

// Where a ray met the section's bilinearly-interpolated surface.
struct HeightfieldHit {
    f32 posCm[3] = {0.0f, 0.0f, 0.0f};   // world hit point, ON the surface (z is the terrain's, not the ray's)
    f32 distCm = 0.0f;                    // distance from `ro` along the (normalised) ray
};

// Marches from `ro` along `rd` (need not be unit length) and finds where it first crosses the
// section's surface. False when the ray never enters the section's horizontal footprint, runs
// parallel to it without crossing, or crosses nothing within `maxDistCm`.
//
// The footprint is clipped first with a slab test against the section's XY bounds, so a ray that
// misses the section entirely costs one bounding-box check, not a march. Inside the footprint the
// walk steps in half-spacing increments -- fine enough that a one-sample-wide spike cannot be
// stepped over -- and a bisection refines the crossing once one is bracketed.
bool raycastHeightfield(const fmt::OcLandData& data, const f32 ro[3], const f32 rd[3],
                         HeightfieldHit& out, f32 maxDistCm = 200000.0f);

// The surface height directly above/below a world (x, y): a straight bilinear lookup, no marching.
//
// SEPARATE FROM raycastHeightfield ON PURPOSE. A vertical query is the common case -- "put this
// plant on the ground" -- and answering it by casting a downward ray would march the grid to
// rediscover a cell that one division already names. This is the call a chunk generator's
// heightSource makes once per candidate, thousands of times per chunk, so the difference is the
// whole cost.
//
// RETURNS FALSE OUTSIDE THE FOOTPRINT, and that distinction is the point. sampleHeight clamps to the
// edge samples internally, which is right for a ray clipping a corner and WRONG for a scatter
// candidate: clamping would smear the section's rim height across the entire rest of the world, so
// every plant beyond the terrain would sit at whatever the nearest edge happened to be. A caller
// that gets false should fall back to its own flat behaviour, which is what leaving
// GeneratorSettings::heightSource unset already does.
bool surfaceHeightAt(const fmt::OcLandData& data, f32 worldXCm, f32 worldYCm, f32& outWorldZCm);

} // namespace aver::landscape
