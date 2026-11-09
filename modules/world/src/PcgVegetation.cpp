#include "aver/world/PcgVegetation.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aver::world {
namespace {

u64 mixHash(u64 h, u64 v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}
u64 bitsOf(f32 f) { u32 u; std::memcpy(&u, &f, 4); return u; }

} // namespace

fmt::OcBiomeData pcgLayerBiome(const std::string& layer, const std::string& foliagePath, f32 densityPer100m2, f32 minWeight) {
    fmt::OcBiomeData b;
    b.name = layer;
    fmt::OcBiomeEntry e;
    e.foliage = foliagePath;
    e.densityPer100m2 = densityPer100m2;
    e.layer = layer;
    e.layerMin = minWeight;
    b.entries.push_back(std::move(e));
    return b;
}

VegetationGenerator::VegetationGenerator(VegetationInputs inputs, fmt::OcVegetationRecord record, PcgGuardrails caps)
    : in_(std::move(inputs)), rec_(std::move(record)), caps_(caps) {}

void VegetationGenerator::setInputs(VegetationInputs inputs) { in_ = std::move(inputs); }

void VegetationGenerator::eraseAt(const Vec3& c, f32 r) {
    fmt::OcVegErased e;
    e.center[0] = c.x; e.center[1] = c.y; e.center[2] = c.z;
    e.radiusCm = r;
    rec_.erased.push_back(e);
}

void VegetationGenerator::addInstance(const std::string& asset, const Transform& xf) {
    fmt::OcVegAdded a;
    a.asset = asset;
    a.pos[0] = xf.position.x; a.pos[1] = xf.position.y; a.pos[2] = xf.position.z;
    a.rot[0] = xf.rotation.x; a.rot[1] = xf.rotation.y; a.rot[2] = xf.rotation.z; a.rot[3] = xf.rotation.w;
    a.scale[0] = xf.scale.x; a.scale[1] = xf.scale.y; a.scale[2] = xf.scale.z;
    rec_.added.push_back(std::move(a));
}

u64 VegetationGenerator::currentInputHash() const {
    fmt::OcGraphData g;
    if (!pcgBuildBiomeGraph(in_.sources, g)) return 0;
    u64 h = pcgGraphHash(g);
    h = mixHash(h, static_cast<u64>(static_cast<u32>(rec_.seed)));
    h = mixHash(h, bitsOf(rec_.areaMin[0])); h = mixHash(h, bitsOf(rec_.areaMin[1]));
    h = mixHash(h, bitsOf(rec_.areaMax[0])); h = mixHash(h, bitsOf(rec_.areaMax[1]));
    h = mixHash(h, static_cast<u64>(rec_.cellCm));
    return h;
}

bool VegetationGenerator::buildProgram(std::string* why) {
    auto prog = std::make_shared<PcgProgram>();
    fmt::OcGraphData g;
    if (!pcgBuildBiomeGraph(in_.sources, g, why)) return false;
    prog->graph = std::make_shared<const fmt::OcGraphData>(std::move(g));
    prog->seed = rec_.seed;
    prog->bounded = true;
    prog->area.min = Vec3{rec_.areaMin[0], rec_.areaMin[1], 0.0f};
    prog->area.max = Vec3{rec_.areaMax[0], rec_.areaMax[1], 0.0f};
    prog->zMin = rec_.zMin;
    prog->zMax = rec_.zMax;
    prog->landscape = in_.landscape;
    prog->splines = in_.splines;
    prog->volumes = in_.volumes;
    prog->environmentStamp = envStamp_;
    prog->maxPointsPerNode = std::max<u32>(caps_.maxInstancesPerCell * 4u, 100000u);
    program_ = std::move(prog);
    return true;
}

u32 VegetationGenerator::invalidate(const PcgBox& box) { return store_ ? store_->invalidateBox(box) : 0; }

