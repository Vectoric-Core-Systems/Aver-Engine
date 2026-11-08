// The particle editor's track C logic (docs/plans/EDITOR_UE_PARITY.md): stats, playback and seek, curve and gradient key edits,
// module isolate, the moving-emitter path, templates, live parameters and scalability. Headless: the tab is built without ImGui.
#include "ParticleEditor.hpp"
#include "ParticleStackModel.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/OcParticle.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

using namespace aver;
namespace px = aver::editor::pxmodel;
using particles::ParticleEffect;
using particles::ParticleSystemDef;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// A scratch .ocparticle on disk for the tab to load.
static std::string writeFixture(const ParticleSystemDef& d, const char* name) {
    const std::filesystem::path p = std::filesystem::temp_directory_path() / name;
    std::string why;
    if (!fmt::saveOcparticleSystem(p.string(), d, &why)) AVER_ERROR("fixture write failed: {}", why);
    return p.string();
}

static ParticleSystemDef smallSystem() {
    ParticleSystemDef d;
    ParticleEffect e;
    e.name = "Main";
    e.emissionRate = 60.0f;
    e.maxParticles = 400;
    e.lifetimeMin = e.lifetimeMax = 1.0f;
    e.spreadDeg = 40.0f;
    e.speedMin = 50.0f; e.speedMax = 150.0f;
    d.emitters.push_back(e);
    return d;
}

int main() {
    // ---- 1. stats ----------------------------------------------------------------------------------------
    AVER_INFO("stats");
    {
        ParticleEffect e;
        e.maxParticles = 512;
        check(px::emitterCapacity(e) == 512, "capacity is maxParticles");
        check(!px::emitterOnGpu(e) && px::gpuMemoryEstimate(e) == 0, "Auto below 2048 is the CPU tier with no GPU memory");
        e.maxParticles = 4096;
        check(px::emitterOnGpu(e), "Auto at 2048 or more is the GPU tier");
        e.simTier = particles::SimTier::Cpu;
        check(!px::emitterOnGpu(e), "a CPU request wins over size");
        e.simTier = particles::SimTier::Gpu;
        e.maxParticles = 100;
        check(px::emitterOnGpu(e), "a GPU request wins over size");
        e.simTier = particles::SimTier::Auto;
        e.maxParticles = 8192;
        e.blend = rhi::BlendMode::Additive;
        const u64 additive = px::gpuMemoryEstimate(e);
        e.blend = rhi::BlendMode::PremultipliedAlpha;
        const u64 sorted = px::gpuMemoryEstimate(e);
        check(additive == 8192ull * 80ull, "additive: 80 bytes per particle (64 + dead 4 + alive 12)");
        check(sorted == additive + 8192ull * 8ull, "a sorted emitter adds 8 bytes of sort keys per particle");
        e.render = particles::RenderKind::Ribbon;
        e.linked = true;
        e.linkedCapacity = 4096;
        check(px::emitterCapacity(e) == 4096, "a linked ribbon's capacity is its ring");
        check(px::formatBytes(1536) == "1.5 KB" && px::formatBytes(3u << 20) == "3.0 MB", "byte formatting");
    }

    AVER_INFO("{} failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
