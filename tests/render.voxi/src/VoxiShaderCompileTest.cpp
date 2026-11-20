// VoxiShaderCompileTest -- compiles every staged ray-driven Voxi pipeline and every NRD2 pass on a
// real device, without a window or a scene, so a shader error shows up here instead of in the editor.
// HLSL compiles at runtime: no other test reaches voxi.hlsl's CSRd* entries or nrd2.hlsl. That includes the
// NeuRaC twins (buildAllVariants builds them), the cache's resolve (a standalone NeuRaC) and the guide pass
// (a standalone NeuRaCGuide and NeuRaCNet). Then once more with an embedded material shader registered (a converted Godot shader's shape):
// the registry reaches every Voxi entry point, the volume clear/resolve/mip compute passes included, which bind no material.
// Arguments: `warp` uses WARP (default: the adapter; ray tracing usually needs it). Skips (77) when the
// device lacks ray tracing tier 1.1 / shader model 6.6, which the staged path requires.

#include "aver/core/Log.hpp"
#include "aver/formats/OcGraphHlsl.hpp"
#include "aver/pbr/MaterialGraphRegistry.hpp"
#include "aver/render/denoise/Nrd2.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/voxi/NeuRaC.hpp"
#include "aver/voxi/NeuRaCGuide.hpp"
#include "aver/voxi/NeuRaCNet.hpp"
#include "aver/voxi/VoxiRenderer.hpp"

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace aver;

namespace {
constexpr int kSkip = 77;

struct Errors { std::vector<std::string> lines; };

void sink(void* ctx, LogLevel level, std::string_view msg) {
    if (level < LogLevel::Warn) return;
    const bool shader = msg.find("compile") != std::string_view::npos || msg.find("not built") != std::string_view::npos ||
                        msg.find("would not build") != std::string_view::npos || msg.find("error X") != std::string_view::npos ||
                        msg.find(": error") != std::string_view::npos || msg.find("did not build") != std::string_view::npos ||
                        msg.find("NeuRaC") != std::string_view::npos;
    if (shader) static_cast<Errors*>(ctx)->lines.emplace_back(msg);
}
}  // namespace

int main(int argc, char** argv) {
    AVER_INFO("=== VoxiShaderCompileTest ===");
    bool warp = false;
    for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], "warp") == 0) warp = true;

    rhi::DeviceDesc desc;
    desc.useWarp = warp;
    desc.preferred[0] = rhi::Backend::D3D12;
    desc.preferred[1] = rhi::Backend::Null;
    desc.preferredCount = 2;
    rhi::IDevice* dev = rhi::createDevice(desc);
    if (!dev || dev->backend() == rhi::Backend::Null || !dev->resources()) {
        AVER_WARN("  SKIP  no D3D12 device");
        if (dev) rhi::destroyDevice(dev);
        return kSkip;
    }
    const rhi::DeviceCaps caps = dev->caps();
    if (caps.rayTracingTier < 11 || caps.shaderModel < 66 || !caps.dxcAvailable) {
        AVER_WARN("  SKIP  {} lacks ray tracing 1.1 / SM 6.6 / DXC", dev->adapterName());
        rhi::destroyDevice(dev);
        return kSkip;
    }
    AVER_INFO("device: {}", dev->adapterName());

    Errors errors;
    setLogSink(&sink, &errors);
    int failures = 0;
    {
        voxi::VoxiRenderer voxi;
        if (!voxi.init(*dev)) { AVER_ERROR("  FAIL  VoxiRenderer::init"); ++failures; }
        else {
            voxi.onRenderTargetsChanged(1, rhi::Format::RGBA16F, rhi::Format::D32Float, 1280, 720);
            voxi.buildAllVariants();
            // MANY LIGHTS (docs/plans/wave4/MANY_LIGHTS.md): the tail pass is authored SM 6.5-clean and compiled at 6.5.
            if (!voxi.manyLightsCompiled()) { AVER_ERROR("  FAIL  CSRdManyLights did not build (cs_6_5)"); ++failures; }
        }
        voxi.shutdown();
    }
    {
        // An embedded shader in the registry: its helpers name the material textures, which a compute pass without a material does not declare.
        // The volume clear, resolve and mip passes failed to compile and the GI volume never built in such a project.
        fmt::EmbeddedHlslPart part;
        part.name = "compile_test";
        part.ns = "ge0000c0de_";
        part.regsU = 1;
        part.hasFragment = true;
        part.fragmentSource = "void EMBN(main_fragment)() { AVER_CUSTOM_ALBEDO = float3(1.0, 0.0, 0.0); }\n";
        fmt::OcGraphData g;
        fmt::packEmbeddedHlsl(part, g);
        pbr::materialGraphs().clear();
        if (pbr::materialGraphs().add("$test/embedded", "VoxiShaderCompileTest embedded", g) == 0 || !pbr::materialGraphs().hasEmbeddedShaders()) {
            AVER_ERROR("  FAIL  the embedded test shader did not register");
            ++failures;
        } else {
            voxi::VoxiRenderer voxi;
            if (!voxi.init(*dev)) { AVER_ERROR("  FAIL  VoxiRenderer::init with an embedded shader registered"); ++failures; }
            voxi.shutdown();
        }
        pbr::materialGraphs().clear();
    }
    {
        // The cache's own pipelines, outside the renderer: the resolve shader and its buffers.
        voxi::NeuRaC cache;
        if (!cache.create(*dev->resources()) || !cache.valid()) { AVER_ERROR("  FAIL  standalone radiance cache (create)"); ++failures; }
        cache.destroy();
    }
    {
        voxi::NeuRaCGuide guide;
        if (!guide.create(*dev) || !guide.valid()) { AVER_ERROR("  FAIL  standalone guide pass (create)"); ++failures; }
        guide.destroy();
    }
    {
        voxi::NeuRaCNet net;
        if (!net.create(*dev) || !net.valid()) { AVER_ERROR("  FAIL  standalone trust network (create)"); ++failures; }
        net.destroy();
    }
    {
        render::denoise::Nrd2 nrd2;
        if (!nrd2.create(*dev) || !nrd2.valid()) { AVER_ERROR("  FAIL  Nrd2::create"); ++failures; }
        nrd2.destroy();
    }
    setLogSink(nullptr, nullptr);

    for (const std::string& l : errors.lines) AVER_ERROR("  FAIL  {}", l);
    failures += static_cast<int>(errors.lines.size());
    rhi::destroyDevice(dev);
    if (failures) {
        AVER_ERROR("=== VoxiShaderCompileTest FAILED === ({} failure(s))", failures);
        return 1;
    }
    AVER_INFO("=== VoxiShaderCompileTest PASSED ===");
    return 0;
}
