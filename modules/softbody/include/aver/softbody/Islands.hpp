// Connected pieces of a torn cage, in the order the system gives them bodies.
//
// Call resolveTears() first (it duplicates particles on the fans a break disconnected), then
// planIslands(). Pure arithmetic over the cage. See docs/SOFTBODY.md.
#pragma once

#include "aver/softbody/Cage.hpp"

#include <vector>

namespace aver::softbody {

struct IslandPlan {
    struct Island {
        std::vector<u32> particles, beams, triangles;   // ascending ids; beams are unbroken, triangles live
        bool anchored = false;                          // holds a pinned particle
        f32 massKg = 0.0f;
    };
    std::vector<Island> islands;   // ordered: anchored first, then by descending mass, ties by lowest particle id
    std::vector<u32> dust;         // particles in unanchored pieces of fewer than minPieceTriangles triangles
};

IslandPlan planIslands(const Cage& c, u32 minPieceTriangles);

} // namespace aver::softbody
