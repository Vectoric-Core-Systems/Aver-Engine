// RhiComputeHost.cpp -- see RhiComputeHost.hpp. The callbacks record into CPU structures under one mutex; replay() turns them into RHI commands on
// the render thread.
#include "aver/render/RhiComputeHost.hpp"

#include "aver/core/Log.hpp"
#include "aver/render/HairRegsTypes.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace aver::render {

namespace hairregs {
using aver::render::HairKernel;
using aver::render::HairKernelBinding;
#include "HairRegs.inc"
} // namespace hairregs

namespace {

constexpr u64 kMaxBufferBytes = 512ull << 20;
constexpr u64 kDummyBytes = 256;
constexpr u32 kDummyStride = 4;

const hairregs::HairKernel* findKernel(const char* name) {
    if (!name) return nullptr;
    for (unsigned i = 0; i < hairregs::kHairKernelCount; ++i)
        if (std::strcmp(hairregs::kHairKernels[i].name, name) == 0) return &hairregs::kHairKernels[i];
    return nullptr;
}

u64 roundPow2(u64 v) {
    u64 p = 256;
    while (p < v) p <<= 1;
    return p;
}

// One immutable version of a buffer's bytes. An op that bound a version keeps it alive; map(write) never changes a version an op holds.
struct Blob {
    std::vector<u8> bytes;
    u64 id = 0;
};
using BlobPtr = std::shared_ptr<Blob>;

} // namespace

// ---- shared state that outlives the host while a buffer or shader is still referenced by a recorded op ----

struct HostShared {
    std::mutex mu;                           // the CPU side: tables, queues, pending batches, blob versions
    std::mutex garbageMu;                    // what dying objects leave for the render thread
    std::vector<rhi::BufferHandle> garbageBuffers;
    std::vector<u64> deadUids;
    std::atomic<bool> alive{true};
};

namespace {

struct HostBuf {
    u64 uid = 0;
    int type = AVER_PHYS_CB_RW;
    u64 count = 0;
    u32 stride = 0;
    u64 bytes = 0;
    BlobPtr cur;                             // guarded by HostShared::mu
    // Render thread only:
    rhi::BufferHandle gpu = 0;
    rhi::BufferHandle readback = 0;
    rhi::ResourceState st = rhi::ResourceState::Common;
    u64 uploadedId = 0;
    bool written = false;
    std::shared_ptr<HostShared> owner;

    ~HostBuf() {
        if (!owner || !owner->alive.load()) return;
        std::lock_guard<std::mutex> lk(owner->garbageMu);
        if (gpu) owner->garbageBuffers.push_back(gpu);
        if (readback) owner->garbageBuffers.push_back(readback);
        owner->deadUids.push_back(uid);
    }
};

struct HostShader {
    u64 uid = 0;
    const hairregs::HairKernel* kernel = nullptr;
    std::shared_ptr<HostShared> owner;
};

// The compiled form of a kernel: one per kernel for the life of the host, whatever number of HostShaders Jolt makes of it.
struct KernelGpu {
    rhi::PipelineHandle pipe = 0;
    bool failed = false;
};

struct BindRec {
    std::shared_ptr<HostBuf> buf;
    BlobPtr blob;
    int role = -1;
    int barrier = 0;
};

struct DispatchRec {
    std::shared_ptr<HostShader> shader;
    std::vector<BindRec> b;                  // one per kernel binding, in table order
    BindRec ctx, iter;                       // constants (b3, b4)
    u32 gx = 0, gy = 0, gz = 0;
    std::shared_ptr<HostBuf> rbDst, rbSrc;   // a readback op when set (shader null)
};

struct QueueRec {
    std::shared_ptr<HostShader> shader;
    std::unordered_map<std::string, BindRec> binds;
    std::vector<DispatchRec> open;
};

struct Staging {
    rhi::BufferHandle h = 0;
    u64 bytes = 0;
    u64 busyUntil = 0;
};

} // namespace

struct RhiComputeHost::Impl {
    std::shared_ptr<HostShared> shared = std::make_shared<HostShared>();
    rhi::IResourceFactory* res = nullptr;

    // ---- the CPU side, guarded by shared->mu ----
    u64 nextHandle = 1;
    u64 nextBlobId = 1;
    std::unordered_map<u64, std::shared_ptr<HostShader>> shaders;
    std::unordered_map<u64, std::shared_ptr<HostBuf>> buffers;
    std::unordered_map<u64, std::unique_ptr<QueueRec>> queues;
    std::deque<std::vector<DispatchRec>> pending;
    std::vector<std::weak_ptr<HostBuf>> all;     // every buffer that may hold an RHI buffer, released or not
    u32 dropped = 0;

