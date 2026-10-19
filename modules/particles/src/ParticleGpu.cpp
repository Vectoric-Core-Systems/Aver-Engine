// ParticleGpu: the GPU tier's kernels, instances and per-frame recording. See ParticleGpu.hpp for the design.
#include "aver/particles/ParticleGpu.hpp"

#include "ParticleShaders.hpp"
#include "aver/core/Log.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace aver::particles {
namespace {

using rhi::ResourceState;

constexpr u32 kHeaderBytes = static_cast<u32>(offsetof(GpuSimCB, req));   // header + user + modules
constexpr u32 kDummyBytes = 16384;
constexpr u32 kEventsBytes = kGpuEventRegionUints * 2u * 4u;

u32 groupsFor(u32 threads, u32 groupSize) { return (threads + groupSize - 1u) / groupSize; }

} // namespace

u32 ParticleGpu::capacityFor(const ParticleEffect& fx) const {
    const bool linked = fx.render == RenderKind::Ribbon && fx.linked;
    const u32 n = linked ? std::clamp<u32>(fx.linkedCapacity, 2u, kMaxLinkedCapacity) : std::max(1u, fx.maxParticles);
    const int cls = ParticleGpuPool::classFor(n);
    return cls < 0 ? 0u : ParticleGpuPool::classCapacity(static_cast<u32>(cls));
}

// ------------------------------------------------------------------------------------------------ setup
rhi::PipelineHandle ParticleGpu::kernel(const char* source, const char* entry, rhi::PipelineLayout layout, rhi::ShaderStage stage) {
    rhi::ShaderDesc sd;
    sd.source = source;
    sd.entry = entry;
    sd.stage = stage;
    sd.minShaderModel = 60;
    const rhi::ShaderHandle cs = res_->createShader(sd);
    if (!cs) {
        AVER_ERROR("[Particles] GPU kernel {} failed to compile", entry);
        return 0;
    }
    rhi::ComputePipelineDesc pd;
    pd.cs = cs;
    pd.layout = layout;
    const rhi::PipelineHandle p = res_->createComputePipeline(pd);
    res_->destroyShader(cs);
    if (!p) AVER_ERROR("[Particles] GPU kernel {} pipeline failed", entry);
    return p;
}

bool ParticleGpu::buildKernels() {
    rhi::PipelineLayout layout;
    layout.uavCount = kSimUavCount;
    layout.slotKindsDeclared = true;
    for (u32 i = 0; i < kSimUavCount; ++i) layout.uavKinds[i] = rhi::SlotKind::StructuredBuffer;
    layout.constantDwords[kSimConstantRegister] = 0;   // a root CBV

    const std::string sim = rhi::shaderFile("particles_sim.hlsl");
    const std::string sort = rhi::shaderFile("particles_sort.hlsl");
    if (sim.empty() || sort.empty()) {
        AVER_ERROR("[Particles] the GPU tier's shader files were not found");
        return false;
    }
    csInit_ = kernel(sim.c_str(), "CSInit", layout);
    csBegin_ = kernel(sim.c_str(), "CSBegin", layout);
    csSpawn_ = kernel(sim.c_str(), "CSSpawn", layout);
    csArgsUpdate_ = kernel(sim.c_str(), "CSArgsUpdate", layout);
    csUpdate_ = kernel(sim.c_str(), "CSUpdate", layout);
    csFinish_ = kernel(sim.c_str(), "CSFinish", layout);
    csProbe_ = kernel(sim.c_str(), "CSProbe", layout);
    csRngProbe_ = kernel(sim.c_str(), "CSRngProbe", layout);
    csSortKeys_ = kernel(sort.c_str(), "CSSortKeys", layout);
    csSortLocal_ = kernel(sort.c_str(), "CSSortLocal", layout);
    csSortGlobal_ = kernel(sort.c_str(), "CSSortGlobal", layout);
    csSortLocalMerge_ = kernel(sort.c_str(), "CSSortLocalMerge", layout);
    csSortFinal_ = kernel(sort.c_str(), "CSSortFinal", layout);
    return csInit_ && csBegin_ && csSpawn_ && csArgsUpdate_ && csUpdate_ && csFinish_ && csProbe_ && csRngProbe_ && csSortKeys_ &&
           csSortLocal_ && csSortGlobal_ && csSortLocalMerge_ && csSortFinal_;
}

