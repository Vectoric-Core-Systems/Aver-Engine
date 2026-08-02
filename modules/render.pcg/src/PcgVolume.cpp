#include "aver/pcg/PcgVolume.hpp"

#include "aver/core/Log.hpp"
#include "PcgShaders.hpp"

#include <cmath>
#include <cstring>
#include <vector>

namespace aver::pcg {
namespace {

// splitmix32, and the C++ half of the mirror. u32 arithmetic wraps here exactly as it does in HLSL
// and in F#, which is what makes a bit-exact comparison between the three possible.
u32 hash32(u32 x) {
    u32 z = x + 0x9E3779B9u;
    z = (z ^ (z >> 16)) * 0x21F0AAADu;
    z = (z ^ (z >> 15)) * 0x735A2D97u;
    return z ^ (z >> 15);
}

u32 hash3(i32 seed, i32 x, i32 y, i32 z) {
    return hash32(static_cast<u32>(seed) ^ hash32(static_cast<u32>(x) ^
                  hash32(static_cast<u32>(y) ^ hash32(static_cast<u32>(z)))));
}

f32 float01(u32 h) { return static_cast<f32>(h >> 8) / 16777216.0f; }

// The constant block, laid out to match PcgVolumeCB byte for byte.
struct alignas(16) VolumeCB {
    u32 res[4];        // xyz resolution, w layer count
    i32 seedFloor[4];
    f32 coverage[4];
    f32 layerA[kMaxLayers][4];
    i32 layerB[kMaxLayers][4];
};

} // namespace

f32 sampleDensity(const VolumeSpec& spec, u32 x, u32 y, u32 z) {
    f32 total = 0.0f, norm = 0.0f;
    const u32 layers = spec.layerCount < kMaxLayers ? spec.layerCount : kMaxLayers;
    for (u32 l = 0; l < layers; ++l) {
        const NoiseLayer& ly = spec.layers[l];
        f32 amp = ly.amplitude;
        f32 freq = ly.frequency;
        const i32 octaves = ly.octaves > 1 ? ly.octaves : 1;
        for (i32 o = 0; o < octaves; ++o) {
            // Truncation toward zero, matching (int) in HLSL and `int` in F#.
            const i32 cx = static_cast<i32>(static_cast<f32>(x) * freq / static_cast<f32>(spec.resX));
            const i32 cy = static_cast<i32>(static_cast<f32>(y) * freq / static_cast<f32>(spec.resY));
            const i32 cz = static_cast<i32>(static_cast<f32>(z) * freq / static_cast<f32>(spec.resZ));
            const f32 v = float01(hash3(spec.seed + ly.seedOffset, cx, cy, cz));
            total += v * amp;
            norm  += amp;
            amp  *= ly.gain;
            freq *= ly.lacunarity;
        }
    }
    if (norm <= 0.0f) return 0.0f;
    const f32 d = total / norm;
    if (d < spec.coverageFloor) return 0.0f;
    const f32 remapped = (d - spec.coverageFloor) / std::fmax(1e-6f, 1.0f - spec.coverageFloor);
    return std::pow(remapped, spec.coverageBias);
}

f32 sampleInfinite(const InfiniteSpec& spec, f32 wx, f32 wy, f32 wz) {
    const f32 cell = spec.cellSizeCm > 0.0f ? spec.cellSizeCm : 1.0f;
    f32 total = 0.0f, norm = 0.0f;
    const u32 layers = spec.layerCount < kMaxLayers ? spec.layerCount : kMaxLayers;
    for (u32 l = 0; l < layers; ++l) {
        const NoiseLayer& ly = spec.layers[l];
        f32 amp = ly.amplitude;
        f32 freq = ly.frequency;
        const i32 octaves = ly.octaves > 1 ? ly.octaves : 1;
        for (i32 o = 0; o < octaves; ++o) {
            const f32 s = freq / cell;
            const i32 cx = static_cast<i32>(std::floor(wx * s));
            const i32 cy = static_cast<i32>(std::floor(wy * s));
            const i32 cz = static_cast<i32>(std::floor(wz * s));
            const f32 v = float01(hash3(spec.seed + ly.seedOffset, cx, cy, cz));
            total += v * amp;
            norm  += amp;
            amp  *= ly.gain;
            freq *= ly.lacunarity;
        }
    }
    if (norm <= 0.0f) return 0.0f;
    const f32 d = total / norm;
    if (d < spec.coverageFloor) return 0.0f;
    const f32 remapped = (d - spec.coverageFloor) / std::fmax(1e-6f, 1.0f - spec.coverageFloor);
    return std::pow(remapped, spec.coverageBias);
}

bool VolumeBuilder::init(rhi::IDevice& dev) {
    res_ = dev.resources();
    if (!res_) return false;   // a GPU-less backend declines here, as designed

    rhi::ShaderDesc sd;
    sd.source = kPcgVolumeHLSL;
    sd.entry  = "CSVolume";
    sd.stage  = rhi::ShaderStage::Compute;
    sd.minShaderModel = 60;
    cs_ = res_->createShader(sd);
    if (!cs_) { AVER_ERROR("[PCG] the volume shader would not compile"); return false; }

    rhi::ComputePipelineDesc pd;
    pd.cs = cs_;
    pd.layout.uavCount = 1;
    // The whole parameter block as root constants. It is 26 float4s at most, which is well inside
    // the root-signature budget and saves a descriptor and an upload per build.
    pd.layout.constantDwords[0] = sizeof(VolumeCB) / 4;
    pipeline_ = res_->createComputePipeline(pd);
    if (!pipeline_) { AVER_ERROR("[PCG] the volume pipeline is unavailable"); return false; }

    AVER_INFO("[PCG] volume builder ready (max {} layers, {} dwords of constants)",
              kMaxLayers, sizeof(VolumeCB) / 4);
    return true;
}

void VolumeBuilder::shutdown() {
    if (res_) {
        if (set_)      res_->destroyBindingSet(set_);
        if (readback_) res_->destroyBuffer(readback_);
        if (out_)      res_->destroyBuffer(out_);
        if (pipeline_) res_->destroyPipeline(pipeline_);
        if (cs_)       res_->destroyShader(cs_);
    }
    set_ = 0; readback_ = 0; out_ = 0; pipeline_ = 0; cs_ = 0;
    bytes_ = 0;
    state_ = State::Idle;
    res_ = nullptr;
}

VolumeBuilder::~VolumeBuilder() { shutdown(); }

bool VolumeBuilder::request(const VolumeSpec& spec) {
    if (!pipeline_ || !res_) return false;
    const u64 voxels = u64(spec.resX) * spec.resY * spec.resZ;
    if (voxels == 0) return false;

    const u64 bytes = voxels * sizeof(f32);
    // Reallocated only when the size actually changes: a caller regenerating the same field every
    // time a seed slider moves would otherwise churn two GPU buffers and a descriptor per keypress.
    if (bytes != bytes_) {
        if (set_)      { res_->destroyBindingSet(set_); set_ = 0; }
        if (readback_) { res_->destroyBuffer(readback_); readback_ = 0; }
        if (out_)      { res_->destroyBuffer(out_); out_ = 0; }

        rhi::BufferDesc od;
        od.bytes = bytes;
        od.kind = rhi::BufferKind::Default;
        od.allowUnorderedAccess = true;
        od.debugName = "Pcg.Volume";
        out_ = res_->createBuffer(od);

        rhi::BufferDesc rd;
        rd.bytes = bytes;
        rd.kind = rhi::BufferKind::Readback;
        rd.debugName = "Pcg.Volume.Readback";
        readback_ = res_->createBuffer(rd);

        rhi::BindingSetDesc sd;
        sd.uavCount = 1;
        sd.uavKinds[0] = rhi::SlotKind::StructuredBuffer;
        set_ = res_->createBindingSet(sd);

        if (!out_ || !readback_ || !set_) {
            AVER_ERROR("[PCG] could not allocate a {}-voxel volume", voxels);
            shutdown();
            return false;
        }
        bytes_ = bytes;
        outState_ = rhi::ResourceState::Common;   // freshly created
    }

    spec_ = spec;
    host_.assign(static_cast<usize>(voxels), 0.0f);
    state_ = State::Dispatch;
    return true;
}

bool VolumeBuilder::read(f32* out, usize count) const {
    if (state_ != State::Done || !out) return false;
    if (count != host_.size()) return false;
    std::memcpy(out, host_.data(), count * sizeof(f32));
    return true;
}

void VolumeBuilder::prePass(rhi::IRenderContext& ctx) {
    if (!pipeline_ || !res_) return;

    if (state_ == State::Dispatch) {
        const u64 voxels = u64(spec_.resX) * spec_.resY * spec_.resZ;

        VolumeCB cb{};
        cb.res[0] = spec_.resX; cb.res[1] = spec_.resY; cb.res[2] = spec_.resZ;
        cb.res[3] = spec_.layerCount < kMaxLayers ? spec_.layerCount : kMaxLayers;
        cb.seedFloor[0] = spec_.seed;
        cb.coverage[0] = spec_.coverageFloor;
        cb.coverage[1] = spec_.coverageBias;
        for (u32 l = 0; l < kMaxLayers; ++l) {
            const NoiseLayer& ly = spec_.layers[l];
            cb.layerA[l][0] = ly.frequency;  cb.layerA[l][1] = ly.amplitude;
            cb.layerA[l][2] = ly.lacunarity; cb.layerA[l][3] = ly.gain;
            cb.layerB[l][0] = ly.octaves;    cb.layerB[l][1] = ly.seedOffset;
        }

        res_->setUavBuffer(set_, 0, out_, sizeof(f32), static_cast<u32>(voxels), 0);

        ctx.pushMarker("Aver.Pcg.Volume");
        if (outState_ != rhi::ResourceState::UnorderedAccess) {
            ctx.bufferBarrier(out_, outState_, rhi::ResourceState::UnorderedAccess);
            outState_ = rhi::ResourceState::UnorderedAccess;
        }
        ctx.setPipeline(pipeline_);
        ctx.setBindingSet(set_);
        ctx.setConstants(0, &cb, sizeof(VolumeCB) / 4);
        // Ceil-divided against the shader's [numthreads(4,4,4)]. The shader bounds-checks, so an
        // odd resolution costs a few idle threads rather than a write past the buffer.
        ctx.dispatch((spec_.resX + 3) / 4, (spec_.resY + 3) / 4, (spec_.resZ + 3) / 4);
        // CopySource before the copy: nothing transitions implicitly in this RHI.
        ctx.bufferBarrier(out_, outState_, rhi::ResourceState::CopySource);
        outState_ = rhi::ResourceState::CopySource;
        ctx.copyBuffer(readback_, out_, bytes_);
        ctx.popMarker();
        state_ = State::Copy;
        return;
    }

    if (state_ == State::Copy) {
        // ONE FRAME LATER, and this wait is the whole reason the state machine exists. readBuffer
        // does no synchronisation of its own -- reading in the same frame the copy was RECORDED
        // returns whatever the buffer held before, which looks like a shader that computed
        // garbage rather than like a read that happened too early.
        if (res_->readBuffer(readback_, host_.data(), bytes_)) {
            state_ = State::Done;
        } else {
            AVER_ERROR("[PCG] the volume readback failed");
            state_ = State::Idle;
        }
    }
}

} // namespace aver::pcg