    // ---- the render side ----
    u64 frame = 1;
    std::shared_ptr<HostBuf> dummy[2];           // [0] bound to unfilled read-only slots, [1] to unfilled read-write ones
    std::vector<Staging> pool;
    std::vector<Staging> retiring;
    std::map<std::vector<u64>, rhi::BindingSetHandle> sets;
    std::unordered_map<const hairregs::HairKernel*, KernelGpu> kernels;
    u32 dispatchesLast = 0, uploadsLast = 0;
    u32 pipelineCount = 0;

    BlobPtr makeBlob(u64 bytes) {
        auto b = std::make_shared<Blob>();
        b->bytes.assign(static_cast<usize>(bytes), 0);
        b->id = nextBlobId++;
        return b;
    }

    // ---- render-side helpers ----
    bool ensureGpu(HostBuf& b);
    void transition(rhi::IRenderContext& ctx, HostBuf& b, rhi::ResourceState to);
    Staging takeStaging(u64 bytes);
    void giveStaging(Staging s);
    bool upload(rhi::IRenderContext& ctx, HostBuf& b, const Blob& blob);
    rhi::PipelineHandle ensurePipeline(const HostShader& s);
    HostBuf* prepare(rhi::IRenderContext& ctx, const BindRec& r, bool uav);
    rhi::BindingSetHandle bindingSetFor(const HostShader& s, const HostBuf* const* bufs);
    void runDispatch(rhi::IRenderContext& ctx, DispatchRec& d);
    void runReadback(rhi::IRenderContext& ctx, DispatchRec& d);
    void drainGarbage();
};

// ================================================================================ render side

bool RhiComputeHost::Impl::ensureGpu(HostBuf& b) {
    if (b.gpu) return true;
    if (b.bytes == 0 || !res) return false;
    rhi::BufferDesc bd;
    bd.bytes = b.bytes;
    bd.kind = rhi::BufferKind::Default;
    bd.allowUnorderedAccess = b.type == AVER_PHYS_CB_RW;
    bd.debugName = "Aver.Hair host buffer";
    b.gpu = res->createBuffer(bd);
    b.st = rhi::ResourceState::Common;
    b.uploadedId = 0;
    if (!b.gpu) AVER_ERROR("[Hair] could not create a {} byte compute buffer", b.bytes);
    return b.gpu != 0;
}

void RhiComputeHost::Impl::transition(rhi::IRenderContext& ctx, HostBuf& b, rhi::ResourceState to) {
    if (!b.gpu || b.st == to) return;
    ctx.bufferBarrier(b.gpu, b.st, to);
    b.st = to;
}

Staging RhiComputeHost::Impl::takeStaging(u64 bytes) {
    // The smallest idle pooled buffer that fits without being absurdly larger.
    usize best = pool.size();
    for (usize i = 0; i < pool.size(); ++i) {
        if (pool[i].busyUntil > frame || pool[i].bytes < bytes || pool[i].bytes > bytes * 4 + 4096) continue;
        if (best == pool.size() || pool[i].bytes < pool[best].bytes) best = i;
    }
    if (best != pool.size()) {
        Staging s = pool[best];
        pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(best));
        return s;
    }
    Staging s;
    s.bytes = roundPow2(bytes);
    rhi::BufferDesc bd;
    bd.bytes = s.bytes;
    bd.kind = rhi::BufferKind::Upload;
    bd.debugName = "Aver.Hair staging";
    s.h = res->createBuffer(bd);
    if (!s.h) AVER_ERROR("[Hair] could not create a {} byte staging buffer", s.bytes);
    return s;
}

void RhiComputeHost::Impl::giveStaging(Staging s) {
    if (!s.h) return;
    s.busyUntil = frame + kHairHostFramesInFlight + 1;
    if (s.bytes >= kHairHostPoolBufferBytes) retiring.push_back(s);   // big uploads (the settings buffers) are one-offs
    else pool.push_back(s);
}

