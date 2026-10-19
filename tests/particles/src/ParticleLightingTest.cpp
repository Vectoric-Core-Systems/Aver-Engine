// selectParticleLights: which scene lights a lit emitter takes, and the unit it converts candela with.
// No GPU, no scene world. Exit code = failure count.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/particles/ParticleLighting.hpp"
#include "aver/rhi/RHI.hpp"

#include <cmath>
#include <string>

using namespace aver;
using namespace aver::particles;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}
static void checkNear(f32 got, f32 want, f32 eps, const std::string& what) {
    ++g_checks;
    if (std::fabs(got - want) <= eps) return;
    AVER_ERROR("   FAIL  {} (got {:.6f}, want {:.6f})", what, got, want);
    ++g_failures;
}

static scene::WorldLight light(f32 x, f32 y, f32 z, f32 candela, i32 kind = scene::kLightPoint, f32 range = 0.0f) {
    scene::WorldLight l;
    l.kind = kind;
    l.pos[0] = x; l.pos[1] = y; l.pos[2] = z;
    l.intensityCd = candela;
    l.rangeCm = range;
    return l;
}

int main() {
    AVER_INFO("ParticleLightingTest");

    checkNear(particleLightE1m(rhi::kLuminanceToCdm2), 1.0f, 1e-6f, "one engine radiance unit of candela is E1m 1");
    checkNear(particleLightE1m(100000.0f), 100000.0f / rhi::kLuminanceToCdm2, 1e-6f, "the conversion is candela / rhi::kLuminanceToCdm2");

    ParticleLight out[kParticleLightCount];
    {
        std::vector<scene::WorldLight> none;
        check(selectParticleLights(none, {0, 0, 0}, 100.0f, out) == 0, "no lights: none selected");
        bool zero = true;
        for (const ParticleLight& l : out) zero = zero && l.radiance[0] == 0.0f && l.radiance[1] == 0.0f && l.radiance[2] == 0.0f && l.axisKind[3] == 0.0f;
        check(zero, "every unused slot has zero radiance and kind 0");
    }
    {
        // Equal brightness: the nearer one wins.
        std::vector<scene::WorldLight> ls = {light(1000, 0, 0, 50000.0f), light(300, 0, 0, 50000.0f), light(600, 0, 0, 50000.0f)};
        check(selectParticleLights(ls, {0, 0, 0}, 0.0f, out) == 3, "three lights: three selected");
        checkNear(out[0].posRange[0], 300.0f, 1e-4f, "nearest first");
        checkNear(out[1].posRange[0], 600.0f, 1e-4f, "then the next nearest");
        checkNear(out[2].posRange[0], 1000.0f, 1e-4f, "the farthest last");
        check(out[3].axisKind[3] == 0.0f && out[3].radiance[0] == 0.0f, "the fourth slot is empty");
    }
    {
        // A far bright light beats a near dim one.
        std::vector<scene::WorldLight> ls = {light(100, 0, 0, 1000.0f), light(400, 0, 0, 1000000.0f)};
        selectParticleLights(ls, {0, 0, 0}, 0.0f, out);
        checkNear(out[0].posRange[0], 400.0f, 1e-4f, "irradiance, not distance, ranks");
    }
    {
        // Directional lights are excluded; range excludes the out-of-reach.
        std::vector<scene::WorldLight> ls = {light(0, 0, 100, 1e6f, scene::kLightDirectional), light(5000, 0, 0, 1e6f, scene::kLightPoint, 100.0f),
                                             light(50, 0, 0, 1e5f, scene::kLightSpot)};
        check(selectParticleLights(ls, {0, 0, 0}, 0.0f, out) == 1, "directional and out-of-range lights are not selected");
        check(out[0].axisKind[3] == 2.0f, "a spot light reports kind 2");
        // The emitter bounds radius brings a light into range.
        check(selectParticleLights(ls, {0, 0, 0}, 5000.0f, out) == 2, "a bounds radius counts toward range");
    }
    {
        // Only the best four of many.
        std::vector<scene::WorldLight> ls;
        for (int i = 0; i < 10; ++i) ls.push_back(light(100.0f * static_cast<f32>(i + 1), 0, 0, 80000.0f));
        check(selectParticleLights(ls, {0, 0, 0}, 0.0f, out) == 4, "at most four");
        checkNear(out[3].posRange[0], 400.0f, 1e-4f, "the four nearest");
        const f32 e1m = particleLightE1m(80000.0f);
        checkNear(out[0].radiance[0], e1m, 1e-5f, "radiance = colour * E1m");
    }
    {
        // Colour scales radiance.
        scene::WorldLight l = light(100, 0, 0, 40000.0f);
        l.colour[0] = 1.0f; l.colour[1] = 0.5f; l.colour[2] = 0.0f;
        selectParticleLights({l}, {0, 0, 0}, 0.0f, out);
        checkNear(out[0].radiance[1], 0.5f * particleLightE1m(40000.0f), 1e-5f, "colour multiplies the radiance");
        checkNear(out[0].radiance[2], 0.0f, 1e-7f, "...per channel");
    }

    AVER_INFO("ParticleLightingTest: {}/{} checks passed", g_checks - g_failures, g_checks);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
