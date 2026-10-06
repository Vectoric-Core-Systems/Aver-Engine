// VoxiShaderCompileTest -- compiles every staged ray-driven Voxi pipeline and every NRD2 pass on a
// real device, without a window or a scene, so a shader error shows up here instead of in the editor.
// HLSL compiles at runtime: no other test reaches voxi.hlsl's CSRd* entries or nrd2.hlsl.
// Arguments: `warp` uses WARP (default: the adapter; ray tracing usually needs it). Skips (77) when the
// device lacks ray tracing tier 1.1 / shader model 6.6, which the staged path requires.

#include "aver/core/Log.hpp"
#include "aver/render/denoise/Nrd2.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
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
                        msg.find(": error") != std::string_view::npos;
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
        }
        voxi.shutdown();
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
