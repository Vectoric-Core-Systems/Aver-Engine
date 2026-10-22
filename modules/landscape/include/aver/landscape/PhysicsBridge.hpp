#pragma once
// Turns a landscape section into the sample array `aver_phys_add_heightfield` wants.
// Physics grid axes disagree with the section's by a transpose and a row flip: a physics column runs
// along engine +Y and a physics row along engine -X, so the corner is the field's +X/-Y corner.
#include "aver/formats/OcLand.hpp"

#include <vector>

namespace aver::landscape {

using aver::f32;
using aver::i32;
using aver::u8;
using aver::u32;

// The sample array, its spacing and the corner to pass as (cx, cy, cz).
struct PhysicsHeightfield {
    std::vector<f32> samples;   // row-major in PHYSICS order, sampleCount^2
    u32 sampleCount = 0;
    f32 spacingCm = 0.0f;
    f32 cornerCm[3] = {0.0f, 0.0f, 0.0f};   // the field's +X/-Y corner

    // PHYSICAL SURFACES PER CELL (SHAPES_MATERIALS). Empty unless toPhysicsHeightfield was handed a surface for each paint layer.
    // cellPalette is (sampleCount-1)^2 palette indices in PHYSICS order (cell (x, y) between physics columns x..x+1 and rows
    // y..y+1), each the DOMINANT paint layer of that cell: the layer with the largest weight summed over the cell's four corners,
    // ties to the lower layer, as landscape::dominantLayer reads the same weights. palette holds the surface ids those indices
    // name, one per distinct surface among the section's layers (at most 8).
    std::vector<u8> cellPalette;
    std::vector<i32> palette;
};

// Converts a section. Returns false only for a section that is not internally consistent.
//
// `sectionLayerSurface`, when given, holds one surface id per SECTION layer (layerNames order; an unpainted section is one
// layer, its base), 0 for a layer with no surface. With at least one non-zero id the cell palette above is built; without
// it (null, empty, or all zero) the result is exactly what it was before surfaces existed.
bool toPhysicsHeightfield(const fmt::OcLandData& data, PhysicsHeightfield& out,
                          const std::vector<i32>* sectionLayerSurface = nullptr);

} // namespace aver::landscape
