#include "aver/pcg/PcgVolume.hpp"

#include "aver/core/Log.hpp"
#include "PcgShaders.hpp"

#include <cmath>
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
        if (pipeline_) res_->destroyPipeline(pipeline_);
        if (cs_)       res_->destroyShader(cs_);
    }
    pipeline_ = 0;
    cs_ = 0;
    res_ = nullptr;
}

} // namespace aver::pcg