bool RhiComputeHost::Impl::upload(rhi::IRenderContext& ctx, HostBuf& b, const Blob& blob) {
    if (!ensureGpu(b)) return false;
    const u64 n = std::min<u64>(blob.bytes.size(), b.bytes);
    if (n == 0) { b.uploadedId = blob.id; return true; }
    Staging s = takeStaging(n);
    if (!s.h) return false;
    if (!res->writeBuffer(s.h, blob.bytes.data(), n)) { giveStaging(s); return false; }
    transition(ctx, b, rhi::ResourceState::CopyDest);
    ctx.copyBuffer(b.gpu, s.h, n);
    b.uploadedId = blob.id;
    giveStaging(s);
    ++uploadsLast;
    // A read-only or read-write buffer's bytes are a one-off (initial data): drop the CPU copy once it is on the GPU. An UPLOAD buffer keeps its
    // current version, which Jolt rewrites every update.
    if (b.type == AVER_PHYS_CB_BUFFER || b.type == AVER_PHYS_CB_RW) {
        std::lock_guard<std::mutex> lk(shared->mu);
        if (b.cur && b.cur->id == blob.id) b.cur.reset();
    }
    return true;
}

rhi::PipelineHandle RhiComputeHost::Impl::ensurePipeline(const HostShader& s) {
    if (!res || !s.kernel) return 0;
    KernelGpu& k = kernels[s.kernel];
    if (k.pipe) return k.pipe;
    if (k.failed) return 0;
    k.failed = true;   // reset below on success; a failure is reported once
    const std::string& src = rhi::shaderFile(s.kernel->file);
    if (src.empty()) {
        AVER_ERROR("[Hair] shader file {} is missing; the {} kernel is off", s.kernel->file, s.kernel->name);
        return 0;
    }
    rhi::ShaderDesc sd;
    sd.source = src.c_str();
    sd.entry = "main";
    sd.stage = rhi::ShaderStage::Compute;
    sd.minShaderModel = 60;
    const rhi::ShaderHandle cs = res->createShader(sd);
    if (!cs) {
        AVER_ERROR("[Hair] the {} kernel ({}) would not compile; hair that needs it stays frozen", s.kernel->name, s.kernel->file);
        return 0;
    }
    rhi::ComputePipelineDesc pd;
    pd.cs = cs;
    pd.layout.srvCount = s.kernel->srvCount;
    pd.layout.uavCount = s.kernel->uavCount;
    pd.layout.slotKindsDeclared = true;
    for (u32 i = 0; i < s.kernel->srvCount; ++i) pd.layout.srvKinds[i] = rhi::SlotKind::StructuredBuffer;
    for (u32 i = 0; i < s.kernel->uavCount; ++i) pd.layout.uavKinds[i] = rhi::SlotKind::StructuredBuffer;
    pd.layout.constantDwords[kHairConstantSlotContext] = 0;     // root CBVs
    pd.layout.constantDwords[kHairConstantSlotIteration] = 0;
    k.pipe = res->createComputePipeline(pd);
    res->destroyShader(cs);
    if (!k.pipe) {
        AVER_ERROR("[Hair] the pipeline of the {} kernel could not be created", s.kernel->name);
        return 0;
    }
    k.failed = false;
    ++pipelineCount;
    return k.pipe;
}

// Makes the RHI buffer of a bind ready for a dispatch: created, filled with the bytes the op bound, and in the state the slot needs.
HostBuf* RhiComputeHost::Impl::prepare(rhi::IRenderContext& ctx, const BindRec& r, bool uav) {
    HostBuf* const dm = dummy[uav ? 1 : 0].get();
    HostBuf* b = r.buf.get();
    if (!b || b->bytes == 0 || !ensureGpu(*b)) {
        b = dm;
    } else if (r.blob && r.blob->id != b->uploadedId && b->type != AVER_PHYS_CB_READBACK) {
        if (!upload(ctx, *b, *r.blob)) b = dm;
    }
    if (!dm || !ensureGpu(*b)) return nullptr;
    if (uav) {
        if (b->st == rhi::ResourceState::UnorderedAccess) {
            if (r.barrier) ctx.uavBarrierBuffer(b->gpu);
        } else {
            transition(ctx, *b, rhi::ResourceState::UnorderedAccess);
        }
        b->written = true;
    } else if (b->st != rhi::ResourceState::NonPixelShaderResource) {
        transition(ctx, *b, rhi::ResourceState::NonPixelShaderResource);
    }
    return b;
}