bool ParticleGpu::init(rhi::IDevice& device) {
    shutdown();
    dev_ = &device;
    res_ = device.resources();
    if (!res_) {
        AVER_INFO("[Particles] GPU tier unavailable: the backend has no resource factory");
        return false;
    }
    const rhi::DeviceCaps caps = device.caps();
    if (!caps.computeShaders || !caps.indirect) {
        AVER_INFO("[Particles] GPU tier unavailable: compute {} indirect {}", caps.computeShaders, caps.indirect);
        return false;
    }
    pool_.init(res_);
    rhi::BufferDesc bd;
    bd.bytes = kDummyBytes;
    bd.kind = rhi::BufferKind::Default;
    bd.allowUnorderedAccess = true;
    bd.debugName = "Aver.Particles dummy";
    dummy_.h = res_->createBuffer(bd);
    if (!dummy_.h || !buildKernels()) {
        shutdown();
        return false;
    }
    dedicatedBytes_ += kDummyBytes;
    ready_ = true;
    AVER_INFO("[Particles] GPU tier ready");
    return true;
}

void ParticleGpu::shutdown() {
    if (res_) {
        for (Instance& in : instances_) {
            if (!in.live) continue;
            if (in.set) res_->destroyBindingSet(in.set);
            if (in.eventsBuf.h) res_->destroyBuffer(in.eventsBuf.h);
            if (in.trailBuf.h) res_->destroyBuffer(in.trailBuf.h);
        }
        for (rhi::PipelineHandle p : {csInit_, csBegin_, csSpawn_, csArgsUpdate_, csUpdate_, csFinish_, csProbe_, csRngProbe_, csSortKeys_,
                                      csSortLocal_, csSortGlobal_, csSortLocalMerge_, csSortFinal_})
            if (p) res_->destroyPipeline(p);
        if (dummy_.h) res_->destroyBuffer(dummy_.h);
    }
    pool_.shutdown();
    instances_.clear();
    freeHandles_.clear();
    counterCache_.clear();
    csInit_ = csBegin_ = csSpawn_ = csArgsUpdate_ = csUpdate_ = csFinish_ = csProbe_ = csRngProbe_ = 0;
    csSortKeys_ = csSortLocal_ = csSortGlobal_ = csSortLocalMerge_ = csSortFinal_ = 0;
    dummy_ = TrackedBuffer{};
    dedicatedBytes_ = 0;
    ready_ = false;
    res_ = nullptr;
    dev_ = nullptr;
}

// ------------------------------------------------------------------------------------------------ instances
ParticleGpu::Instance* ParticleGpu::find(u32 handle) {
    if (handle == 0 || handle > instances_.size()) return nullptr;
    Instance& in = instances_[handle - 1];
    return in.live ? &in : nullptr;
}
const ParticleGpu::Instance* ParticleGpu::find(u32 handle) const {
    if (handle == 0 || handle > instances_.size()) return nullptr;
    const Instance& in = instances_[handle - 1];
    return in.live ? &in : nullptr;
}

