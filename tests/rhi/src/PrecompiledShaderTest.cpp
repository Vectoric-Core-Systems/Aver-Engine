// PrecompiledShaderTest -- a real NRD shader, built into a real pipeline, on a real device.
//
// WHAT IT ACTUALLY DECIDES, because "the handle was non-zero" would decide nothing. This exercises
// the two RHI capabilities added for NVIDIA NRD together:
//
//   1. ShaderDesc::bytecode -- creating a shader from already-compiled bytecode rather than HLSL.
//   2. PipelineLayout::constantSpace / samplerSpace -- a root signature whose constant buffer and
//      static samplers sit in register space 1 while its SRVs and UAVs stay in space 0.
//
// and the reason testing them together is stronger than testing either alone is that **D3D12
// validates the shader against the root signature at CreateComputePipelineState**. A pipeline whose
// root signature does not describe every register the bytecode declares is REJECTED, by name, by
// the runtime. So a pipeline that creates is proof that the spaces were placed where NRD's DXIL
// says they are -- which is not something an assertion written by the same hand that wrote the
// placement could establish.
//
// THE BYTECODE IS NRD'S OWN, not a fixture. A synthetic blob would test that the pointer survived
// the call; NRD's REBLUR permutations are what this was built for, they declare the awkward
// space-1 constant buffer, and they are what will actually run. Using them means this test fails if
// the vendored library, its shader build, or the binding model ever moves.
//
// WARP, so no GPU is required -- and SKIPPED, not failed, wherever a prerequisite is genuinely
// absent: no D3D12 runtime, or an engine built with AVER_WITH_NRD=OFF (which cmake/AverNRD.cmake
// will do by itself on a machine with no offline shader compiler). Neither is a defect.
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/render/nrd/NrdDenoiser.hpp"
#include "aver/core/Log.hpp"

#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

