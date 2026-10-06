#include "aver/synapse/CrowdGpu.hpp"

#include "aver/core/Log.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aver::synapse {
namespace {

u32 bitsOf(f32 f) {
    u32 u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

constexpr u32 kMinAgentCap = 64;

} // namespace

GpuCrowdBackend::~GpuCrowdBackend() { shutdown(); }

bool GpuCrowdBackend::init(rhi::IDevice& dev) {
    shutdown();
    res_ = dev.resources();
    if (!res_) return false;   // a GPU-less backend declines here, as designed

    rhi::ShaderDesc sd;
    sd.source = rhi::shaderFile("synapse_crowd.hlsl").c_str();
    sd.entry = "CSCrowd";
    sd.stage = rhi::ShaderStage::Compute;
    sd.minShaderModel = 60;
    cs_ = res_->createShader(sd);
    if (!cs_) {
        AVER_ERROR("[Synapse] the crowd shader would not compile; crowds stay on the CPU");
        shutdown();
        return false;
    }

    rhi::ComputePipelineDesc pd;
    pd.cs = cs_;
    pd.layout.srvCount = 4;   // agents, cell starts, cell items, wall mask
    pd.layout.uavCount = 1;   // avoided velocities
    pd.layout.constantDwords[1] = kCbDwords;   // b1; b0 is the engine's
    pipeline_ = res_->createComputePipeline(pd);
    if (!pipeline_) {
        AVER_ERROR("[Synapse] the crowd pipeline is unavailable; crowds stay on the CPU");
        shutdown();
        return false;
    }
    AVER_INFO("[Synapse] GPU crowd backend ready");
    return true;
}

void GpuCrowdBackend::destroyBuffers() {
    if (!res_) return;
    if (set_)          res_->destroyBindingSet(set_);
    if (agentsBuf_)    res_->destroyBuffer(agentsBuf_);
    if (cellStartBuf_) res_->destroyBuffer(cellStartBuf_);
    if (itemsBuf_)     res_->destroyBuffer(itemsBuf_);
    if (wallsBuf_)     res_->destroyBuffer(wallsBuf_);
    if (outBuf_)       res_->destroyBuffer(outBuf_);
    if (readbackBuf_)  res_->destroyBuffer(readbackBuf_);
    set_ = agentsBuf_ = cellStartBuf_ = itemsBuf_ = wallsBuf_ = outBuf_ = readbackBuf_ = 0;
    agentCap_ = wallCap_ = 0;
    outState_ = rhi::ResourceState::Common;
    navStamp_ = nullptr;
}

void GpuCrowdBackend::shutdown() {
    destroyBuffers();
    if (res_) {
        if (pipeline_) res_->destroyPipeline(pipeline_);
        if (cs_)       res_->destroyShader(cs_);
    }
    pipeline_ = 0;
    cs_ = 0;
    res_ = nullptr;
    state_ = State::Idle;
    jobCount_ = 0;
}

bool GpuCrowdBackend::ensureCapacity(u32 agents, u64 walls) {
    const u32 wantAgents = std::max(agents, kMinAgentCap);
    const u32 wantWalls = static_cast<u32>(std::max<u64>(walls, 1));
    if (agentsBuf_ && wantAgents <= agentCap_ && wantWalls <= wallCap_) return true;

    // Grown with headroom so a crowd that drifts by a few agents does not reallocate every job.
    const u32 newAgents = std::max(wantAgents, agentCap_ + agentCap_ / 2);
    const u32 newWalls = std::max(wantWalls, wallCap_);
    destroyBuffers();

    const auto make = [&](u64 bytes, rhi::BufferKind kind, bool uav, const char* name) {
        rhi::BufferDesc bd;
        bd.bytes = bytes;
        bd.kind = kind;
        bd.allowUnorderedAccess = uav;
        bd.debugName = name;
        return res_->createBuffer(bd);
    };
    const u32 cellSlots = static_cast<u32>(CrowdGrid::kMaxCells + 1);
    agentsBuf_    = make(u64(newAgents) * sizeof(GpuAgentPacked), rhi::BufferKind::Upload, false, "Synapse.Crowd.Agents");
    cellStartBuf_ = make(u64(cellSlots) * 4, rhi::BufferKind::Upload, false, "Synapse.Crowd.CellStart");
    itemsBuf_     = make(u64(newAgents) * 4, rhi::BufferKind::Upload, false, "Synapse.Crowd.Items");
    wallsBuf_     = make(u64(newWalls) * 4, rhi::BufferKind::Upload, false, "Synapse.Crowd.Walls");
    outBuf_       = make(u64(newAgents) * 8, rhi::BufferKind::Default, true, "Synapse.Crowd.Out");
    readbackBuf_  = make(u64(newAgents) * 8, rhi::BufferKind::Readback, false, "Synapse.Crowd.Readback");

    rhi::BindingSetDesc bs;
    bs.srvCount = 4;
    bs.uavCount = 1;
    for (u32 i = 0; i < 4; ++i) bs.srvKinds[i] = rhi::SlotKind::StructuredBuffer;
    bs.uavKinds[0] = rhi::SlotKind::StructuredBuffer;
    set_ = res_->createBindingSet(bs);

    if (!agentsBuf_ || !cellStartBuf_ || !itemsBuf_ || !wallsBuf_ || !outBuf_ || !readbackBuf_ || !set_) {
        AVER_ERROR("[Synapse] could not allocate GPU crowd buffers for {} agents", newAgents);
        destroyBuffers();
        return false;
    }
    res_->setSrvBuffer(set_, 0, agentsBuf_, sizeof(GpuAgentPacked), newAgents);
    res_->setSrvBuffer(set_, 1, cellStartBuf_, 4, cellSlots);
    res_->setSrvBuffer(set_, 2, itemsBuf_, 4, newAgents);
    res_->setSrvBuffer(set_, 3, wallsBuf_, 4, newWalls);
    res_->setUavBuffer(set_, 0, outBuf_, 8, newAgents);
    agentCap_ = newAgents;
    wallCap_ = newWalls;
    return true;
}

bool GpuCrowdBackend::solve(const CrowdSolveInput& in, CrowdSolveOutput& out) {
    bool delivered = false;
    if (state_ == State::Done) {
        out.ids = jobIds_;
        out.vel = result_;
        state_ = State::Idle;
        delivered = true;
    }
    if (state_ != State::Idle || !pipeline_ || in.count == 0 || !in.agents || !in.params) return delivered;

    const CrowdParams& P = *in.params;
    const CrowdAgent* A = in.agents;
    const u32 n = in.count;
    const bool walls = in.nav && in.nav->valid();
    const u64 wallCells = walls ? u64(in.nav->widthCells) * in.nav->heightCells : 0;
    if (!ensureCapacity(n, wallCells)) return delivered;

    f32 maxR = 0.0f;
    for (u32 i = 0; i < n; ++i) maxR = std::max(maxR, A[i].radius);
    grid_.build(A, n, std::max(P.neighborRadiusCm, 2.0f * maxR + 50.0f));

    packed_.resize(n);
    jobIds_.resize(n);
    for (u32 i = 0; i < n; ++i) {
        const CrowdAgent& a = A[i];
        const V2 pref = CrowdCpuSolver::biasedPreferred(a, P);
        GpuAgentPacked& g = packed_[i];
        g.pos[0] = a.pos.x;  g.pos[1] = a.pos.y;
        g.vel[0] = a.vel.x;  g.vel[1] = a.vel.y;
        g.pref[0] = pref.x;  g.pref[1] = pref.y;
        g.radius = a.radius;
        g.maxSpeed = a.maxSpeed;
        g.priority = a.priority;
        g.stuckSec = a.stuckSec;
        g.flags = a.flags;
        g.id = a.id;
        jobIds_[i] = a.id;
    }
    res_->writeBuffer(agentsBuf_, packed_.data(), u64(n) * sizeof(GpuAgentPacked), 0);
    res_->writeBuffer(cellStartBuf_, grid_.cellStart.data(), u64(grid_.cellStart.size()) * 4, 0);
    res_->writeBuffer(itemsBuf_, grid_.items.data(), u64(n) * 4, 0);

    if (walls && (navStamp_ != in.nav || navCells_ != wallCells)) {
        blocked_.assign(static_cast<usize>(wallCells), 0u);
        for (u32 y = 0; y < in.nav->heightCells; ++y)
            for (u32 x = 0; x < in.nav->widthCells; ++x)
                blocked_[static_cast<usize>(y) * in.nav->widthCells + x] =
                    navCellBlocked(*in.nav, static_cast<i32>(x), static_cast<i32>(y)) ? 1u : 0u;
        res_->writeBuffer(wallsBuf_, blocked_.data(), wallCells * 4, 0);
        navStamp_ = in.nav;
        navCells_ = wallCells;
    }
    hasWalls_ = walls ? 1u : 0u;

    u32* c = cb_;
    c[0] = n;
    c[1] = grid_.w;
    c[2] = grid_.h;
    c[3] = std::min(P.maxNeighbors, kGpuMaxNeighbors);
    c[4] = bitsOf(grid_.minX);
    c[5] = bitsOf(grid_.minY);
    c[6] = bitsOf(grid_.cellSize);
    c[7] = bitsOf(P.neighborRadiusCm);
    c[8] = bitsOf(P.timeHorizon);
    c[9] = bitsOf(P.obstacleHorizon);
    c[10] = bitsOf(P.fixedStep);
    c[11] = 0;
    c[12] = walls ? in.nav->widthCells : 0u;
    c[13] = walls ? in.nav->heightCells : 0u;
    c[14] = bitsOf(walls ? in.nav->originXCm : 0.0f);
    c[15] = bitsOf(walls ? in.nav->originYCm : 0.0f);
    c[16] = bitsOf(walls ? in.nav->cellSizeCm : 1.0f);
    c[17] = std::min(P.maxWallLines, kGpuMaxWalls);
    c[18] = hasWalls_;
    c[19] = bitsOf(P.stuckSeconds);

    jobCount_ = n;
    state_ = State::Dispatch;
    return delivered;
}

void GpuCrowdBackend::prePass(rhi::IRenderContext& ctx) {
    if (!pipeline_ || !res_) return;

    if (state_ == State::Dispatch) {
        ctx.pushMarker("Aver.Synapse.Crowd");
        if (outState_ != rhi::ResourceState::UnorderedAccess) {
            ctx.bufferBarrier(outBuf_, outState_, rhi::ResourceState::UnorderedAccess);
            outState_ = rhi::ResourceState::UnorderedAccess;
        }
        ctx.setPipeline(pipeline_);
        ctx.setBindingSet(set_);
        ctx.setConstants(1, cb_, kCbDwords);
        ctx.dispatch((jobCount_ + 63) / 64, 1, 1);
        ctx.uavBarrierBuffer(outBuf_);
        ctx.bufferBarrier(outBuf_, rhi::ResourceState::UnorderedAccess, rhi::ResourceState::CopySource);
        ctx.copyBuffer(readbackBuf_, outBuf_, u64(jobCount_) * 8);
        ctx.bufferBarrier(outBuf_, rhi::ResourceState::CopySource, rhi::ResourceState::UnorderedAccess);
        ctx.popMarker();
        state_ = State::Copy;
        waited_ = 0;
        return;
    }

    if (state_ == State::Copy) {
        if (++waited_ < kReadbackFrames) return;
        readback_.resize(static_cast<usize>(jobCount_) * 2);
        if (res_->readBuffer(readbackBuf_, readback_.data(), u64(jobCount_) * 8, 0)) {
            result_.resize(jobCount_);
            for (u32 i = 0; i < jobCount_; ++i) {
                const f32 x = readback_[2 * i], y = readback_[2 * i + 1];
                result_[i] = (std::isfinite(x) && std::isfinite(y)) ? V2{x, y} : V2{};
            }
            lastLatency_ = waited_;
            ++completed_;
            state_ = State::Done;
        } else {
            AVER_ERROR("[Synapse] the crowd readback failed");
            state_ = State::Idle;
        }
    }
}

} // namespace aver::synapse
