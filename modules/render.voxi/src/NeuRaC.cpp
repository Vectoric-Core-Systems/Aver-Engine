#include "aver/voxi/NeuRaC.hpp"

#include "aver/core/Log.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace aver::voxi {

namespace rc = neurac;

namespace {

constexpr const char* kShaderName = "voxi_neurac_resolve.hlsl";
constexpr const char* kEntry      = "CSRcResolve";

constexpr f64 kMiB = 1024.0 * 1024.0;

// RcInfo for one frame. cas[c].cellSizeCm travels as float bits in the HLSL int4's .w, which the shader
// reads back with asfloat(); the struct already stores it as a float, so the bytes are what the shader
// expects with no conversion here.
rc::RcInfo makeInfo(const f32 camPosCm[3], u32 frameIndex, u32 flags, const NeuRaC::Params& p) {
    rc::RcInfo info{};
    info.flags         = flags;
    info.frameIndex    = frameIndex;
    info.ageStepFrames = p.ageStepFrames;
    info.sampleCap     = p.sampleCap > rc::kMaxCap ? rc::kMaxCap : p.sampleCap;
    info.alphaMin      = p.alphaMin;
    for (u32 c = 0; c < rc::kCascades; ++c) {
        const f32 size = rc::kCellSizeCm[c];
        for (u32 a = 0; a < 3; ++a) info.cas[c].originCell[a] = rc::snapOrigin(camPosCm[a], size);
        info.cas[c].cellSizeCm = size;
    }
    return info;
}

}  // namespace

NeuRaC::~NeuRaC() { destroy(); }

bool NeuRaC::create(rhi::IResourceFactory& res) {
    if (valid_) return true;
    destroy();   // a failed earlier attempt may have left pieces
    res_ = &res;

    auto fail = [&](const char* why) {
        AVER_WARN("[Voxi] NeuRaC: {}; the cached GI visibility mode falls back to half resolution", why);
        destroy();
        return false;
    };

    // The big buffers. Default kind with UAV access and nothing else: see the header for why no state
    // transition is ever needed.
    {
        rhi::BufferDesc bd;
        bd.kind = rhi::BufferKind::Default;
        bd.allowUnorderedAccess = true;
        bd.bytes = rc::kAccumBytes;
        bd.debugName = "Voxi NeuRaC accumulator";
        accum_ = res.createBuffer(bd);
        bd.bytes = rc::kCellsBytes;
        bd.debugName = "Voxi NeuRaC cells";
        cells_ = res.createBuffer(bd);
        if (!accum_ || !cells_) return fail("the accumulator/cell buffers could not be created");
    }

    // RcInfo ring + the clear's own buffer.
    for (u32 i = 0; i <= kRing; ++i) {
        rhi::BufferDesc bd;
        bd.bytes = rc::kInfoStride;
        bd.kind  = rhi::BufferKind::Upload;
        bd.debugName = i < kRing ? "Voxi NeuRaC info" : "Voxi NeuRaC clear info";
        const rhi::BufferHandle b = res.createBuffer(bd);
        if (!b) return fail("an info buffer could not be created");
        if (i < kRing) info_[i] = b; else clearInfo_ = b;
    }
    {
        // Written ONCE: flags = CLEAR_ALL, everything else zero (the clear needs no origins).
        rc::RcInfo clear{};
        clear.flags = rc::kFlagClearAll;
        if (!res.writeBuffer(clearInfo_, &clear, sizeof(clear), 0)) return fail("the clear info could not be written");
    }

    // The resolve shader. Asking for SM 6.2 selects DXC rather than FXC (the same ask the denoiser's
    // pipelines make), which the f16tof32/f32tof16 packing in voxi_neurac.hlsli is written for.
    // It #includes that pure-maths header, so the two files must be deployed together (the shader
    // directory is deployed whole).
    const std::string& source = rhi::shaderFile(kShaderName);
    if (source.empty()) return fail("voxi_neurac_resolve.hlsl is not deployed beside the executable");
    rhi::ShaderDesc sd{};
    sd.source = source.c_str();
    sd.entry  = kEntry;
    sd.stage  = rhi::ShaderStage::Compute;
    sd.minShaderModel = 62;
    const rhi::ShaderHandle cs = res.createShader(sd);
    if (!cs) return fail("the resolve shader would not compile");

    // t0 = RcInfo, u0 = accumulator, u1 = cells; own registers, own layout, no samplers, no constants.
    rhi::ComputePipelineDesc pd{};
    pd.cs = cs;
    pd.layout.srvCount = 1;
    pd.layout.uavCount = 2;
    pd.layout.slotKindsDeclared = true;
    pd.layout.srvKinds[0] = rhi::SlotKind::StructuredBuffer;
    pd.layout.uavKinds[0] = rhi::SlotKind::StructuredBuffer;
    pd.layout.uavKinds[1] = rhi::SlotKind::StructuredBuffer;
    pipeline_ = res.createComputePipeline(pd);
    res.destroyShader(cs);   // the pipeline owns the bytecode now
    if (!pipeline_) return fail("the resolve pipeline would not build");

    // One set per ring slot and one for the clear: the sets differ only in t0.
    rhi::BindingSetDesc bsd;
    bsd.srvCount = 1;
    bsd.uavCount = 2;
    bsd.srvKinds[0] = rhi::SlotKind::StructuredBuffer;
    bsd.uavKinds[0] = rhi::SlotKind::StructuredBuffer;
    bsd.uavKinds[1] = rhi::SlotKind::StructuredBuffer;
    for (u32 i = 0; i <= kRing; ++i) {
        const rhi::BindingSetHandle s = res.createBindingSet(bsd);
        if (!s) return fail("a resolve binding set could not be created");
        res.setSrvBuffer(s, 0, i < kRing ? info_[i] : clearInfo_, rc::kInfoStride, 1, 0);
        res.setUavBuffer(s, 0, accum_, 4, rc::kAccumInts, 0);
        res.setUavBuffer(s, 1, cells_, rc::kCellStride, rc::kCells, 0);
        if (i < kRing) sets_[i] = s; else clearSet_ = s;
    }

    valid_ = true;
    clearPending_ = true;   // Vulkan does not guarantee zeroed memory and D3D12's zero-fill is not relied on
    ++generation_;
    AVER_INFO("[Voxi] NeuRaC: {} cascade(s) of {}^3 cells at {}/{}/{} cm; accumulator {:.1f} MiB + "
              "cells {:.1f} MiB = {:.1f} MiB VRAM", rc::kCascades, rc::kRes,
              rc::kCellSizeCm[0], rc::kCellSizeCm[1], rc::kCellSizeCm[2],
              static_cast<f64>(rc::kAccumBytes) / kMiB, static_cast<f64>(rc::kCellsBytes) / kMiB,
              static_cast<f64>(rc::kAccumBytes + rc::kCellsBytes) / kMiB);
    return true;
}

