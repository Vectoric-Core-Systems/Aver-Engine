// AverSR's edge-detecting AA resolve: a single full-screen pass that anti-aliases the edges of an
// already-resolved (1x MSAA, i.e. never multisampled to begin with) colour target, so 1x MSAA plus
// this pass is a viable alternative to paying MSAA's per-sample bandwidth for edge quality. See
// modules/render.sr/README.md for how this sits beside SpatialUpscaler, and docs/RENDERING.md's
// depth-prepass section for the sibling feature ("make shading cheaper by shading less") this one
// completes ("get edge AA without paying for it everywhere").
//
// LOOKED AT SpatialUpscaler FIRST, AS INSTRUCTED, BEFORE WRITING THIS. It is a bicubic (Catmull-Rom)
// RESAMPLE with no edge detection of any kind -- needs() answers None and it reads nothing but the
// source texture (see AverSrSpatial.hpp's own comment). There was no edge-adaptive path anywhere in
// this module to reuse; this is a new, second rhi::IUpscaler implementation alongside it, following
// the exact same seam and the exact same "OFF is bit-identical" contract SpatialUpscaler already
// established -- not a parallel mechanism, the SAME one with a different algorithm behind it.
#pragma once

#include "aver/rhi/RHIResources.hpp"

namespace aver::sr {

// A single-pass, luma-based edge-detecting AA filter in the spirit of Timothy Lottes' FXAA (the
// published TECHNIQUE -- early-out on low local contrast, classify each surviving edge as roughly
// horizontal or vertical from a Sobel-like combination of the 3x3 neighbourhood, blend towards the
// steeper neighbour perpendicular to that edge, plus a small subpixel/box-average term for aliasing
// a pure 4-neighbour edge test misses). WRITTEN FROM SCRATCH AGAINST THAT DESCRIPTION, not a port of
// NVIDIA's reference implementation, and it does not attempt that implementation's multi-step
// edge-direction SEARCH (walking outward along the edge to find where luma crosses the local
// average) -- this is the "FXAA-class... single full-screen pass" the brief asked for, not a claim
// of parity with FXAA 3.11's own console-optimised assembly. See spatialUpscaleShaderSource()'s
// sibling comment in AverSrSpatial.cpp for the same "say what this is not" discipline.
//
// RUNS BEFORE THE TONEMAP, on linear HDR radiance -- because that is where THIS SEAM (rhi::IUpscaler,
// see D3D12Device.cpp's "AverSR: Pass A" comment) already runs SpatialUpscaler, and reusing the seam
// rather than inventing a second post-chain hook was the point. Classic FXAA assumes it runs on an
// LDR, gamma-encoded backbuffer, where luma differences track PERCEIVED contrast; here the same luma
// formula is applied to whatever linear radiance the scene produced. It is still monotonic in
// brightness and still finds the same GEOMETRIC edges (an object silhouette against the sky is the
// steepest gradient in either space), so edge detection still works -- but its threshold constants
// were tuned against gamma-space contrast, not linear, so how aggressively it treats a given edge as
// "needs smoothing" shifts versus running it after the tonemap would. Moving this pass after the
// composite's tonemap is a real, separable follow-up (it would need its OWN post-composite hook,
// which does not exist today; see D3D12Device.cpp's composite comment) and was not done here.
//
// NEEDS() ANSWERS None, matching SpatialUpscaler: this reads only the scene colour target.
// UpscalerNeeds::Depth exists on the seam and IDevice::drawMeshDepthPrepass's own depth buffer would
// be the natural source for a depth-aware variant (real edges, not just luma ones -- a checkerboard
// texture painted flat on a wall has plenty of luma contrast and zero depth discontinuity), but nothing
// in D3D12Device.cpp's runPostChain populates UpscalerInput::depth for ANY upscaler today -- confirmed
// by inspection, not assumed: sceneColorTex_/presentHdrTex_ are bridged into TextureHandle-addressable
// resources for the colour path specifically (see the "AverSR: Pass A" comment), and depth has no
// equivalent bridge. Adding one is the exact next step if depth-aware edge detection is wanted; it is
// a separable change to a file outside this module's own boundary (docs/AVERSR.md "module
// boundaries" -- AverSR must not reach into a backend to build its own plumbing) and was left undone
// rather than done partially.
class FxaaResolve final : public rhi::IUpscaler {
public:
    explicit FxaaResolve(rhi::IResourceFactory& factory) : res_(factory) {}
    ~FxaaResolve() override;

    FxaaResolve(const FxaaResolve&) = delete;
    FxaaResolve& operator=(const FxaaResolve&) = delete;

    const char* name() const override { return "AverSR Edge AA (FXAA-class)"; }
    rhi::UpscalerNeeds needs() const override { return rhi::UpscalerNeeds::None; }
    bool isTemporal() const override { return false; }

    void execute(rhi::IRenderContext& ctx, const rhi::UpscalerInput& in, rhi::TextureHandle outTarget) override;

private:
    bool ensurePipeline(rhi::TextureHandle outTarget);

    rhi::IResourceFactory& res_;
    rhi::PipelineHandle    pipeline_ = 0;
    rhi::BindingSetHandle  binding_  = 0;
    rhi::Format            pipelineFormat_ = rhi::Format::Unknown;
    rhi::TextureHandle     boundColor_     = 0;
};

// The HLSL for FxaaResolve's one pass. Self-contained, same reasoning as
// spatialUpscaleShaderSource(): a leaf image-filtering operation, not a scene shader.
const char* fxaaResolveShaderSource();

} // namespace aver::sr
