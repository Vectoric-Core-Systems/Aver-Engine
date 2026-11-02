// ParticleRegressionTest -- the shipped .ocparticle files simulate on the CPU executor as they did before wave 6 added emission shapes and modules.
// Each file is loaded, every emitter spawned and stepped for a fixed number of ticks, and the sums of position, velocity and age compared with values captured
// from the build before the change (a loose relative tolerance: libm differs between compilers, the arithmetic does not). Defaults stay unchanged.
// AVER_PARTICLE_GOLDEN_PRINT=1 prints the table instead of checking it.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcParticle.hpp"
#include "aver/particles/EmitterSim.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace aver;

namespace {

struct Golden {
    const char* file;
    u32 emitter;
    u32 alive;
    f64 pos[3];
    f64 vel[3];
    f64 age;
};

// Captured from the build before the change (origin/08-gdint 78ba3a77).
const Golden kGolden[] = {
    {"", 0, 0, {0, 0, 0}, {0, 0, 0}, 0},   // row 0 is a placeholder so the table is never empty
    {"templates/Vehicle/Content/Particles/Sparks.ocparticle", 0, 301, {2005.4566, 7113.9220, 14354.8690}, {-2244.7139, 3752.1276, -5812.8130}, 103.7000},
    {"templates/Vehicle/Content/Particles/TyreSmoke.ocparticle", 0, 602, {5941.8868, 11968.0675, 34410.1985}, {-192.2220, 236.7375, 26779.5681}, 391.5499},
    {"test-content/ParticleDemo/Content/Particles/BeamRibbon.ocparticle", 0, 64, {2413.9179, 1278.1326, 1907.2813}, {19486.9038, -55.3460, -154.8466}, 5.8500},
    {"test-content/ParticleDemo/Content/Particles/Debris.ocparticle", 0, 750, {7585.4429, 12826.7245, 23411.0254}, {-1399.2371, 3679.6477, -9709.7562}, 581.2499},
    {"test-content/ParticleDemo/Content/Particles/Embers.ocparticle", 0, 750, {12295.4835, 13928.1288, 33741.9968}, {1820.6085, -655.1614, 10823.2208}, 581.2499},
    {"test-content/ParticleDemo/Content/Particles/FireSmoke.ocparticle", 0, 463, {4118.5292, 9056.4665, 38821.5757}, {-458.4417, -988.2304, 51517.8386}, 231.1500},
    {"test-content/ParticleDemo/Content/Particles/FireSmoke.ocparticle", 1, 750, {-8020.3425, 25005.7525, 90443.8507}, {-39713.8694, 31514.8016, 117380.6109}, 581.2499},
    {"test-content/ParticleDemo/Content/Particles/FireSmoke.ocparticle", 2, 575, {5471.1743, 10107.9407, 815.1981}, {-2180.4524, -1452.3450, -103802.1436}, 357.2500},
    {"test-content/ParticleDemo/Content/Particles/Mist.ocparticle", 0, 60, {220.1182, 425.5246, 2126.0665}, {-0.2643, 3.6371, 210.3630}, 87.7500},
    {"test-content/ParticleDemo/Content/Particles/Rain100k.ocparticle", 0, 750, {38903.9353, 46805.3721, -514058.3532}, {-222.1460, -57.0186, -692502.4855}, 581.2499},
    {"test-content/ParticleDemo/Content/Particles/Snowfall.ocparticle", 0, 700, {12351.1927, 19872.8417, 4705.7876}, {-21.5553, -11.0243, -17760.4083}, 577.4999},
    {"test-content/ParticleDemo/Content/Particles/SparkTrails.ocparticle", 0, 732, {5529.6813, 10508.3518, 35083.5317}, {-5342.7189, 3925.4335, -5948.4421}, 555.5999},
    // GOLDEN-TABLE-END
};

int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

bool close(f64 got, f64 want, f64 scale) { return std::fabs(got - want) <= 2e-3 * std::max(scale, 1.0); }

const char* kFiles[] = {
    "templates/Vehicle/Content/Particles/Sparks.ocparticle",
    "templates/Vehicle/Content/Particles/TyreSmoke.ocparticle",
    "test-content/ParticleDemo/Content/Particles/BeamRibbon.ocparticle",
    "test-content/ParticleDemo/Content/Particles/Debris.ocparticle",
    "test-content/ParticleDemo/Content/Particles/Embers.ocparticle",
    "test-content/ParticleDemo/Content/Particles/FireSmoke.ocparticle",
    "test-content/ParticleDemo/Content/Particles/Mist.ocparticle",
    "test-content/ParticleDemo/Content/Particles/Rain100k.ocparticle",
    "test-content/ParticleDemo/Content/Particles/Snowfall.ocparticle",
    "test-content/ParticleDemo/Content/Particles/SparkTrails.ocparticle",
};

} // namespace

