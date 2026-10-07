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
#include "aver/core/Log.hpp"
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

// The RHI reports "<n> shader request(s): <h> served from the blob cache, <c> compiled in <t> ms" at a
// power-of-two cadence; forwarded as progress.
void sink(void*, LogLevel, std::string_view msg) {
    const usize at = msg.find(" shader request(s): ");
    if (at == std::string_view::npos) return;
    usize b = at;
    while (b > 0 && msg[b - 1] >= '0' && msg[b - 1] <= '9') --b;
    const std::string requests(msg.substr(b, at - b));
    const usize c = msg.find("cache, ", at);
    const usize e = c == std::string_view::npos ? c : msg.find(" compiled", c);
    if (c == std::string_view::npos || e == std::string_view::npos) return;
    say("warm: shaders " + requests + " " + std::string(msg.substr(c + 7, e - (c + 7))));
}

}  // namespace

int runShaderWarm(const std::string& projectPath) {
    const auto t0 = std::chrono::steady_clock::now();
    say("warm: start");
    setLogSink(&sink, nullptr);

    rhi::DeviceDesc desc;
    desc.preferred[0] = rhi::Backend::D3D12;
    desc.preferredCount = 1;
    rhi::IDevice* dev = rhi::createDevice(desc);
    if (!dev || dev->backend() != rhi::Backend::D3D12 || !dev->resources()) {
        say("warm: fail no D3D12 device");
        if (dev) rhi::destroyDevice(dev);
        return 77;
    }
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
    setLogSink(nullptr, nullptr);
    rhi::destroyDevice(dev);
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (rc == 0) say("warm: done " + std::to_string(static_cast<long long>(ms)));
    return rc;
}

}  // namespace aver