rhi::BindingSetHandle RhiComputeHost::Impl::bindingSetFor(const HostShader& s, const HostBuf* const* bufs) {
    const hairregs::HairKernel& k = *s.kernel;
    std::vector<u64> key;
    key.reserve(k.bindingCount + 1);
    key.push_back(static_cast<u64>(s.kernel - &hairregs::kHairKernels[0]));   // the kernel, not the shader object: a re-installed host reuses its sets
    for (unsigned i = 0; i < k.bindingCount; ++i) key.push_back(bufs[i]->uid);
    const auto it = sets.find(key);
    if (it != sets.end()) return it->second;

    rhi::BindingSetDesc sd;
    sd.srvCount = k.srvCount;
    sd.uavCount = k.uavCount;
    for (u32 i = 0; i < k.srvCount; ++i) sd.srvKinds[i] = rhi::SlotKind::StructuredBuffer;
    for (u32 i = 0; i < k.uavCount; ++i) sd.uavKinds[i] = rhi::SlotKind::StructuredBuffer;
    const rhi::BindingSetHandle set = res->createBindingSet(sd);
    if (!set) return 0;
    for (unsigned i = 0; i < k.bindingCount; ++i) {
        const hairregs::HairKernelBinding& bd = k.bindings[i];
        const HostBuf* b = bufs[i];
        const u32 stride = b->stride ? b->stride : kDummyStride;
        const u32 count = static_cast<u32>(std::max<u64>(b->count, 1));
        if (bd.kind == 0) res->setSrvBuffer(set, bd.slot, b->gpu, stride, count, 0);
        else              res->setUavBuffer(set, bd.slot, b->gpu, stride, count, 0);
    }
    sets.emplace(std::move(key), set);
    return set;
}

void RhiComputeHost::Impl::runDispatch(rhi::IRenderContext& ctx, DispatchRec& d) {
    HostShader& sh = *d.shader;
    if (!sh.kernel) return;
    const rhi::PipelineHandle pipe = ensurePipeline(sh);
    if (!pipe) return;
    if (d.gx == 0 || d.gy == 0 || d.gz == 0) return;
    const hairregs::HairKernel& k = *sh.kernel;

    const HostBuf* bufs[kHairHostMaxBindings] = {};
    for (unsigned i = 0; i < k.bindingCount && i < kHairHostMaxBindings; ++i) {
        const bool uav = k.bindings[i].kind == 1;
        bufs[i] = prepare(ctx, d.b[i], uav);
        if (!bufs[i]) return;
    }
    const rhi::BindingSetHandle set = bindingSetFor(sh, bufs);
    if (!set) return;

    // Constants, padded to a multiple of 16 bytes (a root CBV view); an absent block reads as zeros.
    u8 cb[2][512] = {};
    u32 cbBytes[2] = {16, 16};
    const BindRec* cr[2] = {&d.ctx, &d.iter};
    for (int i = 0; i < 2; ++i) {
        if (cr[i]->blob) {
            const u32 n = static_cast<u32>(std::min<usize>(cr[i]->blob->bytes.size(), sizeof cb[i]));
            std::memcpy(cb[i], cr[i]->blob->bytes.data(), n);
            cbBytes[i] = std::max<u32>(16, (n + 15u) & ~15u);
        }
    }

    ctx.setPipeline(pipe);
    ctx.setBindingSet(set, 0);
    ctx.setConstantBuffer(kHairConstantSlotContext, cb[0], cbBytes[0]);
    ctx.setConstantBuffer(kHairConstantSlotIteration, cb[1], cbBytes[1]);
    ctx.dispatch(d.gx, d.gy, d.gz);
    ++dispatchesLast;
}

void RhiComputeHost::Impl::runReadback(rhi::IRenderContext& ctx, DispatchRec& d) {
    HostBuf* src = d.rbSrc.get();
    HostBuf* dst = d.rbDst.get();
    if (!src || !dst || src->bytes == 0 || !ensureGpu(*src)) return;
    if (!dst->readback) {
        rhi::BufferDesc bd;
        bd.bytes = dst->bytes;
        bd.kind = rhi::BufferKind::Readback;
        bd.debugName = "Aver.Hair readback";
        dst->readback = res->createBuffer(bd);
        if (!dst->readback) return;
    }
    transition(ctx, *src, rhi::ResourceState::CopySource);
    ctx.copyBuffer(dst->readback, src->gpu, std::min(src->bytes, dst->bytes));
}

