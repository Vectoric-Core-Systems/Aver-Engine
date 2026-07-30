#pragma once
// Turning a landscape section into the sample array `aver_phys_add_heightfield` wants.
//
// THIS IS NOT A RELABELLING, and the whole file exists because it looks like one. Jolt builds a
// heightfield in its OWN axes -- Y-up, with its grid rows and columns laid out to suit that -- and
// Aver.Physics converts only the body's CENTRE through the axis map. Compose the two and a physics
// sample at (column x, row y) lands at engine
//
//     (cornerX - y*spacing,  cornerY + x*spacing,  cornerZ + h)
//
// so a physics COLUMN runs along engine +Y and a physics ROW runs along engine -X. A landscape section
// has its column along +X and its row along +Y (see OcLand.hpp). Those disagree by a transpose AND a
// row flip, and feeding the grid across unchanged gives terrain that collides ninety degrees from
// where it is drawn -- which presents as "the player walks on invisible ground", a long way from here.
//
// It also means the corner passed to the ABI is the field's +X/-Y corner, NOT its minimum corner.
//
// EVIDENCE, not derivation alone: tests/physics builds a 9x9 field pinned at the origin and finds it
// occupying engine x in [-1600, 0] -- negative, i.e. the row running along -X, exactly as above. The
// first version of that test swept x in [-200, 1800] on the assumption they agreed, and every ray
// missed.
//
// NO PHYSICS DEPENDENCY. This emits a plain array and three floats; the caller hands them to the ABI.
// That keeps Aver.Landscape free of Aver.Physics -- terrain that could not be built without a physics
// module would be terrain no headless test could check.
#include "aver/formats/OcLand.hpp"

#include <vector>

namespace aver::landscape {

using aver::f32;
using aver::u32;

// The sample array and the corner to pass as (cx, cy, cz).
//
// `spacingCm` comes back too, unchanged, purely so a caller has every argument to the ABI call in one
// place and cannot pair a converted grid with the wrong spacing.
struct PhysicsHeightfield {
    std::vector<f32> samples;   // row-major in PHYSICS order, sampleCount^2
    u32 sampleCount = 0;
    f32 spacingCm = 0.0f;
    f32 cornerCm[3] = {0.0f, 0.0f, 0.0f};   // the field's +X/-Y corner -- see the note above
};

// Convert. Returns false only for a section that is not internally consistent.
//
// Every sample is carried across: the ABI no longer crops to a multiple of 8, which it used to do on
// the false belief that Jolt required it. That crop is what made a shared edge row impossible, and it
// is why sections are sized (k*q)+1 in the first place.
bool toPhysicsHeightfield(const fmt::OcLandData& data, PhysicsHeightfield& out);

} // namespace aver::landscape
