// Sandbox.exe --warm-shaders [--project <manifest>]: fills the shader caches in the background so the editor's
// first switch to a renderer mode (ReSTIR RT, ReSTIR PT, NRD2, NeuRaC) loads instead of compiling.
//
// The editor starts a second copy of ITSELF in this mode at idle priority after a project opens
// (editor::ShaderWarmup). Itself, not a separate tool: the GPU driver keeps its compiled-pipeline cache per
// executable, so only Sandbox.exe warming fills the cache the editor reads (a separate tool took 144 s to
// fill its own and helped the editor only with DXIL). It creates its own windowless D3D12 device, applies the project's render settings the way the editor does (cache keys
// include every define, and some defines follow the project), then builds every Voxi variant and NRD2. The
// GI volume is forced small: it is not a shader input, and the editor already holds the real one in VRAM.
// Cache writes are atomic (D3D12Device.cpp), so both processes can fill the same cache.
//
// stdout, one line each, for the editor to read: "warm: start", "warm: shaders <requests> <compiled>",
// "warm: done <ms>" or "warm: fail <reason>". Returns 0 done, 1 failed, 77 no capable device.
#include "aver/formats/OcProject.hpp"
#include "aver/render/denoise/Nrd2.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/voxi/ProjectRenderApply.hpp"
#include "aver/voxi/VoxiRenderer.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace aver {

namespace {

void say(const std::string& line) {
    std::fputs(line.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// Every shader request, forwarded as "warm: shaders <requests> <compiled>" at most every 200 ms (and the last one
// at the end, with "warm: done").
std::chrono::steady_clock::time_point g_lastSay{};
u32 g_requests = 0;
void onShaderRequest(u32 requests, u32 cacheHits, void*) {
    g_requests = requests;
    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastSay < std::chrono::milliseconds(200)) return;
    g_lastSay = now;
    say("warm: shaders " + std::to_string(requests) + " " + std::to_string(requests - cacheHits));
}

}  // namespace

int runShaderWarm(const std::string& projectPath) {
    const auto t0 = std::chrono::steady_clock::now();
    say("warm: start");

    rhi::DeviceDesc desc;
    desc.preferred[0] = rhi::Backend::D3D12;
    desc.preferredCount = 1;
    rhi::IDevice* dev = rhi::createDevice(desc);
    if (!dev || dev->backend() != rhi::Backend::D3D12 || !dev->resources()) {
        say("warm: fail no D3D12 device");
        if (dev) rhi::destroyDevice(dev);
        return 77;
    }
    dev->setShaderRequestObserver(&onShaderRequest, nullptr);
    const rhi::DeviceCaps caps = dev->caps();
    if (caps.rayTracingTier < 11 || caps.shaderModel < 66 || !caps.dxcAvailable) {
        say("warm: fail no ray tracing 1.1 / SM 6.6 / DXC");
        rhi::destroyDevice(dev);
        return 77;
    }

    int rc = 0;
    {
        voxi::VoxiRenderer voxi;
        if (!voxi.init(*dev)) {
            say("warm: fail VoxiRenderer::init");
            rc = 1;
        } else {
            fmt::ProjectDesc project;
            if (!projectPath.empty() && fmt::loadOcproject(projectPath, project) && project.hasRenderSettings()) {
                // The renderer keeps no readable copy of what it was given; the two phases commit this one.
                voxi::Settings x{};
                voxi::applyManifestTwoPhase(project, x, [&]() {
                    x.voxelResolution = 64;   // not a shader input; keep this process small
                    voxi.setSettings(x);
                });
            }
            voxi.onRenderTargetsChanged(1, rhi::Format::RGBA16F, rhi::Format::D32Float, 1280, 720);
            voxi.buildAllVariants();
        }
        voxi.shutdown();
    }
    {
        render::denoise::Nrd2 nrd2;
        if (nrd2.create(*dev)) nrd2.destroy();
    }
    dev->setShaderRequestObserver(nullptr, nullptr);
    rhi::destroyDevice(dev);
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    // "warm: done <ms> <requests>": the editor keeps the request count as the next run's total.
    if (rc == 0) say("warm: done " + std::to_string(static_cast<long long>(ms)) + " " + std::to_string(g_requests));
    return rc;
}

}  // namespace aver
