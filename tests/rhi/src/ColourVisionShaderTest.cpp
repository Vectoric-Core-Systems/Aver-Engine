// The colour-vision filter's HLSL against its CPU twin, on a real device (WARP when asked for D3D12). A tiny compute
// kernel #includes aver_colour_vision.hlsli exactly as post.hlsl's PSComposite and ui.hlsl's UiPS do, runs a swatch
// board through every mode, simulation and correction, and two strengths, and the read-back must match
// rhi/ColourVision.hpp within 2/255. Also builds the real post composite and UI pixel shaders with their defines, so a
// broken include or a drifted constant fails here and not in the editor. Catches include resolution and pow() drift
// between the two backends.
//
// Arguments: `vulkan` selects the Vulkan backend (default D3D12); `hw` uses the adapter instead of WARP (D3D12 only).
// Exits 0, 1, or 77 (ctest SKIPPED) when the backend is absent or cannot run standalone compute.
#include "aver/core/Log.hpp"
#include "aver/rhi/ColourVision.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace aver;

namespace {

int g_failures = 0;
constexpr int kSkip = 77;

void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok  {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

const char* kKernel = R"HLSL(
#include "aver_colour_vision.hlsli"
cbuffer CvConstants : register(b3) { float4 gCv; };
StructuredBuffer<float4> gIn : register(t0);
RWStructuredBuffer<float4> gOut : register(u0);
[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    float3 c = gIn[id.x].rgb;
    gOut[id.x] = float4(averColourVision(c, gCv.x), 1.0);
}
)HLSL";

rhi::BufferHandle makeBuffer(rhi::IResourceFactory& res, u64 bytes, rhi::BufferKind kind, bool uav, const char* name) {
    rhi::BufferDesc bd{};
    bd.bytes = bytes;
    bd.kind = kind;
    bd.allowUnorderedAccess = uav;
    bd.debugName = name;
    return res.createBuffer(bd);
}

// The swatch board: primaries, secondaries, a skin-ish tone, and a grey ramp.
std::vector<f32> swatches() {
    const f32 s[][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {0, 1, 1}, {1, 0, 1}, {0.8f, 0.55f, 0.45f},
                        {0.2f, 0.4f, 0.6f}, {0, 0, 0}, {0.25f, 0.25f, 0.25f}, {0.5f, 0.5f, 0.5f}, {0.75f, 0.75f, 0.75f}, {1, 1, 1}};
    std::vector<f32> out;
    for (const auto& c : s) { out.push_back(c[0]); out.push_back(c[1]); out.push_back(c[2]); out.push_back(1.0f); }
    return out;
}

void run(rhi::IDevice& dev) {
    using rhi::ResourceState;
    rhi::IResourceFactory& res = *dev.resources();

    // The real shaders first: a broken include or a stray identifier shows here with DXC's message in the log.
    {
        rhi::ShaderDesc sd{};
        sd.source = rhi::postShaderSource();
        sd.entry = "PSComposite";
        sd.stage = rhi::ShaderStage::Pixel;
        sd.defines = "AVER_POST_DEPTHFX=1;AVER_POST_LUT=1;AVER_POST_BLOOM=1;AVER_POST_AUTOEXPOSURE=1;";
        const rhi::ShaderHandle h = res.createShader(sd);
        check(h != 0, "post.hlsl PSComposite builds with the colour-vision include");
        if (h) res.destroyShader(h);

        const std::string& ui = rhi::shaderFile("ui.hlsl");
        check(!ui.empty() && ui.find("aver_colour_vision.hlsli") != std::string::npos, "ui.hlsl names the include");
        rhi::ShaderDesc us{};
        us.source = ui.c_str();
        us.entry = "UiPS";
        us.stage = rhi::ShaderStage::Pixel;
        const rhi::ShaderHandle u = res.createShader(us);
        check(u != 0, "ui.hlsl UiPS builds with the colour-vision include");
        if (u) res.destroyShader(u);
    }

    rhi::ShaderDesc sd{};
    sd.source = kKernel;
    sd.entry = "CSMain";
    sd.stage = rhi::ShaderStage::Compute;
    sd.minShaderModel = 60;
    const rhi::ShaderHandle cs = res.createShader(sd);
    if (!cs) { check(false, "the swatch kernel builds (aver_colour_vision.hlsli resolves through the include handler)"); return; }
    rhi::ComputePipelineDesc pd{};
    pd.cs = cs;
    pd.layout.srvCount = 1;
    pd.layout.uavCount = 1;
    pd.layout.constantDwords[3] = 0;   // root CBV at b3
    pd.layout.slotKindsDeclared = true;
    pd.layout.srvKinds[0] = rhi::SlotKind::StructuredBuffer;
    pd.layout.uavKinds[0] = rhi::SlotKind::StructuredBuffer;
    const rhi::PipelineHandle pipe = res.createComputePipeline(pd);
    res.destroyShader(cs);
    check(pipe != 0, "the swatch pipeline builds");
    if (!pipe) return;

    const std::vector<f32> in = swatches();
    const u32 count = static_cast<u32>(in.size() / 4);
    const u64 bytes = static_cast<u64>(in.size()) * sizeof(f32);
    const rhi::BufferHandle up = makeBuffer(res, bytes, rhi::BufferKind::Upload, false, "CV upload");
    const rhi::BufferHandle inBuf = makeBuffer(res, bytes, rhi::BufferKind::Default, false, "CV in");
    const rhi::BufferHandle outBuf = makeBuffer(res, bytes, rhi::BufferKind::Default, true, "CV out");
    const rhi::BufferHandle back = makeBuffer(res, bytes, rhi::BufferKind::Readback, false, "CV readback");
    rhi::BindingSetDesc bd{};
    bd.srvCount = 1;
    bd.uavCount = 1;
    bd.srvKinds[0] = rhi::SlotKind::StructuredBuffer;
    bd.uavKinds[0] = rhi::SlotKind::StructuredBuffer;
    const rhi::BindingSetHandle set = res.createBindingSet(bd);
    if (!up || !inBuf || !outBuf || !back || !set || !res.writeBuffer(up, in.data(), bytes)) {
        check(false, "buffers and the binding set allocate");
        return;
    }
    res.setSrvBuffer(set, 0, inBuf, 16, count, 0);
    res.setUavBuffer(set, 0, outBuf, 16, count, 0);

    const bool uploaded = dev.runStandaloneCompute([&](rhi::IRenderContext& ctx) {
        ctx.bufferBarrier(inBuf, ResourceState::Common, ResourceState::CopyDest);
        ctx.copyBuffer(inBuf, up, bytes);
        ctx.bufferBarrier(inBuf, ResourceState::CopyDest, ResourceState::Common);
    });
    check(uploaded, "the swatch board uploads");

    const auto filter = [&](f32 packed, std::vector<f32>& out) {
        struct { f32 v[4]; } cb = {{packed, 0, 0, 0}};
        const bool ran = dev.runStandaloneCompute([&](rhi::IRenderContext& ctx) {
            ctx.bufferBarrier(inBuf, ResourceState::Common, ResourceState::NonPixelShaderResource);
            ctx.bufferBarrier(outBuf, ResourceState::Common, ResourceState::UnorderedAccess);
            ctx.setPipeline(pipe);
            ctx.setBindingSet(set);
            ctx.setConstantBuffer(3, &cb, sizeof cb);
            ctx.dispatch(count, 1, 1);
            ctx.bufferBarrier(outBuf, ResourceState::UnorderedAccess, ResourceState::Common);
            ctx.bufferBarrier(inBuf, ResourceState::NonPixelShaderResource, ResourceState::Common);
            ctx.bufferBarrier(outBuf, ResourceState::Common, ResourceState::CopySource);
            ctx.copyBuffer(back, outBuf, bytes);
            ctx.bufferBarrier(outBuf, ResourceState::CopySource, ResourceState::Common);
        });
        out.assign(in.size(), -1.0f);
        return ran && res.readBuffer(back, out.data(), bytes, 0);
    };

    const struct { u32 mode; bool sim; f32 strength; } cases[] = {
        {1, false, 1.0f}, {1, true, 1.0f}, {2, false, 1.0f}, {2, true, 1.0f}, {3, false, 1.0f}, {3, true, 1.0f},
        {1, false, 0.5f}, {2, true, 0.5f}, {3, false, 0.0f},
    };
    const char* names[] = {"", "protanopia", "deuteranopia", "tritanopia"};
    for (const auto& c : cases) {
        const f32 packed = rhi::packColourVision(c.mode, c.sim, c.strength);
        std::vector<f32> gpu;
        if (!filter(packed, gpu)) { check(false, "the filter kernel ran"); continue; }
        f32 worst = 0.0f;
        for (u32 i = 0; i < count; ++i) {
            f32 want[3];
            rhi::applyColourVision(packed, &in[static_cast<usize>(i) * 4], want);
            for (int k = 0; k < 3; ++k) worst = std::fmax(worst, std::fabs(gpu[static_cast<usize>(i) * 4 + static_cast<usize>(k)] - want[k]));
        }
        char what[160];
        std::snprintf(what, sizeof what, "%s %s at strength %.2f matches the CPU twin (max error %.4f, limit 2/255)", names[c.mode],
                      c.sim ? "simulation" : "correction", static_cast<double>(c.strength), static_cast<double>(worst));
        check(worst <= 2.0f / 255.0f, what);
    }

    // Grey stays grey in every mode at full strength.
    for (u32 mode = 1; mode <= 3; ++mode) {
        std::vector<f32> gpu;
        if (!filter(rhi::packColourVision(mode, true, 1.0f), gpu)) continue;
        bool grey = true;
        for (u32 i = 9; i < 13; ++i) {   // the grey ramp
            const f32 r = gpu[i * 4], g = gpu[i * 4 + 1], b = gpu[i * 4 + 2];
            if (std::fabs(r - g) > 0.01f || std::fabs(g - b) > 0.01f) grey = false;
        }
        check(grey, std::string(names[mode]) + " keeps the grey ramp grey on the GPU");
    }

    res.waitIdle();
    res.destroyBindingSet(set);
    for (rhi::BufferHandle b : {up, inBuf, outBuf, back}) res.destroyBuffer(b);
    res.destroyPipeline(pipe);
}

} // namespace

