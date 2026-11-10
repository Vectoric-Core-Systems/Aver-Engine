// NeuRaCGpuTest -- the radiance cache's real shaders against NeuRaCLayout.hpp's CPU mirrors, on a real device.
//
// NeuRaCTest checks the arithmetic on the CPU only; nothing ran voxi_neurac_io.hlsli or the resolve. Here the
// real NeuRaC class (create, beginFrame, recordResolve) drives a small harness shader that includes the twins'
// scatter and lookup, and every outcome is compared with the CPU mirrors. Skips (77) when no D3D12 device,
// no DXC / SM 6.2, or IDevice::runStandaloneCompute is unavailable. `hw` uses the adapter instead of WARP.
//   a) rcHash16 vs neurac::hash16 on random samples
//   b) scatter: arrivals land exactly in the cells scatterCell predicts, per cascade (offset binning, interior,
//      cascade choice), and nowhere else
//   c) the per-cell cap per cascade (64 / 16 / 8) and the load-adaptive accept rate across frames
//   d) the resolve: first-sample mean, running blend, relaxAlpha, trust carry, stale-tag replacement, aging
//   e) rcLookup (value and remaining) vs a CPU lookup built from lookupCornerWeight, with and without trust
//   g-i) stage 2a: voxi_neurac_net.hlsl's Reset/Features/Finish/Apply vs NeuRaCNetReference.hpp on a synthetic cell
//      buffer: counters, inference and training records, hash thinning, the trust bits Apply writes, its stats
#include "aver/core/Log.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/rhi/ShaderFiles.hpp"
#include "aver/voxi/NeuRaC.hpp"
#include "aver/voxi/NeuRaCLayout.hpp"
#include "aver/voxi/NeuRaCNetReference.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef AVER_REPO_ROOT
#error "AVER_REPO_ROOT must name the repository root"
#endif

using namespace aver;
namespace rc = aver::voxi::neurac;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok  {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

constexpr int kSkip = 77;   // ctest's SKIP_RETURN_CODE

// The harness. Declares what the twins declare (t22/u20/u21 there, t0/u0/u1 here) plus an input and an output
// buffer, and exposes the real scatter and lookup as kernels. Inputs are uint4 so index words never travel as
// float denormals. The pure-maths and I/O headers are the real files, found through the shader source dir.
const char* kHarness = R"HLSL(
#include "voxi_neurac.hlsli"

StructuredBuffer<RcInfo>   gRcInfo  : register(t0);
StructuredBuffer<uint4>    gIn      : register(t1);
RWStructuredBuffer<int>    gRcAccum : register(u0);
RWStructuredBuffer<RcCell> gRcCells : register(u1);
RWStructuredBuffer<uint4>  gOut     : register(u2);

static const float4 gAmbientParams = float4(0.0, 0.0, 0.0, 128.0);

#include "voxi_neurac_io.hlsli"

float3 hIn3(uint i)
{
    const uint4 v = gIn[i];
    return float3(asfloat(v.x), asfloat(v.y), asfloat(v.z));
}

// 4 uint4 per sample: (hit, cos), (N), (dir2), (radiance). cos <= 0 (the zero padding) is skipped.
[numthreads(64, 1, 1)]
void CSScatter(uint3 t : SV_DispatchThreadID)
{
    const float cosd = asfloat(gIn[t.x * 4u].w);
    if (cosd <= 0.0) return;
    rcScatter(hIn3(t.x * 4u), hIn3(t.x * 4u + 1u), hIn3(t.x * 4u + 2u), hIn3(t.x * 4u + 3u), cosd);
}

// 2 uint4 per query: (p, 1 = valid), (N). Out: (value.xyz, remaining) as bits.
[numthreads(64, 1, 1)]
void CSLookup(uint3 t : SV_DispatchThreadID)
{
    if (gIn[t.x * 2u].w == 0u) return;
    float rem;
    const float3 v = rcLookup(hIn3(t.x * 2u), hIn3(t.x * 2u + 1u), rem);
    gOut[t.x] = uint4(asuint(v.x), asuint(v.y), asuint(v.z), asuint(rem));
}

// 1 uint4 per write: (cell index, normal word, 1 = valid).
[numthreads(64, 1, 1)]
void CSPoke(uint3 t : SV_DispatchThreadID)
{
    const uint4 r = gIn[t.x];
    if (r.z == 0u) return;
    gRcCells[r.x].b.z = r.y;
}

// 2 uint4 per record: (pos, cascade), (dir, 1 = valid). Out: the 16-bit hash.
[numthreads(64, 1, 1)]
void CSHash(uint3 t : SV_DispatchThreadID)
{
    if (gIn[t.x * 2u + 1u].w == 0u) return;
    const uint4 a = gIn[t.x * 2u];
    gOut[t.x] = uint4(rcHash16(asfloat(a.xyz), hIn3(t.x * 2u + 1u), a.w), 0u, 0u, 0u);
}
)HLSL";

// Deterministic uniform [-1, 1) from a 32-bit LCG, so every machine feeds the shaders the same data.
struct Lcg {
    u32 s = 12345u;
    f32 next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<f32>(s >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }
};

