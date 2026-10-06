#include "aver/softbody/RenderBinding.hpp"

#include <unordered_map>

namespace aver::softbody {

RenderBinding RenderBinding::identity(const Cage& cage) {
    RenderBinding rb;
    rb.vertexParticle.resize(cage.particles.size());
    for (u32 i = 0; i < rb.vertexParticle.size(); ++i) rb.vertexParticle[i] = i;
    rb.indices.reserve(cage.triangles.size() * 3);
    for (u32 t = 0; t < cage.triangles.size(); ++t) {
        for (u32 k = 0; k < 3; ++k) rb.indices.push_back(cage.triangles[t].v0[k]);
        rb.triSim.push_back(t);
    }
    rb.finalize(static_cast<u32>(cage.triangles.size()));
    return rb;
}

void RenderBinding::finalize(u32 simTriangles) {
    baseVertexCount = vertexCount();
    indices0 = indices;
    simStart.assign(static_cast<usize>(simTriangles) + 1, 0);
    for (const u32 s : triSim)
        if (s < simTriangles) ++simStart[s + 1];
    for (u32 i = 0; i < simTriangles; ++i) simStart[i + 1] += simStart[i];
    simList.assign(simStart[simTriangles], 0);
    std::vector<u32> cursor(simStart.begin(), simStart.end() - 1);
    for (u32 r = 0; r < triSim.size(); ++r)
        if (triSim[r] < simTriangles) simList[cursor[triSim[r]]++] = r;
}

void RenderBinding::applySplits(const std::vector<SplitEvent>& events, std::vector<u32>* newVertexSources) {
    for (const SplitEvent& ev : events) {
        std::unordered_map<u32, u32> copies;   // source render vertex -> its duplicate for this event
        for (const u32 simTri : ev.triangles) {
            if (simTri + 1 >= simStart.size()) continue;
            for (u32 s = simStart[simTri]; s < simStart[simTri + 1]; ++s) {
                const u32 r = simList[s];
                for (u32 k = 0; k < 3; ++k) {
                    u32& slot = indices[r * 3 + k];
                    if (vertexParticle[slot] != ev.oldParticle) continue;
                    const auto it = copies.find(slot);
                    if (it != copies.end()) { slot = it->second; continue; }
                    const u32 fresh = vertexCount();
                    vertexParticle.push_back(ev.newParticle);
                    if (newVertexSources) newVertexSources->push_back(slot);
                    copies.emplace(slot, fresh);
                    slot = fresh;
                }
            }
        }
    }
}

void RenderBinding::visibleIndices(const std::vector<u8>& triDead, std::vector<u32>& out) const {
    out.clear();
    out.reserve(indices.size());
    for (u32 r = 0; r < triSim.size(); ++r) {
        const u32 s = triSim[r];
        if (s < triDead.size() && triDead[s]) continue;
        out.push_back(indices[r * 3 + 0]);
        out.push_back(indices[r * 3 + 1]);
        out.push_back(indices[r * 3 + 2]);
    }
}

void RenderBinding::gatherPositions(const std::vector<Vec3>& particlePositions, std::vector<Vec3>& out) const {
    out.resize(vertexParticle.size());
    for (u32 v = 0; v < vertexParticle.size(); ++v) {
        const u32 p = vertexParticle[v];
        out[v] = p < particlePositions.size() ? particlePositions[p] : Vec3{0, 0, 0};
    }
}

void RenderBinding::reset() {
    vertexParticle.resize(baseVertexCount);
    indices = indices0;
}

} // namespace aver::softbody