VegetationResult VegetationGenerator::run(bool ignoreCaps) {
    VegetationResult res;
    std::string why;
    if (!buildProgram(&why)) { res.error = why; return res; }
    if (!rec_.valid(&why)) { res.error = why; return res; }
    if (!in_.landscape) { res.error = "no landscape to generate on"; return res; }

    res.estimate = pcgEstimateBiome(in_.sources, static_cast<f32>(rec_.cellCm), caps_, in_.meshHasLods, in_.user, in_.knownLayers);
    res.warnings = res.estimate.warnings;
    if (res.estimate.overCap && !ignoreCaps) {
        res.error = "refused before generating: " + res.estimate.warnings.back();
        return res;
    }

    if (!store_) store_ = std::make_unique<PcgCellStore>(program_, rec_.cellCm);
    else store_->setProgram(program_);

    const f32 cell = static_cast<f32>(rec_.cellCm);
    const i32 cx0 = static_cast<i32>(std::floor(rec_.areaMin[0] / cell)), cx1 = static_cast<i32>(std::ceil(rec_.areaMax[0] / cell)) - 1;
    const i32 cy0 = static_cast<i32>(std::floor(rec_.areaMin[1] / cell)), cy1 = static_cast<i32>(std::ceil(rec_.areaMax[1] / cell)) - 1;

    PcgResult merged;
    const auto groupOf = [&](const PcgInstanceGroup& g) -> PcgInstanceGroup& {
        for (PcgInstanceGroup& x : merged.groups)
            if (x.mesh == g.mesh && x.material == g.material && x.cullEndCm == g.cullEndCm && x.flags == g.flags) return x;
        merged.groups.push_back({g.mesh, g.material, {}, g.cullStartCm, g.cullEndCm, g.lodScaleBias, g.flags});
        return merged.groups.back();
    };

    for (i32 cx = cx0; cx <= cx1; ++cx)
        for (i32 cy = cy0; cy <= cy1; ++cy) {
            const bool current = store_->isCurrent(cx, cy);
            const PcgResult& r = store_->get(cx, cy);
            ++(current ? res.cellsReused : res.cellsRun);
            if (!current)
                for (const PcgDiag& d : r.diags)
                    if (d.error) { res.error = "cell " + std::to_string(cx) + "," + std::to_string(cy) + ": " + d.node + ": " + d.message; return res; }
                    else res.warnings.push_back(d.node + ": " + d.message);

            usize total = 0;
            for (const PcgInstanceGroup& g : r.groups) total += g.items.size();
            // Per-cell instance cap: keep the instances with the smallest seeds (a pure function of the data, so a repeat run
            // keeps the same ones) and say so.
            i32 seedCut = 0;
            bool capped = false;
            if (total > caps_.maxInstancesPerCell) {
                std::vector<i32> seeds;
                seeds.reserve(total);
                for (const PcgInstanceGroup& g : r.groups) for (const PcgInstance& i : g.items) seeds.push_back(i.seed);
                std::nth_element(seeds.begin(), seeds.begin() + caps_.maxInstancesPerCell, seeds.end());
                seedCut = seeds[caps_.maxInstancesPerCell];
                capped = true;
                ++res.cellsCapped;
                res.warnings.push_back("cell " + std::to_string(cx) + "," + std::to_string(cy) + " produced " + std::to_string(total) +
                                       " instances; capped to " + std::to_string(caps_.maxInstancesPerCell));
            }
            for (const PcgInstanceGroup& g : r.groups) {
                PcgInstanceGroup& dst = groupOf(g);
                for (const PcgInstance& i : g.items) if (!capped || i.seed < seedCut) dst.items.push_back(i);
            }
        }

    for (const PcgInstanceGroup& g : merged.groups) res.generated += static_cast<u32>(g.items.size());

    // Overrides on the fresh baseline: erasures first, then additions.
    if (!rec_.erased.empty())
        for (PcgInstanceGroup& g : merged.groups) {
            const auto gone = [&](const PcgInstance& i) {
                for (const fmt::OcVegErased& e : rec_.erased) {
                    const f32 dx = i.xf.position.x - e.center[0], dy = i.xf.position.y - e.center[1];
                    if (dx * dx + dy * dy <= e.radiusCm * e.radiusCm) return true;
                }
                return false;
            };
            const usize before = g.items.size();
            g.items.erase(std::remove_if(g.items.begin(), g.items.end(), gone), g.items.end());
            res.erasedByOverride += static_cast<u32>(before - g.items.size());
        }
    for (const fmt::OcVegAdded& a : rec_.added) {
        PcgInstanceGroup key;
        key.mesh = a.asset;
        auto it = std::find_if(merged.groups.begin(), merged.groups.end(), [&](const PcgInstanceGroup& g) { return g.mesh == a.asset; });
        if (it == merged.groups.end()) { merged.groups.push_back(key); it = merged.groups.end() - 1; }
        PcgInstance inst;
        inst.xf.position = Vec3{a.pos[0], a.pos[1], a.pos[2]};
        inst.xf.rotation = Quat{a.rot[0], a.rot[1], a.rot[2], a.rot[3]};
        inst.xf.scale = Vec3{a.scale[0], a.scale[1], a.scale[2]};
        inst.seed = static_cast<i32>(pcgHashString(a.asset) ^ static_cast<u32>(res.addedByOverride));
        it->items.push_back(inst);
        ++res.addedByOverride;
    }

    merged.groups.erase(std::remove_if(merged.groups.begin(), merged.groups.end(),
                                       [](const PcgInstanceGroup& g) { return g.items.empty(); }), merged.groups.end());
    // Group radius (cell bounds grow by it): the type's collision radius where the mesh is a known type's.
    std::vector<f32> radii;
    for (const PcgInstanceGroup& g : merged.groups) {
        f32 r = 0.0f;
        for (const PcgBiomeSource& s : in_.sources)
            for (const fmt::OcFoliageData& t : s.types) if (t.meshPath == g.mesh) r = std::max(r, t.collisionRadiusCm);
        radii.push_back(r);
    }
    if (!pcgToInstanceData(merged, res.table, cell, radii, &why)) { res.error = why; return res; }

    res.inputHash = currentInputHash();
    rec_.inputHash = res.inputHash;
    rec_.count = static_cast<u32>(res.table.transforms.size() / 12);
    res.ok = true;
    return res;
}

} // namespace aver::world
