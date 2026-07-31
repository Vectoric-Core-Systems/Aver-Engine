#pragma once
// Turns a landscape section into the sample array `aver_phys_add_heightfield` wants.
// Physics grid axes disagree with the section's by a transpose and a row flip: a physics column runs
// along engine +Y and a physics row along engine -X, so the corner is the field's +X/-Y corner.
#include "aver/formats/OcLand.hpp"

#include <vector>

namespace aver::landscape {

using aver::f32;
using aver::u32;

// The sample array, its spacing and the corner to pass as (cx, cy, cz).
struct PhysicsHeightfield {
    std::vector<f32> samples;   // row-major in PHYSICS order, sampleCount^2
    u32 sampleCount = 0;
    f32 spacingCm = 0.0f;
    f32 cornerCm[3] = {0.0f, 0.0f, 0.0f};   // the field's +X/-Y corner
};

// Converts a section. Returns false only for a section that is not internally consistent.
bool toPhysicsHeightfield(const fmt::OcLandData& data, PhysicsHeightfield& out);

} // namespace aver::landscape