void RhiComputeHost::Impl::drainGarbage() {
    std::vector<rhi::BufferHandle> gb;
    std::vector<u64> dead;
    {
        std::lock_guard<std::mutex> lk(shared->garbageMu);
        gb.swap(shared->garbageBuffers);
        dead.swap(shared->deadUids);
    }
    if (!res) return;
    for (rhi::BufferHandle h : gb) res->destroyBuffer(h);
    if (dead.empty()) return;
    // A binding set that names a buffer which no longer exists is destroyed with it (destruction is deferred by contract).
    const std::unordered_set<u64> deadSet(dead.begin(), dead.end());
    for (auto it = sets.begin(); it != sets.end();) {
        bool hit = false;
        for (usize i = 1; i < it->first.size() && !hit; ++i) hit = deadSet.count(it->first[i]) != 0;
        if (hit) { res->destroyBindingSet(it->second); it = sets.erase(it); }
        else ++it;
    }
}


// ================================================================================ the table (CPU side)

namespace {

using HostImpl = RhiComputeHost::Impl;

HostImpl& im(void* user) { return *static_cast<HostImpl*>(user); }

// The callbacks lock this for their whole body. A host that has been shut down answers every call with 0 / nothing.
struct Guard {
    explicit Guard(HostImpl& m) : lk(m.shared->mu), alive(m.shared->alive.load()) {}
    std::lock_guard<std::mutex> lk;
    bool alive;
};

AverPhysHostHandle cbCreateShader(void* user, const char* name, uint32_t, uint32_t, uint32_t) {
    HostImpl& m = im(user);
    const hairregs::HairKernel* k = findKernel(name);
    if (!k) return 0;
    Guard g(m);
    if (!g.alive) return 0;
    auto s = std::make_shared<HostShader>();
    s->uid = m.nextHandle++;
    s->kernel = k;
    s->owner = m.shared;
    const u64 h = s->uid;
    m.shaders.emplace(h, std::move(s));
    return h;
}

AverPhysHostHandle cbCreateBuffer(void* user, int32_t type, uint64_t count, uint32_t stride, const void* data) {
    if (type < AVER_PHYS_CB_UPLOAD || type > AVER_PHYS_CB_RW || stride == 0) return 0;
    if (count > kMaxBufferBytes / stride) return 0;
    const u64 bytes = count * stride;
    if (type == AVER_PHYS_CB_BUFFER && bytes != 0 && !data) return 0;
    HostImpl& m = im(user);
    Guard g(m);
    if (!g.alive) return 0;
    auto b = std::make_shared<HostBuf>();
    b->uid = m.nextHandle++;
    b->type = type;
    b->count = count;
    b->stride = stride;
    b->bytes = bytes;
    b->owner = m.shared;
    if (type == AVER_PHYS_CB_UPLOAD || type == AVER_PHYS_CB_CONSTANT) {
        b->cur = m.makeBlob(bytes);
    } else if (data && bytes) {
        b->cur = m.makeBlob(bytes);
        std::memcpy(b->cur->bytes.data(), data, static_cast<usize>(bytes));
    }
    const u64 h = b->uid;
    m.all.push_back(b);
    m.buffers.emplace(h, std::move(b));
    return h;
}

void* cbMap(void* user, AverPhysHostHandle h, int32_t write) {
    if (!write) return nullptr;   // a GPU host cannot hand the CPU what the GPU has not computed yet
    HostImpl& m = im(user);
    Guard g(m);
    if (!g.alive) return nullptr;
    const auto it = m.buffers.find(h);
    if (it == m.buffers.end() || it->second->type == AVER_PHYS_CB_READBACK) return nullptr;
    HostBuf& b = *it->second;
    if (!b.cur) {
        b.cur = m.makeBlob(b.bytes);
    } else if (b.cur.use_count() > 1) {
        // Copy on write: a recorded op still names the earlier bytes.
        auto n = std::make_shared<Blob>(*b.cur);
        n->id = m.nextBlobId++;
        b.cur = std::move(n);
    }
    return b.cur->bytes.data();
}

void cbUnmap(void*, AverPhysHostHandle) {}

void cbRelease(void* user, AverPhysHostHandle h) {
    HostImpl& m = im(user);
    Guard g(m);
    if (!g.alive) return;
    // Recorded ops keep what they reference alive; the object dies with the last of them.
    if (m.buffers.erase(h)) return;
    if (m.shaders.erase(h)) return;
    m.queues.erase(h);
}

AverPhysHostHandle cbCreateQueue(void* user) {
    HostImpl& m = im(user);
    Guard g(m);
    if (!g.alive) return 0;
    const u64 h = m.nextHandle++;
    m.queues.emplace(h, std::make_unique<QueueRec>());
    return h;
}

QueueRec* queueOf(HostImpl& m, AverPhysHostHandle q) {
    const auto it = m.queues.find(q);
    return it == m.queues.end() ? nullptr : it->second.get();
}

void cbSetShader(void* user, AverPhysHostHandle q, AverPhysHostHandle shader) {
    HostImpl& m = im(user);
    Guard g(m);
    if (!g.alive) return;
    QueueRec* qr = queueOf(m, q);
    if (!qr) return;
    const auto it = m.shaders.find(shader);
    qr->shader = it == m.shaders.end() ? nullptr : it->second;
    qr->binds.clear();
}

void cbBind(void* user, AverPhysHostHandle q, const char* name, AverPhysHostHandle buffer, int32_t role, int32_t barrier) {
    if (!name) return;
    HostImpl& m = im(user);
    Guard g(m);
    if (!g.alive) return;
    QueueRec* qr = queueOf(m, q);
    const auto it = m.buffers.find(buffer);
    if (!qr || it == m.buffers.end()) return;
    BindRec r;
    r.buf = it->second;
    r.blob = it->second->cur;     // the version current NOW: a later map(write) copies on write and leaves this one alone
    r.role = role;
    r.barrier = barrier;
    qr->binds[name] = std::move(r);
}

void cbDispatch(void* user, AverPhysHostHandle q, uint32_t gx, uint32_t gy, uint32_t gz) {
    HostImpl& m = im(user);
    Guard g(m);
    if (!g.alive) return;
    QueueRec* qr = queueOf(m, q);
    if (!qr || !qr->shader || !qr->shader->kernel) return;
    const hairregs::HairKernel& k = *qr->shader->kernel;
    DispatchRec d;
    d.shader = qr->shader;
    d.b.resize(k.bindingCount);
    for (unsigned i = 0; i < k.bindingCount; ++i) {
        const auto it = qr->binds.find(k.bindings[i].name);
        if (it != qr->binds.end()) d.b[i] = it->second;
    }
    auto ctx = qr->binds.find("gContext");
    if (ctx == qr->binds.end()) ctx = qr->binds.find("gWind");
    if (ctx != qr->binds.end()) d.ctx = ctx->second;
    const auto iter = qr->binds.find("gIterationContext");
    if (iter != qr->binds.end()) d.iter = iter->second;
    d.gx = gx; d.gy = gy; d.gz = gz;
    qr->open.push_back(std::move(d));
}

void cbReadback(void* user, AverPhysHostHandle q, AverPhysHostHandle dst, AverPhysHostHandle src) {
    HostImpl& m = im(user);
    Guard g(m);
    if (!g.alive) return;
    QueueRec* qr = queueOf(m, q);
    const auto d = m.buffers.find(dst);
    const auto s = m.buffers.find(src);
    if (!qr || d == m.buffers.end() || s == m.buffers.end()) return;
    DispatchRec r;
    r.rbDst = d->second;
    r.rbSrc = s->second;
    qr->open.push_back(std::move(r));
}

void cbExecute(void* user, AverPhysHostHandle q) {
    HostImpl& m = im(user);
    Guard g(m);
    if (!g.alive) return;
    QueueRec* qr = queueOf(m, q);
    if (!qr || qr->open.empty()) return;
    m.pending.push_back(std::move(qr->open));
    qr->open.clear();
    while (m.pending.size() > kHairHostMaxPendingBatches) {
        m.pending.pop_front();
        ++m.dropped;
    }
}

void cbWait(void*, AverPhysHostHandle) {}

} // namespace