u32 ParticleGpu::acquire(const ParticleEffect& fx, u32 parent) {
    if (!ready_) return 0;
    const bool linked = fx.render == RenderKind::Ribbon && fx.linked;
    const u32 n = linked ? std::clamp<u32>(fx.linkedCapacity, 2u, kMaxLinkedCapacity) : std::max(1u, fx.maxParticles);
    ParticleGpuPool::Range range;
    if (!pool_.acquire(n, range)) return 0;
    ParticleGpuPool::Page& pg = pool_.page(range.page);

    Instance in;
    in.live = true;
    in.range = range;
    in.capacity = n;
    in.poolCapacity = range.capacity;
    in.linked = linked;
    in.trail = fx.render == RenderKind::Ribbon && !linked;
    in.trailRing = in.trail ? std::clamp<u32>(fx.trailPoints, 2u, kMaxTrailPoints) - 1u : 0u;
    in.hasEvents = !fx.events.empty();
    in.parent = parent;
    const bool wantsSort = fx.sort == SortMode::Depth || (fx.sort == SortMode::Auto && fx.blend != rhi::BlendMode::Additive);
    in.sorted = wantsSort && !linked && pg.sortable;
    if (wantsSort && !linked && !pg.sortable && !sortWarned_) {
        sortWarned_ = true;
        AVER_WARN("[Particles] an emitter above {} particles is drawn unsorted; use Additive blending for dense clouds", kSortMaxEntries);
    }

    u64 extra = 0;
    if (in.hasEvents) {
        rhi::BufferDesc bd;
        bd.bytes = kEventsBytes;
        bd.kind = rhi::BufferKind::Default;
        bd.allowUnorderedAccess = true;
        bd.debugName = "Aver.Particles events";
        in.eventsBuf.h = res_->createBuffer(bd);
        extra += kEventsBytes;
    }
    if (in.trail) {
        rhi::BufferDesc bd;
        bd.bytes = static_cast<u64>(n) * in.trailRing * 16u;
        bd.kind = rhi::BufferKind::Default;
        bd.allowUnorderedAccess = true;
        bd.debugName = "Aver.Particles trails";
        in.trailBuf.h = res_->createBuffer(bd);
        extra += bd.bytes;
    }
    const Instance* par = find(parent);
    rhi::BindingSetDesc sd;
    sd.uavCount = kSimUavCount;
    for (u32 i = 0; i < kSimUavCount; ++i) sd.uavKinds[i] = rhi::SlotKind::StructuredBuffer;
    in.set = res_->createBindingSet(sd);
    if (!in.set || (in.hasEvents && !in.eventsBuf.h) || (in.trail && !in.trailBuf.h)) {
        AVER_ERROR("[Particles] GPU instance allocation failed");
        if (in.set) res_->destroyBindingSet(in.set);
        if (in.eventsBuf.h) res_->destroyBuffer(in.eventsBuf.h);
        if (in.trailBuf.h) res_->destroyBuffer(in.trailBuf.h);
        pool_.release(range);
        return 0;
    }
    const u32 C = in.poolCapacity, idx = range.index;
    res_->setUavBuffer(in.set, 0, pg.particles.h, sizeof(Particle), C, idx * C);
    res_->setUavBuffer(in.set, 1, pg.dead.h, 4, C, idx * C);
    res_->setUavBuffer(in.set, 2, pg.alive.h, 4, 3 * C, idx * 3 * C);
    res_->setUavBuffer(in.set, 3, pg.counters.h, 4, kGpuCounterCount, idx * ParticleGpuPool::kCountersStride);
    res_->setUavBuffer(in.set, 4, pg.args.h, 4, kArgsUints, idx * ParticleGpuPool::kArgsStride);
    res_->setUavBuffer(in.set, 5, in.eventsBuf.h ? in.eventsBuf.h : dummy_.h, 4, in.eventsBuf.h ? kGpuEventRegionUints * 2u : kDummyBytes / 4u, 0);
    res_->setUavBuffer(in.set, 6, (par && par->eventsBuf.h) ? par->eventsBuf.h : dummy_.h, 4,
                       (par && par->eventsBuf.h) ? kGpuEventRegionUints * 2u : kDummyBytes / 4u, 0);
    if (pg.sortable) res_->setUavBuffer(in.set, 7, pg.sortKeys.h, 8, C, idx * C);
    else             res_->setUavBuffer(in.set, 7, dummy_.h, 8, kDummyBytes / 8u, 0);
    res_->setUavBuffer(in.set, 8, in.trailBuf.h ? in.trailBuf.h : dummy_.h, 16, in.trailBuf.h ? n * in.trailRing : kDummyBytes / 16u, 0);
    dedicatedBytes_ += extra;

    u32 handle;
    if (!freeHandles_.empty()) {
        handle = freeHandles_.back();
        freeHandles_.pop_back();
        instances_[handle - 1] = in;
    } else {
        instances_.push_back(in);
        handle = static_cast<u32>(instances_.size());
    }
    return handle;
}