void NeuRaC::destroy() {
    if (res_) {
        for (rhi::BindingSetHandle& s : sets_) { if (s) res_->destroyBindingSet(s); s = 0; }
        if (clearSet_) res_->destroyBindingSet(clearSet_);
        if (pipeline_) res_->destroyPipeline(pipeline_);
        for (rhi::BufferHandle& b : info_) { if (b) res_->destroyBuffer(b); b = 0; }
        if (clearInfo_) res_->destroyBuffer(clearInfo_);
        if (accum_) res_->destroyBuffer(accum_);
        if (cells_) res_->destroyBuffer(cells_);
        if (statsReadback_) res_->destroyBuffer(statsReadback_);
    }
    statsReadback_ = 0;
    framesLive_ = statsCopyFrame_ = statsState_ = 0;
    for (rhi::BindingSetHandle& s : sets_) s = 0;
    for (rhi::BufferHandle& b : info_) b = 0;
    clearSet_ = 0; pipeline_ = 0; clearInfo_ = 0; accum_ = 0; cells_ = 0;
    res_ = nullptr;
    valid_ = false;
    clearPending_ = false;
    slot_ = kRing - 1;
    // generation_ is deliberately NOT reset: a destroy/create pair must still read as "the handles
    // changed" to a renderer that last bound the old generation.
}

