// HairSelfTest -- the GPU compute host against Jolt's CPU backend, on a real device. HEADED (it needs a D3D12 or Vulkan device; it exits 77, "skipped",
// when there is none, as the other on-device suites do). docs/plans/wave11/SOFTBODY_HAIR.md section 6, check 11.
//
// WHAT IT DECIDES. Jolt's fifteen hair kernels and Aver's wind kernel are compiled by DXC from the vendored HLSL with explicit registers, recorded
// through the plain-C host table and replayed by RhiComputeHost. The same small groom is simulated twice, once on Jolt's CPU backend (the HLSL compiled
// as C++, the reference) and once through the table onto the GPU, for 120 steps of 1/60 s, with and without wind; the render positions must agree to
// within kTolerance centimetres. A binding on the wrong register, a stale uploaded constant, a missing barrier or a wrong structured-buffer stride each
// show up here as positions that diverge or explode, which no headless test can see.
//
// ARGUMENTS: `vulkan` selects the Vulkan backend (default D3D12); `hw` uses the adapter instead of WARP (D3D12 only); `debug` the validation layer.
// Exit code: 0 passed, 1 failed, 77 skipped. NOTHING IN HERE HAS BEEN RUN: kTolerance is the design's gate (2 mm), an UNVERIFIED starting value.
#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_hair_abi.h"
#include "aver/render/RhiComputeHost.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace aver;