void ParticleGpu::release(u32 handle) {
    Instance* in = find(handle);
    if (!in) return;
    if (res_) {
        if (in->set) res_->destroyBindingSet(in->set);
        if (in->eventsBuf.h) { res_->destroyBuffer(in->eventsBuf.h); dedicatedBytes_ = dedicatedBytes_ >= kEventsBytes ? dedicatedBytes_ - kEventsBytes : 0u; }
        if (in->trailBuf.h) {
            const u64 b = static_cast<u64>(in->capacity) * in->trailRing * 16u;
            res_->destroyBuffer(in->trailBuf.h);
            dedicatedBytes_ = dedicatedBytes_ >= b ? dedicatedBytes_ - b : 0u;
        }
    }
    pool_.release(in->range);
    *in = Instance{};
    freeHandles_.push_back(handle);
}

void ParticleGpu::setIndexCount(u32 handle, u32 indexCount) {
    if (Instance* in = find(handle)) in->indexCount = indexCount;
}

// ------------------------------------------------------------------------------------------------ states and barriers
void ParticleGpu::to(rhi::IRenderContext& ctx, TrackedBuffer& b, ResourceState target) {
    if (!b.h || b.st == target) return;
    ctx.bufferBarrier(b.h, b.st, target);
    b.st = target;
}
void ParticleGpu::barrier(rhi::IRenderContext& ctx, TrackedBuffer& b) {
    if (b.h && b.st == ResourceState::UnorderedAccess) ctx.uavBarrierBuffer(b.h);
}

// ------------------------------------------------------------------------------------------------ stats
void ParticleGpu::refreshStats() {
    if (!res_) return;
    counterCache_.resize(pool_.pageCount());
    for (u32 pi = 0; pi < pool_.pageCount(); ++pi) {
        ParticleGpuPool::Page& pg = pool_.page(pi);
        if (!pg.live || pg.used == 0) continue;
        std::vector<u32>& cache = counterCache_[pi];
        cache.resize(static_cast<usize>(pg.perPage) * ParticleGpuPool::kCountersStride);
        // The slot recorded three frames ago is the one this frame will overwrite: complete on the GPU by now.
        res_->readBuffer(pg.readback[frame_ % 3], cache.data(), cache.size() * sizeof(u32), 0);
    }
}

bool ParticleGpu::counters(u32 handle, u32* out, u32 count) const {
    const Instance* in = find(handle);
    if (!in || !out || in->range.page >= counterCache_.size()) return false;
    const std::vector<u32>& cache = counterCache_[in->range.page];
    const usize base = static_cast<usize>(in->range.index) * ParticleGpuPool::kCountersStride;
    if (base + count > cache.size()) return false;
    for (u32 i = 0; i < count; ++i) out[i] = cache[base + i];
    return true;
}

u32 ParticleGpu::aliveApprox(u32 handle) const {
    const Instance* in = find(handle);
    if (!in || in->firstRan == 0 || frame_ < in->firstRan + 3) return 0u;
    u32 c[kGpuCounterCount];
    return counters(handle, c, kGpuCounterCount) ? c[kCtrStatAlive] : 0u;
}

