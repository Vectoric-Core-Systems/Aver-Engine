// FxaaResolve: AverSR's edge-detecting AA implementation of the rhi::IUpscaler seam (see
// AverSrFxaa.hpp for what it is, what it is not, and why it runs where it runs).
#include "aver/sr/AverSrFxaa.hpp"
#include "aver/core/Log.hpp"

#include <string>

namespace aver::sr {

// Mirrors `cbuffer AverSrFxaaCB` in the HLSL below exactly, same shape as SpatialUpscalerCB.
struct FxaaCB {
    f32 srcSize[4];   // xy source size in texels, zw its reciprocal
};

// Own root CBV register, same reasoning as AverSrSpatial.cpp's kUpscaleConstantRegister: this
// shader never includes rhi::sharedShaderPrelude(), so b1 here is unrelated to what b1 means to a
// pipeline that does.
constexpr u32 kFxaaConstantRegister = 1;

FxaaResolve::~FxaaResolve() {
    if (pipeline_) res_.destroyPipeline(pipeline_);
    if (binding_)  res_.destroyBindingSet(binding_);
}

bool FxaaResolve::ensurePipeline(rhi::TextureHandle outTarget) {
    rhi::TextureDesc dstDesc{};
    if (!res_.textureInfo(outTarget, dstDesc)) {
        AVER_ERROR("[AverSR] FxaaResolve: outTarget handle {} has no texture info", outTarget);
        return false;
    }

    if (pipeline_ && dstDesc.format == pipelineFormat_) return true;

    if (pipeline_) { res_.destroyPipeline(pipeline_); pipeline_ = 0; }

    const char* src = fxaaResolveShaderSource();
    rhi::ShaderDesc vsd; vsd.source = src; vsd.entry = "AverSrFxaaVS";   vsd.stage = rhi::ShaderStage::Vertex; vsd.minShaderModel = 51;
    rhi::ShaderDesc psd; psd.source = src; psd.entry = "AverSrFxaaMain"; psd.stage = rhi::ShaderStage::Pixel;  psd.minShaderModel = 51;
    const rhi::ShaderHandle vs = res_.createShader(vsd);
    const rhi::ShaderHandle ps = res_.createShader(psd);

    bool ok = false;
    if (vs && ps) {
        rhi::GraphicsPipelineDesc p;
        p.vs = vs;
        p.ps = ps;
        p.layout.srvCount = 1;
        p.layout.samplers[0] = rhi::SamplerDesc{rhi::Filter::Linear, rhi::AddressMode::Clamp};
        p.layout.samplerCount = 1;
        p.cull = rhi::CullMode::None;
        p.depthClip = false;
        p.renderTargetCount = 1;
        p.renderTargets[0] = dstDesc.format;
        p.sampleCount = 1;   // outTarget is always single-sample -- the post chain resolves MSAA first
        pipeline_ = res_.createGraphicsPipeline(p);
        ok = pipeline_ != 0;
    }
    if (vs) res_.destroyShader(vs);
    if (ps) res_.destroyShader(ps);

    if (!ok) {
        AVER_ERROR("[AverSR] FxaaResolve: pipeline unavailable");
        return false;
    }
    pipelineFormat_ = dstDesc.format;

    if (!binding_) {
        rhi::BindingSetDesc bd;
        bd.srvCount = 1;
        bd.srvKinds[0] = rhi::SlotKind::Texture2D;
        binding_ = res_.createBindingSet(bd);
        boundColor_ = 0;
        if (!binding_) {
            AVER_ERROR("[AverSR] FxaaResolve: binding set unavailable");
            res_.destroyPipeline(pipeline_);
            pipeline_ = 0;
            return false;
        }
    }
    return true;
}

void FxaaResolve::execute(rhi::IRenderContext& ctx, const rhi::UpscalerInput& in, rhi::TextureHandle outTarget) {
    if (!in.color || !outTarget || !in.srcWidth || !in.srcHeight) return;
    if (!ensurePipeline(outTarget)) return;

    if (in.color != boundColor_) {
        res_.setSrv(binding_, 0, in.color);
        boundColor_ = in.color;
    }

    FxaaCB cb{};
    cb.srcSize[0] = static_cast<f32>(in.srcWidth);
    cb.srcSize[1] = static_cast<f32>(in.srcHeight);
    cb.srcSize[2] = 1.0f / static_cast<f32>(in.srcWidth);
    cb.srcSize[3] = 1.0f / static_cast<f32>(in.srcHeight);

    ctx.setPipeline(pipeline_);
    ctx.setBindingSet(binding_);
    ctx.setConstantBuffer(kFxaaConstantRegister, &cb, sizeof(cb));
    ctx.drawFullscreen();
}

const char* fxaaResolveShaderSource() {
    static const std::string s = R"(
// ================= AverSR: edge-detecting AA resolve (FXAA-class) =================
// Scene colour in and out at the SAME resolution (t0 -> SV_TARGET), read through the identical
// fullscreen-triangle vertex shader trick spatialUpscaleShaderSource()'s AverSrSpatialVS uses.

cbuffer AverSrFxaaCB : register(b1) {
    float4 gFxaaSrc;   // xy source size in texels, zw its reciprocal
};

Texture2D<float4> gFxaaColorTex : register(t0);
SamplerState       gFxaaSamp    : register(s0);

struct AverFxaaVSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

AverFxaaVSOut AverSrFxaaVS(uint id : SV_VertexID) {
    AverFxaaVSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// Perceptual luma weights (Rec. 601), applied to whatever radiance this pass is handed -- see
// AverSrFxaa.hpp's own comment on running before the tonemap and what that costs the threshold
// constants below versus running on a gamma-encoded backbuffer.
float averFxaaLuma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float4 AverSrFxaaMain(AverFxaaVSOut i) : SV_TARGET {
    float2 uv = i.uv;
    float2 px = gFxaaSrc.zw;

    float3 rgbM  = gFxaaColorTex.Sample(gFxaaSamp, uv).rgb;
    float3 rgbN  = gFxaaColorTex.Sample(gFxaaSamp, uv + float2(0.0, -px.y)).rgb;
    float3 rgbS  = gFxaaColorTex.Sample(gFxaaSamp, uv + float2(0.0,  px.y)).rgb;
    float3 rgbE  = gFxaaColorTex.Sample(gFxaaSamp, uv + float2( px.x, 0.0)).rgb;
    float3 rgbW  = gFxaaColorTex.Sample(gFxaaSamp, uv + float2(-px.x, 0.0)).rgb;
    float3 rgbNW = gFxaaColorTex.Sample(gFxaaSamp, uv + float2(-px.x, -px.y)).rgb;
    float3 rgbNE = gFxaaColorTex.Sample(gFxaaSamp, uv + float2( px.x, -px.y)).rgb;
    float3 rgbSW = gFxaaColorTex.Sample(gFxaaSamp, uv + float2(-px.x,  px.y)).rgb;
    float3 rgbSE = gFxaaColorTex.Sample(gFxaaSamp, uv + float2( px.x,  px.y)).rgb;

    float lumaM  = averFxaaLuma(rgbM);
    float lumaN  = averFxaaLuma(rgbN),  lumaS  = averFxaaLuma(rgbS);
    float lumaE  = averFxaaLuma(rgbE),  lumaW  = averFxaaLuma(rgbW);
    float lumaNW = averFxaaLuma(rgbNW), lumaNE = averFxaaLuma(rgbNE);
    float lumaSW = averFxaaLuma(rgbSW), lumaSE = averFxaaLuma(rgbSE);

    float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    float range = lumaMax - lumaMin;

    // FXAA 3.11's own default thresholds. Below either, this pixel's neighbourhood is too flat to be
    // an aliased edge -- return it untouched. This early-out is what keeps the pass cheap over most
    // of a frame: the sky, a wall's interior and foliage far from its own silhouette all take it.
    const float kEdgeThresholdMin = 0.0312;
    const float kEdgeThresholdRel = 0.125;
    if (range < max(kEdgeThresholdMin, lumaMax * kEdgeThresholdRel))
        return float4(rgbM, 1.0);

    // Edge ORIENTATION from a Sobel-like second-derivative combination of the 3x3 neighbourhood --
    // whichever axis changes faster is the one the aliasing runs ALONG, so blending happens
    // PERPENDICULAR to it.
    float edgeVert = abs(lumaNW + lumaSW - 2.0 * lumaW) + 2.0 * abs(lumaN + lumaS - 2.0 * lumaM)
                    + abs(lumaNE + lumaSE - 2.0 * lumaE);
    float edgeHorz = abs(lumaNW + lumaNE - 2.0 * lumaN) + 2.0 * abs(lumaW + lumaE - 2.0 * lumaM)
                    + abs(lumaSW + lumaSE - 2.0 * lumaS);
    bool isHorizontalEdge = edgeHorz >= edgeVert;

    float luma1 = isHorizontalEdge ? lumaN : lumaW;
    float luma2 = isHorizontalEdge ? lumaS : lumaE;
    float gradient1 = abs(luma1 - lumaM);
    float gradient2 = abs(luma2 - lumaM);
    bool side1Steeper = gradient1 >= gradient2;
    float gradientSteepest = side1Steeper ? gradient1 : gradient2;

    float3 rgbDir = side1Steeper ? (isHorizontalEdge ? rgbN : rgbW)
                                  : (isHorizontalEdge ? rgbS : rgbE);
    // Directional blend: pull the centre pixel toward whichever perpendicular neighbour has the
    // steeper gradient, in proportion to how much of the local contrast that one step already
    // explains. Capped at 0.5 so a real edge softens rather than duplicates its neighbour outright.
    float blendFactor = saturate(gradientSteepest / max(range, 1e-4)) * 0.5;
    float3 rgbEdgeBlend = lerp(rgbM, rgbDir, blendFactor);

    // Subpixel term: FXAA's catch for aliasing a 4-neighbour edge test alone misses -- thin geometry
    // (a wire, a distant railing) that the immediate N/S/E/W samples straddle without registering as
    // a directional edge. Blends toward the full 3x3 box average by how far this pixel sits from that
    // box's own luma, relative to the local contrast range.
    float3 rgbBox = (rgbN + rgbS + rgbE + rgbW + rgbNW + rgbNE + rgbSW + rgbSE) * (1.0 / 8.0);
    float lumaBox = averFxaaLuma(rgbBox);
    float subpixelBlend = saturate(abs(lumaBox - lumaM) / max(range, 1e-4)) * 0.5;

    float3 result = lerp(rgbEdgeBlend, rgbBox, subpixelBlend);
    return float4(result, 1.0);
}
)";
    return s.c_str();
}

} // namespace aver::sr
