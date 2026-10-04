#pragma once
// Tiling an otherwise-single .ocland SECTION so terrain the level did not author is still there: an
// authored section keeps its own heights and WINS across its own footprint (nothing here touches it);
// every OTHER tile is synthesized from terrainHeightAt (TerrainNoise.hpp) -- the SAME pure function of
// world (x, y) TerrainGenTool used to originate an authored section in the first place (see
// TerrainNoise.hpp's header for the byte-identical proof). Two adjacent tiles' shared edge row is
// therefore identical up to floating-point rounding BY CONSTRUCTION: it is one continuous field read
// at two overlapping windows, not two fields stitched together.
//
// This is the CPU half only, same split the rest of this module keeps: a TileCoord and an OcLandData
// are plain data, with no opinion about a quadtree, a mesh, or a device. A caller (the streamer,
// currently sandbox/src/SandboxApp.cpp -- see docs/LANDSCAPE_EDITOR.md's blocker 9 before adding a
// second one) builds a LandscapeTree/LandscapeRenderer over the result exactly as it would for any
// other section.
#include "aver/landscape/TerrainNoise.hpp"
#include "aver/formats/OcLand.hpp"

#include <functional>

namespace aver::landscape {

// One tile's grid coordinate, relative to wherever the CALLER's tile (0, 0) is centred -- there is no
// fixed world origin baked in here, because a level's LANDSCAPE record can place its authored section
// anywhere (`at x y z`), and the grid has to follow that placement so the authored tile is still (0,0).
struct TileCoord {
    i32 tx = 0, ty = 0;
    bool operator==(const TileCoord& o) const { return tx == o.tx && ty == o.ty; }
    bool operator!=(const TileCoord& o) const { return !(*this == o); }
};

// Which tile (worldXCm, worldYCm) falls in, given tile (0,0)'s own CENTRE at (centreXCm, centreYCm)
// and a square `tileSizeCm` a side. `round`, not `floor`, because tile (0,0) is defined to be CENTRED
// on (centreXCm, centreYCm) -- it spans centre +/- tileSizeCm/2, matching how an authored section's own
// footprint sits relative to its centre.
TileCoord tileAt(f32 worldXCm, f32 worldYCm, f32 centreXCm, f32 centreYCm, f32 tileSizeCm);

// The world position of tile `t`'s own sample (0,0) -- its corner, not its centre; assignable straight
// to OcLandData::originCm[0]/[1].
void tileCornerCm(TileCoord t, f32 centreXCm, f32 centreYCm, f32 tileSizeCm, f32 outXY[2]);

// Fills a fresh OcLandData for tile `t`: `samplesPerTile` samples a side (must satisfy
// LandscapeTree::build's own tiling rule -- pass the SAME sampleCount an authored neighbour uses, and
// it does, since that section already validated), spaced so the tile spans exactly `tileSizeCm`,
// heights from terrainHeightAt. Returns false (and leaves `out` unspecified) on a degenerate request --
// same failure shape as fmt::loadOcLand, checked with OcLandData::valid() besides.
bool synthesizeTerrainTile(TileCoord t, f32 centreXCm, f32 centreYCm, f32 tileSizeCm,
                           u32 samplesPerTile, const TerrainNoiseParams& noise, fmt::OcLandData& out);

} // namespace aver::landscape

// So a bare std::unordered_map<aver::landscape::TileCoord, V> works with no caller-supplied hasher --
// every current caller (SandboxApp's ring-tile map) wants exactly this key, and there is no second,
// competing notion of "hash a tile coordinate" for a caller to prefer instead.
template <>
struct std::hash<aver::landscape::TileCoord> {
    aver::usize operator()(const aver::landscape::TileCoord& t) const noexcept {
        const aver::usize ux = static_cast<aver::u32>(t.tx);
        const aver::usize uy = static_cast<aver::u32>(t.ty);
        return (ux * 73856093u) ^ (uy * 19349663u);
    }
};