// ================================================================================ public

RhiComputeHost::RhiComputeHost() : impl_(std::make_unique<Impl>()) {}

RhiComputeHost::~RhiComputeHost() { shutdown(); }

bool RhiComputeHost::init(rhi::IDevice& dev) {
    shutdown();
    Impl& m = *impl_;
    m.res = dev.resources();
    if (!m.res) {
        AVER_WARN("[Hair] this device has no resource factory; strand hair is off");
        return false;
    }
    if (!dev.caps().computeShaders) {
        AVER_WARN("[Hair] this device cannot run compute shaders; strand hair is off");
        m.res = nullptr;
        return false;
    }
    for (unsigned i = 0; i < hairregs::kHairKernelCount; ++i)
        if (hairregs::kHairKernels[i].bindingCount > kHairHostMaxBindings) {
            AVER_ERROR("[Hair] the {} kernel has {} bindings; the host supports {}", hairregs::kHairKernels[i].name,
                       hairregs::kHairKernels[i].bindingCount, kHairHostMaxBindings);
            m.res = nullptr;
            return false;
        }
    m.shared = std::make_shared<HostShared>();
    m.nextHandle = 16;
    m.nextBlobId = 1;
    m.frame = 1;
    m.dropped = 0;
    for (int i = 0; i < 2; ++i) {
        auto d = std::make_shared<HostBuf>();
        d->uid = static_cast<u64>(i + 1);
        d->type = AVER_PHYS_CB_RW;
        d->count = kDummyBytes / kDummyStride;
        d->stride = kDummyStride;
        d->bytes = kDummyBytes;
        d->owner = m.shared;
        m.dummy[i] = d;
        m.all.push_back(d);
    }

    table_ = {};
    table_.structSize = sizeof(AverPhysComputeHostV1);
    table_.version = 1;
    table_.user = impl_.get();
    table_.create_shader = &cbCreateShader;
    table_.create_buffer = &cbCreateBuffer;
    table_.map = &cbMap;
    table_.unmap = &cbUnmap;
    table_.release = &cbRelease;
    table_.create_queue = &cbCreateQueue;
    table_.set_shader = &cbSetShader;
    table_.bind = &cbBind;
    table_.dispatch = &cbDispatch;
    table_.readback = &cbReadback;
    table_.execute = &cbExecute;
    table_.wait = &cbWait;
    ready_ = true;
    return true;
}

