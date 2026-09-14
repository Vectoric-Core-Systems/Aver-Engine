// ShaderIncludeResolveTest -- can a deployed shader #include a file in a SUBDIRECTORY, and can it
// do it with ANGLED brackets?
//
// WHY THIS IS A TEST AND NOT A COMMENT. HLSL in this engine compiles at RUNTIME. A broken include
// is therefore not a build error, not a link error and not something any amount of reading catches:
// it surfaces as DXC reporting `use of undeclared identifier` or `unknown type name` from a file
// three levels away, at the moment a pipeline is first built, on a machine with a GPU. Both defects
// below shipped exactly that way and both were found by a user launching the editor.
//
//   1. THE FLAT NAMESPACE. DxcShaderInclude::LoadSource used to keep only the FILENAME, so
//      bin/shaders -- which every module deploys into -- was one flat, and on Windows
//      case-insensitive, namespace. Deploying a vendored tree that ships Color.hlsli beside this
//      engine's own color.hlsli put ONE file on disk and silently deleted srgbToLin.
//
//   2. ANGLED INCLUDES NEVER REACHED THE HANDLER AT ALL. A custom IDxcIncludeHandler is consulted
//      for `#include "x"` because a quoted include searches the including file's own directory.
//      `#include <x>` searches ONLY the -I list, and both backends passed no -I, so DXC never asked
//      the handler and answered `file not found with <angled> include; use "quotes" instead`. That
//      is not advice this engine can take: the angled includes are inside vendored third-party
//      HLSL, and rewriting a vendored tree is what third_party/*/AVER_README.md exists to avoid.
//      Because voxi.hlsl is ONE translation unit holding every entry point, this took down
//      PSMainVoxi, PSRayDriven, VSky and VSMain together -- the entire ray-tracing feature set,
//      including configurations that never asked for the vendored feature.
//
// THE FIXTURE IS THIS TEST'S OWN (tests/rhi/shaders/AverIncProbe), deliberately not RTXDI's. A test
// that included the real vendored tree would couple Aver.RHI's test to whichever feature module
// happens to deploy that tree, and would start SKIPping the day that module is disabled -- which is
// the day this regression would return.
//
// WARP, so no GPU is required, and SKIPPED rather than failed where D3D12 is genuinely absent.
// Vulkan takes the identical argument list through the identical handler (see
// VulkanShaderCompiler.cpp), but it needs a real driver, so asserting on D3D12 is what can be done
// everywhere this runs.
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/rhi/ShaderFiles.hpp"
#include "aver/core/Log.hpp"

#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// A compute shader, because it is the only stage with no input/output signature to satisfy -- the
// subject here is the preprocessor, and a vertex shader would add a second way to fail that has
// nothing to do with includes.
static rhi::ShaderHandle compile(rhi::IResourceFactory* res, const char* src) {
    rhi::ShaderDesc sd{};
    sd.source = src;
    sd.entry  = "main";
    sd.stage  = rhi::ShaderStage::Compute;
    return res->createShader(sd);
}

int main() {
    AVER_INFO("=== ShaderIncludeResolveTest ===");

    // The fixture has to have been DEPLOYED, not merely committed. Checked first and by name, so a
    // CMake copy that stops working reports as itself rather than as four mysterious compile
    // failures -- which is precisely the confusion this whole test exists to end.
    if (rhi::shaderFileIfPresent("AverIncProbe/Inner.hlsli") == nullptr) {
        AVER_ERROR("  FAIL  AverIncProbe/Inner.hlsli was not deployed to bin/shaders (CMake copy)");
        AVER_ERROR("=== ShaderIncludeResolveTest FAILED === (1 failure(s))");
        return 1;
    }

    rhi::DeviceDesc desc;
    desc.useWarp = true;
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

    // ---- 0. the negative control, FIRST ---------------------------------------------------------
    // Without it every assertion below is far weaker than it looks: a compiler that ignored includes
    // entirely, or a handler that answered a miss with an empty blob, would pass all three. Running
    // it first also means the expected error text in the log sits above the passes rather than
    // looking like a late failure.
    AVER_INFO("(the next lines are expected to report a compile failure -- it is the negative control)");
    const rhi::ShaderHandle missing = compile(res,
        "#include \"AverIncProbe/NoSuchFile.hlsli\"\n"
        "[numthreads(1,1,1)] void main() {}\n");
    check(missing == 0, "an include that does not exist is a COMPILE FAILURE, not an empty file");

    // ---- 1. quoted, with a directory component --------------------------------------------------
    // The defect this catches is the old basename-only handler, which resolved this to "Inner.hlsli"
    // and would have found a file of that name deployed by ANY module. Calling the function proves
    // the include actually contributed declarations rather than merely being found.
    const rhi::ShaderHandle quoted = compile(res,
        "#include \"AverIncProbe/Inner.hlsli\"\n"
        "RWStructuredBuffer<float> gOut : register(u0);\n"
        "[numthreads(1,1,1)] void main() { gOut[0] = averIncProbeInner(1.0); }\n");
    check(quoted != 0, "a QUOTED include resolves through a subdirectory path");

    // ---- 2. angled, at the top level ------------------------------------------------------------
    const rhi::ShaderHandle angled = compile(res,
        "#include <AverIncProbe/Inner.hlsli>\n"
        "RWStructuredBuffer<float> gOut : register(u0);\n"
        "[numthreads(1,1,1)] void main() { gOut[0] = averIncProbeInner(1.0); }\n");
    check(angled != 0, "an ANGLED include reaches the include handler at all (needs -I)");

    // ---- 3. angled, NESTED inside an included file ----------------------------------------------
    // THE ONE THAT ACTUALLY BROKE, and it is not the same lookup as case 2. Here the including file
    // is itself a handler-supplied blob, so DXC has a notion of "the includer's directory" that it
    // invented rather than read off a disk -- which is what makes the resolved path DXC hands back
    // shaped differently (a duplicated prefix) from either of the cases above. RTXDI's
    // Utils/RandomSamplerState.hlsli including <Rtxdi/Utils/Math.hlsli> is exactly this shape.
    const rhi::ShaderHandle nested = compile(res,
        "#include \"AverIncProbe/Outer.hlsli\"\n"
        "RWStructuredBuffer<float> gOut : register(u0);\n"
        "[numthreads(1,1,1)] void main() { gOut[0] = averIncProbeOuter(1.0); }\n");
    check(nested != 0, "an ANGLED include INSIDE an included file resolves (the RTXDI shape)");

    for (rhi::ShaderHandle h : {quoted, angled, nested}) if (h) res->destroyShader(h);
    rhi::destroyDevice(dev);

    if (g_failures != 0) {
        AVER_ERROR("=== ShaderIncludeResolveTest FAILED === ({} failure(s))", g_failures);
        return 1;
    }
    AVER_INFO("=== ShaderIncludeResolveTest passed ===");
    return 0;
}