int main() {
    AVER_INFO("=== ParticleRegressionTest ===");
    const bool print = std::getenv("AVER_PARTICLE_GOLDEN_PRINT") != nullptr;
    const std::string root = AVER_REPO_ROOT;
    usize row = 1;
    for (const char* rel : kFiles) {
        particles::ParticleSystemDef def;
        std::string err;
        const bool ok = fmt::loadOcparticleSystem(root + "/" + rel, def, nullptr, &err);
        check(ok, std::string("loads ") + rel + " " + err);
        if (!ok) continue;
        for (u32 ei = 0; ei < def.emitters.size(); ++ei) {
            particles::ParticleEffect fx = def.emitters[ei];
            fx.maxParticles = std::min<u32>(fx.maxParticles, 4000u);
            particles::EmitterSim sim;
            sim.init(fx, def.params, 1234u + ei);
            sim.setOrigin({10, 20, 30});
            particles::SpawnRequest req;
            req.xf = Mat4::translation({10, 20, 30});
            req.velocity = {30, 0, 0};
            req.count = 25;
            f32 time = 0;
            for (int tick = 0; tick < 90; ++tick) {
                if (tick % 3 == 0) sim.spawn(req);
                sim.step(1.0f / 60.0f, time, nullptr);
                time += 1.0f / 60.0f;
            }
            Golden g{rel, ei, 0, {0, 0, 0}, {0, 0, 0}, 0};
            f64 scale = 0;
            for (const particles::Particle& p : sim.particles()) {
                if (!particles::particleAlive(p)) continue;
                ++g.alive;
                g.pos[0] += p.position.x; g.pos[1] += p.position.y; g.pos[2] += p.position.z;
                g.vel[0] += p.velocity.x; g.vel[1] += p.velocity.y; g.vel[2] += p.velocity.z;
                g.age += p.age;
            }
            scale = std::fabs(g.pos[0]) + std::fabs(g.pos[1]) + std::fabs(g.pos[2]) + std::fabs(g.vel[0]) + std::fabs(g.vel[1]) + std::fabs(g.vel[2]);
            if (print) {
                std::printf("    {\"%s\", %u, %u, {%.4f, %.4f, %.4f}, {%.4f, %.4f, %.4f}, %.4f},\n", rel, ei, g.alive, g.pos[0], g.pos[1], g.pos[2], g.vel[0], g.vel[1], g.vel[2], g.age);
                continue;
            }
            if (row >= sizeof(kGolden) / sizeof(kGolden[0])) { check(false, "golden table is short"); continue; }
            const Golden& w = kGolden[row++];
            const std::string tag = std::string(rel) + " emitter " + std::to_string(ei);
            check(w.emitter == ei, tag + ": table order");
            check(g.alive == w.alive, tag + ": alive count " + std::to_string(g.alive) + " vs " + std::to_string(w.alive));
            for (int k = 0; k < 3; ++k) {
                check(close(g.pos[k], w.pos[k], scale), tag + ": position sum");
                check(close(g.vel[k], w.vel[k], scale), tag + ": velocity sum");
            }
            check(close(g.age, w.age, g.age), tag + ": age sum");
        }
    }
    AVER_INFO("{} failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
