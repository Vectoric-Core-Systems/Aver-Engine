// Tile coordinates and procedural tile synthesis: see TerrainTile.hpp.
#include "aver/landscape/TerrainTile.hpp"

#include <cmath>

namespace aver::landscape {

TileCoord tileAt(f32 worldXCm, f32 worldYCm, f32 centreXCm, f32 centreYCm, f32 tileSizeCm) {
    TileCoord t;
    if (tileSizeCm > 0.0f) {
        t.tx = static_cast<i32>(std::lround((worldXCm - centreXCm) / tileSizeCm));
        t.ty = static_cast<i32>(std::lround((worldYCm - centreYCm) / tileSizeCm));
    }
    return t;
}

void tileCornerCm(TileCoord t, f32 centreXCm, f32 centreYCm, f32 tileSizeCm, f32 outXY[2]) {
    outXY[0] = centreXCm + static_cast<f32>(t.tx) * tileSizeCm - tileSizeCm * 0.5f;
    outXY[1] = centreYCm + static_cast<f32>(t.ty) * tileSizeCm - tileSizeCm * 0.5f;
}

bool synthesizeTerrainTile(TileCoord t, f32 centreXCm, f32 centreYCm, f32 tileSizeCm,
                           u32 samplesPerTile, const TerrainNoiseParams& noise, fmt::OcLandData& out) {
    if (samplesPerTile < fmt::kOcLandMinSamples || !(tileSizeCm > 0.0f)) return false;
    const f32 spacing = tileSizeCm / static_cast<f32>(samplesPerTile - 1);
    if (!(spacing > 0.0f)) return false;

    f32 corner[2];
    tileCornerCm(t, centreXCm, centreYCm, tileSizeCm, corner);

    out.sampleCount = samplesPerTile;
    out.spacingCm = spacing;
    out.originCm[0] = corner[0];
    out.originCm[1] = corner[1];
    out.originCm[2] = 0.0f;
    out.heights.assign(static_cast<usize>(samplesPerTile) * samplesPerTile, 0.0f);

    f32 lo = 1e30f, hi = -1e30f;
    for (u32 iy = 0; iy < samplesPerTile; ++iy) {
        for (u32 ix = 0; ix < samplesPerTile; ++ix) {
            const f32 wx = corner[0] + static_cast<f32>(ix) * spacing;
            const f32 wy = corner[1] + static_cast<f32>(iy) * spacing;
            const f32 h = terrainHeightAt(wx, wy, noise);
            out.heights[static_cast<usize>(iy) * samplesPerTile + ix] = h;
            lo = std::fmin(lo, h);
            hi = std::fmax(hi, h);
        }
    }

    // Mirrors what fmt::OcLand.cpp computes on load and aver::landscape::Sculpt recomputes after an
    // edit -- nothing in this module reads it (LandscapeTree::build works from heights/originCm/
    // spacingCm/sampleCount alone), but a consumer outside it may, and a tile built here should look
    // exactly like one loaded from disk.
    out.boundsMin[0] = out.originCm[0];
    out.boundsMin[1] = out.originCm[1];
    out.boundsMin[2] = lo;
    out.boundsMax[0] = out.originCm[0] + out.extentCm();
    out.boundsMax[1] = out.originCm[1] + out.extentCm();
    out.boundsMax[2] = hi;

    return out.valid();
}

} // namespace aver::landscape
