// SpatialUpscaler: AverSR's built-in implementation of the rhi::IUpscaler seam (see
// AverSrSpatial.hpp for what it is and why it is deliberately not FSR).
#include "aver/sr/AverSrSpatial.hpp"
#include "aver/core/Log.hpp"

#include <string>

namespace aver::sr {

// Constants for the pass -- packed to match `cbuffer AverSrSpatialCB` in the HLSL below exactly.
struct SpatialUpscalerCB {
    f32 srcSize[4];   // xy source size in texels, zw its reciprocal
};

// Root CBV register the pass's constants land at. Not rhi::kFeatureFrameConstantRegister (b4) or
// rhi::kObjectConstantRegister (b1)'s shared-prelude meaning: this shader never includes the shared
// prelude (see spatialUpscaleShaderSource()'s own comment), so those conventions do not apply to
// it -- b0 stays reserved for the engine's per-frame block same as every pipeline, and b1 here is
// this pass's own, unrelated to what b1 means to a pipeline that DOES compile against the prelude.
constexpr u32 kUpscaleConstantRegister = 1;

SpatialUpscaler::~SpatialUpscaler() {
    if (pipeline_) res_.destroyPipeline(pipeline_);
    if (binding_)  res_.destroyBindingSet(binding_);
}

bool SpatialUpscaler::ensurePipeline(rhi::TextureHandle outTarget) {
    rhi::TextureDesc dstDesc{};
    if (!res_.textureInfo(outTarget, dstDesc)) {
        AVER_ERROR("[AverSR] SpatialUpscaler: outTarget handle {} has no texture info", outTarget);
        return false;
    }

    if (pipeline_ && dstDesc.format == pipelineFormat_) return true;   // already built for this format

    if (pipeline_) { res_.destroyPipeline(pipeline_); pipeline_ = 0; }

    const char* src = spatialUpscaleShaderSource();
    rhi::ShaderDesc vsd; vsd.source = src; vsd.entry = "VSUpscale";        vsd.stage = rhi::ShaderStage::Vertex; vsd.minShaderModel = 51;
    rhi::ShaderDesc psd; psd.source = src; psd.entry = "PSSpatialUpscale"; psd.stage = rhi::ShaderStage::Pixel;  psd.minShaderModel = 51;
    const rhi::ShaderHandle vs = res_.createShader(vsd);
    const rhi::ShaderHandle ps = res_.createShader(psd);

    bool ok = false;
    if (vs && ps) {
        rhi::GraphicsPipelineDesc p;
        p.vs = vs;
        p.ps = ps;
        p.layout.srvCount = 1;
        // NO srvKinds HERE: that member belongs to BindingSetDesc, which describes the SET a
        // draw binds, not to PipelineLayout, which only declares how many registers the
        // pipeline reserves. A Texture2D SRV needs no per-slot kind at layout time -- the kind
        // is stated when the binding set is created, and stating it twice would let the two
        // disagree.
        p.layout.samplers[0] = rhi::SamplerDesc{rhi::Filter::Linear, rhi::AddressMode::Clamp};
        p.layout.samplerCount = 1;
        p.cull = rhi::CullMode::None;
        p.depthClip = false;
        p.renderTargetCount = 1;
        p.renderTargets[0] = dstDesc.format;
        p.sampleCount = 1;   // outTarget is always single-sample: the post chain resolves MSAA first
        pipeline_ = res_.createGraphicsPipeline(p);
        ok = pipeline_ != 0;
    }
    // Shaders are only needed to build the pipeline, exactly like every other feature module's
    // compile step (see VoxiRenderer's ShaderScope) -- the compiled bytecode lives in the pipeline.
    if (vs) res_.destroyShader(vs);
    if (ps) res_.destroyShader(ps);

    if (!ok) {
        AVER_ERROR("[AverSR] SpatialUpscaler: pipeline unavailable");
        return false;
    }
    pipelineFormat_ = dstDesc.format;

    if (!binding_) {
        rhi::BindingSetDesc bd;
        bd.srvCount = 1;
        bd.srvKinds[0] = rhi::SlotKind::Texture2D;
        binding_ = res_.createBindingSet(bd);
        boundColor_ = 0;   // a fresh binding set has nothing bound yet
        if (!binding_) {
            AVER_ERROR("[AverSR] SpatialUpscaler: binding set unavailable");
            res_.destroyPipeline(pipeline_);
            pipeline_ = 0;
            return false;
        }
    }
    return true;
}

void SpatialUpscaler::execute(rhi::IRenderContext& ctx, const rhi::UpscalerInput& in, rhi::TextureHandle outTarget) {
    if (!in.color || !outTarget || !in.srcWidth || !in.srcHeight) return;
    if (!ensurePipeline(outTarget)) return;

    if (in.color != boundColor_) {
        res_.setSrv(binding_, 0, in.color);
        boundColor_ = in.color;
    }

    SpatialUpscalerCB cb{};
    cb.srcSize[0] = static_cast<f32>(in.srcWidth);
    cb.srcSize[1] = static_cast<f32>(in.srcHeight);
    cb.srcSize[2] = 1.0f / static_cast<f32>(in.srcWidth);
    cb.srcSize[3] = 1.0f / static_cast<f32>(in.srcHeight);

    ctx.setPipeline(pipeline_);
    ctx.setBindingSet(binding_);
    ctx.setConstantBuffer(kUpscaleConstantRegister, &cb, sizeof(cb));
    ctx.drawFullscreen();
}

// rhi::postShaderSource() stays self-contained (no rhi::sharedShaderPrelude()); this pass follows
// the identical rule for the identical reason.
const char* spatialUpscaleShaderSource() {
    static const std::string s = R"(
// ================= AverSR: the built-in spatial upscaler =================
// Scene-resolution colour in (t0), present-resolution colour out. Bicubic (Catmull-Rom, a = -0.5)
// resample, separable in x and y over the 4x4 texel neighbourhood around the destination pixel's
// mapped source position. Sharper than a bilinear stretch without reading anything but the source
// texture: no depth, no motion vectors, no jitter, no history -- see SpatialUpscaler::needs().

cbuffer AverSrSpatialCB : register(b1) {
    float4 gUpscaleSrc;   // xy source size in texels, zw its reciprocal
};

Texture2D<float4> gUpscaleSceneTex : register(t0);
SamplerState       gUpscaleSamp    : register(s0);

struct AverSrVSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

// Emits one vertex of the fullscreen upscale triangle from SV_VertexID -- no vertex buffer bound.
AverSrVSOut VSUpscale(uint id : SV_VertexID) {
    AverSrVSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// The four Catmull-Rom cubic-convolution weights (a = -0.5) for a fractional offset t in [0, 1),
// for the samples at relative texel offsets -1, 0, +1, +2. Sums to exactly 1 for any t, so the
// four taps in each of x and y are already a partition of unity -- no separate normalisation.
float4 averSrCatmullRomWeights(float t) {
    float t2 = t * t;
    float t3 = t2 * t;
    return float4(
        -0.5 * t3 +       t2 - 0.5 * t,
         1.5 * t3 - 2.5 * t2 + 1.0,
        -1.5 * t3 + 2.0 * t2 + 0.5 * t,
         0.5 * t3 - 0.5 * t2);
}

float4 PSSpatialUpscale(AverSrVSOut i) : SV_TARGET {
    float2 srcTexel = i.uv * gUpscaleSrc.xy - 0.5;
    float2 f    = frac(srcTexel);
    float2 base = floor(srcTexel);

    float4 wx = averSrCatmullRomWeights(f.x);
    float4 wy = averSrCatmullRomWeights(f.y);

    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll] for (int y = -1; y <= 2; ++y) {
        [unroll] for (int x = -1; x <= 2; ++x) {
            // Clamped to the source's edge, matching this pass's own AddressMode::Clamp sampler.
            float2 texel = clamp(base + float2(x, y), 0.0, gUpscaleSrc.xy - 1.0);
            float2 uv = (texel + 0.5) * gUpscaleSrc.zw;
            sum += gUpscaleSceneTex.SampleLevel(gUpscaleSamp, uv, 0).rgb * (wx[x + 1] * wy[y + 1]);
        }
    }
    // Catmull-Rom can ring (overshoot) at hard edges; radiance below zero is meaningless, so clamp
    // the low end and leave the high end alone -- this runs before the post chain's own tonemap
    // when wired in, and that is what is responsible for compressing an overshoot back into range.
    return float4(max(sum, 0.0), 1.0);
}
)";
    return s.c_str();
}

} // namespace aver::sr
