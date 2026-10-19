// ParticleGpuPool: page buffers for the GPU tier's per-instance ranges. See ParticleGpu.hpp.
#include "aver/particles/ParticleGpu.hpp"

#include "aver/core/Log.hpp"
#include "aver/particles/ParticleGpuTypes.hpp"

#include <algorithm>

namespace aver::particles {
namespace {

constexpr u64 kPageBudgetBytes = 8ull << 20;   // a page tries to hold this much; big classes hold one range
constexpr u32 kMaxRangesPerPage = 32;
constexpr u32 kSortableCapacity = kSortMaxEntries;

// Bytes one range takes in each buffer of a page.
u64 particleBytes(u32 c) { return static_cast<u64>(c) * sizeof(Particle); }
u64 deadBytes(u32 c) { return static_cast<u64>(c) * 4u; }
u64 aliveBytes(u32 c) { return static_cast<u64>(c) * 12u; }          // two survivor lists and the sorted-order region
u64 sortBytes(u32 c) { return static_cast<u64>(c) * 8u; }
u64 blockBytes() { return static_cast<u64>(ParticleGpuPool::kCountersStride) * 4u; }

} // namespace

int ParticleGpuPool::classFor(u32 particles) {
    for (u32 c = 0; c < kClassCount; ++c)
        if (classCapacity(c) >= particles) return static_cast<int>(c);
    return -1;
}

rhi::BufferHandle ParticleGpuPool::make(u64 bytes, rhi::BufferKind kind, bool uav, const char* name, u64& total) {
    rhi::BufferDesc bd;
    bd.bytes = bytes;
    bd.kind = kind;
    bd.allowUnorderedAccess = uav;
    bd.debugName = name;
    const rhi::BufferHandle h = res_->createBuffer(bd);
    if (h) total += bytes;
    return h;
}

bool ParticleGpuPool::makePage(Page& p, u32 cls) {
    p = Page{};
    p.cls = cls;
    p.capacity = classCapacity(cls);
    p.sortable = p.capacity <= kSortableCapacity;
    const u64 per = particleBytes(p.capacity) + deadBytes(p.capacity) + aliveBytes(p.capacity) +
                    (p.sortable ? sortBytes(p.capacity) : 0u) + 2 * blockBytes();
    p.perPage = static_cast<u32>(std::clamp<u64>(kPageBudgetBytes / per, 1ull, kMaxRangesPerPage));
    const u32 n = p.perPage;
    u64 total = 0;
    p.particles.h = make(particleBytes(p.capacity) * n, rhi::BufferKind::Default, true, "Aver.Particles particles", total);
    p.dead.h      = make(deadBytes(p.capacity) * n, rhi::BufferKind::Default, true, "Aver.Particles dead list", total);
    p.alive.h     = make(aliveBytes(p.capacity) * n, rhi::BufferKind::Default, true, "Aver.Particles alive lists", total);
    p.counters.h  = make(blockBytes() * n, rhi::BufferKind::Default, true, "Aver.Particles counters", total);
    p.args.h      = make(blockBytes() * n, rhi::BufferKind::Default, true, "Aver.Particles indirect args", total);
    if (p.sortable) p.sortKeys.h = make(sortBytes(p.capacity) * n, rhi::BufferKind::Default, true, "Aver.Particles sort keys", total);
    for (int i = 0; i < 3; ++i) p.readback[i] = make(blockBytes() * n, rhi::BufferKind::Readback, false, "Aver.Particles stats readback", total);
    const bool ok = p.particles.h && p.dead.h && p.alive.h && p.counters.h && p.args.h && (!p.sortable || p.sortKeys.h) &&
                    p.readback[0] && p.readback[1] && p.readback[2];
    p.bytes = total;
    bytes_ += total;
    p.taken.assign(n, 0);
    p.live = ok;
    if (!ok) {
        AVER_ERROR("[Particles] GPU pool page allocation failed (class {}, {} MB)", cls, total >> 20);
        destroyPage(p);
        return false;
    }
    return true;
}

void ParticleGpuPool::destroyPage(Page& p) {
    if (res_) {
        for (rhi::BufferHandle h : {p.particles.h, p.dead.h, p.alive.h, p.counters.h, p.args.h, p.sortKeys.h,
                                    p.readback[0], p.readback[1], p.readback[2]})
            if (h) res_->destroyBuffer(h);
    }
    bytes_ = bytes_ >= p.bytes ? bytes_ - p.bytes : 0u;
    p = Page{};
}

bool ParticleGpuPool::acquire(u32 particles, Range& out) {
    const int cls = classFor(particles);
    if (cls < 0 || !res_) return false;
    for (u32 pi = 0; pi < pages_.size(); ++pi) {
        Page& p = pages_[pi];
        if (!p.live || p.cls != static_cast<u32>(cls) || p.used >= p.perPage) continue;
        for (u32 i = 0; i < p.perPage; ++i) {
            if (p.taken[i]) continue;
            p.taken[i] = 1;
            ++p.used;
            out = Range{pi, i, p.capacity};
            return true;
        }
    }
    u32 slot = static_cast<u32>(pages_.size());
    for (u32 pi = 0; pi < pages_.size(); ++pi)
        if (!pages_[pi].live) { slot = pi; break; }
    if (slot == pages_.size()) pages_.emplace_back();
    if (!makePage(pages_[slot], static_cast<u32>(cls))) return false;
    Page& p = pages_[slot];
    p.taken[0] = 1;
    p.used = 1;
    out = Range{slot, 0, p.capacity};
    return true;
}

void ParticleGpuPool::release(const Range& r) {
    if (!r.valid() || r.page >= pages_.size()) return;
    Page& p = pages_[r.page];
    if (!p.live || r.index >= p.taken.size() || !p.taken[r.index]) return;
    p.taken[r.index] = 0;
    if (p.used > 0) --p.used;
    // A big class gives its memory back when its last range goes; the small ones stay for the next effect.
    if (p.used == 0 && p.cls >= 4) destroyPage(p);
}

void ParticleGpuPool::shutdown() {
    for (Page& p : pages_) destroyPage(p);
    pages_.clear();
    bytes_ = 0;
    res_ = nullptr;
}

} // namespace aver::particles
