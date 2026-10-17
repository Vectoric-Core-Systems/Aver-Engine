#include "aver/softbody/Islands.hpp"

#include <algorithm>
#include <numeric>

namespace aver::softbody {

IslandPlan planIslands(const Cage& c, u32 minPieceTriangles) {
    IslandPlan plan;
    const u32 n = static_cast<u32>(c.particles.size());
    if (n == 0) return plan;

    // Union-find whose root is the smallest member, so a component's id is its lowest particle.
    std::vector<u32> parent(n);
    std::iota(parent.begin(), parent.end(), 0u);
    auto find = [&](u32 x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    auto unite = [&](u32 a, u32 b) {
        a = find(a); b = find(b);
        if (a != b) parent[std::max(a, b)] = std::min(a, b);
    };
    for (const Beam& b : c.beams)
        if (!b.broken) unite(b.a, b.b);

    // Component slot per root, in ascending root order.
    std::vector<u32> slot(n, kNone);
    std::vector<IslandPlan::Island> comps;
    for (u32 p = 0; p < n; ++p) {
        const u32 r = find(p);
        if (slot[r] == kNone) { slot[r] = static_cast<u32>(comps.size()); comps.emplace_back(); }
        IslandPlan::Island& isl = comps[slot[r]];
        isl.particles.push_back(p);
        isl.massKg += c.particles[p].mass;
        isl.anchored = isl.anchored || c.particles[p].pinned;
    }
    for (u32 bi = 0; bi < c.beams.size(); ++bi)
        if (!c.beams[bi].broken) comps[slot[find(c.beams[bi].a)]].beams.push_back(bi);
    for (u32 ti = 0; ti < c.triangles.size(); ++ti) {
        if (ti < c.triDead.size() && c.triDead[ti]) continue;
        comps[slot[find(c.triangles[ti].v[0])]].triangles.push_back(ti);
    }

    std::vector<IslandPlan::Island> kept;
    for (IslandPlan::Island& isl : comps) {
        if (!isl.anchored && isl.triangles.size() < std::max(1u, minPieceTriangles))
            plan.dust.insert(plan.dust.end(), isl.particles.begin(), isl.particles.end());
        else kept.push_back(std::move(isl));
    }
    std::sort(plan.dust.begin(), plan.dust.end());

    std::stable_sort(kept.begin(), kept.end(), [](const IslandPlan::Island& a, const IslandPlan::Island& b) {
        if (a.anchored != b.anchored) return a.anchored;
        if (a.massKg != b.massKg) return a.massKg > b.massKg;
        return a.particles.front() < b.particles.front();
    });
    plan.islands = std::move(kept);
    return plan;
}

} // namespace aver::softbody
