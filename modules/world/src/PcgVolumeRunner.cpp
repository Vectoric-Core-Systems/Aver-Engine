#include "aver/world/PcgVolumeRunner.hpp"

#include <algorithm>
#include <cmath>

namespace aver::world {

u32 pcgGeneratorVersion(const PcgProgram& p) {
    const u64 h = p.graph ? pcgGraphHash(*p.graph) : 0;
    return (3u << 28) | (static_cast<u32>(h ^ (h >> 32)) & 0x0FFFFFFFu);
}

PcgCellGenerator::PcgCellGenerator(std::shared_ptr<const PcgProgram> program, i32 cellSizeCm)
    : program_(std::move(program)), cell_(cellSizeCm > 0 ? cellSizeCm : 1600), margin_(0.0f) {
    if (program_ && program_->graph) {
        PcgContext c;
        c.graphLoader = program_->graphLoader;
        margin_ = pcgGraphMargin(*program_->graph, &c);
    }
}

PcgBox PcgCellGenerator::cellBox(i32 cx, i32 cy) const {
    const f64 s = cell_;
    PcgBox b;
    b.min = Vec3{static_cast<f32>(cx * s), static_cast<f32>(cy * s), program_->zMin};
    b.max = Vec3{static_cast<f32>((cx + 1) * s), static_cast<f32>((cy + 1) * s), program_->zMax};
    return b;
}

PcgContext PcgCellGenerator::makeContext(const PcgBox& evalBox, bool partitioned, PcgCache* cache) const {
    const PcgProgram& p = *program_;
    PcgContext c;
    c.seed = p.seed;
    c.bounds = evalBox;
    c.landscape = p.landscape.get();
    c.splines = p.splines;
    c.actors = p.actors;
    c.volumes = p.volumes;
    c.graphLoader = p.graphLoader;
    c.environmentStamp = p.environmentStamp;
    c.budgetMs = p.budgetMs;
    c.maxPoints = p.maxPointsPerNode;
    c.partitioned = partitioned;
    c.cache = cache;
    return c;
}

PcgResult PcgCellGenerator::generateBox(const PcgBox& box, PcgCache* cache) const {
    PcgBox b = box;
    if (program_->bounded) {
        b.min.x = std::max(b.min.x, program_->area.min.x); b.min.y = std::max(b.min.y, program_->area.min.y);
        b.max.x = std::min(b.max.x, program_->area.max.x); b.max.y = std::min(b.max.y, program_->area.max.y);
    }
    if (!program_->graph) { PcgResult r; r.ok = false; r.diags.push_back({"", "program has no graph", true}); return r; }
    return pcgEvaluate(*program_->graph, makeContext(b, false, cache));
}

PcgResult PcgCellGenerator::generate(i32 cx, i32 cy, PcgCache* cache) const {
    if (!program_->graph) { PcgResult r; r.ok = false; r.diags.push_back({"", "program has no graph", true}); return r; }
    const PcgBox own = cellBox(cx, cy);
    PcgBox eval = own;
    eval.min.x -= margin_; eval.min.y -= margin_;
    eval.max.x += margin_; eval.max.y += margin_;
    if (program_->bounded) {
        eval.min.x = std::max(eval.min.x, program_->area.min.x); eval.min.y = std::max(eval.min.y, program_->area.min.y);
        eval.max.x = std::min(eval.max.x, program_->area.max.x); eval.max.y = std::min(eval.max.y, program_->area.max.y);
    }
    PcgResult r = pcgEvaluate(*program_->graph, makeContext(eval, true, cache));

    const auto owned = [&](const Vec3& p) {
        if (!own.containsXY(p)) return false;
        return !program_->bounded || program_->area.containsXY(p);
    };
    for (PcgInstanceGroup& g : r.groups)
        g.items.erase(std::remove_if(g.items.begin(), g.items.end(),
                                     [&](const PcgInstance& i) { return !owned(i.xf.position); }), g.items.end());
    r.groups.erase(std::remove_if(r.groups.begin(), r.groups.end(),
                                  [](const PcgInstanceGroup& g) { return g.items.empty(); }), r.groups.end());
    r.actors.erase(std::remove_if(r.actors.begin(), r.actors.end(),
                                  [&](const PcgActorSpawn& a) { return !owned(a.xf.position); }), r.actors.end());
    return r;
}

// ---- PcgCellStore -------------------------------------------------------------------------------------------------------

PcgCellStore::PcgCellStore(std::shared_ptr<const PcgProgram> program, i32 cellSizeCm, bool useNodeCache)
    : program_(std::move(program)), cellSizeCm_(cellSizeCm), gen_(program_, cellSizeCm), useCache_(useNodeCache) {
    graphHash_ = program_ && program_->graph ? pcgGraphHash(*program_->graph) : 0;
}

void PcgCellStore::setProgram(std::shared_ptr<const PcgProgram> program) {
    program_ = std::move(program);
    gen_ = PcgCellGenerator(program_, cellSizeCm_);
    graphHash_ = program_ && program_->graph ? pcgGraphHash(*program_->graph) : 0;
}

bool PcgCellStore::isCurrent(i32 cx, i32 cy) const {
    const auto it = cells_.find({cx, cy});
    return it != cells_.end() && it->second.valid && !it->second.stale && it->second.graphHash == graphHash_ &&
           it->second.stamp == program_->environmentStamp;
}

const PcgResult& PcgCellStore::get(i32 cx, i32 cy) {
    if (!isCurrent(cx, cy)) {
        if (nodeCache_.entries.size() > 4096) nodeCache_.clear();
        Entry& e = cells_[{cx, cy}];
        e.res = gen_.generate(cx, cy, useCache_ ? &nodeCache_ : nullptr);
        e.graphHash = graphHash_;
        e.stamp = program_->environmentStamp;
        e.valid = true;
        e.stale = false;
        ++generateCalls_;
    }
    return cells_[{cx, cy}].res;
}

u32 PcgCellStore::invalidateBox(const PcgBox& box) {
    u32 marked = 0;
    nodeCache_.clear();   // node results are keyed by graph and region, not by the landscape under them
    const f32 m = gen_.margin();
    const f32 s = static_cast<f32>(gen_.cellSizeCm());
    for (auto& kv : cells_) {
        const f32 x0 = kv.first.first * s - m, x1 = (kv.first.first + 1) * s + m;
        const f32 y0 = kv.first.second * s - m, y1 = (kv.first.second + 1) * s + m;
        if (box.max.x < x0 || box.min.x >= x1 || box.max.y < y0 || box.min.y >= y1) continue;
        if (!kv.second.stale) { kv.second.stale = true; ++marked; }
    }
    return marked;
}

} // namespace aver::world
