#pragma once
// Deterministic, seamless value noise for terrain HEIGHT.
//
// DELIBERATELY NOT pcg::sampleInfinite (modules/render.pcg): that function is a per-lattice-cell
// CONSTANT with no interpolation between cells -- exactly right for the scatter density field it
// answers WHETHER decisions for, and visibly terraced (a staircase, not a hill) the moment it is read
// directly as a height. Density decides whether something is placed; height decides where it sits,
// and the two are not the same signal (see tests/landscape/src/TerrainGenTool.cpp's own header for
// the longer version of this argument -- this is that tool's noise, ported here so it is linkable
// rather than copy-pasted a second time).
//
// PORTED VERBATIM from TerrainGenTool.cpp's hash2/valueNoise/fbm: same integer hash, same smoothstep
// fade, same ridged-fBm fold. Bit-for-bit the same formula, confirmed by regenerating
// Content/Maps/Default.ocland with TerrainGenTool's own (matching) defaults and comparing byte for
// byte -- identical. That is what makes a section this noise fills and a section TerrainGenTool wrote
// to disk agree at a shared edge with no seam-fixing pass: they are the same function evaluated at the
// same world (x, y), not two approximations of one idea.
//
// PURE FUNCTION OF WORLD (x, y). No section, no grid, no state -- exactly as cheap for a single height
// QUERY (a chunk generator's heightSource) as it is per-sample when filling a whole tile's grid, and
// the two calls can never disagree because they are the same call.
#include "aver/core/Types.hpp"

namespace aver::landscape {

using aver::f32;
using aver::i32;
using aver::u32;

// The knobs TerrainGenTool.cpp exposed on its command line, minus sampleCount/spacingCm (those
// describe a GRID a caller lays over the field, not the field itself) and minus an origin (the
// caller's own placement). Defaults match the tool's own defaults, which is what
// Content/Maps/Default.ocland was generated with.
struct TerrainNoiseParams {
    u32 seed = 20260809u;
    f32 featureSizeCm = 9000.0f;   // world size one noise cell covers
    f32 amplitudeCm = 900.0f;      // peak-to-zero height scale
    i32 octaves = 5;
};

// World-Z at (worldXCm, worldYCm): ridged fBm, amplitude-scaled, zero-centred at the origin of the
// noise field (not necessarily at world (0,0) -- the field itself has no origin; a section merely
// samples a window of it). Deterministic across runs, platforms and build types: integer hashing and
// no floating-point accumulation whose rounding could differ.
f32 terrainHeightAt(f32 worldXCm, f32 worldYCm, const TerrainNoiseParams& params = {});

} // namespace aver::landscape
