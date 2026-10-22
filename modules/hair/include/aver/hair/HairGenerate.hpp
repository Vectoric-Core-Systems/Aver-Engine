#pragma once
// The procedural groom (docs/plans/wave11/SOFTBODY_HAIR.md section 2.13, source 1): strands grown from a scalp mesh by a GENERATE record. Pure
// arithmetic on arrays -- no scene, no GPU, no Jolt, no texture decoder -- so tests/hair decides it on a machine with none of them.
//
// DETERMINISTIC from the seed: the same record and the same scalp give the same bytes (a PCG32 stream, no clock, no rand). Everything below is
// textbook procedural placement written from this description; no external code.
//
//   ROOTS      area-weighted barycentric sampling on the scalp triangles, weighted by the per-vertex mask, with a minimum spacing by dart
//              rejection (spacing from the strand count and the masked area; it relaxes by 15% a round when the scalp cannot hold the count).
//   SHAPE      a strand starts along the scalp normal and bends towards `style` (the "fall" direction of long hair) by `styleWeight` along its
//              length, smoothly. The part pushes roots on either side of the part line away from it, most at the root.
//   CLUMPS     `clumps` guide strands are drawn among the roots; every other strand within clumpRadiusCm of a guide pulls toward the guide's curve
//              by clumpTightness * (fraction along the strand), the usual guide-and-follower construction. Roots never move.
//   CURL       a helix around the strand axis, amplitude curlCm growing from zero at the root, curlFrequency turns per 10 cm of length.
#include "aver/core/Types.hpp"
#include "aver/formats/OcHair.hpp"

#include <string>
#include <vector>

namespace aver::hair {

// A scalp as the generator sees it: positions in cm, triangles, and an optional mask.
struct ScalpMesh {
    std::vector<f32> positions;     // 3 per vertex, cm, mesh-local, engine axes
    std::vector<u32> triangles;     // 3 per triangle
    std::vector<f32> mask;          // per vertex, 0..1; empty = uniform (1). A root grows where the mask is high; 0 grows nothing
    usize vertexCount() const { return positions.size() / 3; }
    usize triangleCount() const { return triangles.size() / 3; }
};

struct GenerateStats {
    u32 strands = 0;
    u32 guides = 0;                 // clump guide strands drawn
    u32 followers = 0;              // strands that were pulled toward a guide
    f32 scalpAreaCm2 = 0.0f;        // mask-weighted
    f32 spacingCm = 0.0f;           // the minimum root spacing the last round used (0 = the scalp ran out of room and spacing was dropped)
    u32 relaxRounds = 0;
};

// The mask-weighted area of the scalp in cm^2.
f32 scalpArea(const ScalpMesh& scalp);

// Grows g.strands strands from the scalp into `out` (points, start, material, no width). False with `why` set for a scalp with no area under the
// mask, an unusable record, or a record the scalp cannot satisfy. The roots lie exactly on the scalp triangles.
bool generateHair(const fmt::OcHairGenerate& g, const ScalpMesh& scalp, fmt::OcHairStrands& out, GenerateStats* stats = nullptr,
                  std::string* why = nullptr);

} // namespace aver::hair