void RhiComputeHost::shutdown() {
    if (!impl_) return;
    Impl& m = *impl_;
    ready_ = false;
    if (!m.res) { m.shared->alive.store(false); return; }

    // Objects dying from here on leave nothing behind: this function destroys every RHI object itself.
    m.shared->alive.store(false);
    std::vector<std::shared_ptr<HostBuf>> bufs;
    {
        std::lock_guard<std::mutex> lk(m.shared->mu);
        for (auto& w : m.all) if (auto p = w.lock()) bufs.push_back(p);
        m.pending.clear();
        m.queues.clear();
        m.buffers.clear();
        m.shaders.clear();
        m.all.clear();
    }
    for (auto& b : bufs) {
        if (b->gpu) m.res->destroyBuffer(b->gpu);
        if (b->readback) m.res->destroyBuffer(b->readback);
        b->gpu = 0;
        b->readback = 0;
    }
    for (auto& kv : m.kernels)
        if (kv.second.pipe) m.res->destroyPipeline(kv.second.pipe);
    m.kernels.clear();
    for (auto& kv : m.sets) m.res->destroyBindingSet(kv.second);
    m.sets.clear();
    for (Staging& s : m.pool) if (s.h) m.res->destroyBuffer(s.h);
    for (Staging& s : m.retiring) if (s.h) m.res->destroyBuffer(s.h);
    m.pool.clear();
    m.retiring.clear();
    {
        std::lock_guard<std::mutex> lk(m.shared->garbageMu);
        for (rhi::BufferHandle h : m.shared->garbageBuffers) m.res->destroyBuffer(h);
        m.shared->garbageBuffers.clear();
        m.shared->deadUids.clear();
    }
    m.dummy[0].reset();
    m.dummy[1].reset();
    m.pipelineCount = 0;
    m.res = nullptr;
    table_ = {};
}

void RhiComputeHost::beginFrame() {
    if (!ready_) return;
    Impl& m = *impl_;
    ++m.frame;
    m.drainGarbage();
    for (usize i = 0; i < m.retiring.size();) {
        if (m.retiring[i].busyUntil <= m.frame) {
            if (m.retiring[i].h) m.res->destroyBuffer(m.retiring[i].h);
            m.retiring[i] = m.retiring.back();
            m.retiring.pop_back();
        } else {
            ++i;
        }
    }
    std::lock_guard<std::mutex> lk(m.shared->mu);
    m.all.erase(std::remove_if(m.all.begin(), m.all.end(), [](const std::weak_ptr<HostBuf>& w) { return w.expired(); }), m.all.end());
}