namespace {

constexpr int kSkip = 77;
constexpr f32 kTolerance = 0.2f;     // centimetres, i.e. 2 mm
constexpr int kSteps = 120;
constexpr f32 kDt = 1.0f / 60.0f;

int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// A small groom: a 4 x 4 grid of strands growing up (+Z) from a flat 20 cm scalp, 9 points each, 1.5 cm apart. Every strand is simulated.
struct Groom {
    std::vector<f32> points;
    std::vector<i32> start, material;
    std::vector<f32> scalp;
    std::vector<i32> tris;
    i32 vertexCount = 0, strandCount = 0;
};

Groom makeGroom() {
    Groom g;
    constexpr int kGrid = 4, kPoints = 9;
    for (int gy = 0; gy < kGrid; ++gy)
        for (int gx = 0; gx < kGrid; ++gx) {
            g.start.push_back(static_cast<i32>(g.points.size() / 3));
            g.material.push_back(0);
            const f32 x = -6.0f + 4.0f * static_cast<f32>(gx);
            const f32 y = -6.0f + 4.0f * static_cast<f32>(gy);
            for (int k = 0; k < kPoints; ++k) {
                g.points.push_back(x);
                g.points.push_back(y);
                g.points.push_back(1.5f * static_cast<f32>(k));
            }
        }
    g.vertexCount = static_cast<i32>(g.points.size() / 3);
    g.strandCount = kGrid * kGrid;
    g.start.push_back(g.vertexCount);
    g.scalp = {-10, -10, 0, 10, -10, 0, 10, 10, 0, -10, 10, 0};
    g.tris = {0, 1, 2, 0, 2, 3};
    return g;
}

// Builds the groom's settings on whatever host is installed. Returns the handle, 0 on failure.
i32 buildSettings(const Groom& g) {
    const i32 b = aver_phys_hair_settings_begin();
    if (!b) return 0;
    if (!aver_phys_hair_settings_strands(b, g.points.data(), nullptr, g.vertexCount, g.start.data(), g.material.data(), g.strandCount)) return 0;
    if (!aver_phys_hair_settings_scalp(b, g.scalp.data(), 4, g.tris.data(), 2, nullptr, nullptr, 1)) return 0;
    f32 params[AVER_PHYS_HAIRP_COUNT];
    for (f32& p : params) p = std::numeric_limits<f32>::quiet_NaN();
    params[AVER_PHYS_HAIRP_SIM_FRACTION] = 1.0f;
    if (!aver_phys_hair_settings_params(b, params, AVER_PHYS_HAIRP_COUNT)) return 0;
    f32 root = 0.0f;
    return aver_phys_hair_settings_finish(b, 0, &root);
}

const f32 kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

// One step of input shared by both runs.
void feed(i32 hair, bool wind, int step) {
    aver_phys_hair_set_transform(hair, kIdentity);
    aver_phys_hair_set_palette(hair, kIdentity, 1);
    if (wind) {
        // A gust that builds and fades, so a wrong constant block shows up as a wrong curve and not just a wrong end point.
        const f32 s = 400.0f * std::sin(static_cast<f32>(step) * 0.05f);
        const f32 w[3] = {s, 100.0f, 0.0f};
        aver_phys_hair_set_wind(hair, w, 3.0f, 60.0f);
    }
}

// Jolt metres (X right, Y up, -Z forward) to engine centimetres.
void toEngine(const f32* jolt, usize vertices, std::vector<f32>& out) {
    out.resize(vertices * 3);
    for (usize i = 0; i < vertices; ++i) {
        out[i * 3 + 0] = -jolt[i * 3 + 2] * 100.0f;
        out[i * 3 + 1] = jolt[i * 3 + 0] * 100.0f;
        out[i * 3 + 2] = jolt[i * 3 + 1] * 100.0f;
    }
}

// The reference run: Jolt's CPU backend. Returns the render positions in engine centimetres.
bool runCpu(const Groom& g, bool wind, std::vector<f32>& out) {
    aver_phys_hair_set_compute_host(nullptr);
    const i32 settings = buildSettings(g);
    if (!settings) return false;
    const i32 hair = aver_phys_hair_create(settings, kIdentity, 0);
    if (!hair) { aver_phys_hair_settings_destroy(settings); return false; }
    for (int s = 0; s < kSteps; ++s) {
        feed(hair, wind, s);
        aver_phys_hair_update(hair, kDt);
        aver_phys_hair_flush();
    }
    out.assign(static_cast<usize>(g.vertexCount) * 3, 0.0f);
    const bool ok = aver_phys_hair_read_positions(hair, nullptr, 0, out.data(), g.vertexCount) != 0;
    aver_phys_hair_destroy(hair);
    aver_phys_hair_settings_destroy(settings);
    return ok;
}

// The GPU run: the same groom through the host table. Returns the render positions in engine centimetres.
bool runGpu(rhi::IDevice& dev, render::RhiComputeHost& host, const Groom& g, bool wind, std::vector<f32>& out, render::RhiComputeHostStats& stats) {
    if (!aver_phys_hair_set_compute_host(&host.table())) { AVER_ERROR("the physics layer refused the compute host"); return false; }
    const i32 settings = buildSettings(g);
    if (!settings) return false;
    const i32 hair = aver_phys_hair_create(settings, kIdentity, 0);
    if (!hair) { aver_phys_hair_settings_destroy(settings); aver_phys_hair_set_compute_host(nullptr); return false; }

    bool ok = true;
    for (int s = 0; s < kSteps && ok; ++s) {
        feed(hair, wind, s);
        ok = aver_phys_hair_update(hair, kDt) != 0;
        aver_phys_hair_flush();
        ok = dev.runStandaloneCompute([&](rhi::IRenderContext& ctx) {
                 host.beginFrame();
                 host.replay(ctx);
                 host.endFrame(ctx);
             }) && ok;
    }
    stats = host.stats();

    // Read the render positions back.
    rhi::BufferHandle rb = 0;
    u64 bytes = 0;
    const u64 handle = aver_phys_hair_render_buffer(hair);
    ok = ok && handle != 0 &&
         dev.runStandaloneCompute([&](rhi::IRenderContext& ctx) {
             host.beginFrame();
             host.replay(ctx);
             rb = host.recordReadback(ctx, handle, &bytes);
             host.endFrame(ctx);
         });
    std::vector<f32> jolt;
    if (ok && rb && bytes >= static_cast<u64>(g.vertexCount) * 12) {
        jolt.resize(static_cast<usize>(g.vertexCount) * 3);
        ok = dev.resources()->readBuffer(rb, jolt.data(), static_cast<u64>(g.vertexCount) * 12, 0);
        toEngine(jolt.data(), static_cast<usize>(g.vertexCount), out);
    } else {
        ok = false;
    }
    if (rb) { dev.resources()->waitIdle(); dev.resources()->destroyBuffer(rb); }

    aver_phys_hair_destroy(hair);
    aver_phys_hair_settings_destroy(settings);
    aver_phys_hair_set_compute_host(nullptr);
    return ok;
}

f32 maxDiff(const std::vector<f32>& a, const std::vector<f32>& b, usize* worst = nullptr) {
    f32 m = 0.0f;
    for (usize i = 0; i < a.size() && i < b.size(); ++i) {
        const f32 d = std::fabs(a[i] - b[i]);
        if (!std::isfinite(d)) { if (worst) *worst = i; return std::numeric_limits<f32>::infinity(); }
        if (d > m) { m = d; if (worst) *worst = i; }
    }
    return m;
}

} // namespace

