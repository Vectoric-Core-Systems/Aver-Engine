// The map from a render mesh to the cage, kept in step with tears.
//
// A render vertex follows one particle. When a tear duplicates a particle, the render vertices on
// the moved triangles are duplicated too (the host copies their UV/normal from the reported source
// vertex), and triangles spanning a gap are left out of the index list. No GPU types here: the host
// uploads whatever these arrays say.
#pragma once

#include "aver/softbody/Cage.hpp"

#include <vector>

namespace aver::softbody {

struct RenderBinding {
    std::vector<u32> vertexParticle;   // render vertex -> particle
    std::vector<u32> indices;          // 3 per render triangle
    std::vector<u32> triSim;           // render triangle -> sim triangle, or kNone (never hidden)

    // Derived by finalize().
    u32 baseVertexCount = 0;
    std::vector<u32> indices0;         // as bound, for reset()
    std::vector<u32> simStart, simList;   // sim triangle -> render triangles (CSR)

    // One render vertex per particle and one render triangle per sim triangle.
    static RenderBinding identity(const Cage& cage);

    // Call once after filling the three arrays by hand. `simTriangles` = Cage::triangles.size().
    void finalize(u32 simTriangles);

    u32 vertexCount() const { return static_cast<u32>(vertexParticle.size()); }

    // Replays tears. Appends, per new render vertex in creation order, the vertex it was copied
    // from to `newVertexSources`; new ids are vertexCount() + position in that list.
    void applySplits(const std::vector<SplitEvent>& events, std::vector<u32>* newVertexSources = nullptr);

    // Triangles whose sim triangle is dead are omitted.
    void visibleIndices(const std::vector<u8>& triDead, std::vector<u32>& out) const;

    // Render vertex positions from particle positions.
    void gatherPositions(const std::vector<Vec3>& particlePositions, std::vector<Vec3>& out) const;

    // Back to the bound state (after repair(cage)).
    void reset();
};

} // namespace aver::softbody