f32 dot3(const f32 a[3], const f32 b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void normalize3(f32 v[3]) {
    const f32 l = std::sqrt(dot3(v, v));
    for (int i = 0; i < 3; ++i) v[i] /= l;
}

struct Sample {
    f32 pos[3]{}, n[3]{}, dir[3]{}, L[3]{};
    f32 cosd = 0.0f;
};

// dir2 spread around n; cos floored at 0.12 so the sample weight stays modest (the shader floors it at 0.1).
void setDir(Sample& s, Lcg& rng, f32 spread) {
    for (int i = 0; i < 3; ++i) s.dir[i] = s.n[i] + spread * rng.next();
    normalize3(s.dir);
    s.cosd = std::max(dot3(s.dir, s.n), 0.12f);
}

u32 bitsOf(f32 v) { u32 u; std::memcpy(&u, &v, 4); return u; }
f32 floatOf(u32 u) { f32 v; std::memcpy(&v, &u, 4); return v; }

// 16 floats (as uint4 x 4) per sample.
std::vector<u32> packSamples(const std::vector<Sample>& samples) {
    std::vector<u32> out;
    out.reserve(samples.size() * 16);
    for (const Sample& s : samples) {
        out.insert(out.end(), {bitsOf(s.pos[0]), bitsOf(s.pos[1]), bitsOf(s.pos[2]), bitsOf(s.cosd)});
        out.insert(out.end(), {bitsOf(s.n[0]), bitsOf(s.n[1]), bitsOf(s.n[2]), 0u});
        out.insert(out.end(), {bitsOf(s.dir[0]), bitsOf(s.dir[1]), bitsOf(s.dir[2]), 0u});
        out.insert(out.end(), {bitsOf(s.L[0]), bitsOf(s.L[1]), bitsOf(s.L[2]), 0u});
    }
    return out;
}

struct Query { f32 p[3], n[3]; };

constexpr u32 kInRecords  = 65536;   // uint4
constexpr u32 kOutRecords = 8192;    // uint4

// The cell the first resolve of a sample set produces: mean of identical samples, quantised like the shader.
void expectedSh(const Sample& s, f32 out[rc::kShFloats]) {
    f32 y[4];
    rc::shBasis(s.dir, y);
    const f32 wgt = 3.14159265f / std::max(s.cosd, static_cast<f32>(rc::kMinCos));
    for (int k = 0; k < 4; ++k)
        for (int ch = 0; ch < 3; ++ch) {
            const f32 t = s.L[ch] * (y[k] * wgt * static_cast<f32>(rc::kShScale));
            out[3 * k + ch] = static_cast<f32>(static_cast<f64>(std::llround(t)) / rc::kShScale);
        }
}

void cellSh(const rc::RcCell& c, f32 sh[rc::kShFloats]) {
    const u32 w[6] = {c.a[0], c.a[1], c.a[2], c.a[3], c.b[0], c.b[1]};
    rc::unpackSh(w, sh);
}

bool shClose(const f32 got[rc::kShFloats], const f32 want[rc::kShFloats], f32 absTol, f32 relTol, f32* worst = nullptr) {
    bool ok = true;
    f32 w = 0.0f;
    for (u32 k = 0; k < rc::kShFloats; ++k) {
        const f32 err = std::fabs(got[k] - want[k]);
        w = std::max(w, err);
        if (!(err <= absTol + relTol * std::fabs(want[k]))) ok = false;
    }
    if (worst) *worst = w;
    return ok;
}

// A point where a +-jitter box around it, binned for normal n, falls in one interior cell of every cascade.
bool stableCenter(const f32 cam[3], const f32 off0[3], const f32 step[3], const f32 n[3], f32 jitter, f32 out[3]) {
    for (int i = 0; i < 300; ++i) {
        f32 c[3];
        for (int a = 0; a < 3; ++a) c[a] = cam[a] + off0[a] + step[a] * static_cast<f32>(i);
        bool ok = true;
        for (u32 cas = 0; cas < rc::kCascades && ok; ++cas) {
            const f32 size = rc::kCellSizeCm[cas];
            i32 org[3], first[3];
            for (int a = 0; a < 3; ++a) org[a] = rc::snapOrigin(cam[a], size);
            if (!rc::scatterCell(c, n, cas, org, size, first)) { ok = false; break; }
            for (int m = 0; m < 8 && ok; ++m) {
                f32 p[3];
                for (int a = 0; a < 3; ++a) p[a] = c[a] + ((m >> a) & 1 ? jitter : -jitter);
                i32 cell[3];
                if (!rc::scatterCell(p, n, cas, org, size, cell) || cell[0] != first[0] || cell[1] != first[1] ||
                    cell[2] != first[2])
                    ok = false;
            }
        }
        if (ok) { for (int a = 0; a < 3; ++a) out[a] = c[a]; return true; }
    }
    return false;
}

struct Env {
    rhi::IDevice* dev = nullptr;
    rhi::IResourceFactory* res = nullptr;
    voxi::NeuRaC cache;
    voxi::NeuRaC::Bindings last{};
    f32 cam[3] = {1234.5f, 87.0f, -400.0f};
    rhi::PipelineHandle psScatter = 0, psLookup = 0, psPoke = 0, psHash = 0;
    rhi::BufferHandle inBuf = 0, outBuf = 0;
    std::map<rhi::BufferHandle, rhi::BindingSetHandle> sets;
    u32 frameCounter = 0;

    ~Env() {
        if (!res) return;
        res->waitIdle();
        for (auto& kv : sets) if (kv.second) res->destroyBindingSet(kv.second);
        for (rhi::PipelineHandle p : {psScatter, psLookup, psPoke, psHash}) if (p) res->destroyPipeline(p);
        for (rhi::BufferHandle b : {inBuf, outBuf}) if (b) res->destroyBuffer(b);
        cache.destroy();
    }

    // Normal frames use index 16k + 1 so the resolve's aging cadence (index % 16 == 0) never fires by accident.
    u32 nextFrame() { return 16u * (++frameCounter) + 1u; }

    rhi::PipelineHandle makePipeline(const char* entry) {
        rhi::ShaderDesc sd{};
        sd.source = kHarness;
        sd.entry = entry;
        sd.stage = rhi::ShaderStage::Compute;
        sd.minShaderModel = 62;
        const rhi::ShaderHandle cs = res->createShader(sd);
        if (!cs) return 0;
        rhi::ComputePipelineDesc pd{};
        pd.cs = cs;
        pd.layout.srvCount = 2;
        pd.layout.uavCount = 3;
        pd.layout.slotKindsDeclared = true;
        for (u32 i = 0; i < 2; ++i) pd.layout.srvKinds[i] = rhi::SlotKind::StructuredBuffer;
        for (u32 i = 0; i < 3; ++i) pd.layout.uavKinds[i] = rhi::SlotKind::StructuredBuffer;
        const rhi::PipelineHandle p = res->createComputePipeline(pd);
        res->destroyShader(cs);
        return p;
    }

    bool create() {
        res = dev->resources();
        if (!cache.create(*res)) return false;
        rhi::BufferDesc bd{};
        bd.kind = rhi::BufferKind::Upload;
        bd.bytes = static_cast<u64>(kInRecords) * 16;
        bd.debugName = "NeuRaC test input";
        inBuf = res->createBuffer(bd);
        bd.kind = rhi::BufferKind::Default;
        bd.allowUnorderedAccess = true;
        bd.bytes = static_cast<u64>(kOutRecords) * 16;
        bd.debugName = "NeuRaC test output";
        outBuf = res->createBuffer(bd);
        if (!inBuf || !outBuf) return false;
        psScatter = makePipeline("CSScatter");
        psLookup = makePipeline("CSLookup");
        psPoke = makePipeline("CSPoke");
        psHash = makePipeline("CSHash");
        return psScatter && psLookup && psPoke && psHash;
    }

    rhi::BindingSetHandle setFor(const voxi::NeuRaC::Bindings& b) {
        const auto it = sets.find(b.info);
        if (it != sets.end()) return it->second;
        rhi::BindingSetDesc bsd;
        bsd.srvCount = 2;
        bsd.uavCount = 3;
        for (u32 i = 0; i < 2; ++i) bsd.srvKinds[i] = rhi::SlotKind::StructuredBuffer;
        for (u32 i = 0; i < 3; ++i) bsd.uavKinds[i] = rhi::SlotKind::StructuredBuffer;
        const rhi::BindingSetHandle s = res->createBindingSet(bsd);
        res->setSrvBuffer(s, 0, b.info, b.infoStride, b.infoCount, 0);
        res->setSrvBuffer(s, 1, inBuf, 16, kInRecords, 0);
        res->setUavBuffer(s, 0, b.accum, 4, b.accumInts, 0);
        res->setUavBuffer(s, 1, b.cells, b.cellStride, b.cellCount, 0);
        res->setUavBuffer(s, 2, outBuf, 16, kOutRecords, 0);
        sets[b.info] = s;
        return s;
    }

    // Writes the records and zero-fills the rest, so a padding thread reads a record that says "skip".
    bool upload(const std::vector<u32>& words) {
        std::vector<u32> all(static_cast<usize>(kInRecords) * 4, 0u);
        if (words.size() > all.size()) return false;
        std::copy(words.begin(), words.end(), all.begin());
        return res->writeBuffer(inBuf, all.data(), all.size() * sizeof(u32), 0);
    }

    bool readBytes(rhi::BufferHandle src, u64 offset, u64 bytes, void* dst) {
        rhi::BufferDesc bd{};
        bd.bytes = bytes;
        bd.kind = rhi::BufferKind::Readback;
        bd.debugName = "NeuRaC test readback";
        const rhi::BufferHandle rb = res->createBuffer(bd);
        if (!rb) return false;
        using rhi::ResourceState;
        const bool ran = dev->runStandaloneCompute([&](rhi::IRenderContext& ctx) {
            ctx.bufferBarrier(src, ResourceState::Common, ResourceState::CopySource);
            ctx.copyBuffer(rb, src, bytes, 0, offset);
            ctx.bufferBarrier(src, ResourceState::CopySource, ResourceState::Common);
        });
        const bool ok = ran && res->readBuffer(rb, dst, bytes, 0);
        res->waitIdle();
        res->destroyBuffer(rb);
        return ok;
    }

    // beginFrame plus the scatter dispatch; the resolve is a separate submission so the accumulator can be read between.
    bool scatterFrame(u32 frameIndex, const voxi::NeuRaC::Params& p, const std::vector<Sample>& samples) {
        if (!upload(packSamples(samples))) return false;
        const u32 groups = static_cast<u32>((samples.size() + 63) / 64);
        bool began = false;
        const bool ran = dev->runStandaloneCompute([&](rhi::IRenderContext& ctx) {
            last = cache.beginFrame(ctx, cam, frameIndex, p);
            if (!last.info || !last.accum || !last.cells) return;
            began = true;
            if (groups == 0) return;
            ctx.setPipeline(psScatter);
            ctx.setBindingSet(setFor(last));
            ctx.dispatch(groups, 1, 1);
            ctx.uavBarrierBuffer(last.accum);
        });
        return ran && began;
    }
    bool resolve() {
        return dev->runStandaloneCompute([&](rhi::IRenderContext& ctx) { cache.recordResolve(ctx); });
    }
    bool frame(u32 frameIndex, const voxi::NeuRaC::Params& p, const std::vector<Sample>& samples) {
        return scatterFrame(frameIndex, p, samples) && resolve();
    }

    rc::RcCell readCell(u32 idx) {
        rc::RcCell c{};
        readBytes(last.cells, static_cast<u64>(idx) * rc::kCellStride, rc::kCellStride, &c);
        return c;
    }
    bool readAllCells(std::vector<rc::RcCell>& out) {
        out.assign(rc::kCells, rc::RcCell{});
        return readBytes(last.cells, 0, rc::kCellsBytes, out.data());
    }

    // One lookup frame (no scatter, no resolve): value.xyz and remaining per query.
    bool lookups(f32 netStrength, const std::vector<Query>& q, std::vector<f32>& out) {
        std::vector<u32> w;
        for (const Query& x : q) {
            w.insert(w.end(), {bitsOf(x.p[0]), bitsOf(x.p[1]), bitsOf(x.p[2]), 1u});
            w.insert(w.end(), {bitsOf(x.n[0]), bitsOf(x.n[1]), bitsOf(x.n[2]), 0u});
        }
        if (q.size() > kOutRecords || !upload(w)) return false;
        voxi::NeuRaC::Params p{};
        p.netStrength = netStrength;
        const u32 groups = static_cast<u32>((q.size() + 63) / 64);
        bool began = false;
        const bool ran = dev->runStandaloneCompute([&](rhi::IRenderContext& ctx) {
            last = cache.beginFrame(ctx, cam, nextFrame(), p);
            if (!last.info) return;
            began = true;
            ctx.setPipeline(psLookup);
            ctx.setBindingSet(setFor(last));
            ctx.dispatch(groups, 1, 1);
        });
        if (!ran || !began) return false;
        std::vector<u32> raw(q.size() * 4);
        if (!readBytes(outBuf, 0, raw.size() * sizeof(u32), raw.data())) return false;
        out.resize(raw.size());
        for (usize i = 0; i < raw.size(); ++i) out[i] = floatOf(raw[i]);
        return true;
    }

    bool poke(const std::vector<std::pair<u32, u32>>& writes) {
        std::vector<u32> w;
        for (const auto& kv : writes) w.insert(w.end(), {kv.first, kv.second, 1u, 0u});
        if (!upload(w)) return false;
        const u32 groups = static_cast<u32>((writes.size() + 63) / 64);
        return dev->runStandaloneCompute([&](rhi::IRenderContext& ctx) {
            ctx.setPipeline(psPoke);
            ctx.setBindingSet(setFor(last));
            ctx.dispatch(groups, 1, 1);
        });
    }

    bool hashes(const std::vector<Sample>& s, const std::vector<u32>& cascade, std::vector<u32>& out) {
        std::vector<u32> w;
        for (usize i = 0; i < s.size(); ++i) {
            w.insert(w.end(), {bitsOf(s[i].pos[0]), bitsOf(s[i].pos[1]), bitsOf(s[i].pos[2]), cascade[i]});
            w.insert(w.end(), {bitsOf(s[i].dir[0]), bitsOf(s[i].dir[1]), bitsOf(s[i].dir[2]), 1u});
        }
        if (s.size() > kOutRecords || !upload(w)) return false;
        const u32 groups = static_cast<u32>((s.size() + 63) / 64);
        const bool ran = dev->runStandaloneCompute([&](rhi::IRenderContext& ctx) {
            ctx.setPipeline(psHash);
            ctx.setBindingSet(setFor(last));
            ctx.dispatch(groups, 1, 1);
        });
        if (!ran) return false;
        std::vector<u32> raw(s.size() * 4);
        if (!readBytes(outBuf, 0, raw.size() * sizeof(u32), raw.data())) return false;
        out.resize(s.size());
        for (usize i = 0; i < s.size(); ++i) out[i] = raw[i * 4];
        return true;
    }

    // Accepted count (low half), last frame's arrivals (high half), this frame's arrivals, and the number of
    // payload additions (summed normal x over its integer step) for one cell.
    struct CellLoad { u32 accepted = 0, prev = 0, arrivals = 0, payloadCount = 0; i32 sqSum = 0; };
    CellLoad readLoad(u32 idx, const f32 n[3]) {
        CellLoad l;
        u32 word = 0, arr = 0;
        readBytes(last.accum, static_cast<u64>(idx) * 4, 4, &word);
        readBytes(last.accum, static_cast<u64>(rc::kArrivalsBase + idx) * 4, 4, &arr);
        i32 nsum[3] = {};
        readBytes(last.accum, (static_cast<u64>(rc::kPayloadBase) + static_cast<u64>(idx) * rc::kPayloadInts + 12) * 4,
                  12, nsum);
        readBytes(last.accum, (static_cast<u64>(rc::kSqBase) + idx) * 4, 4, &l.sqSum);
        l.accepted = word & 0xFFFFu;
        l.prev = word >> 16;
        l.arrivals = arr;
        // The largest component carries the count best.
        int a = 0;
        for (int i = 1; i < 3; ++i) if (std::fabs(n[i]) > std::fabs(n[a])) a = i;
        const i32 step = static_cast<i32>(std::lround(n[a] * static_cast<f32>(rc::kNScale)));
        l.payloadCount = step != 0 ? static_cast<u32>(std::llabs(static_cast<long long>(nsum[a]) / step)) : 0u;
        return l;
    }
};

u32 cellIdx(const Env& e, u32 cascade, const f32 pos[3], const f32 n[3], bool* interior = nullptr) {
    const f32 size = rc::kCellSizeCm[cascade];
    i32 org[3], cell[3];
    for (int a = 0; a < 3; ++a) org[a] = rc::snapOrigin(e.cam[a], size);
    const bool in = rc::scatterCell(pos, n, cascade, org, size, cell);
    if (interior) *interior = in;
    return rc::cellIndex(cascade, cell[0], cell[1], cell[2]);
}

// CPU lookup over a cell snapshot: rcLookup's loop with the shared corner weight.
void lookupRef(const Env& e, const rc::RcCell* cells, f32 netStrength, const f32 p[3], const f32 N[3], f32 outV[3], f32& rem) {
    rem = 1.0f;
    outV[0] = outV[1] = outV[2] = 0.0f;
    for (u32 c = 0; c < rc::kCascades; ++c) {
        if (rem < 0.01f) break;
        const f32 size = rc::kCellSizeCm[c];
        i32 org[3];
        f32 q[3], g[3], f[3];
        i32 b[3];
        bool skip = false;
        for (int a = 0; a < 3; ++a) {
            org[a] = rc::snapOrigin(e.cam[a], size);
            q[a] = (p[a] + N[a] * (0.5f * size)) / size - static_cast<f32>(org[a]);
            g[a] = q[a] - 0.5f;
            const f32 fl = std::floor(g[a]);
            f[a] = g[a] - fl;
            b[a] = static_cast<i32>(fl);
            if (b[a] < static_cast<i32>(rc::kMarginCells) || b[a] > static_cast<i32>(rc::kRes) - 2 - static_cast<i32>(rc::kMarginCells))
                skip = true;
        }
        if (skip) continue;
        f32 dEdge = 1e30f;
        for (int a = 0; a < 3; ++a) dEdge = std::min(dEdge, std::min(q[a], static_cast<f32>(rc::kRes) - q[a]));
        const f32 aEdge = std::min(1.0f, std::max(0.0f, (dEdge - static_cast<f32>(rc::kMarginCells)) / static_cast<f32>(rc::kCrossFadeCells)));
        f32 acc[4][3] = {};
        f32 wsum = 0.0f, tsum = 0.0f;
        for (int i = 0; i < 8; ++i) {
            const int o[3] = {i & 1, (i >> 1) & 1, (i >> 2) & 1};
            i32 wc[3];
            f32 tri = 1.0f;
            for (int a = 0; a < 3; ++a) {
                wc[a] = b[a] + o[a] + org[a];
                tri *= o[a] ? f[a] : 1.0f - f[a];
            }
            const rc::RcCell& cell = cells[rc::cellIndex(c, wc[0], wc[1], wc[2])];
            const u32 meta = cell.b[3];
            if (!rc::cellValid(meta, rc::tagPack(wc[0], wc[1], wc[2]))) continue;
            f32 dir[3], len, trust;
            rc::unpackNormal(cell.b[2], dir, len, trust);
            const f32 w = rc::lookupCornerWeight(tri, dot3(dir, N), len, rc::metaNEff(meta), rc::metaAge(meta), trust, netStrength);
            if (w <= 0.0f) continue;
            tsum += tri;
            f32 sh[rc::kShFloats];
            cellSh(cell, sh);
            for (int k = 0; k < 4; ++k)
                for (int ch = 0; ch < 3; ++ch) acc[k][ch] += w * sh[3 * k + ch];
            wsum += w;
        }
        if (wsum <= 1e-5f) continue;
        f32 blended[4][3], value[3];
        for (int k = 0; k < 4; ++k)
            for (int ch = 0; ch < 3; ++ch) blended[k][ch] = acc[k][ch] / wsum;
        rc::shIrradianceOverPi(blended, N, value);
        const f32 a = aEdge * std::min(1.0f, wsum / tsum) * std::min(1.0f, tsum / static_cast<f32>(rc::kMinCover));
        for (int ch = 0; ch < 3; ++ch) outV[ch] += rem * a * value[ch];
        rem *= 1.0f - a;
    }
}

void runGpu(rhi::IDevice& dev) {
    Env e;
    e.dev = &dev;
    rhi::setShaderSourceDir(std::string(AVER_REPO_ROOT) + "/modules/render.voxi/shaders");
    if (!e.create()) {
        check(false, "NeuRaC::create and the four harness pipelines (the resolve and the real scatter/lookup compile)");
        return;
    }
    check(true, "NeuRaC::create and the four harness pipelines (the resolve and the real scatter/lookup compile)");

    const voxi::NeuRaC::Params params{};   // cap 64, coarse 16 / 8, alphaMin 1/16, aging every 16 frames, no net
    Lcg rng;

    // ---- a) rcHash16 vs neurac::hash16 ----------------------------------------------------------------
    {
        // Needs a bound set; any frame will do.
        check(e.scatterFrame(e.nextFrame(), params, {}), "first frame: the clear dispatch and beginFrame run");
        std::vector<Sample> s(384);
        std::vector<u32> cas(s.size());
        for (usize i = 0; i < s.size(); ++i) {
            for (int a = 0; a < 3; ++a) { s[i].pos[a] = rng.next() * 3000.0f; s[i].dir[a] = rng.next(); }
            normalize3(s[i].dir);
            cas[i] = static_cast<u32>(i % 3);
        }
        std::vector<u32> got;
        check(e.hashes(s, cas, got), "hash kernel ran");
        usize bad = 0;
        for (usize i = 0; i < s.size() && i < got.size(); ++i)
            if (got[i] != rc::hash16(s[i].pos, s[i].dir, cas[i])) ++bad;
        check(got.size() == s.size() && bad == 0, "rcHash16 equals neurac::hash16 on 384 samples (" + std::to_string(bad) + " differ)");
        std::vector<bool> seen(65536, false);
        usize distinct = 0;
        for (u32 h : got) if (h < 65536 && !seen[h]) { seen[h] = true; ++distinct; }
        check(distinct > 300, "the hashes are spread (" + std::to_string(distinct) + " distinct of 384)");
    }

    // ---- b/c/d) one cluster of samples through three cascades ------------------------------------------
    f32 n1[3] = {0.5f, 0.8f, 0.33f};
    normalize3(n1);
    f32 center1[3];
    {
        const f32 off0[3] = {40.0f, 21.0f, 10.0f}, step[3] = {3.17f, 1.1f, 0.7f};
        const bool found = stableCenter(e.cam, off0, step, n1, 1.7f, center1);
        check(found, "a cluster centre whose +-1.7 cm box bins to one interior cell in all three cascades");
        if (!found) return;
    }
    u32 idx1[rc::kCascades];
    for (u32 c = 0; c < rc::kCascades; ++c) idx1[c] = cellIdx(e, c, center1, n1);

    Sample base1;
    for (int a = 0; a < 3; ++a) base1.n[a] = n1[a];
    {
        const f32 d[3] = {n1[0] * 0.9f + 0.2f, n1[1] * 0.9f - 0.3f, n1[2] * 0.9f + 0.1f};
        for (int a = 0; a < 3; ++a) base1.dir[a] = d[a];
        normalize3(base1.dir);
        base1.cosd = std::max(dot3(base1.dir, base1.n), 0.12f);
        base1.L[0] = 1.0f; base1.L[1] = 0.5f; base1.L[2] = 0.25f;
    }
    const auto cluster = [&](const f32 centre[3], u32 count, bool varyDir, const Sample& tmpl, f32 shift) {
        std::vector<Sample> v(count, tmpl);
        for (Sample& s : v) {
            for (int a = 0; a < 3; ++a) s.pos[a] = centre[a] + 1.5f * rng.next();
            s.pos[0] += shift;
            if (varyDir) setDir(s, rng, 0.8f);
        }
        return v;
    };

    {
        AVER_INFO("--- per-cell cap and load: 400 identical-direction samples into one cell per cascade");
        const std::vector<Sample> s = cluster(center1, 400, false, base1, 0.0f);
        check(e.scatterFrame(e.nextFrame(), params, s), "frame 1: 400 samples scattered (prev arrivals 0, rate 1)");
        const u32 caps[3] = {64, 16, 8};
        for (u32 c = 0; c < 3; ++c) {
            const Env::CellLoad l = e.readLoad(idx1[c], n1);
            const std::string tag = "cascade " + std::to_string(c) + ": ";
            check(l.arrivals == 400u, tag + "every arrival is counted (" + std::to_string(l.arrivals) + ")");
            check(l.payloadCount == caps[c], tag + "exactly cap " + std::to_string(caps[c]) + " samples reach the payload (got " +
                                                  std::to_string(l.payloadCount) + ")");
            check(l.accepted >= caps[c] && l.accepted <= 400u, tag + "the accepted counter reached the cap (" + std::to_string(l.accepted) + ")");
        }
        // Nothing else trained: arrivals are non-zero in exactly the three predicted cells.
        std::vector<u32> arr(rc::kCells);
        check(e.readBytes(e.last.accum, static_cast<u64>(rc::kArrivalsBase) * 4, rc::kCells * 4ull, arr.data()), "arrivals array read");
        usize nonzero = 0;
        for (u32 v : arr) if (v) ++nonzero;
        check(nonzero == 3, "arrivals are non-zero in exactly 3 cells (the sample's cell per cascade), got " + std::to_string(nonzero));
        // The sum of squares: each accepted sample adds round(s^2 * 2^11), s = lum(L) * PI / max(cos, 0.1) / (4 PI).
        {
            const f32 wgt = static_cast<f32>(rc::kPi) / std::max(base1.cosd, static_cast<f32>(rc::kMinCos));
            const f32 sc = (0.2126f * base1.L[0] + 0.7152f * base1.L[1] + 0.0722f * base1.L[2]) * wgt * static_cast<f32>(rc::kDcToE);
            const i64 one = std::llround(sc * sc * rc::kSqScale);
            bool ok = true;
            for (u32 c = 0; c < 3; ++c) {
                const Env::CellLoad l = e.readLoad(idx1[c], n1);
                ok = ok && std::llabs(static_cast<i64>(l.sqSum) - one * caps[c]) <= static_cast<i64>(caps[c]);   // +-1 per sample
            }
            check(ok && one > 0, "the sum of squares of each cell is cap x round(s^2 * 2048), one atomic per accepted sample (s^2 * 2048 = " +
                                     std::to_string(one) + ")");
            std::vector<i32> sq(rc::kCells);
            check(e.readBytes(e.last.accum, static_cast<u64>(rc::kSqBase) * 4, rc::kCells * 4ull, sq.data()), "sum-of-squares block read");
            usize sqNonzero = 0;
            for (i32 v : sq) if (v) ++sqNonzero;
            check(sqNonzero == 3, "the sum of squares is non-zero in exactly the same 3 cells, got " + std::to_string(sqNonzero));
        }
        check(e.resolve(), "frame 1: resolve");
        {
            std::vector<i32> sq(rc::kCells);
            check(e.readBytes(e.last.accum, static_cast<u64>(rc::kSqBase) * 4, rc::kCells * 4ull, sq.data()), "sum-of-squares block read after the resolve");
            usize sqNonzero = 0;
            for (i32 v : sq) if (v) ++sqNonzero;
            check(sqNonzero == 0, "the resolve zeroes the sum of squares together with the payload, got " + std::to_string(sqNonzero) + " non-zero");
        }

        f32 want[rc::kShFloats];
        expectedSh(base1, want);
        for (u32 c = 0; c < 3; ++c) {
            const rc::RcCell cell = e.readCell(idx1[c]);
            const std::string tag = "cascade " + std::to_string(c) + " after the first resolve: ";
            i32 org[3], wc[3];
            for (int a = 0; a < 3; ++a) org[a] = rc::snapOrigin(e.cam[a], rc::kCellSizeCm[c]);
            rc::scatterCell(center1, n1, c, org, rc::kCellSizeCm[c], wc);
            check(rc::metaNEff(cell.b[3]) == 1 && rc::metaAge(cell.b[3]) == 0 &&
                      rc::metaTag(cell.b[3]) == rc::tagPack(wc[0], wc[1], wc[2]),
                  tag + "n_eff 1, age 0 and the right world-cell tag");
            f32 sh[rc::kShFloats], worst = 0.0f;
            cellSh(cell, sh);
            check(shClose(sh, want, 2e-3f, 2e-3f, &worst), tag + "the SH is the mean of the samples (worst error " + std::to_string(worst) + ")");
            f32 dir[3], len, trust;
            rc::unpackNormal(cell.b[2], dir, len, trust);
            check(dot3(dir, n1) > 0.99998f && len > 0.98f && trust == 1.0f,
                  tag + "mean normal = N, planarity ~1, a new cell has trust 1.0");
        }
        {
            u32 w0 = 0, a0 = 0;
            e.readBytes(e.last.accum, static_cast<u64>(idx1[0]) * 4, 4, &w0);
            e.readBytes(e.last.accum, static_cast<u64>(rc::kArrivalsBase + idx1[0]) * 4, 4, &a0);
            check(w0 == (400u << 16) && a0 == 0u, "the resolve turns the arrivals into next frame's hint (400 << 16) and clears the rest");
        }

        // Frame 2: prev = 400 > cap, so the rate drops and a random subset is kept.
        check(e.scatterFrame(e.nextFrame(), params, s), "frame 2: the same 400 samples (prev arrivals 400)");
        const Env::CellLoad l0 = e.readLoad(idx1[0], n1), l1 = e.readLoad(idx1[1], n1), l2 = e.readLoad(idx1[2], n1);
        check(l0.prev == 400u && l1.prev == 400u && l2.prev == 400u, "the resolve left 400 as last frame's arrivals in all three cells");
        check(l0.arrivals == 400u && l1.arrivals == 400u && l2.arrivals == 400u, "arrivals are still counted in full");
        check(l0.payloadCount >= 30 && l0.payloadCount <= 64,
              "cap 64 at rate 0.16: a random ~64 are kept (got " + std::to_string(l0.payloadCount) + ")");
        check(l1.payloadCount >= 4 && l1.payloadCount <= 16,
              "cap 16 at rate 0.04: up to 16 are kept (got " + std::to_string(l1.payloadCount) + ")");
        check(l2.payloadCount >= 1 && l2.payloadCount <= 8,
              "cap 8 at the 1/32 floor: up to 8 are kept (got " + std::to_string(l2.payloadCount) + ")");
        check(e.resolve(), "frame 2: resolve");

        // The floor: 4000 arrivals one frame, then 1024 distinct samples. Rate 1/32 keeps ~32 of 1024, under cap 64.
        const std::vector<Sample> big = cluster(center1, 4000, true, base1, 0.0f);
        check(e.frame(e.nextFrame(), params, big), "frame 3: 4000 samples with varied directions, resolved");
        const std::vector<Sample> mid = cluster(center1, 1024, true, base1, 0.0f);
        check(e.scatterFrame(e.nextFrame(), params, mid), "frame 4: 1024 samples (prev arrivals 4000)");
        const Env::CellLoad f0 = e.readLoad(idx1[0], n1), f1 = e.readLoad(idx1[1], n1), f2 = e.readLoad(idx1[2], n1);
        check(f0.prev == 4000u && f0.arrivals == 1024u, "cascade 0: prev 4000, arrivals 1024");
        check(f0.accepted >= 12 && f0.accepted <= 56,
              "cascade 0: at the 1/32 floor ~32 of 1024 are accepted (got " + std::to_string(f0.accepted) + ")");
        check(f1.payloadCount >= 10 && f1.payloadCount <= 16 && f2.payloadCount >= 5 && f2.payloadCount <= 8,
              "cascades 1 and 2: the same rate fills their caps of 16 and 8 (got " + std::to_string(f1.payloadCount) + " and " +
                  std::to_string(f2.payloadCount) + ")");
        check(e.resolve(), "frame 4: resolve");
    }

    // ---- b) cascade choice, offset binning, interior: many cells at once -------------------------------
    std::vector<Sample> spread;
    {
        AVER_INFO("--- scatter targets: 104 samples across the three windows");
        const f32 scales[3] = {600.0f, 3000.0f, 12000.0f};
        usize tries = 0;
        while (spread.size() < 96 && tries++ < 5000) {
            Sample s;
            const f32 scale = scales[spread.size() % 3];
            for (int a = 0; a < 3; ++a) { s.pos[a] = e.cam[a] + scale * rng.next(); s.n[a] = rng.next(); }
            normalize3(s.n);
            // Stay clear of cell boundaries so a one-ulp difference in the division cannot flip a floor().
            bool clear = true;
            for (u32 c = 0; c < rc::kCascades; ++c)
                for (int a = 0; a < 3; ++a) {
                    const f32 size = rc::kCellSizeCm[c];
                    const f32 qv = (s.pos[a] + s.n[a] * 0.5f * size) / size;
                    const f32 fr = qv - std::floor(qv);
                    if (fr < 0.03f || fr > 0.97f) clear = false;
                }
            if (!clear) continue;
            setDir(s, rng, 0.6f);
            for (int a = 0; a < 3; ++a) s.L[a] = 0.5f + 1.5f * std::fabs(rng.next());
            spread.push_back(s);
        }
        // Window-edge samples: facing +y so only y shifts; x at relative cells 1, 2, 3, 5, 7 and 62, 61, 58.
        const i32 relx[8] = {1, 2, 3, 5, 7, 62, 61, 58};
        const i32 orgx = rc::snapOrigin(e.cam[0], rc::kCellSizeCm[0]);
        const i32 orgy = rc::snapOrigin(e.cam[1], rc::kCellSizeCm[0]);
        for (int i = 0; i < 8; ++i) {
            Sample s;
            s.n[0] = 0.0f; s.n[1] = 1.0f; s.n[2] = 0.0f;
            s.pos[0] = (static_cast<f32>(orgx + relx[i]) + 0.37f) * rc::kCellSizeCm[0];
            s.pos[1] = (static_cast<f32>(orgy + 35) + 0.41f) * rc::kCellSizeCm[0] - 12.5f;
            s.pos[2] = e.cam[2] + 100.0f + 7.0f * static_cast<f32>(i);
            setDir(s, rng, 0.4f);
            s.L[0] = 1.0f; s.L[1] = 2.0f; s.L[2] = 0.5f;
            spread.push_back(s);
        }

        // Predicted arrivals per (cascade, cell).
        std::map<u32, u32> expect;
        usize trained[3] = {};
        for (const Sample& s : spread)
            for (u32 c = 0; c < rc::kCascades; ++c) {
                bool in = false;
                const u32 idx = cellIdx(e, c, s.pos, s.n, &in);
                if (in) { ++expect[idx]; ++trained[c]; }
            }
        check(trained[0] > 10 && trained[1] > 20 && trained[2] > 30,
              "the samples exercise every window (trained " + std::to_string(trained[0]) + " / " + std::to_string(trained[1]) +
                  " / " + std::to_string(trained[2]) + " cascade cells; the rest fall outside it)");
        check(trained[0] < trained[1] && trained[1] < trained[2], "coarser windows catch more of the spread samples");

        check(e.scatterFrame(e.nextFrame(), params, spread), "spread samples scattered");
        std::vector<u32> arr(rc::kCells);
        check(e.readBytes(e.last.accum, static_cast<u64>(rc::kArrivalsBase) * 4, rc::kCells * 4ull, arr.data()), "arrivals array read");
        usize wrong = 0, stray = 0;
        for (u32 i = 0; i < rc::kCells; ++i) {
            const auto it = expect.find(i);
            const u32 want = it == expect.end() ? 0u : it->second;
            if (arr[i] != want) { if (want == 0) ++stray; else ++wrong; }
        }
        check(wrong == 0 && stray == 0, "every arrival lands in the cell scatterCell predicts and in no other (" +
                                            std::to_string(wrong) + " wrong, " + std::to_string(stray) + " stray)");
        check(e.resolve(), "spread samples: resolve");
        // Three more frames of the same samples bring n_eff to 4, where the lookup weight is no longer 1/4.
        for (int i = 0; i < 3; ++i) check(e.frame(e.nextFrame(), params, spread), "spread samples: another frame");
    }

    // ---- e) lookup vs the CPU reference ---------------------------------------------------------------
    {
        AVER_INFO("--- rcLookup against the CPU lookup");
        std::vector<rc::RcCell> cells;
        check(e.readAllCells(cells), "the whole cell buffer read back");
        std::vector<Query> queries;
        for (const Sample& s : spread) {
            Query q{};
            for (int a = 0; a < 3; ++a) { q.p[a] = s.pos[a]; q.n[a] = s.n[a]; }
            queries.push_back(q);
            for (int a = 0; a < 3; ++a) q.p[a] = s.pos[a] + 9.0f * rng.next();
            queries.push_back(q);
            for (int a = 0; a < 3; ++a) { q.p[a] = s.pos[a]; q.n[a] = -s.n[a]; }
            queries.push_back(q);
        }
        for (int i = 0; i < 64; ++i) {
            Query q{};
            for (int a = 0; a < 3; ++a) { q.p[a] = e.cam[a] + 700.0f * rng.next(); q.n[a] = rng.next(); }
            normalize3(q.n);
            queries.push_back(q);
        }
        const auto compare = [&](f32 netStrength, const char* label) {
            std::vector<f32> gpu;
            if (!e.lookups(netStrength, queries, gpu)) { check(false, std::string(label) + ": lookup frame ran"); return; }
            usize bad = 0, strong = 0, partial = 0;
            f32 worstV = 0.0f, worstR = 0.0f;
            for (usize i = 0; i < queries.size(); ++i) {
                f32 v[3], rem;
                lookupRef(e, cells.data(), netStrength, queries[i].p, queries[i].n, v, rem);
                bool ok = std::fabs(gpu[i * 4 + 3] - rem) <= 2e-4f;
                worstR = std::max(worstR, std::fabs(gpu[i * 4 + 3] - rem));
                for (int ch = 0; ch < 3; ++ch) {
                    const f32 err = std::fabs(gpu[i * 4 + ch] - v[ch]);
                    worstV = std::max(worstV, err);
                    if (!(err <= 1e-3f + 1e-3f * std::fabs(v[ch]))) ok = false;
                }
                if (!ok) ++bad;
                if (rem < 0.9f) ++strong;
                if (rem < 0.999f) ++partial;
            }
            check(bad == 0, std::string(label) + ": value and remaining match the CPU lookup on " + std::to_string(queries.size()) +
                                " queries (" + std::to_string(bad) + " differ; worst value " + std::to_string(worstV) +
                                ", remaining " + std::to_string(worstR) + ")");
            // A sample's own cell holds >= 1/8 of the lookup weight at its position, so most (a) queries read < 0.9.
            check(strong > 40 && partial > 100,
                  std::string(label) + ": the queries are not vacuous (" + std::to_string(strong) + " below 0.9 remaining, " +
                      std::to_string(partial) + " with some cache light)");
        };
        compare(0.0f, "network off");

        // Trust: lower it on every c0 cell the first 40 samples trained, then look up with the network at full strength.
        std::vector<std::pair<u32, u32>> pokes;
        std::map<u32, bool> poked;   // one write per cell: two writes to one cell would race
        usize count = 0;
        for (usize i = 0; i < spread.size() && count < 40; ++i) {
            bool in = false;
            const u32 idx = cellIdx(e, 0, spread[i].pos, spread[i].n, &in);
            if (!in || poked[idx]) continue;
            poked[idx] = true;
            const u32 t6 = (count % 3 == 0) ? 0u : (count % 3 == 1 ? 21u : 42u);
            const u32 word = (cells[idx].b[2] & 0x03FFFFFFu) | (t6 << 26);
            cells[idx].b[2] = word;
            pokes.emplace_back(idx, word);
            ++count;
        }
        check(e.poke(pokes), "trust written into " + std::to_string(pokes.size()) + " cells");
        std::vector<rc::RcCell> after;
        check(e.readAllCells(after) && std::memcmp(after.data(), cells.data(), cells.size() * sizeof(rc::RcCell)) == 0,
              "the cells hold exactly the poked words (nothing else changed)");
        compare(1.0f, "network at strength 1");
        compare(0.0f, "network off, trust ignored");
    }

    // ---- d) the resolve: blend, relax, trust carry, replacement, aging ---------------------------------
    {
        AVER_INFO("--- resolve: running blend, relaxAlpha, trust carry, stale-tag replacement, aging");
        f32 n3[3] = {-0.3f, 0.7f, 0.5f};
        normalize3(n3);
        f32 center3[3];
        const f32 off0[3] = {-400.0f, -60.0f, 150.0f}, step[3] = {2.9f, 0.8f, -1.3f};
        const bool found = stableCenter(e.cam, off0, step, n3, 1.7f, center3);
        check(found, "a second cluster centre in one interior cell");
        if (!found) return;
        const u32 idx = cellIdx(e, 0, center3, n3);
        {
            const rc::RcCell c0 = e.readCell(idx);
            check(rc::metaNEff(c0.b[3]) == 0, "the second cluster's cascade-0 cell starts empty");
        }
        Sample s3;
        for (int a = 0; a < 3; ++a) s3.n[a] = n3[a];
        for (int a = 0; a < 3; ++a) s3.dir[a] = n3[a] * 0.8f + (a == 0 ? 0.25f : (a == 1 ? -0.2f : 0.15f));
        normalize3(s3.dir);
        s3.cosd = std::max(dot3(s3.dir, s3.n), 0.12f);

        const f32 Ls[3][3] = {{1.0f, 0.5f, 0.25f}, {0.2f, 0.8f, 2.0f}, {0.7f, 0.7f, 0.1f}};
        const auto run = [&](const f32 L[3], f32 shift) {
            Sample t = s3;
            for (int a = 0; a < 3; ++a) t.L[a] = L[a];
            check(e.frame(e.nextFrame(), params, cluster(center3, 50, false, t, shift)), "resolve scenario frame ran");
            return t;
        };
        f32 prevSh[rc::kShFloats];
        const auto stepCheck = [&](const Sample& t, f32 alpha, u32 wantNEff, const std::string& what) {
            f32 mean[rc::kShFloats], want[rc::kShFloats], got[rc::kShFloats], worst = 0.0f;
            expectedSh(t, mean);
            for (u32 k = 0; k < rc::kShFloats; ++k) want[k] = prevSh[k] + (mean[k] - prevSh[k]) * alpha;
            const rc::RcCell cell = e.readCell(idx);
            cellSh(cell, got);
            check(shClose(got, want, 3e-3f, 3e-3f, &worst) && rc::metaNEff(cell.b[3]) == wantNEff && rc::metaAge(cell.b[3]) == 0,
                  what + " (n_eff " + std::to_string(rc::metaNEff(cell.b[3])) + ", worst SH error " + std::to_string(worst) + ")");
            std::memcpy(prevSh, got, sizeof prevSh);
        };

        Sample t = run(Ls[0], 0.0f);
        {
            f32 want[rc::kShFloats], got[rc::kShFloats], worst = 0.0f;
            expectedSh(t, want);
            const rc::RcCell cell = e.readCell(idx);
            cellSh(cell, got);
            check(shClose(got, want, 2e-3f, 2e-3f, &worst) && rc::metaNEff(cell.b[3]) == 1,
                  "first sample: the cell is the mean itself (worst error " + std::to_string(worst) + ")");
            std::memcpy(prevSh, got, sizeof prevSh);
        }
        t = run(Ls[1], 0.0f);
        stepCheck(t, 0.5f, 2, "second frame blends at 1/(n_eff+1) = 1/2");
        t = run(Ls[1], 0.0f);
        stepCheck(t, 1.0f / 3.0f, 3, "third frame blends at 1/3");

        // Lower the cell's trust, then request a relax of two frames at 0.9.
        {
            const rc::RcCell cell = e.readCell(idx);
            const u32 word = (cell.b[2] & 0x03FFFFFFu) | (20u << 26);
            check(e.poke({{idx, word}}), "trust 20 written into the cell");
        }
        e.cache.requestRelax(2, 0.9f);
        t = run(Ls[2], 0.0f);
        stepCheck(t, 0.9f, 4, "relaxAlpha 0.9 beats 1/(n_eff+1) = 1/4 on the first relax frame");
        {
            const rc::RcCell cell = e.readCell(idx);
            check(rc::normalTrust6(cell.b[2]) == 20u, "a valid cell carries its trust through the resolve (20 stays 20)");
        }
        t = run(Ls[2], 0.0f);
        stepCheck(t, 0.9f, 5, "the second relax frame blends at 0.9 too");
        t = run(Ls[2], 0.0f);
        stepCheck(t, 1.0f / 6.0f, 6, "after two frames the relax is spent: 1/(n_eff+1) = 1/6");

        // Stale tag: the camera moves 64 cells of cascade 0 along x, so the same texel now holds another world cell.
        e.cam[0] += 64.0f * rc::kCellSizeCm[0];
        t = run(Ls[0], 64.0f * rc::kCellSizeCm[0]);
        {
            Sample shifted = s3;
            for (int a = 0; a < 3; ++a) shifted.pos[a] = center3[a];
            shifted.pos[0] += 64.0f * rc::kCellSizeCm[0];
            check(cellIdx(e, 0, shifted.pos, n3) == idx, "64 cells along x keep the texel and change the world cell");
            f32 want[rc::kShFloats], got[rc::kShFloats], worst = 0.0f;
            expectedSh(t, want);
            const rc::RcCell cell = e.readCell(idx);
            cellSh(cell, got);
            i32 org[3], wc[3];
            for (int a = 0; a < 3; ++a) org[a] = rc::snapOrigin(e.cam[a], rc::kCellSizeCm[0]);
            rc::scatterCell(shifted.pos, n3, 0, org, rc::kCellSizeCm[0], wc);
            check(shClose(got, want, 2e-3f, 2e-3f, &worst) && rc::metaNEff(cell.b[3]) == 1 &&
                      rc::metaTag(cell.b[3]) == rc::tagPack(wc[0], wc[1], wc[2]) && rc::normalTrust6(cell.b[2]) == rc::kTrustMax,
                  "a stale-tag cell is replaced, not blended: n_eff 1, new tag, SH = the new mean, trust back to 63 (worst " +
                      std::to_string(worst) + ")");
        }

        // Aging: no samples. Frames whose index is not a multiple of 16 change nothing.
        const auto ageOf = [&]() { return rc::metaAge(e.readCell(idx).b[3]); };
        const auto emptyFrame = [&](u32 frameIndex) { e.frame(frameIndex, params, {}); };
        emptyFrame(e.nextFrame());
        check(ageOf() == 0 && rc::metaNEff(e.readCell(idx).b[3]) == 1, "a frame off the aging cadence leaves the cell alone");
        bool ageOk = true;
        for (u32 j = 1; j <= 14; ++j) {
            emptyFrame(16u * (200u + j));
            const rc::RcCell c = e.readCell(idx);
            if (rc::metaAge(c.b[3]) != j || rc::metaNEff(c.b[3]) != 1) ageOk = false;
        }
        check(ageOk, "each aging frame without samples adds one age step (1..14)");
        {
            const rc::RcCell c = e.readCell(idx);
            i32 org[3], wc[3];
            for (int a = 0; a < 3; ++a) org[a] = rc::snapOrigin(e.cam[a], rc::kCellSizeCm[0]);
            Sample shifted = s3;
            for (int a = 0; a < 3; ++a) shifted.pos[a] = center3[a];
            shifted.pos[0] += 64.0f * rc::kCellSizeCm[0];
            rc::scatterCell(shifted.pos, n3, 0, org, rc::kCellSizeCm[0], wc);
            check(rc::cellValid(c.b[3], rc::tagPack(wc[0], wc[1], wc[2])), "at age 14 the cell is still valid");
        }
        emptyFrame(16u * 300u);
        {
            const rc::RcCell c = e.readCell(idx);
            check(c.b[3] == 0u && c.a[0] == 0u && c.b[2] == 0u, "the 15th aging step empties the cell");
        }
    }
}

// ================================================================================ stage 2a: the net kernels
// voxi_neurac_net.hlsl against NeuRaCNetReference.hpp's buildForCell, on a synthetic cell buffer: dense random cells in a
// box [-6, 6)^3 of every cascade, in a window whose origin is -32 so negative cells and tags are exercised. The kernels
// are compiled from the real shader file with the layout NeuRaCNet.cpp binds (t0, u0..u11); the Mlp is not involved.
//   g) CSRcNetReset/Features/Finish: counters, raw totals, inference records and training records vs the CPU
//   h) hash thinning at rate 0.5 on both lists: exactly the cells thinKeep selects
//   i) CSRcNetApply: trust bits written only where the cell is still valid and the prediction is finite, the other
//      bits untouched, the stats sums
namespace net = aver::voxi::neurac::net;

struct NetScene {
    std::vector<rc::RcCell> cells;
    std::vector<i32> accum;
    rc::RcInfo info{};
    std::vector<u32> region;   // every cell index inside the box
    NetScene() : cells(rc::kCells), accum(rc::kAccumInts, 0) {}
};

void buildNetScene(NetScene& S) {
    S.info.frameIndex = 37;
    S.info.ageStepFrames = 16;
    S.info.sampleCap = 64;
    S.info.alphaMin = 0.0625f;
    S.info.coarseCaps = rc::kDefaultCoarseCap1 | (rc::kDefaultCoarseCap2 << 8);
    for (u32 c = 0; c < rc::kCascades; ++c) {
        for (int a = 0; a < 3; ++a) S.info.cas[c].originCell[a] = -32;
        S.info.cas[c].cellSizeCm = rc::kCellSizeCm[c];
    }
    Lcg rng;
    rng.s = 777u;
    const auto u01 = [&] { return rng.next() * 0.5f + 0.5f; };
    const auto uni = [&](f32 a, f32 b) { return a + (b - a) * u01(); };
    const auto below = [&](u32 n) { return std::min(n - 1u, static_cast<u32>(u01() * static_cast<f32>(n))); };
    for (u32 c = 0; c < rc::kCascades; ++c)
        for (i32 z = -6; z < 6; ++z)
            for (i32 y = -6; y < 6; ++y)
                for (i32 x = -6; x < 6; ++x) {
                    const u32 idx = rc::cellIndex(c, x, y, z);
                    S.region.push_back(idx);
                    if (u01() < 0.15f) continue;   // an empty cell
                    f32 nu[3];
                    do { for (f32& v : nu) v = rng.next(); } while (dot3(nu, nu) < 0.05f);
                    normalize3(nu);
                    const f32 len = uni(0.2f, 1.0f);
                    const u32 trust6 = below(64), nEff = 1 + below(15), age = below(16);
                    const u32 tag = rc::tagPack(x, y, z) ^ (u01() < 0.1f ? 1u : 0u);
                    f32 sh[rc::kShFloats];
                    const f32 base = uni(0.05f, 3.0f);
                    const f32 a1 = uni(-0.3f, 0.3f), a2 = uni(-0.3f, 0.3f), a3 = uni(-0.3f, 0.3f);
                    for (int ch = 0; ch < 3; ++ch) {
                        sh[ch] = 3.5449077f * base * uni(0.6f, 1.0f);
                        sh[3 + ch] = a1 * sh[ch]; sh[6 + ch] = a2 * sh[ch]; sh[9 + ch] = a3 * sh[ch];
                    }
                    u32 w[6];
                    rc::packSh(sh, w);
                    rc::RcCell& cell = S.cells[idx];
                    cell.a[0] = w[0]; cell.a[1] = w[1]; cell.a[2] = w[2]; cell.a[3] = w[3];
                    cell.b[0] = w[4]; cell.b[1] = w[5];
                    const f32 v[3] = {nu[0] * len, nu[1] * len, nu[2] * len};
                    cell.b[2] = rc::packNormal(v, trust6);
                    cell.b[3] = rc::metaPack(tag, nEff, age);
                    // This frame's samples: a mean that differs from the old light by a random factor.
                    const u32 n = u01() < 0.7f ? below(91) : 0u;
                    S.accum[idx] = static_cast<i32>(n | (below(40) << 16));
                    const u32 nUsed = std::min(n, rc::capForCascade(S.info, c));
                    // Half the cells near the old light (labels 1 or in between), half anywhere (labels 0 .. 1).
                    const f32 factor = u01() < 0.5f ? uni(0.85f, 1.2f) : uni(0.2f, 3.0f);
                    const f32 cv = uni(0.1f, 1.5f);   // spread of the fresh samples, as the sum of squares
                    const u32 pb = rc::kPayloadBase + idx * rc::kPayloadInts;
                    for (u32 k = 0; k < rc::kShFloats && nUsed; ++k)
                        S.accum[pb + k] = static_cast<i32>(std::llround(static_cast<f64>(sh[k] * factor) * nUsed * rc::kShScale));
                    if (nUsed) {
                        const f64 ef = 0.282095 * (0.2126 * sh[0] + 0.7152 * sh[1] + 0.0722 * sh[2]) * factor;   // the mean of the scalar
                        S.accum[rc::kSqBase + idx] = static_cast<i32>(std::llround(nUsed * ef * ef * (1.0 + static_cast<f64>(cv) * cv) * rc::kSqScale));
                    }
                }
}

struct NetEnv {
    rhi::IDevice* dev = nullptr;
    rhi::IResourceFactory* res = nullptr;
    rhi::PipelineHandle ps[4] = {};
    rhi::BindingSetHandle set = 0;
    rhi::BufferHandle info = 0, accum = 0, cells = 0, infRec = 0, trainRec = 0, trainTgt = 0, infCnt = 0, trainCnt = 0,
                      recCell = 0, recLabel = 0, infOut = 0, state = 0, stats = 0;

    ~NetEnv() {
        if (!res) return;
        res->waitIdle();
        if (set) res->destroyBindingSet(set);
        for (rhi::PipelineHandle p : ps) if (p) res->destroyPipeline(p);
        for (rhi::BufferHandle b : {info, accum, cells, infRec, trainRec, trainTgt, infCnt, trainCnt, recCell, recLabel, infOut, state, stats})
            if (b) res->destroyBuffer(b);
    }

    bool create() {
        res = dev->resources();
        const std::string& source = rhi::shaderFile("voxi_neurac_net.hlsl");
        if (source.empty()) return false;
        const char* entries[4] = {"CSRcNetReset", "CSRcNetFeatures", "CSRcNetFinish", "CSRcNetApply"};
        for (u32 k = 0; k < 4; ++k) {
            rhi::ShaderDesc sd{};
            sd.source = source.c_str();
            sd.entry = entries[k];
            sd.stage = rhi::ShaderStage::Compute;
            sd.minShaderModel = 62;
            const rhi::ShaderHandle cs = res->createShader(sd);
            if (!cs) return false;
            rhi::ComputePipelineDesc pd{};
            pd.cs = cs;
            pd.layout.srvCount = 1;
            pd.layout.uavCount = 12;
            pd.layout.slotKindsDeclared = true;
            pd.layout.srvKinds[0] = rhi::SlotKind::StructuredBuffer;
            for (u32 i = 0; i < 12; ++i) pd.layout.uavKinds[i] = rhi::SlotKind::StructuredBuffer;
            ps[k] = res->createComputePipeline(pd);
            res->destroyShader(cs);
            if (!ps[k]) return false;
        }
        const auto make = [&](u64 bytes, rhi::BufferKind kind, bool uav, const char* name) {
            rhi::BufferDesc bd{};
            bd.bytes = bytes;
            bd.kind = kind;
            bd.allowUnorderedAccess = uav;
            bd.debugName = name;
            return res->createBuffer(bd);
        };
        const rhi::BufferKind def = rhi::BufferKind::Default;
        info = make(rc::kInfoStride, rhi::BufferKind::Upload, false, "net test info");
        accum = make(rc::kAccumBytes, def, true, "net test accum");
        cells = make(rc::kCellsBytes, def, true, "net test cells");
        infRec = make(static_cast<u64>(net::kMaxInfer) * net::kInputs * 4, def, true, "net test inference records");
        trainRec = make(static_cast<u64>(net::kMaxTrain) * net::kInputs * 4, def, true, "net test training records");
        trainTgt = make(static_cast<u64>(net::kMaxTrain) * 4, def, true, "net test training targets");
        infCnt = make(8, def, true, "net test inference count");
        trainCnt = make(8, def, true, "net test training count");
        recCell = make(static_cast<u64>(net::kMaxInfer) * 8, def, true, "net test record cells");
        recLabel = make(static_cast<u64>(net::kMaxInfer) * 4, def, true, "net test record labels");
        infOut = make(static_cast<u64>(net::kMaxInfer) * 4, def, true, "net test predictions");
        state = make(16, def, true, "net test state");
        stats = make(net::kStatCount * 4, def, true, "net test stats");
        if (!info || !accum || !cells || !infRec || !trainRec || !trainTgt || !infCnt || !trainCnt || !recCell || !recLabel ||
            !infOut || !state || !stats)
            return false;

        rhi::BindingSetDesc bsd{};
        bsd.srvCount = 1;
        bsd.uavCount = 12;
        bsd.srvKinds[0] = rhi::SlotKind::StructuredBuffer;
        for (u32 i = 0; i < 12; ++i) bsd.uavKinds[i] = rhi::SlotKind::StructuredBuffer;
        set = res->createBindingSet(bsd);
        if (!set) return false;
        res->setSrvBuffer(set, 0, info, rc::kInfoStride, 1, 0);
        res->setUavBuffer(set, 0, accum, 4, rc::kAccumInts, 0);
        res->setUavBuffer(set, 1, cells, rc::kCellStride, rc::kCells, 0);
        res->setUavBuffer(set, 2, infRec, 4, net::kMaxInfer * net::kInputs, 0);
        res->setUavBuffer(set, 3, trainRec, 4, net::kMaxTrain * net::kInputs, 0);
        res->setUavBuffer(set, 4, trainTgt, 4, net::kMaxTrain, 0);
        res->setUavBuffer(set, 5, infCnt, 4, 2, 0);
        res->setUavBuffer(set, 6, trainCnt, 4, 2, 0);
        res->setUavBuffer(set, 7, recCell, 8, net::kMaxInfer, 0);
        res->setUavBuffer(set, 8, recLabel, 4, net::kMaxInfer, 0);
        res->setUavBuffer(set, 9, infOut, 4, net::kMaxInfer, 0);
        res->setUavBuffer(set, 10, state, 4, 4, 0);
        res->setUavBuffer(set, 11, stats, 4, net::kStatCount, 0);
        return true;
    }

    // Copies host data into a Default buffer through a throwaway Upload buffer.
    bool upload(rhi::BufferHandle dst, const void* data, u64 bytes, u64 dstOffset = 0) {
        rhi::BufferDesc bd{};
        bd.bytes = bytes;
        bd.kind = rhi::BufferKind::Upload;
        bd.debugName = "net test staging";
        const rhi::BufferHandle up = res->createBuffer(bd);
        if (!up) return false;
        const bool wrote = res->writeBuffer(up, data, bytes, 0);
        using rhi::ResourceState;
        const bool ran = wrote && dev->runStandaloneCompute([&](rhi::IRenderContext& ctx) {
            ctx.bufferBarrier(dst, ResourceState::Common, ResourceState::CopyDest);
            ctx.copyBuffer(dst, up, bytes, dstOffset, 0);
            ctx.bufferBarrier(dst, ResourceState::CopyDest, ResourceState::Common);
        });
        res->waitIdle();
        res->destroyBuffer(up);
        return ran;
    }

    bool readBack(rhi::BufferHandle src, u64 offset, u64 bytes, void* out) {
        if (bytes == 0) return true;
        rhi::BufferDesc bd{};
        bd.bytes = bytes;
        bd.kind = rhi::BufferKind::Readback;
        bd.debugName = "net test readback";
        const rhi::BufferHandle rb = res->createBuffer(bd);
        if (!rb) return false;
        using rhi::ResourceState;
        const bool ran = dev->runStandaloneCompute([&](rhi::IRenderContext& ctx) {
            ctx.bufferBarrier(src, ResourceState::Common, ResourceState::CopySource);
            ctx.copyBuffer(rb, src, bytes, 0, offset);
            ctx.bufferBarrier(src, ResourceState::CopySource, ResourceState::Common);
        });
        const bool ok = ran && res->readBuffer(rb, out, bytes, 0);
        res->waitIdle();
        res->destroyBuffer(rb);
        return ok;
    }

    // Reset, Features, Finish in one submission, the barriers NeuRaCNet::recordPre uses.
    bool runFeatures() {
        using rhi::ResourceState;
        const auto lists = {infRec, trainRec, trainTgt, infCnt, trainCnt, recCell, recLabel, state, stats};
        return dev->runStandaloneCompute([&](rhi::IRenderContext& ctx) {
            for (rhi::BufferHandle b : lists) ctx.bufferBarrier(b, ResourceState::Common, ResourceState::UnorderedAccess);
            const auto go = [&](u32 k, u32 groups) {   // pipeline first, then the set, as NeuRaCNet does
                ctx.setPipeline(ps[k]);
                ctx.setBindingSet(set);
                ctx.dispatch(groups, 1, 1);
            };
            go(0, 1);
            ctx.uavBarrierBuffer(infCnt);
            ctx.uavBarrierBuffer(trainCnt);
            ctx.uavBarrierBuffer(stats);
            go(1, rc::kResolveGroups);
            ctx.uavBarrierBuffer(infCnt);
            ctx.uavBarrierBuffer(trainCnt);
            ctx.uavBarrierBuffer(state);
            go(2, 1);
            for (rhi::BufferHandle b : lists) ctx.bufferBarrier(b, ResourceState::UnorderedAccess, ResourceState::Common);
        });
    }
    bool runApply() {
        using rhi::ResourceState;
        const auto lists = {infCnt, recCell, recLabel, infOut, stats};
        return dev->runStandaloneCompute([&](rhi::IRenderContext& ctx) {
            for (rhi::BufferHandle b : lists) ctx.bufferBarrier(b, ResourceState::Common, ResourceState::UnorderedAccess);
            ctx.setPipeline(ps[3]);
            ctx.setBindingSet(set);
            ctx.dispatch(net::kMaxInfer / 64u, 1, 1);
            for (rhi::BufferHandle b : lists) ctx.bufferBarrier(b, ResourceState::UnorderedAccess, ResourceState::Common);
        });
    }
};

struct NetResult {
    u32 inf[2] = {}, train[2] = {}, st[4] = {};
    std::vector<u32> recCell;      // 2 per record: cell index, expected tag
    std::vector<f32> recLabel, infRec, trainRec, trainTgt;
};

bool readNetResult(NetEnv& e, NetResult& r) {
    bool ok = e.readBack(e.infCnt, 0, 8, r.inf) && e.readBack(e.trainCnt, 0, 8, r.train) && e.readBack(e.state, 0, 16, r.st);
    if (!ok) return false;
    const u32 ni = std::min(r.inf[0], net::kMaxInfer), nt = std::min(r.train[0], net::kMaxTrain);
    r.recCell.resize(static_cast<usize>(ni) * 2);
    r.recLabel.resize(ni);
    r.infRec.resize(static_cast<usize>(ni) * net::kInputs);
    r.trainRec.resize(static_cast<usize>(nt) * net::kInputs);
    r.trainTgt.resize(nt);
    return e.readBack(e.recCell, 0, static_cast<u64>(ni) * 8, r.recCell.data()) &&
           e.readBack(e.recLabel, 0, static_cast<u64>(ni) * 4, r.recLabel.data()) &&
           e.readBack(e.infRec, 0, static_cast<u64>(ni) * net::kInputs * 4, r.infRec.data()) &&
           e.readBack(e.trainRec, 0, static_cast<u64>(nt) * net::kInputs * 4, r.trainRec.data()) &&
           e.readBack(e.trainTgt, 0, static_cast<u64>(nt) * 4, r.trainTgt.data());
}

// Checks the GPU's lists against the cells the CPU says to process, after thinning at the given rates.
void compareNetResult(const NetScene& S, const NetResult& r, f32 inferRate, f32 trainRate, const std::string& tag) {
    std::map<u32, net::Features> expectInf;      // cell -> features of the cells that should have a record
    std::vector<net::Features> expectTrain;
    u32 touched = 0, eligible = 0;
    for (u32 idx : S.region) {
        const net::Features f = net::buildForCell(S.cells.data(), S.accum.data(), S.info, idx);
        if (!f.touched) continue;
        ++touched;
        if (!net::thinKeep(inferRate, idx, S.info.frameIndex, net::kSaltInfer)) continue;
        expectInf[idx] = f;
        if (!f.eligible) continue;
        ++eligible;
        if (net::thinKeep(trainRate, idx, S.info.frameIndex, net::kSaltTrain)) expectTrain.push_back(f);
    }
    const u32 nInf = static_cast<u32>(expectInf.size()), nTrain = static_cast<u32>(expectTrain.size());
    check(touched > 500 && eligible > 200, tag + ": the synthetic cache has enough touched and eligible cells (" +
                                                std::to_string(touched) + " / " + std::to_string(eligible) + ")");
    check(r.inf[1] == touched && r.inf[0] == nInf, tag + ": counters: touched " + std::to_string(r.inf[1]) + " (want " +
                                                        std::to_string(touched) + "), inference records " + std::to_string(r.inf[0]) +
                                                        " (want " + std::to_string(nInf) + ")");
    check(r.train[1] == eligible && r.train[0] == nTrain, tag + ": counters: eligible " + std::to_string(r.train[1]) + " (want " +
                                                              std::to_string(eligible) + "), training records " + std::to_string(r.train[0]) +
                                                              " (want " + std::to_string(nTrain) + ")");
    check(r.st[0] == touched && r.st[1] == eligible && r.st[2] == nInf && r.st[3] == nTrain,
          tag + ": Finish keeps the raw totals (touched, eligible, processed, appended) for the next frame's rates");

    // Inference records: one per expected cell, features within float tolerance, label iff eligible.
    std::map<u32, bool> seen;
    usize badSet = 0, badTag = 0, badFeat = 0, badLabel = 0, badElig = 0;
    f32 worst = 0.0f;
    for (usize i = 0; i < r.recLabel.size(); ++i) {
        const u32 cell = r.recCell[2 * i], tagWord = r.recCell[2 * i + 1];
        const auto it = expectInf.find(cell);
        if (it == expectInf.end() || seen[cell]) { ++badSet; continue; }
        seen[cell] = true;
        const net::Features& f = it->second;
        u32 cascade;
        i32 world[3];
        net::cellWorld(cell, S.info, cascade, world);
        if (tagWord != rc::tagPack(world[0], world[1], world[2])) ++badTag;
        bool featOk = true;
        for (u32 k = 0; k < net::kInputs; ++k) {
            const f32 d = std::fabs(r.infRec[i * net::kInputs + k] - f.in[k]);
            worst = std::max(worst, d);
            if (!(d <= 3e-4f)) featOk = false;
        }
        if (!featOk) ++badFeat;
        if ((r.recLabel[i] >= 0.0f) != f.eligible) ++badElig;
        else if (f.eligible && !(std::fabs(r.recLabel[i] - f.label) <= 5e-4f)) ++badLabel;
    }
    check(badSet == 0 && seen.size() == expectInf.size(), tag + ": the inference records name exactly the expected cells, once each (" +
                                                              std::to_string(badSet) + " unexpected, " + std::to_string(seen.size()) + " of " +
                                                              std::to_string(expectInf.size()) + " found)");
    check(badTag == 0, tag + ": each record carries its cell's expected tag");
    check(badFeat == 0, tag + ": inference features equal buildForCell's, worst difference " + std::to_string(worst) + " (" +
                            std::to_string(badFeat) + " records off)");
    check(badElig == 0 && badLabel == 0, tag + ": labels are set exactly for the eligible cells and equal the CPU label");

    // Training records: no cell index travels with them, so match each to an unused expected record.
    std::vector<bool> used(expectTrain.size(), false);
    usize unmatched = 0;
    for (usize i = 0; i < r.trainTgt.size(); ++i) {
        bool found = false;
        for (usize j = 0; j < expectTrain.size() && !found; ++j) {
            if (used[j]) continue;
            const net::Features& f = expectTrain[j];
            bool same = std::fabs(r.trainTgt[i] - f.label) <= 5e-4f;
            for (u32 k = 0; k < net::kInputs && same; ++k) {
                same = std::fabs(r.trainRec[i * net::kInputs + k] - f.in[k]) <= 3e-4f;
            }
            if (same) { used[j] = true; found = true; }
        }
        if (!found) ++unmatched;
    }
    check(unmatched == 0 && r.trainTgt.size() == expectTrain.size(),
          tag + ": every training record matches an expected one (the same inputs as inference, input 3 = 1) with its label (" +
              std::to_string(unmatched) + " unmatched)");
}

void runNetGpu(rhi::IDevice& dev) {
    AVER_INFO("--- stage 2a: CSRcNetReset / Features / Finish / Apply vs NeuRaCNetReference ---");
    rhi::setShaderSourceDir(std::string(AVER_REPO_ROOT) + "/modules/render.voxi/shaders");
    NetEnv e;
    e.dev = &dev;
    const bool made = e.create();
    check(made, "the four net kernels compile from voxi_neurac_net.hlsl and the 12-UAV binding set builds");
    if (!made) return;

    const auto owner = std::make_unique<NetScene>();   // ~80 MB of images: off the stack
    NetScene& S = *owner;
    buildNetScene(S);
    const bool uploaded = e.res->writeBuffer(e.info, &S.info, sizeof(S.info), 0) && e.upload(e.accum, S.accum.data(), rc::kAccumBytes) &&
                          e.upload(e.cells, S.cells.data(), rc::kCellsBytes);
    check(uploaded, "the synthetic accumulator and cell images upload");
    if (!uploaded) return;

    // ---- h) thinning at 0.5 on both lists (state says last frame had twice the target) ----
    {
        const u32 st[4] = {2 * net::kInferTarget, 2 * net::kTrainTarget, 0, 0};
        const bool ok = e.upload(e.state, st, sizeof(st)) && e.runFeatures();
        check(ok, "thinning: the three kernels run with last frame's totals at twice the targets");
        NetResult r;
        if (ok && readNetResult(e, r)) compareNetResult(S, r, net::thinRate(st[0], net::kInferTarget), net::thinRate(st[1], net::kTrainTarget), "thinning 0.5");
        else check(false, "thinning: results read back");
    }

    // ---- g) no thinning ----
    NetResult r;
    {
        const u32 zero[4] = {};
        const bool ok = e.upload(e.state, zero, sizeof(zero)) && e.runFeatures();
        check(ok, "features: the three kernels run");
        if (ok && readNetResult(e, r)) compareNetResult(S, r, 1.0f, 1.0f, "features");
        else { check(false, "features: results read back"); return; }
    }

    // ---- i) apply ----
    {
        const u32 n = std::min(r.inf[0], net::kMaxInfer);
        std::vector<f32> pred(net::kMaxInfer, 0.0f);
        for (u32 i = 0; i < n; ++i) pred[i] = 0.05f * static_cast<f32>(i % 25);   // 0 .. 1.2
        const f32 nan = floatOf(0x7FC00000u), inf = floatOf(0x7F800000u);
        for (u32 i = 7; i < n; i += 25) pred[i] = nan;
        for (u32 i = 11; i < n; i += 25) pred[i] = inf;

        // The resolve may have emptied or replaced a cell between the features and the apply.
        if (n > 12) {
            const rc::RcCell zero{};
            for (u32 i : {3u, 4u, 5u}) {
                S.cells[r.recCell[2 * i]] = zero;
            }
            rc::RcCell stale = S.cells[r.recCell[2 * 6]];
            stale.b[3] ^= 1u;   // tag bit
            S.cells[r.recCell[2 * 6]] = stale;
            for (u32 i : {3u, 4u, 5u, 6u})
                if (!e.upload(e.cells, &S.cells[r.recCell[2 * i]], rc::kCellStride, static_cast<u64>(r.recCell[2 * i]) * rc::kCellStride))
                    check(false, "apply: a cell could not be overwritten");
        }
        const bool ok = e.upload(e.infOut, pred.data(), pred.size() * sizeof(f32)) && e.runApply();
        check(ok, "apply: the kernel runs");

        std::vector<rc::RcCell> got(rc::kCells);
        u32 stats[net::kStatCount] = {};
        const bool read = ok && e.readBack(e.cells, 0, rc::kCellsBytes, got.data()) && e.readBack(e.stats, 0, sizeof(stats), stats);
        check(read, "apply: cells and stats read back");
        if (read) {
            std::vector<rc::RcCell> want = S.cells;
            u32 cnt = 0, applied = 0, trustSum = 0, skipped = 0;
            u64 sLabel = 0, sPred = 0, sErr = 0;
            for (u32 i = 0; i < n; ++i) {
                const u32 cell = r.recCell[2 * i], tag = r.recCell[2 * i + 1];
                if (!rc::cellValid(S.cells[cell].b[3], tag) || !net::trustUsable(pred[i])) { ++skipped; continue; }
                const u32 t6 = net::trustBits(pred[i]);
                want[cell].b[2] = net::withTrust(S.cells[cell].b[2], t6);
                ++applied;
                trustSum += t6;
                const f32 label = r.recLabel[i];
                if (label >= 0.0f) {
                    const f32 p = std::min(std::max(pred[i], 0.0f), 1.0f), err = p - label;
                    ++cnt;
                    sLabel += static_cast<u32>(label * net::kStatScale + 0.5f);
                    sPred += static_cast<u32>(p * net::kStatScale + 0.5f);
                    sErr += static_cast<u32>(err * err * net::kStatScale + 0.5f);
                }
            }
            usize diffCells = 0, diffOther = 0;
            for (u32 idx : S.region) {
                if (std::memcmp(&got[idx], &want[idx], sizeof(rc::RcCell)) != 0) ++diffCells;
                if (got[idx].a[0] != S.cells[idx].a[0] || got[idx].a[3] != S.cells[idx].a[3] || got[idx].b[0] != S.cells[idx].b[0] ||
                    got[idx].b[3] != S.cells[idx].b[3])
                    ++diffOther;
            }
            check(diffCells == 0, "apply: every cell equals the CPU's: trust bits baked where valid and finite, nothing else changed (" +
                                      std::to_string(diffCells) + " differ)");
            check(diffOther == 0, "apply: the SH and meta words are untouched");
            check(skipped > 0 && applied > 0, "apply: both paths are exercised (" + std::to_string(applied) + " applied, " +
                                                  std::to_string(skipped) + " skipped: killed cells, NaN and inf predictions)");
            check(stats[4] == applied && stats[5] == trustSum && stats[0] == cnt,
                  "apply stats: applied " + std::to_string(stats[4]) + "/" + std::to_string(applied) + ", trust sum " + std::to_string(stats[5]) +
                      "/" + std::to_string(trustSum) + ", eligible " + std::to_string(stats[0]) + "/" + std::to_string(cnt));
            const auto near = [](u64 a, u64 b) { return (a > b ? a - b : b - a) <= 8; };
            check(near(stats[1], sLabel) && near(stats[2], sPred) && near(stats[3], sErr),
                  "apply stats: label, prediction and squared-error sums within rounding of the CPU's (" + std::to_string(stats[1]) + "/" +
                      std::to_string(sLabel) + ", " + std::to_string(stats[2]) + "/" + std::to_string(sPred) + ", " + std::to_string(stats[3]) +
                      "/" + std::to_string(sErr) + ")");
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    AVER_INFO("=== NeuRaCGpuTest ===");

    bool hardware = false, debug = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "hw") == 0) hardware = true;
        if (std::strcmp(argv[i], "debug") == 0) debug = true;
    }

    rhi::DeviceDesc desc;
    desc.useWarp = !hardware;
    desc.enableDebug = debug;
    desc.preferred[0] = rhi::Backend::D3D12;
    desc.preferred[1] = rhi::Backend::Null;
    desc.preferredCount = 2;   // no fallback: an absent D3D12 must report SKIPPED
    rhi::IDevice* dev = rhi::createDevice(desc);
    if (!dev || dev->backend() == rhi::Backend::Null) {
        AVER_WARN("  SKIP  no D3D12 device on this machine");
        if (dev) rhi::destroyDevice(dev);
        return kSkip;
    }
    if (!dev->resources()) {
        AVER_WARN("  SKIP  this device exposes no resource factory");
        rhi::destroyDevice(dev);
        return kSkip;
    }
    const rhi::DeviceCaps caps = dev->caps();
    if (!caps.dxcAvailable || caps.shaderModel < 62) {
        AVER_WARN("  SKIP  {} lacks DXC / shader model 6.2", dev->adapterName());
        rhi::destroyDevice(dev);
        return kSkip;
    }
    AVER_INFO("device: {} ({})", dev->adapterName(), rhi::backendName(dev->backend()));
    // Probe: an empty recording tells "cannot run standalone compute" from a real failure.
    if (!dev->runStandaloneCompute([](rhi::IRenderContext&) {})) {
        AVER_WARN("  SKIP  IDevice::runStandaloneCompute is unavailable on this backend");
        rhi::destroyDevice(dev);
        return kSkip;
    }

    runGpu(*dev);
    runNetGpu(*dev);
    rhi::destroyDevice(dev);

    if (g_failures != 0) {
        AVER_ERROR("=== NeuRaCGpuTest FAILED === ({} failure(s))", g_failures);
        return 1;
    }
    AVER_INFO("=== NeuRaCGpuTest PASSED ===");
    return 0;
}