NeuRaC::Bindings NeuRaC::beginFrame(rhi::IRenderContext& ctx, const f32 camPosCm[3],
                                                  u32 frameIndex, const Params& p) {
    Bindings out;
    out.generation = generation_;
    if (!valid_) return out;

    // Rotate BEFORE writing, so this frame never touches the buffer an in-flight frame still reads.
    slot_ = (slot_ + 1) % kRing;
    const rc::RcInfo info = makeInfo(camPosCm, frameIndex, 0u, p);
    res_->writeBuffer(info_[slot_], &info, sizeof(info), 0);

    if (clearPending_) {
        clearPending_ = false;
        // Zeroes the whole accumulator and every cell. Recorded here, outside the scene pass, so the
        // dispatch is ordinary compute; the barriers make the zeros visible to the trace twins.
        rhi::ScopedGpuStat stat(ctx, "Voxi NeuRaC clear");
        ctx.setPipeline(pipeline_);
        ctx.setBindingSet(clearSet_);
        ctx.dispatch(rc::kResolveGroups, 1, 1);
        ctx.uavBarrierBuffer(accum_);
        ctx.uavBarrierBuffer(cells_);
    }

    // The warm-up report (see the header). Recorded here, before any pass this frame touches the cells.
    ++framesLive_;
    if (statsState_ == 0 && framesLive_ == kStatsAtFrame) {
        rhi::BufferDesc bd;
        bd.bytes = rc::kCellsBytes;
        bd.kind  = rhi::BufferKind::Readback;
        bd.debugName = "Voxi NeuRaC warm-up readback";
        statsReadback_ = res_->createBuffer(bd);
        if (statsReadback_) {
            ctx.copyBuffer(statsReadback_, cells_, rc::kCellsBytes, 0, 0);
            statsInfo_      = info;
            statsCopyFrame_ = framesLive_;
            statsState_     = 1;
        } else {
            statsState_ = 2;   // no report rather than a retry every frame
        }
    } else if (statsState_ == 1 && framesLive_ >= statsCopyFrame_ + kStatsReadDelay) {
        logWarmupStats();
        res_->destroyBuffer(statsReadback_);
        statsReadback_ = 0;
        statsState_ = 2;
    }

    out.info  = info_[slot_];
    out.accum = accum_;
    out.cells = cells_;
    return out;
}

void NeuRaC::logWarmupStats() {
    std::vector<rc::RcCell> cells(rc::kCells);
    if (!res_->readBuffer(statsReadback_, cells.data(), rc::kCellsBytes, 0)) {
        AVER_WARN("[Voxi] NeuRaC warm-up report: the readback could not be read");
        return;
    }
    for (u32 c = 0; c < rc::kCascades; ++c) {
        const i32 ox = statsInfo_.cas[c].originCell[0], oy = statsInfo_.cas[c].originCell[1],
                  oz = statsInfo_.cas[c].originCell[2];
        u64 occupied = 0, valid = 0, nEffSum = 0, ageSum = 0;
        f64 lenSum = 0.0, ambientSum = 0.0;
        for (u32 local = 0; local < rc::kCellsPerCascade; ++local) {
            const rc::RcCell& cell = cells[c * rc::kCellsPerCascade + local];
            const u32 meta = cell.b[3];
            if (rc::metaNEff(meta) == 0) continue;
            ++occupied;
            // The world cell this texel stands for under the window at copy time -- the resolve's rule.
            const i32 tx = static_cast<i32>(local & 63u), ty = static_cast<i32>((local >> 6) & 63u),
                      tz = static_cast<i32>(local >> 12);
            const i32 wx = ox + ((tx - ox) & 63), wy = oy + ((ty - oy) & 63), wz = oz + ((tz - oz) & 63);
            if (!rc::cellValid(meta, rc::tagPack(wx, wy, wz))) continue;
            ++valid;
            nEffSum += rc::metaNEff(meta);
            ageSum  += rc::metaAge(meta);
            lenSum  += static_cast<f64>(cell.b[2] >> 22) / 1023.0;
            // c0 (the ambient term) is the first three fp16 values: a[0] low, a[0] high, a[1] low.
            const f32 r = rc::halfToFloat(cell.a[0] & 0xFFFFu), g = rc::halfToFloat(cell.a[0] >> 16),
                      b = rc::halfToFloat(cell.a[1] & 0xFFFFu);
            ambientSum += 0.282095 * (0.2126 * r + 0.7152 * g + 0.0722 * b);
        }
        const f64 v = valid ? static_cast<f64>(valid) : 1.0;
        AVER_INFO("[Voxi] NeuRaC warm-up (frame {}), cascade {} ({} cm): {} cells hold samples, {} valid "
                  "under the current window ({:.2f}% of {}); valid cells: mean n_eff {:.1f}, mean age {:.1f}, "
                  "mean planarity {:.2f}, mean ambient luminance {:.4f}",
                  statsCopyFrame_, c, rc::kCellSizeCm[c], occupied, valid,
                  100.0 * static_cast<f64>(valid) / rc::kCellsPerCascade, rc::kCellsPerCascade,
                  static_cast<f64>(nEffSum) / v, static_cast<f64>(ageSum) / v, lenSum / v, ambientSum / v);
    }
}

void NeuRaC::recordResolve(rhi::IRenderContext& ctx) {
    if (!valid_) return;
    rhi::ScopedGpuStat stat(ctx, "Voxi NeuRaC resolve");
    ctx.setPipeline(pipeline_);
    ctx.setBindingSet(sets_[slot_]);
    ctx.dispatch(rc::kResolveGroups, 1, 1);
}

}  // namespace aver::voxi