// ------------------------------------------------------------------------------------------------ recording
namespace {

// Unique page indices among the items.
void uniquePages(const std::vector<u32>& pages, std::vector<u32>& out) {
    out = pages;
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

} // namespace

u32 ParticleGpu::record(rhi::IRenderContext& ctx, std::vector<GpuJob>& jobs, const View& view) {
    if (!ready_) return 0;
    ++frame_;
    refreshStats();
    if (jobs.empty()) return 0;

    std::vector<std::vector<Item>> rounds;
    std::vector<u32> seen(instances_.size() + 1, 0);
    for (GpuJob& j : jobs) {
        if (!j.visible) continue;
        Instance* in = find(j.handle);
        if (!in) continue;
        const u32 r = seen[j.handle]++;
        if (rounds.size() <= r) rounds.resize(r + 1);
        rounds[r].push_back({&j, in, j.handle});
    }
    if (rounds.empty()) return 0;

    ctx.pushMarker("Aver.Particles.Sim");
    to(ctx, dummy_, ResourceState::UnorderedAccess);
    for (std::vector<Item>& round : rounds) recordRound(ctx, round, view);

    // Rest states for the draw, and the stats copy.
    std::vector<u32> pageIds;
    u32 ran = 0;
    for (std::vector<Item>& round : rounds)
        for (Item& it : round) pageIds.push_back(it.inst->range.page);
    std::vector<u32> pages;
    uniquePages(pageIds, pages);
    for (u32 pi : pages) {
        ParticleGpuPool::Page& pg = pool_.page(pi);
        to(ctx, pg.particles, ResourceState::GeometryRead);
        to(ctx, pg.alive, ResourceState::GeometryRead);
        to(ctx, pg.args, ResourceState::IndirectArgument);
        to(ctx, pg.counters, ResourceState::CopySource);
        ctx.copyBuffer(pg.readback[frame_ % 3], pg.counters.h, static_cast<u64>(pg.perPage) * ParticleGpuPool::kCountersStride * 4u);
        to(ctx, pg.counters, ResourceState::UnorderedAccess);
    }
    for (std::vector<Item>& round : rounds)
        for (Item& it : round) {
            Instance& in = *it.inst;
            if (in.trail) to(ctx, in.trailBuf, ResourceState::GeometryRead);
            if (in.ranFrame != frame_) {
                if (in.firstRan == 0) in.firstRan = frame_;
                in.ranFrame = frame_;
                ++ran;
            }
        }
    ctx.popMarker();
    return ran;
}

void ParticleGpu::recordRound(rhi::IRenderContext& ctx, std::vector<Item>& items, const View& view) {
    using rhi::ResourceState;
    // ---- finalise headers, bring every touched buffer into UnorderedAccess ----
    std::vector<u32> pageIds;
    for (Item& it : items) {
        Instance& in = *it.inst;
        GpuSimHeader& h = it.job->cb.h;
        ParticleGpuPool::Page& pg = pool_.page(in.range.page);
        pageIds.push_back(in.range.page);
        to(ctx, pg.particles, ResourceState::UnorderedAccess);
        to(ctx, pg.dead, ResourceState::UnorderedAccess);
        to(ctx, pg.alive, ResourceState::UnorderedAccess);
        to(ctx, pg.counters, ResourceState::UnorderedAccess);
        to(ctx, pg.args, ResourceState::UnorderedAccess);
        if (in.sorted) to(ctx, pg.sortKeys, ResourceState::UnorderedAccess);
        if (in.trail) to(ctx, in.trailBuf, ResourceState::UnorderedAccess);
        to(ctx, in.eventsBuf, ResourceState::UnorderedAccess);
        if (Instance* parent = find(in.parent)) to(ctx, parent->eventsBuf, ResourceState::UnorderedAccess);

        h.capacity = in.capacity;
        h.aliveRead = in.readIndex * in.poolCapacity;
        h.aliveWrite = (1u - in.readIndex) * in.poolCapacity;
        h.sortedBase = 2u * in.poolCapacity;
        h.sortEnabled = in.sorted ? 1u : 0u;
        h.indexCount = in.indexCount;
        h.evWriteBase = static_cast<u32>(frame_ & 1u) * kGpuEventRegionUints;
        h.evReadBase = static_cast<u32>((frame_ + 1u) & 1u) * kGpuEventRegionUints;
        const Instance* par = find(in.parent);
        h.evActive = (h.evActive != 0 && par && par->hasEvents && par->ranFrame != 0 && par->ranFrame + 1 == frame_) ? 1u : 0u;
        if (in.hasEvents) h.flags |= kGpuFlagHasEvents;
        in.lastAliveWrite = in.sorted ? h.sortedBase : h.aliveWrite;
    }
    std::vector<u32> pages;
    uniquePages(pageIds, pages);

    const auto barrierAll = [&](bool withArgs) {
        for (u32 pi : pages) {
            ParticleGpuPool::Page& pg = pool_.page(pi);
            barrier(ctx, pg.particles);
            barrier(ctx, pg.dead);
            barrier(ctx, pg.alive);
            barrier(ctx, pg.counters);
            if (withArgs) barrier(ctx, pg.args);
            barrier(ctx, pg.sortKeys);
        }
        for (Item& it : items) {
            barrier(ctx, it.inst->eventsBuf);
            barrier(ctx, it.inst->trailBuf);
        }
    };
    const auto bind = [&](Item& it, u32 cbBytes) {
        ctx.setBindingSet(it.inst->set, 0);
        ctx.setConstantBuffer(kSimConstantRegister, &it.job->cb, cbBytes);
    };
    const auto argsTo = [&](ResourceState st) { for (u32 pi : pages) to(ctx, pool_.page(pi).args, st); };
    const auto argsOffset = [&](Item& it, u32 which) {
        return static_cast<u64>(it.inst->range.index * ParticleGpuPool::kArgsStride + which) * 4u;
    };

    // ---- Phase 0: init (new instances) ----
    {
        bool any = false;
        for (Item& it : items) {
            if (!it.inst->needsInit) continue;
            if (!any) { ctx.setPipeline(csInit_); any = true; }
            bind(it, kHeaderBytes);
            ctx.dispatch(groupsFor(it.inst->capacity, 64), 1, 1);
            it.inst->needsInit = false;
        }
        if (any) barrierAll(true);
    }

    // ---- Phase 1: begin ----
    ctx.setPipeline(csBegin_);
    for (Item& it : items) { bind(it, kHeaderBytes); ctx.dispatch(1, 1, 1); }
    barrierAll(true);
    argsTo(ResourceState::IndirectArgument);

    // ---- Phase 2: spawn (batch 0 is sized on the GPU, extra batches by the CPU) ----
    ctx.setPipeline(csSpawn_);
    for (Item& it : items) {
        bind(it, static_cast<u32>(sizeof(GpuSimCB)));
        ctx.dispatchIndirect(pool_.page(it.inst->range.page).args.h, argsOffset(it, kArgSpawn));
    }
    for (Item& it : items) {
        GpuJob& j = *it.job;
        if (j.extra.empty()) continue;
        GpuSimCB batch = j.cb;
        u32 firstId = j.cb.h.firstId + j.cb.h.spawnTotal;
        for (usize start = 0; start < j.extra.size(); start += kGpuMaxRequests) {
            const u32 n = static_cast<u32>(std::min<usize>(kGpuMaxRequests, j.extra.size() - start));
            batch.h.requestCount = n;
            batch.h.spawnTotal = 0;
            batch.h.firstId = firstId;
            batch.h.evActive = 0;
            for (u32 k = 0; k < n; ++k) {
                batch.req[k] = j.extra[start + k];
                batch.h.spawnTotal += batch.req[k].count;
            }
            firstId += batch.h.spawnTotal;
            ctx.setBindingSet(it.inst->set, 0);
            ctx.setConstantBuffer(kSimConstantRegister, &batch, static_cast<u32>(sizeof(GpuSimCB)));
            ctx.dispatch(groupsFor(batch.h.spawnTotal, 64), 1, 1);
        }
    }
    barrierAll(false);
    argsTo(ResourceState::UnorderedAccess);

    // ---- Phase 3: update arguments ----
    ctx.setPipeline(csArgsUpdate_);
    for (Item& it : items) { bind(it, kHeaderBytes); ctx.dispatch(1, 1, 1); }
    barrierAll(true);
    argsTo(ResourceState::IndirectArgument);

    // ---- Phase 4: update ----
    ctx.setPipeline(csUpdate_);
    for (Item& it : items) {
        bind(it, kHeaderBytes);
        ctx.dispatchIndirect(pool_.page(it.inst->range.page).args.h, argsOffset(it, kArgUpdate));
    }
    barrierAll(false);
    argsTo(ResourceState::UnorderedAccess);

    // ---- Phase 5: finish ----
    ctx.setPipeline(csFinish_);
    for (Item& it : items) { bind(it, kHeaderBytes); ctx.dispatch(1, 1, 1); }
    barrierAll(true);

    recordSort(ctx, items, view);

    for (Item& it : items)
        if (!it.inst->linked) it.inst->readIndex ^= 1u;
}

void ParticleGpu::recordSort(rhi::IRenderContext& ctx, std::vector<Item>& items, const View& view) {
    bool any = false;
    for (Item& it : items) any = any || it.inst->sorted;
    if (!any || !view.valid) return;

    GpuSortCB cb{};
    cb.viewRow[0] = view.viewProj[3];
    cb.viewRow[1] = view.viewProj[7];
    cb.viewRow[2] = view.viewProj[11];
    cb.viewRow[3] = view.viewProj[15];

    std::vector<SortPass> plan;
    for (Item& it : items) {
        Instance& in = *it.inst;
        if (!in.sorted) continue;
        ParticleGpuPool::Page& pg = pool_.page(in.range.page);
        cb.aliveWrite = it.job->cb.h.aliveWrite;
        cb.sortedBase = it.job->cb.h.sortedBase;
        const u32 N = in.poolCapacity;   // a power of two by construction; the kernels return early for the actual count
        const u32 tile = std::min(N, kSortTile);
        const auto run = [&](rhi::PipelineHandle p, u32 k, u32 j, u32 groups) {
            cb.k = k;
            cb.j = j;
            ctx.setPipeline(p);
            ctx.setBindingSet(in.set, 0);
            ctx.setConstantBuffer(kSimConstantRegister, &cb, static_cast<u32>(sizeof cb));
            ctx.dispatch(groups, 1, 1);
            ctx.uavBarrierBuffer(pg.sortKeys.h);
        };
        run(csSortKeys_, 0, 0, groupsFor(N, 256));
        buildSortPlan(N, plan);
        for (const SortPass& sp : plan) {
            switch (sp.kind) {
            case SortPassKind::Local:      run(csSortLocal_, sp.k, 0, N / tile); break;
            case SortPassKind::Global:     run(csSortGlobal_, sp.k, sp.j, groupsFor(N / 2u, 256)); break;
            case SortPassKind::LocalMerge: run(csSortLocalMerge_, sp.k, 0, N / kSortTile); break;
            }
        }
        run(csSortFinal_, 0, 0, groupsFor(N, 256));
        ctx.uavBarrierBuffer(pg.alive.h);
    }
}

// ------------------------------------------------------------------------------------------------ draw
bool ParticleGpu::drawBuffers(u32 handle, DrawBuffers& out) const {
    const Instance* in = find(handle);
    if (!in || in->ranFrame != frame_) return false;
    const ParticleGpuPool::Page& pg = pool_.page(in->range.page);
    const u32 C = in->poolCapacity, idx = in->range.index;
    out = DrawBuffers{};
    out.particles = pg.particles.h;
    out.particleFirst = idx * C;
    out.particleCount = C;
    out.order = pg.alive.h;
    out.orderFirst = idx * 3u * C + in->lastAliveWrite;
    out.orderCount = C;
    out.trail = in->trailBuf.h ? in->trailBuf.h : dummy_.h;
    out.trailFirst = 0;
    out.trailCount = in->trailBuf.h ? in->capacity * in->trailRing : kDummyBytes / 16u;
    out.args = pg.args.h;
    out.argsOffset = static_cast<u64>(idx * ParticleGpuPool::kArgsStride + kArgDraw) * 4u;
    out.capacity = in->capacity;
    out.trailRing = in->trailRing;
    out.linked = in->linked;
    return true;
}

// ------------------------------------------------------------------------------------------------ test seam
u64 ParticleGpu::dumpBytes(u32 handle, Dump what) const {
    const Instance* in = find(handle);
    if (!in) return 0;
    switch (what) {
    case Dump::Particles:    return static_cast<u64>(in->capacity) * sizeof(Particle);
    case Dump::SurvivorList: return static_cast<u64>(in->poolCapacity) * 12u;
    case Dump::Counters:     return static_cast<u64>(kGpuCounterCount) * 4u;
    case Dump::DeadList:     return static_cast<u64>(in->capacity) * 4u;
    case Dump::Events:       return in->hasEvents ? static_cast<u64>(kEventsBytes) : 0u;
    }
    return 0;
}

u64 ParticleGpu::dump(rhi::IRenderContext& ctx, u32 handle, Dump what, rhi::BufferHandle dst) {
    Instance* in = find(handle);
    const u64 bytes = dumpBytes(handle, what);
    if (!in || !bytes || !dst) return 0;
    ParticleGpuPool::Page& pg = pool_.page(in->range.page);
    const u32 C = in->poolCapacity, idx = in->range.index;
    TrackedBuffer* buf = nullptr;
    u64 srcOffset = 0;
    switch (what) {
    case Dump::Particles:    buf = &pg.particles; srcOffset = static_cast<u64>(idx) * C * sizeof(Particle); break;
    case Dump::SurvivorList: buf = &pg.alive;     srcOffset = static_cast<u64>(idx) * 3u * C * 4u; break;
    case Dump::Counters:     buf = &pg.counters;  srcOffset = static_cast<u64>(idx) * ParticleGpuPool::kCountersStride * 4u; break;
    case Dump::DeadList:     buf = &pg.dead;      srcOffset = static_cast<u64>(idx) * C * 4u; break;
    case Dump::Events:       buf = nullptr; break;
    }
    if (what == Dump::Events) {
        // Events stay in UnorderedAccess for their whole life.
        to(ctx, in->eventsBuf, ResourceState::CopySource);
        ctx.copyBuffer(dst, in->eventsBuf.h, bytes);
        to(ctx, in->eventsBuf, ResourceState::UnorderedAccess);
        return bytes;
    }
    const ResourceState rest = buf->st;
    to(ctx, *buf, ResourceState::CopySource);
    ctx.copyBuffer(dst, buf->h, bytes, 0, srcOffset);
    to(ctx, *buf, rest);
    return bytes;
}

namespace {
bool runProbe(ParticleGpu& self, rhi::IRenderContext& ctx, rhi::PipelineHandle pipe, rhi::BindingSetHandle set, const GpuSimCB& cb) {
    (void)self;
    ctx.setPipeline(pipe);
    ctx.setBindingSet(set, 0);
    ctx.setConstantBuffer(kSimConstantRegister, &cb, static_cast<u32>(sizeof(GpuSimCB)));
    ctx.dispatch(1, 1, 1);
    return true;
}
} // namespace

bool ParticleGpu::probe(rhi::IRenderContext& ctx, u32 handle, const GpuSimCB& cb, rhi::BufferHandle dst) {
    Instance* in = find(handle);
    if (!in || !dst || !csProbe_) return false;
    ParticleGpuPool::Page& pg = pool_.page(in->range.page);
    const ResourceState rest = pg.dead.st;
    to(ctx, dummy_, ResourceState::UnorderedAccess);
    to(ctx, pg.dead, ResourceState::UnorderedAccess);
    runProbe(*this, ctx, csProbe_, in->set, cb);
    ctx.uavBarrierBuffer(pg.dead.h);
    to(ctx, pg.dead, ResourceState::CopySource);
    ctx.copyBuffer(dst, pg.dead.h, 64u * 4u, 0, static_cast<u64>(in->range.index) * in->poolCapacity * 4u);
    to(ctx, pg.dead, rest == ResourceState::Common ? ResourceState::UnorderedAccess : rest);
    return true;
}

bool ParticleGpu::probeRng(rhi::IRenderContext& ctx, u32 handle, const GpuSimCB& cb, rhi::BufferHandle dst) {
    Instance* in = find(handle);
    if (!in || !dst || !csRngProbe_) return false;
    ParticleGpuPool::Page& pg = pool_.page(in->range.page);
    const ResourceState rest = pg.dead.st;
    to(ctx, dummy_, ResourceState::UnorderedAccess);
    to(ctx, pg.dead, ResourceState::UnorderedAccess);
    runProbe(*this, ctx, csRngProbe_, in->set, cb);
    ctx.uavBarrierBuffer(pg.dead.h);
    to(ctx, pg.dead, ResourceState::CopySource);
    ctx.copyBuffer(dst, pg.dead.h, 32u * 4u, 0, static_cast<u64>(in->range.index) * in->poolCapacity * 4u);
    to(ctx, pg.dead, rest == ResourceState::Common ? ResourceState::UnorderedAccess : rest);
    return true;
}

} // namespace aver::particles