int main(int argc, char** argv) {
    AVER_INFO("=== HairSelfTest ===");
    bool wantVulkan = false, hardware = false, debug = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "vulkan") == 0) wantVulkan = true;
        if (std::strcmp(argv[i], "hw") == 0) hardware = true;
        if (std::strcmp(argv[i], "debug") == 0) debug = true;
    }

    rhi::DeviceDesc desc;
    desc.useWarp = !hardware;
    desc.enableDebug = debug;
    desc.preferred[0] = wantVulkan ? rhi::Backend::Vulkan : rhi::Backend::D3D12;
    desc.preferred[1] = rhi::Backend::Null;
    desc.preferredCount = 2;
    rhi::IDevice* dev = rhi::createDevice(desc);
    if (!dev || dev->backend() == rhi::Backend::Null) {
        AVER_WARN("  SKIP  no {} device on this machine", wantVulkan ? "Vulkan" : "D3D12");
        if (dev) rhi::destroyDevice(dev);
        return kSkip;
    }
    if (!dev->resources() || !dev->caps().computeShaders) {
        AVER_WARN("  SKIP  this device has no resource factory or no compute shaders");
        rhi::destroyDevice(dev);
        return kSkip;
    }
    if (!dev->runStandaloneCompute([](rhi::IRenderContext&) {})) {
        AVER_WARN("  SKIP  IDevice::runStandaloneCompute is unavailable on this backend");
        rhi::destroyDevice(dev);
        return kSkip;
    }
    AVER_INFO("device: {} ({})", dev->adapterName(), rhi::backendName(dev->backend()));

    if (!aver_phys_init()) {
        AVER_ERROR("  FAIL  aver_phys_init");
        rhi::destroyDevice(dev);
        return 1;
    }

    {
        render::RhiComputeHost host;
        check(host.init(*dev), "the compute host initialises on this device");
        const Groom groom = makeGroom();
        for (const bool wind : {false, true}) {
            const std::string tag = wind ? "with wind" : "no wind";
            std::vector<f32> cpu, gpu;
            render::RhiComputeHostStats stats;
            check(runCpu(groom, wind, cpu), tag + ": the CPU reference runs");
            check(runGpu(*dev, host, groom, wind, gpu, stats), tag + ": the GPU run completes");
            if (cpu.empty() || gpu.empty()) continue;

            usize worst = 0;
            const f32 diff = maxDiff(cpu, gpu, &worst);
            check(std::isfinite(diff) && diff <= kTolerance,
                  tag + ": GPU and CPU render positions agree within " + std::to_string(kTolerance) + " cm after " + std::to_string(kSteps) +
                      " steps (largest difference " + std::to_string(diff) + " cm at component " + std::to_string(worst) + ")");

            // The simulation really moved the hair (a host that ran nothing would also "agree" with a reference that ran nothing).
            f32 moved = 0.0f;
            for (usize i = 0; i < cpu.size() && i < groom.points.size(); ++i) moved = std::max(moved, std::fabs(cpu[i] - groom.points[i]));
            check(moved > 0.05f, tag + ": the reference hair moved from its modelled pose (" + std::to_string(moved) + " cm)");
            bool finite = true;
            for (f32 v : gpu) finite = finite && std::isfinite(v);
            check(finite, tag + ": every GPU position is finite");
            check(stats.droppedBatches == 0, tag + ": no recorded batch was dropped");
            // Pipelines build on first dispatch. Of Jolt's 15 kernels this scene (no collider, no global pose, no head move) dispatches 12 (measured),
            // and the wind run adds Aver's wind kernel (the count is cumulative on one host).
            const u32 expectPipelines = wind ? 13 : 12;
            check(stats.pipelines >= expectPipelines, tag + ": the kernels this scene dispatches compiled (" + std::to_string(stats.pipelines) + " pipelines)");
        }
        host.shutdown();
    }

    aver_phys_shutdown();
    rhi::destroyDevice(dev);
    if (g_failures != 0) {
        AVER_ERROR("=== HairSelfTest FAILED === ({} failure(s))", g_failures);
        return 1;
    }
    AVER_INFO("=== HairSelfTest PASSED ===");
    return 0;
}