void RhiComputeHost::replay(rhi::IRenderContext& ctx) {
    if (!ready_) return;
    Impl& m = *impl_;
    std::deque<std::vector<DispatchRec>> batches;
    {
        std::lock_guard<std::mutex> lk(m.shared->mu);
        batches.swap(m.pending);
    }
    m.dispatchesLast = 0;
    m.uploadsLast = 0;
    if (batches.empty()) return;
    ctx.pushMarker("Aver.Hair.Sim");
    for (auto& batch : batches)
        for (DispatchRec& d : batch) {
            if (d.rbDst) m.runReadback(ctx, d);
            else if (d.shader) m.runDispatch(ctx, d);
        }
    ctx.popMarker();
}

void RhiComputeHost::endFrame(rhi::IRenderContext& ctx) {
    if (!ready_) return;
    Impl& m = *impl_;
    std::vector<std::shared_ptr<HostBuf>> live;
    {
        std::lock_guard<std::mutex> lk(m.shared->mu);
        for (auto& w : m.all) if (auto p = w.lock()) live.push_back(p);
    }
    for (auto& b : live)
        if (b->gpu && b->st != rhi::ResourceState::Common) m.transition(ctx, *b, rhi::ResourceState::Common);
}

namespace {
std::shared_ptr<HostBuf> findBuf(RhiComputeHost::Impl& m, AverPhysHostHandle h) {
    std::lock_guard<std::mutex> lk(m.shared->mu);
    const auto it = m.buffers.find(h);
    return it == m.buffers.end() ? nullptr : it->second;
}
} // namespace

rhi::BufferHandle RhiComputeHost::gpuBuffer(AverPhysHostHandle h) {
    if (!ready_) return 0;
    const std::shared_ptr<HostBuf> b = findBuf(*impl_, h);
    if (!b || !impl_->ensureGpu(*b)) return 0;
    return b->gpu;
}

void RhiComputeHost::ensureState(rhi::IRenderContext& ctx, AverPhysHostHandle h, rhi::ResourceState state) {
    if (!ready_) return;
    const std::shared_ptr<HostBuf> b = findBuf(*impl_, h);
    if (!b || !impl_->ensureGpu(*b)) return;
    impl_->transition(ctx, *b, state);
}

bool RhiComputeHost::written(AverPhysHostHandle h) const {
    if (!ready_) return false;
    const std::shared_ptr<HostBuf> b = findBuf(*impl_, h);
    return b && b->written;
}

u64 RhiComputeHost::bufferCount(AverPhysHostHandle h) const {
    if (!ready_) return 0;
    const std::shared_ptr<HostBuf> b = findBuf(*impl_, h);
    return b ? b->count : 0;
}

u32 RhiComputeHost::bufferStride(AverPhysHostHandle h) const {
    if (!ready_) return 0;
    const std::shared_ptr<HostBuf> b = findBuf(*impl_, h);
    return b ? b->stride : 0;
}

rhi::BufferHandle RhiComputeHost::recordReadback(rhi::IRenderContext& ctx, AverPhysHostHandle h, u64* outBytes) {
    if (!ready_) return 0;
    Impl& m = *impl_;
    const std::shared_ptr<HostBuf> b = findBuf(m, h);
    if (!b || b->bytes == 0 || !m.ensureGpu(*b)) return 0;
    rhi::BufferDesc bd;
    bd.bytes = b->bytes;
    bd.kind = rhi::BufferKind::Readback;
    bd.debugName = "Aver.Hair debug readback";
    const rhi::BufferHandle rb = m.res->createBuffer(bd);
    if (!rb) return 0;
    m.transition(ctx, *b, rhi::ResourceState::CopySource);
    ctx.copyBuffer(rb, b->gpu, b->bytes);
    if (outBytes) *outBytes = b->bytes;
    return rb;
}

RhiComputeHostStats RhiComputeHost::stats() const {
    RhiComputeHostStats s;
    if (!ready_) return s;
    Impl& m = *impl_;
    {
        std::lock_guard<std::mutex> lk(m.shared->mu);
        s.pendingBatches = static_cast<u32>(m.pending.size());
        s.droppedBatches = m.dropped;
        s.hostBuffers = static_cast<u32>(m.buffers.size());
        for (const auto& kv : m.buffers)
            if (kv.second->gpu) { ++s.gpuBuffers; s.gpuBytes += kv.second->bytes; }
    }
    s.dispatchesLastReplay = m.dispatchesLast;
    s.uploadsLastReplay = m.uploadsLast;
    s.bindingSets = static_cast<u32>(m.sets.size());
    s.pipelines = m.pipelineCount;
    return s;
}

} // namespace aver::render
