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

    // ---- 2. playback ---------------------------------------------------------------------------------------
    AVER_INFO("playback");
    {
        const std::string path = writeFixture(smallSystem(), "pxTrackC_playback.ocparticle");
        editor::ParticleEditor a(path), b(path);
        check(a.loaded() && b.loaded(), "the fixture loads");
        a.seekPreview(2.0f);
        b.seekPreview(0.7f);
        b.seekPreview(2.0f);   // from a different time: still resimulates from 0
        check(std::fabs(a.previewTime() - 2.0f) < 1e-3f && std::fabs(b.previewTime() - 2.0f) < 1e-3f, "seek lands on the asked time");
        const auto& pa = a.previewParticles();
        const auto& pb = b.previewParticles();
        bool same = pa.size() == pb.size() && !pa.empty();
        for (usize i = 0; same && i < pa.size(); ++i)
            same = pa[i].position.x == pb[i].position.x && pa[i].position.y == pb[i].position.y && pa[i].position.z == pb[i].position.z &&
                   pa[i].age == pb[i].age && pa[i].id == pb[i].id;
        check(same, "seeking the same time gives bit-identical particles, whatever came before");
        check(a.previewTotal() > 30, "a 60/s emitter has particles at 2 s: " + std::to_string(a.previewTotal()));

        editor::ParticleEditor c(path);
        for (int i = 0; i < 120; ++i) c.stepPreviewFrame();
        const auto& pc = c.previewParticles();
        bool stepSame = pc.size() == pa.size();
        for (usize i = 0; stepSame && i < pc.size(); ++i) stepSame = pc[i].position.x == pa[i].position.x && pc[i].id == pa[i].id;
        check(stepSame, "120 frame steps equal a seek to 2 s");
        check(!c.previewPlaying(), "frame step pauses");
        check(c.previewFrame() == 120, "the frame readout counts steps");

        editor::ParticleEditor d(path);
        d.setPlaybackSpeed(2.0f);
        d.advancePreview(1.0f);
        check(std::fabs(d.previewTime() - 2.0f) < 0.05f, "speed 2 advances two simulated seconds per second: " + std::to_string(d.previewTime()));
        d.setPlaybackSpeed(100.0f);
        check(d.playbackSpeed() == 4.0f, "speed is clamped");
        d.setPlaybackSpeed(1.0f);
        d.restartPreview();
        d.setLoop(true);
        d.setLoopLength(1.0f);
        d.advancePreview(0.9f);
        d.advancePreview(0.5f);
        check(d.previewTime() < 1.0f && d.previewTime() > 0.2f, "a looping preview wraps at its length: " + std::to_string(d.previewTime()));
        d.seekPreview(50.0f);
        check(d.previewTime() <= 1.0f + 1e-3f, "a looping seek stops at the loop length");
        d.setLoop(false);
        d.seekPreview(-5.0f);
        check(d.previewTime() == 0.0f, "a negative seek is time 0");
    }

    AVER_INFO("{} failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
