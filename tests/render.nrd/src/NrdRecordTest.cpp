// NrdRecordTest -- NRD's dispatches, actually recorded onto a real command list.
//
// WHAT THIS DECIDES THAT NrdLinkTest CANNOT. That test exercises the whole CPU half with no device:
// it proves NRD is linked, that it embeds shaders, and that it PLANS a frame. Every one of those
// can pass while the plan is unrecordable -- and for most of this module's life it was, because
// nothing had ever tried. This test is the other half: real device, real pipelines built from NRD's
// own DXIL, real texture pools, real descriptor tables, real dispatches on a real command list.
//
// THE ASSERTION THAT MATTERS IS THAT record() RETURNS TRUE ON A SUBMITTED FRAME. D3D12 validates a
// compute PSO against its root signature at creation and rejects a mismatch by name, so eleven
// pipelines building is already the runtime agreeing that NRD's register spaces were placed where
// its bytecode says. Recording then proves the rest: that every slot in every dispatch resolved to
// a texture, that the descriptor tables fit, and that the constant blocks upload.
//
// WARP, so no GPU is required. SKIPPED rather than failed wherever a prerequisite is genuinely
// absent -- NRD compiled out, or no D3D12 at all. Vulkan is not tested and must not be: this pass
// is D3D12-only BY DESIGN (NRD wants its constant buffer and samplers in register space 1, which
// VulkanResourceFactory::descriptorLayout deliberately refuses), so a failure there would be
// reporting a documented decision as a defect.
#include "aver/render/nrd/NrdRecorder.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/core/Log.hpp"

#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

int main() {
    AVER_INFO("=== NrdRecordTest ===");

    if (!render::nrd::Denoiser::available()) {
        AVER_INFO("  SKIP  NRD is not in this build (AVER_WITH_NRD=OFF)");
        return 0;
    }

    rhi::DeviceDesc desc;
    desc.useWarp = true;
    desc.preferred[0] = rhi::Backend::D3D12;
    desc.preferred[1] = rhi::Backend::Null;
    desc.preferredCount = 2;
    rhi::IDevice* dev = rhi::createDevice(desc);
    if (!dev || dev->backend() != rhi::Backend::D3D12) {
        AVER_WARN("  SKIP  no D3D12 device (not even WARP) on this machine");
        if (dev) rhi::destroyDevice(dev);
        return 0;
    }
    rhi::IResourceFactory* res = dev->resources();
    rhi::IRenderContext*   ctx = dev->renderContext();
    if (!res || !ctx) {
        AVER_WARN("  SKIP  this device exposes no resource factory or render context");
        rhi::destroyDevice(dev);
        return 0;
    }

    // ---- the pass ------------------------------------------------------------------------------
    const render::nrd::DenoiserKind kinds[] = {render::nrd::DenoiserKind::ReblurDiffuseOcclusion};
    render::nrd::Recorder rec;
    check(rec.create(*dev, kinds, 1), "create builds a pipeline for every shader NRD embedded");
    if (!rec.valid()) {
        AVER_ERROR("=== NrdRecordTest FAILED === ({} failure(s))", g_failures ? g_failures : 1);
        rhi::destroyDevice(dev);
        return 1;
    }

    const u32 kW = 320, kH = 180;
    check(rec.resize(kW, kH), "resize allocates NRD's permanent and transient pools");
    // A FRESH POOL IS A DEAD HISTORY, and the recorder is expected to say so without being asked --
    // a caller that trusted an uninitialised permanent pool would get confident garbage for the
    // first frames rather than an error.
    check(rec.historyIsStale(), "a freshly sized pass reports its history as stale");

    // ---- the inputs NRD reads -------------------------------------------------------------------
    // Contents are irrelevant here: this test decides whether the plan can be RECORDED, not what it
    // computes. Formats are the ones the real integration will hand over.
    auto tex = [&](rhi::Format f, const char* name) {
        rhi::TextureDesc d{};
        d.width = kW; d.height = kH; d.format = f;
        d.bind = static_cast<rhi::ResourceBind>(
                     static_cast<u32>(rhi::ResourceBind::ShaderResource) |
                     static_cast<u32>(rhi::ResourceBind::UnorderedAccess));
        d.initialState = rhi::ResourceState::UnorderedAccess;
        d.debugName = name;
        return res->createTexture(d);
    };
    render::nrd::Recorder::Inputs in;
    in.viewZ           = tex(rhi::Format::R32Float,  "test IN_VIEWZ");
    in.motionVectors   = tex(rhi::Format::RGBA16F,   "test IN_MV");
    in.normalRoughness = tex(rhi::Format::RGBA16F,   "test IN_NORMAL_ROUGHNESS");
    in.diffuseHitDist  = tex(rhi::Format::R16Unorm,  "test IN_DIFF_HITDIST");
    check(in.viewZ && in.motionVectors && in.normalRoughness && in.diffuseHitDist,
          "the four NRD inputs allocate");

    // ---- a null input must be REFUSED, not bound --------------------------------------------------
    // The negative control, and it runs before the real one so its expected warning cannot be
    // mistaken for a late failure. Without it, "record returned true" would be satisfied by a
    // recorder that never checked anything.
    render::nrd::Recorder::Inputs broken = in;
    broken.viewZ = 0;
    render::nrd::FrameSettings fs{};
    fs.resourceWidth = kW; fs.resourceHeight = kH;
    fs.rectWidth = kW; fs.rectHeight = kH;
    fs.frameIndex = 0;
    fs.resetHistory = true;
    // Identity-ish camera; NRD only needs these to be self-consistent for a plan to be produced.
    for (int i = 0; i < 4; ++i) { fs.viewToClip[i * 5] = 1.0f; fs.worldToView[i * 5] = 1.0f;
                                  fs.viewToClipPrev[i * 5] = 1.0f; fs.worldToViewPrev[i * 5] = 1.0f; }
    const u32 which[] = {0};

    dev->beginFrame();
    AVER_INFO("(the next line is expected to report a null input -- it is the negative control)");
    check(!rec.record(*ctx, fs, broken, which, 1), "a null input texture is REFUSED, not bound");

    // ---- the real thing --------------------------------------------------------------------------
    const bool recorded = rec.record(*ctx, fs, in, which, 1);
    check(recorded, "record plans and records NRD's dispatches onto a live command list");
    check(!recorded || rec.outputDiffuseHitDistance() != 0,
          "a recorded frame names the texture holding OUT_DIFF_HITDIST");
    check(!recorded || !rec.historyIsStale(), "a recorded frame clears the stale-history flag");
    dev->endFrame();

    // ---- a SECOND frame, because the first one proves less than it looks --------------------------
    // NRD's permanent pool is its temporal history, and the interesting failure is a plan whose
    // second frame binds something the first one did not create. resetHistory is false now, which
    // is the path a real run spends every frame but its first.
    fs.frameIndex = 1;
    fs.resetHistory = false;
    dev->beginFrame();
    check(rec.record(*ctx, fs, in, which, 1), "a second, history-consuming frame also records");
    dev->endFrame();

    rec.destroy();
    check(!rec.valid(), "destroy releases the pass");
    for (rhi::TextureHandle t : {in.viewZ, in.motionVectors, in.normalRoughness, in.diffuseHitDist})
        if (t) res->destroyTexture(t);
    rhi::destroyDevice(dev);

    if (g_failures != 0) {
        AVER_ERROR("=== NrdRecordTest FAILED === ({} failure(s))", g_failures);
        return 1;
    }
    AVER_INFO("=== NrdRecordTest passed ===");
    return 0;
}
