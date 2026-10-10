// PrecompiledShaderTest -- a precompiled shader, built into a real pipeline, on a real device.
//
// WHAT IT ACTUALLY DECIDES, because "the handle was non-zero" would decide nothing. This exercises
// two RHI capabilities together:
//
//   1. ShaderDesc::bytecode -- creating a shader from already-compiled bytecode rather than HLSL.
//   2. PipelineLayout::constantSpace / samplerSpace -- a root signature whose constant buffer and
//      static samplers sit in register space 1 while its SRVs and UAVs stay in space 0. A shader this
//      engine did not compile may make exactly that choice, and without these fields a root
//      signature built here could not describe it.
//
// and the reason testing them together is stronger than testing either alone is that **D3D12
// validates the shader against the root signature at CreateComputePipelineState**. A pipeline whose
// root signature does not describe every register the bytecode declares is REJECTED, by name, by
// the runtime. So a pipeline that creates is proof that the spaces were placed where the DXIL says
// they are -- which is not something an assertion written by the same hand that wrote the
// placement could establish.
//
// THE BYTECODE IS A FIXTURE: tests/rhi/shaders/precompiled_space1.hlsl, compiled offline to DXIL and
// embedded as src/PrecompiledShaderFixture.inc (its header says how to regenerate it). It is a
// compute shader with a cbuffer and a sampler in space 1 and one SRV and one UAV in space 0, all of
// them used so none is optimised away. Embedding the bytes, rather than compiling at test time,
// keeps this a test of ShaderDesc::bytecode: the DXIL never passes through the engine's compiler.
//
// WARP, so no GPU is required -- and SKIPPED, not failed, wherever a prerequisite is genuinely
// absent: no D3D12 runtime. That is not a defect.
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/core/Log.hpp"

#include <string>

#include "PrecompiledShaderFixture.inc"

using namespace aver;

static int g_failures = 0;
static constexpr int kSkip = 77;  // ctest SKIP_RETURN_CODE (root CMakeLists.txt); not a pass

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

int main() {
    AVER_INFO("=== PrecompiledShaderTest ===");

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
        return kSkip;
    }
    rhi::IResourceFactory* res = dev->resources();
    if (!res) {
        AVER_WARN("  SKIP  this device exposes no resource factory");
        rhi::destroyDevice(dev);
        return kSkip;
    }

    // What the fixture declares (see precompiled_space1.hlsl): one SRV (t0) and one UAV (u0) in
    // space 0, one cbuffer (b0) and one sampler (s0) in space 1.
    const u32 space = 1;
    AVER_INFO("fixture: {} bytes of DXIL; cbv/sampler in space {}", sizeof(kPrecompiledSpace1Dxil), space);

    // ---- 1. a shader from bytecode ------------------------------------------------------------
    rhi::ShaderDesc sd{};
    sd.stage        = rhi::ShaderStage::Compute;
    sd.bytecode     = kPrecompiledSpace1Dxil;
    sd.bytecodeSize = sizeof(kPrecompiledSpace1Dxil);
    check(sd.precompiled(), "ShaderDesc::precompiled() recognises a bytecode desc");
    const rhi::ShaderHandle cs = res->createShader(sd);
    check(cs != 0, "createShader accepts DXIL with no source, entry point or shader model");

    // THE BYTES ARE NOT KEPT. ShaderDesc::bytecode promises the caller may free them the moment
    // createShader returns, so the pipeline below must build from the backend's own copy. The array
    // here is static and cannot be freed, so this is the contract's wording, not a check of it.

    // ---- 2. a pipeline whose root signature puts the CBV and samplers in space 1 ----------------
    // Counts match the fixture's declarations. A layout that under-declares would be rejected by
    // D3D12 exactly as a mis-spaced one would, so these must be right for the space assertion
    // below to mean anything.
    rhi::PipelineLayout pl{};
    pl.srvCount      = 1;
    pl.uavCount      = 1;
    pl.constantSpace = space;
    pl.samplerSpace  = space;
    pl.samplerCount  = 1;
    pl.samplers[0].filter  = rhi::Filter::Linear;
    pl.samplers[0].address = rhi::AddressMode::Clamp;

    rhi::ComputePipelineDesc cpd{};
    cpd.cs     = cs;
    cpd.layout = pl;
    const rhi::PipelineHandle pipe = res->createComputePipeline(cpd);
    // THE CENTRAL ASSERTION. D3D12 rejects a compute PSO whose root signature does not cover every
    // register the bytecode declares, so this succeeding is the runtime confirming that b0 and s0
    // were emitted in space 1 and t/u in space 0 -- the placement the fixture's DXIL was compiled against.
    check(pipe != 0, "createComputePipeline builds the fixture against a space-1 constant buffer");

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