int main(int argc, char** argv) {
    AVER_INFO("=== ColourVisionShaderTest ===");
    bool wantVulkan = false, hardware = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "vulkan") == 0) wantVulkan = true;
        if (std::strcmp(argv[i], "hw") == 0) hardware = true;
    }

    rhi::DeviceDesc desc;
    desc.useWarp = !hardware;
    desc.preferred[0] = wantVulkan ? rhi::Backend::Vulkan : rhi::Backend::D3D12;
    desc.preferred[1] = rhi::Backend::Null;
    desc.preferredCount = 2;   // no fallback to the other backend: an absent one must report SKIPPED
    rhi::IDevice* dev = rhi::createDevice(desc);
    if (!dev || dev->backend() == rhi::Backend::Null) {
        AVER_WARN("  SKIP  no {} device on this machine", wantVulkan ? "Vulkan" : "D3D12");
        if (dev) rhi::destroyDevice(dev);
        return kSkip;
    }
    if (!dev->resources() || !dev->runStandaloneCompute([](rhi::IRenderContext&) {})) {
        AVER_WARN("  SKIP  this backend cannot run standalone compute");
        rhi::destroyDevice(dev);
        return kSkip;
    }
    AVER_INFO("device: {} ({})", dev->adapterName(), rhi::backendName(dev->backend()));
    run(*dev);
    rhi::destroyDevice(dev);

    if (g_failures != 0) {
        AVER_ERROR("=== ColourVisionShaderTest FAILED === ({} failure(s))", g_failures);
        return 1;
    }
    AVER_INFO("=== ColourVisionShaderTest PASSED ===");
    return 0;
}