int main() {
    AVER_INFO("=== PrecompiledShaderTest ===");

    if (!render::nrd::Denoiser::available()) {
        AVER_INFO("  SKIP  NRD is not in this build, so there is no precompiled bytecode to test "
                  "with (AVER_WITH_NRD=OFF)");
        return 0;
    }

    const render::nrd::DenoiserKind kinds[] = {render::nrd::DenoiserKind::ReblurDiffuseOcclusion};
    render::nrd::Denoiser den;
    if (!den.create(kinds, 1)) {
        AVER_ERROR("  FAIL  could not create an NRD instance to source bytecode from");
        AVER_ERROR("=== PrecompiledShaderTest FAILED === (1 failure(s))");
        return 1;
    }
    const render::nrd::InstanceLayout lay = den.layout();
    check(lay.pipelineCount > 0, "NRD describes at least one pipeline");
    if (lay.pipelineCount == 0) {
        AVER_ERROR("=== PrecompiledShaderTest FAILED === ({} failure(s))", g_failures);
        return 1;
    }

    rhi::DeviceDesc desc;
    desc.useWarp = true;
    // D3D12 ONLY. Vulkan deliberately REFUSES a non-zero register space (see
    // VulkanResourceFactory::descriptorLayout for the argument), so running this there would be
    // asserting that a documented refusal refuses -- which the refusal's own early return already
    // says, and which would report as a failure of this test rather than as the design it is.
    desc.preferred[0] = rhi::Backend::D3D12;
    desc.preferred[1] = rhi::Backend::Null;
    desc.preferredCount = 2;
    rhi::IDevice* dev = rhi::createDevice(desc);
    if (!dev || dev->backend() == rhi::Backend::Null) {
        AVER_WARN("  SKIP  no D3D12 device (not even WARP) on this machine");
        if (dev) rhi::destroyDevice(dev);
        return 0;
    }
    rhi::IResourceFactory* res = dev->resources();
    if (!res) {
        AVER_WARN("  SKIP  this device exposes no resource factory");
        rhi::destroyDevice(dev);
        return 0;
    }

    // ---- pick a pipeline that carries DXIL --------------------------------------------------
    const render::nrd::PipelineInfo* pick = nullptr;
    for (u32 i = 0; i < lay.pipelineCount && pick == nullptr; ++i)
        if (lay.pipelines[i].dxil.valid()) pick = &lay.pipelines[i];
    if (pick == nullptr) {
        AVER_WARN("  SKIP  this build embedded no DXIL (SPIR-V only), so D3D12 has nothing to load");
        rhi::destroyDevice(dev);
        return 0;
    }
    AVER_INFO("using NRD pipeline '{}' ({} bytes of DXIL)", pick->debugName ? pick->debugName : "?",
              pick->dxil.size);

    // EVERYTHING NEEDED FROM THE INSTANCE IS COPIED OUT *HERE*, before it is destroyed below.
    // InstanceLayout hands out pointers into the Denoiser's own translation buffers and says so;
    // reading pick->ranges after destroy() is a use-after-free. This test found that the honest
    // way -- the first version did exactly that and reported "736 SRV, 0 UAV" from freed memory,
    // which is also a good argument for asserting the counts are sane rather than trusting them.
    u32 srvCount = 0, uavCount = 0;
    for (u32 r = 0; r < pick->rangeCount; ++r) {
        if (pick->ranges[r].cls == render::nrd::ResourceClass::StorageTexture) uavCount += pick->ranges[r].count;
        else                                                                   srvCount += pick->ranges[r].count;
    }
    const u32 space        = lay.binding.constantBufferAndSamplersSpace;
    const u32 samplerCount = lay.binding.samplerCount;
    render::nrd::SamplerKind samplerKinds[8]{};
    for (u32 i = 0; i < samplerCount && i < 8; ++i) samplerKinds[i] = lay.binding.samplers[i];

    AVER_INFO("declared ranges: {} SRV, {} UAV; binding model wants cbv/samplers in space {}",
              srvCount, uavCount, space);
    // A ROOT SIGNATURE CANNOT HOLD MORE THAN kMaxBindingSlots PER TABLE, so a count past it means
    // the numbers came from somewhere they should not have -- freed memory, as above. Checked
    // rather than assumed, because the pipeline would fail below either way and the failure would
    // be blamed on the register spaces.
    check(srvCount <= rhi::kMaxBindingSlots && uavCount <= rhi::kMaxBindingSlots,
          "the declared slot counts fit a binding table");

    // ---- 1. a shader from bytecode ------------------------------------------------------------
    rhi::ShaderDesc sd{};
    sd.stage        = rhi::ShaderStage::Compute;
    sd.bytecode     = pick->dxil.data;
    sd.bytecodeSize = pick->dxil.size;
    check(sd.precompiled(), "ShaderDesc::precompiled() recognises a bytecode desc");
    const rhi::ShaderHandle cs = res->createShader(sd);
    check(cs != 0, "createShader accepts DXIL with no source, entry point or shader model");

    // THE COPY IS THE CONTRACT. ShaderDesc::bytecode promises the caller may free the bytes the
    // moment createShader returns, so destroying the NRD instance -- which owns this bytecode --
    // must leave the shader usable. If it were borrowed, the pipeline built below would be reading
    // freed memory, and the tell would be a corrupt PSO or a device removal rather than a crash
    // here. That is precisely the failure this ordering is arranged to catch.
    den.destroy();
    check(!den.valid(), "the NRD instance that owned the bytecode is gone");

    // ---- 2. a pipeline whose root signature puts the CBV and samplers in space 1 ----------------
    // Counts come from NRD's own declared ranges, captured above while the instance was alive. A
    // layout that under-declares would be rejected by D3D12 exactly as a mis-spaced one would, so
    // these must be right for the space assertion below to mean anything.
    rhi::PipelineLayout pl{};
    pl.srvCount      = srvCount;
    pl.uavCount      = uavCount;
    pl.constantSpace = space;
    pl.samplerSpace  = space;
    pl.samplerCount  = samplerCount < 4 ? samplerCount : 4;
    for (u32 i = 0; i < pl.samplerCount; ++i) {
        pl.samplers[i].filter  = samplerKinds[i] == render::nrd::SamplerKind::LinearClamp
                                     ? rhi::Filter::Linear : rhi::Filter::Point;
        pl.samplers[i].address = rhi::AddressMode::Clamp;
    }

    rhi::ComputePipelineDesc cpd{};
    cpd.cs     = cs;
    cpd.layout = pl;
    const rhi::PipelineHandle pipe = res->createComputePipeline(cpd);
    // THE CENTRAL ASSERTION. D3D12 rejects a compute PSO whose root signature does not cover every
    // register the bytecode declares, so this succeeding is the runtime confirming that b0 and s0
    // were emitted in space 1 and t/u in space 0 -- the placement NRD's DXIL was compiled against.
    check(pipe != 0, "createComputePipeline builds NRD's shader against a space-1 constant buffer");

    // ---- 3. the same shader with the spaces left at 0 must NOT build ----------------------------
    // A NEGATIVE CONTROL, and without it the assertion above is much weaker than it looks: a root
    // signature that happened to satisfy the shader for some other reason would pass it. This is
    // the same bytecode and the same counts with only the two new fields returned to their
    // defaults, so a failure here is attributable to those fields and nothing else.
    rhi::PipelineLayout wrong = pl;
    wrong.constantSpace = 0;
    wrong.samplerSpace  = 0;
    rhi::ComputePipelineDesc bad{};
    bad.cs     = cs;
    bad.layout = wrong;
    AVER_INFO("(the next line is expected to report a pipeline failure -- it is the negative control)");
    const rhi::PipelineHandle badPipe = res->createComputePipeline(bad);
    check(badPipe == 0, "the same shader is REJECTED when the constant buffer is left in space 0");

    res->destroyShader(cs);
    rhi::destroyDevice(dev);

    if (g_failures != 0) {
        AVER_ERROR("=== PrecompiledShaderTest FAILED === ({} failure(s))", g_failures);
        return 1;
    }
    AVER_INFO("=== PrecompiledShaderTest passed ===");
    return 0;
}
